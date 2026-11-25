/*
 * Mach-O loader: the part of Cider that plays the role of dyld.
 *
 * x86_64 Mach-O images are mapped into this process and executed directly
 * on the CPU (no emulation). Imports from macOS system libraries are bound
 * to Cider's libSystem implementation (libsystem.c); dylibs shipped inside
 * the app bundle are loaded recursively and bound to each other.
 */
#include "cider.h"

#define MH_MAGIC_64   0xfeedfacfu
#define FAT_MAGIC     0xcafebabeu
#define FAT_MAGIC_64  0xcafebabfu
#define CPU_X86_64    0x01000007
#define CPU_ARM64     0x0100000c

#define MH_EXECUTE 2
#define MH_DYLIB   6
#define MH_BUNDLE  8

#define LC_REQ_DYLD 0x80000000u
#define LC_SYMTAB 0x2
#define LC_UNIXTHREAD 0x5
#define LC_DYSYMTAB 0xb
#define LC_LOAD_DYLIB 0xc
#define LC_ID_DYLIB 0xd
#define LC_SEGMENT_64 0x19
#define LC_LOAD_WEAK_DYLIB (0x18 | LC_REQ_DYLD)
#define LC_RPATH (0x1c | LC_REQ_DYLD)
#define LC_REEXPORT_DYLIB (0x1f | LC_REQ_DYLD)
#define LC_LAZY_LOAD_DYLIB 0x20
#define LC_DYLD_INFO 0x22
#define LC_DYLD_INFO_ONLY (0x22 | LC_REQ_DYLD)
#define LC_LOAD_UPWARD_DYLIB (0x23 | LC_REQ_DYLD)
#define LC_MAIN (0x28 | LC_REQ_DYLD)
#define LC_DYLD_EXPORTS_TRIE (0x33 | LC_REQ_DYLD)
#define LC_DYLD_CHAINED_FIXUPS (0x34 | LC_REQ_DYLD)

#define S_MOD_INIT_FUNC_POINTERS 0x9
#define S_THREAD_LOCAL_REGULAR 0x11
#define S_THREAD_LOCAL_ZEROFILL 0x12
#define S_THREAD_LOCAL_VARIABLES 0x13
#define S_INIT_FUNC_OFFSETS 0x16

#pragma pack(push, 1)
typedef struct { uint32_t magic; int32_t cputype, cpusubtype; uint32_t filetype, ncmds, sizeofcmds, flags, reserved; } mach_header_64;
typedef struct { uint32_t cmd, cmdsize; } load_command;
typedef struct { uint32_t cmd, cmdsize; char segname[16]; uint64_t vmaddr, vmsize, fileoff, filesize; int32_t maxprot, initprot; uint32_t nsects, flags; } segment_command_64;
typedef struct { char sectname[16], segname[16]; uint64_t addr, size; uint32_t offset, align, reloff, nreloc, flags, r1, r2, r3; } section_64;
typedef struct { uint32_t cmd, cmdsize, name_off, timestamp, cur, compat; } dylib_command;
typedef struct { uint32_t cmd, cmdsize, path_off; } rpath_command;
typedef struct { uint32_t cmd, cmdsize; uint64_t entryoff, stacksize; } entry_point_command;
typedef struct { uint32_t cmd, cmdsize, rebase_off, rebase_size, bind_off, bind_size, weak_off, weak_size, lazy_off, lazy_size, export_off, export_size; } dyld_info_command;
typedef struct { uint32_t cmd, cmdsize, dataoff, datasize; } linkedit_data_command;
typedef struct { uint32_t cmd, cmdsize, symoff, nsyms, stroff, strsize; } symtab_command;
typedef struct { uint32_t n_strx; uint8_t n_type, n_sect; uint16_t n_desc; uint64_t n_value; } nlist_64;
typedef struct { uint32_t version, starts_offset, imports_offset, symbols_offset, imports_count, imports_format, symbols_format; } chained_header;
typedef struct { uint32_t size; uint16_t page_size, pointer_format; uint64_t segment_offset; uint32_t max_valid_pointer; uint16_t page_count; uint16_t page_start[1]; } chained_starts_seg;
#pragma pack(pop)

typedef struct { char name[17]; uint64_t vmaddr, vmsize, fileoff, filesize; int prot; } segment;
typedef struct { char *name; uint64_t addr; } export_sym;

typedef struct image {
    char *path, *dir;
    uint8_t *file;             /* whole file */
    uint8_t *mh;               /* chosen slice */
    size_t slicelen;
    uint32_t filetype;
    uint64_t pref_base, span;
    uint8_t *base;
    int64_t slide;
    int nsegs;
    segment segs[32];
    int ndylibs;
    char *dylibs[256];
    int dylib_weak[256];
    struct image *deps[256];
    int nrpaths;
    char *rpaths[64];
    int has_main, has_thread;
    uint64_t entryoff, thread_rip;
    dyld_info_command *dyld_info;
    linkedit_data_command *chained, *exports_trie;
    symtab_command *symtab;
    export_sym *exports;
    size_t nexports, cap_exports;
    uint64_t tlv_start, tlv_size;
    DWORD tls_index;
    int index;
} image;

static image *g_images[256];
static int g_nimages;
static image *g_main;
static uint8_t *g_stub_page;
static size_t g_stub_used;
static int g_missing;

/* ------------------------------------------------------------------ */
/* Probe                                                               */

static uint32_t rd32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* Locate the slice for `cpu` in a (possibly fat) file. */
static uint8_t *find_slice(uint8_t *f, size_t len, int32_t cpu, size_t *slen, int *has_x86, int *has_arm, int *has_other)
{
    if (len < 32) return NULL;
    uint32_t magic = rd32be(f);
    if (magic == FAT_MAGIC || magic == FAT_MAGIC_64) {
        uint32_t n = rd32be(f + 4);
        int is64 = magic == FAT_MAGIC_64;
        size_t esz = is64 ? 32 : 20;
        uint8_t *found = NULL;
        if (n > 64 || 8 + n * esz > len) return NULL;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t *e = f + 8 + i * esz;
            int32_t ct = (int32_t)rd32be(e);
            uint64_t off = is64 ? ((uint64_t)rd32be(e + 8) << 32 | rd32be(e + 12)) : rd32be(e + 8);
            uint64_t sz = is64 ? ((uint64_t)rd32be(e + 16) << 32 | rd32be(e + 20)) : rd32be(e + 12);
            if (ct == CPU_X86_64) { if (has_x86) *has_x86 = 1; }
            else if (ct == CPU_ARM64) { if (has_arm) *has_arm = 1; }
            else if (has_other) *has_other = 1;
            if (ct == cpu && off + sz <= len) { found = f + off; *slen = (size_t)sz; }
        }
        return found;
    }
    mach_header_64 *mh = (mach_header_64 *)f;
    if (mh->magic != MH_MAGIC_64) return NULL;
    if (mh->cputype == CPU_X86_64) { if (has_x86) *has_x86 = 1; }
    else if (mh->cputype == CPU_ARM64) { if (has_arm) *has_arm = 1; }
    else if (has_other) *has_other = 1;
    if (mh->cputype != cpu) return NULL;
    *slen = len;
    return f;
}

int macho_is_macho_file(const char *path)
{
    wchar_t *w = utf8_to_w(path);
    FILE *f = _wfopen(w, L"rb");
    free(w);
    if (!f) return 0;
    uint8_t b[4] = {0};
    size_t n = fread(b, 1, 4, f);
    fclose(f);
    if (n < 4) return 0;
    uint32_t be = rd32be(b), le = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
    return be == FAT_MAGIC || be == FAT_MAGIC_64 || le == MH_MAGIC_64 || le == 0xfeedface;
}

int macho_probe(const char *path, macho_probe_t *out)
{
    memset(out, 0, sizeof *out);
    size_t len;
    uint8_t *f = read_file(path, &len);
    if (!f) return -1;
    size_t slen = 0;
    uint8_t *mh = find_slice(f, len, CPU_X86_64, &slen, &out->has_x86_64, &out->has_arm64, &out->has_other);
    if (!mh) mh = find_slice(f, len, CPU_ARM64, &slen, NULL, NULL, NULL);
    out->is_macho = out->has_x86_64 || out->has_arm64 || out->has_other;
    if (mh) {
        mach_header_64 *h = (mach_header_64 *)mh;
        out->filetype = (int)h->filetype;
        uint8_t *p = mh + sizeof *h;
        for (uint32_t i = 0; i < h->ncmds && p + 8 <= mh + slen; i++) {
            load_command *lc = (load_command *)p;
            if ((lc->cmd == LC_LOAD_DYLIB || lc->cmd == LC_LOAD_WEAK_DYLIB || lc->cmd == LC_REEXPORT_DYLIB ||
                 lc->cmd == LC_LOAD_UPWARD_DYLIB || lc->cmd == LC_LAZY_LOAD_DYLIB) && out->ndylibs < MAX_PROBE_DYLIBS) {
                dylib_command *d = (dylib_command *)lc;
                out->dylibs[out->ndylibs++] = xstrdup((char *)p + d->name_off);
            }
            if (!lc->cmdsize) break;
            p += lc->cmdsize;
        }
    }
    free(f);
    return 0;
}

void macho_probe_free(macho_probe_t *p)
{
    for (int i = 0; i < p->ndylibs; i++) free(p->dylibs[i]);
    p->ndylibs = 0;
}

/* ------------------------------------------------------------------ */
/* Symbol stubs for functions Cider does not implement                */

SV static void missing_symbol(const char *name)
{
    fflush(stdout);
    fprintf(stderr,
            "\n[cider] The app called '%s', which Cider does not implement yet.\n"
            "[cider] The app cannot continue.\n", name[0] == '_' ? name + 1 : name);
    fflush(stderr);
    if (g_gui) {
        char *msg = xasprintf("The app called the macOS function '%s', which Cider does not implement yet.",
                              name[0] == '_' ? name + 1 : name);
        ui_message(1, "This app needs a macOS feature Cider doesn't have yet", msg, NULL);
    }
    ExitProcess(127);
}

static void *make_missing_stub(const char *name)
{
    if (!g_stub_page || g_stub_used + 32 > 1 << 20) {
        g_stub_page = VirtualAlloc(NULL, 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        g_stub_used = 0;
    }
    uint8_t *s = g_stub_page + g_stub_used;
    char *keep = xstrdup(name);
    /* mov rdi, imm64 ; mov rax, imm64 ; jmp rax */
    s[0] = 0x48; s[1] = 0xBF; memcpy(s + 2, &keep, 8);
    void *fn = (void *)missing_symbol;
    s[10] = 0x48; s[11] = 0xB8; memcpy(s + 12, &fn, 8);
    s[20] = 0xFF; s[21] = 0xE0;
    g_stub_used += 32;
    g_missing++;
    return s;
}

/* ------------------------------------------------------------------ */
/* Exports                                                             */

static void add_export(image *im, const char *name, uint64_t addr)
{
    if (im->nexports == im->cap_exports) {
        im->cap_exports = im->cap_exports ? im->cap_exports * 2 : 256;
        im->exports = xrealloc(im->exports, im->cap_exports * sizeof(export_sym));
    }
    im->exports[im->nexports].name = (char *)name;
    im->exports[im->nexports].addr = addr;
    im->nexports++;
}

static int export_cmp(const void *a, const void *b)
{
    return strcmp(((const export_sym *)a)->name, ((const export_sym *)b)->name);
}

static void build_exports(image *im)
{
    if (!im->symtab) return;
    nlist_64 *syms = (nlist_64 *)(im->mh + im->symtab->symoff);
    const char *strs = (const char *)(im->mh + im->symtab->stroff);
    for (uint32_t i = 0; i < im->symtab->nsyms; i++) {
        nlist_64 *s = &syms[i];
        if (s->n_type & 0xe0) continue;               /* stab */
        if (!(s->n_type & 0x01)) continue;            /* not external */
        if ((s->n_type & 0x0e) != 0x0e) continue;     /* not defined in a section */
        if (s->n_strx >= im->symtab->strsize) continue;
        add_export(im, strs + s->n_strx, (uint64_t)(im->base + (s->n_value - im->pref_base)));
    }
    qsort(im->exports, im->nexports, sizeof(export_sym), export_cmp);
}

static void *image_lookup(image *im, const char *name)
{
    if (!im || !im->nexports) return NULL;
    export_sym key = { (char *)name, 0 };
    export_sym *e = bsearch(&key, im->exports, im->nexports, sizeof(export_sym), export_cmp);
    return e ? (void *)e->addr : NULL;
}

void *macho_dlsym(const char *name)
{
    for (int i = 0; i < g_nimages; i++) {
        void *p = image_lookup(g_images[i], name);
        if (p) return p;
    }
    return NULL;
}

static void *resolve(image *im, int ordinal, const char *name, int weak)
{
    void *p = NULL;
    if (ordinal > 0 && ordinal <= im->ndylibs && im->deps[ordinal - 1])
        p = image_lookup(im->deps[ordinal - 1], name);
    else if (ordinal == 0)
        p = image_lookup(im, name);
    else if (ordinal == -1)
        p = image_lookup(g_main, name);
    if (!p) p = shim_lookup(name);
    if (!p) p = macho_dlsym(name);
    if (!p) {
        if (weak) return NULL;
        vlog("missing symbol %s (needed by %s)", name, path_basename(im->path));
        p = make_missing_stub(name);
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* Loading                                                             */

static uint64_t uleb(const uint8_t **p, const uint8_t *end)
{
    uint64_t r = 0;
    int shift = 0;
    while (*p < end) {
        uint8_t b = *(*p)++;
        r |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    return r;
}

static int64_t sleb(const uint8_t **p, const uint8_t *end)
{
    int64_t r = 0;
    int shift = 0;
    uint8_t b = 0;
    while (*p < end) {
        b = *(*p)++;
        r |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    if (shift < 64 && (b & 0x40)) r |= -((int64_t)1 << shift);
    return r;
}

static int is_system_dylib(const char *p)
{
    return !strncmp(p, "/usr/lib/", 9) || !strncmp(p, "/System/", 8);
}

static char *subst_path(const char *p, image *im)
{
    if (!strncmp(p, "@executable_path", 16)) return path_join(g_main ? g_main->dir : im->dir, p + 16);
    if (!strncmp(p, "@loader_path", 12)) return path_join(im->dir, p + 12);
    return xstrdup(p);
}

static image *load_image(const char *path, image *loader);

static image *load_dep(image *im, const char *name)
{
    if (is_system_dylib(name)) return NULL;
    char *cands[130];
    int nc = 0;
    if (!strncmp(name, "@rpath/", 7)) {
        image *chain[2] = { im, g_main };
        for (int c = 0; c < 2; c++) {
            if (!chain[c]) continue;
            for (int r = 0; r < chain[c]->nrpaths && nc < 128; r++) {
                char *base = subst_path(chain[c]->rpaths[r], chain[c]);
                cands[nc++] = path_join(base, name + 7);
                free(base);
            }
        }
        cands[nc++] = path_join(im->dir, name + 7);
        if (g_main) {
            char *fw = path_join(g_main->dir, "..\\Frameworks");
            cands[nc++] = path_join(fw, name + 7);
            free(fw);
        }
    } else {
        cands[nc++] = subst_path(name, im);
    }
    image *found = NULL;
    for (int i = 0; i < nc; i++) {
        if (!found && is_file(cands[i])) found = load_image(cands[i], im);
        free(cands[i]);
    }
    if (!found) vlog("could not find dylib %s", name);
    return found;
}

static void parse_commands(image *im)
{
    mach_header_64 *h = (mach_header_64 *)im->mh;
    uint8_t *p = im->mh + sizeof *h;
    uint8_t *end = im->mh + im->slicelen;
    for (uint32_t i = 0; i < h->ncmds && p + 8 <= end; i++) {
        load_command *lc = (load_command *)p;
        switch (lc->cmd) {
        case LC_SEGMENT_64: {
            segment_command_64 *s = (segment_command_64 *)lc;
            if (im->nsegs < 32) {
                segment *sg = &im->segs[im->nsegs++];
                memcpy(sg->name, s->segname, 16);
                sg->name[16] = 0;
                sg->vmaddr = s->vmaddr;
                sg->vmsize = s->vmsize;
                sg->fileoff = s->fileoff;
                sg->filesize = s->filesize;
                sg->prot = s->initprot;
            }
            break;
        }
        case LC_LOAD_DYLIB: case LC_LOAD_WEAK_DYLIB: case LC_REEXPORT_DYLIB:
        case LC_LOAD_UPWARD_DYLIB: case LC_LAZY_LOAD_DYLIB:
            if (im->ndylibs < 256) {
                im->dylib_weak[im->ndylibs] = lc->cmd == LC_LOAD_WEAK_DYLIB;
                im->dylibs[im->ndylibs++] = (char *)p + ((dylib_command *)lc)->name_off;
            }
            break;
        case LC_RPATH:
            if (im->nrpaths < 64) im->rpaths[im->nrpaths++] = (char *)p + ((rpath_command *)lc)->path_off;
            break;
        case LC_MAIN:
            im->has_main = 1;
            im->entryoff = ((entry_point_command *)lc)->entryoff;
            break;
        case LC_UNIXTHREAD: {
            uint32_t flavor = *(uint32_t *)(p + 8);
            if (flavor == 4) { im->has_thread = 1; im->thread_rip = *(uint64_t *)(p + 16 + 16 * 8); }
            break;
        }
        case LC_DYLD_INFO: case LC_DYLD_INFO_ONLY:
            im->dyld_info = (dyld_info_command *)lc;
            break;
        case LC_DYLD_CHAINED_FIXUPS:
            im->chained = (linkedit_data_command *)lc;
            break;
        case LC_DYLD_EXPORTS_TRIE:
            im->exports_trie = (linkedit_data_command *)lc;
            break;
        case LC_SYMTAB:
            im->symtab = (symtab_command *)lc;
            break;
        }
        if (!lc->cmdsize) break;
        p += lc->cmdsize;
    }
}

static int map_image(image *im)
{
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < im->nsegs; i++) {
        segment *s = &im->segs[i];
        if (!strcmp(s->name, "__PAGEZERO") || !s->vmsize) continue;
        if (s->vmaddr < lo) lo = s->vmaddr;
        if (s->vmaddr + s->vmsize > hi) hi = s->vmaddr + s->vmsize;
    }
    if (lo == UINT64_MAX) return -1;
    im->pref_base = lo;
    im->span = hi - lo;
    uint8_t *b = VirtualAlloc((void *)(uintptr_t)lo, im->span, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!b) b = VirtualAlloc(NULL, im->span, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!b) return -1;
    im->base = b;
    im->slide = (int64_t)((uint64_t)b - lo);
    for (int i = 0; i < im->nsegs; i++) {
        segment *s = &im->segs[i];
        if (!strcmp(s->name, "__PAGEZERO") || !s->filesize) continue;
        if (s->fileoff + s->filesize > im->slicelen) return -1;
        memcpy(b + (s->vmaddr - lo), im->mh + s->fileoff, (size_t)min(s->filesize, s->vmsize));
    }
    return 0;
}

static image *load_image(const char *path, image *loader)
{
    for (int i = 0; i < g_nimages; i++)
        if (!_stricmp(g_images[i]->path, path)) return g_images[i];
    if (g_nimages >= 256) return NULL;

    image *im = xcalloc(1, sizeof *im);
    size_t len;
    im->file = read_file(path, &len);
    if (!im->file) { free(im); return NULL; }
    im->path = xstrdup(path);
    im->dir = path_dirname(path);
    im->mh = find_slice(im->file, len, CPU_X86_64, &im->slicelen, NULL, NULL, NULL);
    if (!im->mh) {
        vlog("%s has no x86_64 code", path);
        free(im->file);
        free(im);
        return NULL;
    }
    im->filetype = ((mach_header_64 *)im->mh)->filetype;
    parse_commands(im);
    if (map_image(im)) {
        vlog("failed to map %s", path);
        return NULL;
    }
    im->index = g_nimages;
    g_images[g_nimages++] = im;
    if (!g_main) g_main = im;
    vlog("mapped %s at %p (slide %+lld)", path_basename(path), im->base, (long long)im->slide);
    libsystem_register_image(im->base, im->path);
    build_exports(im);
    (void)loader;
    for (int i = 0; i < im->ndylibs; i++)
        im->deps[i] = load_dep(im, im->dylibs[i]);
    return im;
}

/* ------------------------------------------------------------------ */
/* Fixups                                                              */

static uint8_t *seg_addr(image *im, int seg, uint64_t off)
{
    if (seg < 0 || seg >= im->nsegs) return NULL;
    return im->base + (im->segs[seg].vmaddr - im->pref_base) + off;
}

static void do_rebase(image *im)
{
    if (!im->dyld_info || !im->dyld_info->rebase_size || !im->slide) return;
    const uint8_t *p = im->mh + im->dyld_info->rebase_off;
    const uint8_t *end = p + im->dyld_info->rebase_size;
    int seg = 0;
    uint64_t off = 0;
    while (p < end) {
        uint8_t b = *p++, op = b & 0xF0, imm = b & 0x0F;
        uint64_t cnt, skip;
        switch (op) {
        case 0x00: return;
        case 0x10: break;
        case 0x20: seg = imm; off = uleb(&p, end); break;
        case 0x30: off += uleb(&p, end); break;
        case 0x40: off += (uint64_t)imm * 8; break;
        case 0x50: case 0x60:
            cnt = op == 0x50 ? imm : uleb(&p, end);
            while (cnt--) { *(uint64_t *)seg_addr(im, seg, off) += (uint64_t)im->slide; off += 8; }
            break;
        case 0x70:
            *(uint64_t *)seg_addr(im, seg, off) += (uint64_t)im->slide;
            off += 8 + uleb(&p, end);
            break;
        case 0x80:
            cnt = uleb(&p, end);
            skip = uleb(&p, end);
            while (cnt--) { *(uint64_t *)seg_addr(im, seg, off) += (uint64_t)im->slide; off += 8 + skip; }
            break;
        default: return;
        }
    }
}

static void do_bind_stream(image *im, uint32_t boff, uint32_t bsize, int lazy)
{
    if (!bsize) return;
    const uint8_t *p = im->mh + boff;
    const uint8_t *end = p + bsize;
    int seg = 0, ordinal = 0, weak = 0;
    uint64_t off = 0;
    int64_t addend = 0;
    const char *name = "";
    while (p < end) {
        uint8_t b = *p++, op = b & 0xF0, imm = b & 0x0F;
        uint64_t cnt, skip;
        switch (op) {
        case 0x00:
            if (!lazy) return;
            break;
        case 0x10: ordinal = imm; break;
        case 0x20: ordinal = (int)uleb(&p, end); break;
        case 0x30: ordinal = imm ? (int)(int8_t)(0xF0 | imm) : 0; break;
        case 0x40:
            weak = imm & 1;
            name = (const char *)p;
            while (p < end && *p) p++;
            p++;
            break;
        case 0x50: break;
        case 0x60: addend = sleb(&p, end); break;
        case 0x70: seg = imm; off = uleb(&p, end); break;
        case 0x80: off += uleb(&p, end); break;
        case 0x90: case 0xA0: case 0xB0: case 0xC0: {
            uint8_t *val = resolve(im, ordinal, name, weak);
            uint64_t v = (uint64_t)val ? (uint64_t)val + (uint64_t)addend : 0;
            if (op == 0xC0) {
                cnt = uleb(&p, end);
                skip = uleb(&p, end);
                while (cnt--) { *(uint64_t *)seg_addr(im, seg, off) = v; off += 8 + skip; }
                break;
            }
            *(uint64_t *)seg_addr(im, seg, off) = v;
            off += 8;
            if (op == 0xA0) off += uleb(&p, end);
            else if (op == 0xB0) off += (uint64_t)imm * 8;
            break;
        }
        default:
            return;
        }
    }
}

static void do_chained(image *im)
{
    uint8_t *data = im->mh + im->chained->dataoff;
    chained_header *h = (chained_header *)data;
    uint64_t *imports = xcalloc(h->imports_count + 1, sizeof(uint64_t));
    const char *syms = (const char *)data + h->symbols_offset;
    uint8_t *imp = data + h->imports_offset;
    for (uint32_t i = 0; i < h->imports_count; i++) {
        int ordinal, weak;
        uint32_t nameoff;
        int64_t addend = 0;
        if (h->imports_format == 1 || h->imports_format == 2) {
            uint32_t v = ((uint32_t *)imp)[h->imports_format == 1 ? i : i * 2];
            ordinal = (int)(int8_t)(v & 0xFF);
            weak = (v >> 8) & 1;
            nameoff = v >> 9;
            if (h->imports_format == 2) addend = ((int32_t *)imp)[i * 2 + 1];
        } else {
            uint64_t v = ((uint64_t *)imp)[i * 2];
            ordinal = (int)(int16_t)(v & 0xFFFF);
            weak = (v >> 16) & 1;
            nameoff = (uint32_t)(v >> 32);
            addend = (int64_t)((uint64_t *)imp)[i * 2 + 1];
        }
        uint8_t *val = resolve(im, ordinal, syms + nameoff, weak);
        imports[i] = val ? (uint64_t)val + (uint64_t)addend : 0;
    }

    uint8_t *starts = data + h->starts_offset;
    uint32_t segcount = *(uint32_t *)starts;
    for (uint32_t s = 0; s < segcount; s++) {
        uint32_t soff = ((uint32_t *)(starts + 4))[s];
        if (!soff) continue;
        chained_starts_seg *ss = (chained_starts_seg *)(starts + soff);
        for (uint16_t pg = 0; pg < ss->page_count; pg++) {
            uint16_t start = ss->page_start[pg];
            if (start == 0xFFFF) continue;
            uint8_t *page = im->base + ss->segment_offset + (uint64_t)pg * ss->page_size;
            uint8_t *loc = page + start;
            for (;;) {
                uint64_t raw = *(uint64_t *)loc;
                uint64_t next = (raw >> 51) & 0xFFF;
                uint64_t v;
                if (raw >> 63) {
                    uint32_t ord = raw & 0xFFFFFF;
                    uint64_t add = (raw >> 24) & 0xFF;
                    v = ord < h->imports_count ? imports[ord] + add : 0;
                } else {
                    uint64_t target = raw & 0xFFFFFFFFFULL, high8 = (raw >> 36) & 0xFF;
                    if (ss->pointer_format == 6) v = (uint64_t)im->base + target;    /* PTR_64_OFFSET */
                    else v = target + (uint64_t)im->slide;                           /* PTR_64 */
                    v |= high8 << 56;
                }
                *(uint64_t *)loc = v;
                if (!next) break;
                loc += next * 4;
            }
        }
    }
    free(imports);
}

/* ------------------------------------------------------------------ */
/* Thread-local variables                                              */

extern void cider_tlv_thunk(void);   /* thunks.S: preserves all registers */

void *cider_tlv_addr(uint64_t *desc)
{
    image *im = g_images[desc[1]];
    uint8_t *blk = TlsGetValue(im->tls_index);
    if (!blk) {
        blk = xcalloc(1, (size_t)im->tlv_size + 16);
        memcpy(blk, im->base + (im->tlv_start - im->pref_base), (size_t)im->tlv_size);
        TlsSetValue(im->tls_index, blk);
    }
    return blk + desc[2];
}

static void sections(image *im, void (*cb)(image *, section_64 *, void *), void *ctx)
{
    mach_header_64 *h = (mach_header_64 *)im->mh;
    uint8_t *p = im->mh + sizeof *h;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        load_command *lc = (load_command *)p;
        if (lc->cmd == LC_SEGMENT_64) {
            segment_command_64 *s = (segment_command_64 *)lc;
            section_64 *sec = (section_64 *)(s + 1);
            for (uint32_t j = 0; j < s->nsects; j++) cb(im, &sec[j], ctx);
        }
        p += lc->cmdsize;
    }
}

static void tlv_scan(image *im, section_64 *s, void *ctx)
{
    (void)ctx;
    uint8_t type = s->flags & 0xFF;
    if (type == S_THREAD_LOCAL_REGULAR || type == S_THREAD_LOCAL_ZEROFILL) {
        if (!im->tlv_size) { im->tlv_start = s->addr; im->tlv_size = s->size; }
        else {
            uint64_t end = max(im->tlv_start + im->tlv_size, s->addr + s->size);
            im->tlv_start = min(im->tlv_start, s->addr);
            im->tlv_size = end - im->tlv_start;
        }
    }
}

static void tlv_fix(image *im, section_64 *s, void *ctx)
{
    (void)ctx;
    if ((s->flags & 0xFF) != S_THREAD_LOCAL_VARIABLES) return;
    uint64_t *d = (uint64_t *)(im->base + (s->addr - im->pref_base));
    for (uint64_t i = 0; i < s->size / 24; i++) {
        d[i * 3 + 0] = (uint64_t)cider_tlv_thunk;
        d[i * 3 + 1] = (uint64_t)im->index;
    }
}

/* ------------------------------------------------------------------ */
/* Initializers, protection, entry                                    */

typedef SV void (*init_fn)(int, char **, char **, char **, void *);
static int g_argc;
static char **g_argv, **g_envp;

static void run_inits(image *im, section_64 *s, void *ctx)
{
    (void)ctx;
    uint8_t type = s->flags & 0xFF;
    static char *apple[] = { "executable_path=", NULL };
    if (type == S_MOD_INIT_FUNC_POINTERS) {
        uint64_t *fp = (uint64_t *)(im->base + (s->addr - im->pref_base));
        for (uint64_t i = 0; i < s->size / 8; i++)
            ((init_fn)fp[i])(g_argc, g_argv, g_envp, apple, NULL);
    } else if (type == S_INIT_FUNC_OFFSETS) {
        uint32_t *off = (uint32_t *)(im->base + (s->addr - im->pref_base));
        for (uint64_t i = 0; i < s->size / 4; i++)
            ((init_fn)(im->base + off[i]))(g_argc, g_argv, g_envp, apple, NULL);
    }
}

static void protect(image *im)
{
    for (int i = 0; i < im->nsegs; i++) {
        segment *s = &im->segs[i];
        if (!strcmp(s->name, "__PAGEZERO") || !s->vmsize) continue;
        DWORD prot, old;
        int r = s->prot & 1, w = s->prot & 2, x = s->prot & 4;
        if (x) prot = w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
        else if (w) prot = PAGE_READWRITE;
        else if (r) prot = PAGE_READONLY;
        else prot = PAGE_NOACCESS;
        VirtualProtect(im->base + (s->vmaddr - im->pref_base), (SIZE_T)s->vmsize, prot, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), im->base, (SIZE_T)im->span);
}

typedef SV int (*main_fn)(int, char **, char **, char **);
extern char **cider_environ;
extern void cider_jump_start(void *entry, void *sp);  /* thunks.S */

static DWORD WINAPI main_thread(LPVOID unused)
{
    (void)unused;
    static char *apple[4];
    apple[0] = xasprintf("executable_path=%s", g_main->path);
    /* dependencies were appended after their dependents, so run inits back to front */
    for (int i = g_nimages - 1; i >= 0; i--) sections(g_images[i], run_inits, NULL);
    if (g_main->has_main) {
        main_fn m = (main_fn)(g_main->base + g_main->entryoff);
        int rc = m(g_argc, g_argv, g_envp, apple);
        shim_exit(rc);
    }
    /* Classic LC_UNIXTHREAD: build the initial process stack and jump to start. */
    int nenv = 0;
    while (g_envp[nenv]) nenv++;
    size_t words = 1 + (size_t)g_argc + 1 + (size_t)nenv + 1 + 2 + 2;
    uint64_t *stk = __builtin_alloca(words * 8 + 64);
    stk = (uint64_t *)(((uintptr_t)stk + 15) & ~(uintptr_t)15);
    size_t k = 0;
    stk[k++] = (uint64_t)g_argc;
    for (int i = 0; i < g_argc; i++) stk[k++] = (uint64_t)g_argv[i];
    stk[k++] = 0;
    for (int i = 0; i < nenv; i++) stk[k++] = (uint64_t)g_envp[i];
    stk[k++] = 0;
    stk[k++] = (uint64_t)apple[0];
    stk[k++] = 0;
    cider_jump_start((void *)(g_main->thread_rip + (uint64_t)g_main->slide), stk);
    return 0;
}

int macho_run(const char *path, int argc, char **argv)
{
    vlog("loading %s", path);
    image *im = load_image(path, NULL);
    if (!im) return -1;
    if (im->filetype != MH_EXECUTE || (!im->has_main && !im->has_thread)) {
        fprintf(stderr, "cider: %s is not a runnable macOS program\n", path);
        return -1;
    }

    libsystem_init(argc, argv, path);
    g_argc = argc;
    g_argv = argv;
    g_envp = cider_environ;

    for (int i = 0; i < g_nimages; i++) {
        image *m = g_images[i];
        sections(m, tlv_scan, NULL);
        if (m->tlv_size) m->tls_index = TlsAlloc();
        do_rebase(m);
        if (m->chained) do_chained(m);
        if (m->dyld_info) {
            do_bind_stream(m, m->dyld_info->bind_off, m->dyld_info->bind_size, 0);
            do_bind_stream(m, m->dyld_info->lazy_off, m->dyld_info->lazy_size, 1);
        }
        sections(m, tlv_fix, NULL);
    }
    if (g_missing) vlog("%d imported symbols are not implemented by Cider", g_missing);
    for (int i = 0; i < g_nimages; i++) protect(g_images[i]);

    HANDLE t = CreateThread(NULL, 8u << 20, main_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t) return -1;
    WaitForSingleObject(t, INFINITE);
    DWORD code = 0;
    GetExitCodeThread(t, &code);
    shim_exit((int)code);
}
