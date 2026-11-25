/*
 * Apple disk image (.dmg) reader.
 *
 * Handles the UDIF container (raw, zero-fill, zlib and ADC chunks) and the
 * HFS+ / HFSX filesystem inside it, extracting the volume to a folder.
 * Images using bzip2/LZFSE/LZMA chunks or APFS are left to 7-Zip (see main.c).
 */
#include "cider.h"

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

/* ------------------------------------------------------------------ */
/* inflate (RFC 1951)                                                 */

typedef struct {
    const uint8_t *in;
    size_t inlen, inpos;
    uint8_t *out;
    size_t outlen, outpos;
    uint32_t bitbuf;
    int bitcnt, err;
} inf;

typedef struct { short count[16], symbol[288]; } huff;

static int bits(inf *s, int need)
{
    uint32_t v = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->inpos >= s->inlen) { s->err = 1; return 0; }
        v |= (uint32_t)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> need;
    s->bitcnt -= need;
    return (int)(v & ((1u << need) - 1));
}

static int decode(inf *s, const huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= bits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = 1;
    return -1;
}

static void construct(huff *h, const short *len, int n)
{
    short offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (short)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++) if (len[i]) h->symbol[offs[len[i]]++] = (short)i;
}

static const short lbase[] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const short lext[] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const short dbase[] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const short dext[] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static int codes(inf *s, const huff *lc, const huff *dc)
{
    for (;;) {
        int sym = decode(s, lc);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->outpos >= s->outlen) return -1;
            s->out[s->outpos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = lbase[sym] + bits(s, lext[sym]);
            int ds = decode(s, dc);
            if (ds < 0 || ds >= 30) return -1;
            size_t dist = (size_t)(dbase[ds] + bits(s, dext[ds]));
            if (s->err || dist > s->outpos || s->outpos + (size_t)len > s->outlen) return -1;
            for (int i = 0; i < len; i++, s->outpos++) s->out[s->outpos] = s->out[s->outpos - dist];
        }
    }
}

static int inflate_raw(inf *s)
{
    static huff flc, fdc;
    static int fixed_ready;
    if (!fixed_ready) {
        short l[288];
        int i;
        for (i = 0; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        construct(&flc, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        construct(&fdc, l, 30);
        fixed_ready = 1;
    }
    int last;
    do {
        last = bits(s, 1);
        int type = bits(s, 2);
        if (s->err) return -1;
        if (type == 0) {
            s->bitbuf = 0;
            s->bitcnt = 0;
            if (s->inpos + 4 > s->inlen) return -1;
            unsigned len = s->in[s->inpos] | s->in[s->inpos + 1] << 8;
            s->inpos += 4;
            if (s->inpos + len > s->inlen || s->outpos + len > s->outlen) return -1;
            memcpy(s->out + s->outpos, s->in + s->inpos, len);
            s->inpos += len;
            s->outpos += len;
        } else if (type == 1) {
            if (codes(s, &flc, &fdc)) return -1;
        } else if (type == 2) {
            static const short order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            short lengths[320];
            int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
            if (nlen > 286 || ndist > 30) return -1;
            int i;
            for (i = 0; i < ncode; i++) lengths[order[i]] = (short)bits(s, 3);
            for (; i < 19; i++) lengths[order[i]] = 0;
            huff lc, dc;
            construct(&lc, lengths, 19);
            i = 0;
            while (i < nlen + ndist) {
                int sym = decode(s, &lc);
                if (sym < 0) return -1;
                if (sym < 16) { lengths[i++] = (short)sym; continue; }
                int len = 0, rep;
                if (sym == 16) { if (!i) return -1; len = lengths[i - 1]; rep = 3 + bits(s, 2); }
                else if (sym == 17) rep = 3 + bits(s, 3);
                else rep = 11 + bits(s, 7);
                if (i + rep > nlen + ndist) return -1;
                while (rep--) lengths[i++] = (short)len;
            }
            construct(&lc, lengths, nlen);
            construct(&dc, lengths + nlen, ndist);
            if (codes(s, &lc, &dc)) return -1;
        } else {
            return -1;
        }
    } while (!last);
    return 0;
}

int inflate_zlib(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen, size_t *produced)
{
    if (inlen < 2) return -1;
    size_t skip = ((in[0] & 0x0F) == 8 && ((in[0] << 8) | in[1]) % 31 == 0) ? 2 : 0;
    inf s = { in, inlen, skip, out, outlen, 0, 0, 0, 0 };
    int r = inflate_raw(&s);
    if (produced) *produced = s.outpos;
    return r;
}

static int adc_decompress(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen)
{
    size_t i = 0, o = 0;
    while (i < inlen && o < outlen) {
        uint8_t b = in[i];
        if (b & 0x80) {
            size_t n = (size_t)(b & 0x7F) + 1;
            if (i + 1 + n > inlen || o + n > outlen) return -1;
            memcpy(out + o, in + i + 1, n);
            i += 1 + n;
            o += n;
            continue;
        }
        size_t n, off;
        if (b & 0x40) {
            if (i + 3 > inlen) return -1;
            n = (size_t)(b & 0x3F) + 4;
            off = (size_t)in[i + 1] << 8 | in[i + 2];
            i += 3;
        } else {
            if (i + 2 > inlen) return -1;
            n = (size_t)((b & 0x3C) >> 2) + 3;
            off = (size_t)(b & 3) << 8 | in[i + 1];
            i += 2;
        }
        if (off + 1 > o || o + n > outlen) return -1;
        for (size_t k = 0; k < n; k++, o++) out[o] = out[o - off - 1];
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* UDIF virtual disk                                                  */

typedef struct { uint32_t type; uint64_t sector, nsectors, coff, clen; } chunk;

typedef struct {
    HANDLE h;
    chunk *chunks;
    size_t nchunks;
    uint64_t total_sectors;
    uint8_t *cache;
    size_t cache_idx, cache_cap;
    int cache_valid;
    uint8_t *cbuf;
    size_t cbuf_cap;
    char *err;
    size_t errlen;
    int unsupported;
} vdisk;

static int file_read_at(HANDLE h, uint64_t off, void *buf, size_t n)
{
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    return ReadFile(h, buf, (DWORD)n, &got, &ov) && got == n ? 0 : -1;
}

static int chunk_cmp(const void *a, const void *b)
{
    const chunk *x = a, *y = b;
    return x->sector < y->sector ? -1 : x->sector > y->sector;
}

static int load_chunk(vdisk *d, size_t idx)
{
    if (d->cache_valid && d->cache_idx == idx) return 0;
    chunk *c = &d->chunks[idx];
    size_t outn = (size_t)c->nsectors * 512;
    if (outn > d->cache_cap) { d->cache = xrealloc(d->cache, outn); d->cache_cap = outn; }
    d->cache_valid = 0;
    switch (c->type) {
    case 0x00000000: case 0x00000002:
        memset(d->cache, 0, outn);
        break;
    case 0x00000001:
        if (c->clen < outn) memset(d->cache, 0, outn);
        if (file_read_at(d->h, c->coff, d->cache, (size_t)min(c->clen, (uint64_t)outn))) return -1;
        break;
    case 0x80000004: case 0x80000005: {
        if (c->clen > d->cbuf_cap) { d->cbuf = xrealloc(d->cbuf, (size_t)c->clen); d->cbuf_cap = (size_t)c->clen; }
        if (file_read_at(d->h, c->coff, d->cbuf, (size_t)c->clen)) return -1;
        size_t got = outn;
        int r = c->type == 0x80000005 ? inflate_zlib(d->cbuf, (size_t)c->clen, d->cache, outn, &got)
                                      : adc_decompress(d->cbuf, (size_t)c->clen, d->cache, outn);
        if (r) return -1;
        if (got < outn) memset(d->cache + got, 0, outn - got);
        break;
    }
    default:
        snprintf(d->err, d->errlen, "disk image uses compression type 0x%08x", c->type);
        d->unsupported = 1;
        return -1;
    }
    d->cache_idx = idx;
    d->cache_valid = 1;
    return 0;
}

static int vread(vdisk *d, uint64_t off, void *buf, size_t n)
{
    uint8_t *out = buf;
    while (n) {
        uint64_t sec = off / 512;
        size_t lo = 0, hi = d->nchunks;
        while (lo < hi) {
            size_t m = (lo + hi) / 2;
            if (d->chunks[m].sector + d->chunks[m].nsectors <= sec) lo = m + 1;
            else hi = m;
        }
        if (lo >= d->nchunks || d->chunks[lo].sector > sec) {
            /* gap between chunks reads as zeroes */
            memset(out, 0, n);
            return 0;
        }
        if (load_chunk(d, lo)) return -1;
        chunk *c = &d->chunks[lo];
        uint64_t cstart = c->sector * 512, cend = cstart + c->nsectors * 512;
        size_t take = (size_t)min((uint64_t)n, cend - off);
        memcpy(out, d->cache + (off - cstart), take);
        out += take;
        off += take;
        n -= take;
    }
    return 0;
}

static int udif_open(vdisk *d, const char *path)
{
    wchar_t *w = utf8_to_w(path);
    d->h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    free(w);
    if (d->h == INVALID_HANDLE_VALUE) { snprintf(d->err, d->errlen, "cannot open file"); return -1; }
    LARGE_INTEGER sz;
    GetFileSizeEx(d->h, &sz);
    uint8_t koly[512];
    if (sz.QuadPart < 512 || file_read_at(d->h, (uint64_t)sz.QuadPart - 512, koly, 512) || memcmp(koly, "koly", 4)) {
        /* Not UDIF: treat the whole file as a raw disk image. */
        d->chunks = xcalloc(1, sizeof(chunk));
        d->chunks[0].type = 1;
        d->chunks[0].nsectors = (uint64_t)sz.QuadPart / 512;
        d->chunks[0].clen = (uint64_t)sz.QuadPart;
        d->nchunks = 1;
        d->total_sectors = d->chunks[0].nsectors;
        return 0;
    }
    uint64_t dataoff = be64(koly + 24);
    uint64_t xmloff = be64(koly + 216), xmllen = be64(koly + 224);
    if (!xmllen || xmllen > (64u << 20)) { snprintf(d->err, d->errlen, "disk image has no partition table"); return -1; }
    char *xml = xmalloc((size_t)xmllen + 1);
    if (file_read_at(d->h, xmloff, xml, (size_t)xmllen)) { free(xml); return -1; }
    xml[xmllen] = 0;
    plnode *root = plist_parse((uint8_t *)xml, (size_t)xmllen);
    free(xml);
    plnode *blkx = pl_get(pl_get(root, "resource-fork"), "blkx");
    if (!blkx || blkx->t != PL_ARRAY) { plist_free(root); snprintf(d->err, d->errlen, "disk image has no block table"); return -1; }
    size_t cap = 0;
    for (int i = 0; i < blkx->n; i++) {
        plnode *data = pl_get(blkx->items[i], "Data");
        if (!data || data->t != PL_DATA || data->len < 204) continue;
        const uint8_t *m = (const uint8_t *)data->str;
        if (memcmp(m, "mish", 4)) continue;
        uint64_t first = be64(m + 8), mdata = be64(m + 24);
        uint32_t n = be32(m + 200);
        if (204 + (size_t)n * 40 > data->len) continue;
        for (uint32_t k = 0; k < n; k++) {
            const uint8_t *e = m + 204 + k * 40;
            uint32_t type = be32(e);
            if (type == 0xFFFFFFFF || type == 0x7FFFFFFE) continue;
            if (d->nchunks == cap) { cap = cap ? cap * 2 : 1024; d->chunks = xrealloc(d->chunks, cap * sizeof(chunk)); }
            chunk *c = &d->chunks[d->nchunks++];
            c->type = type;
            c->sector = first + be64(e + 8);
            c->nsectors = be64(e + 16);
            c->coff = dataoff + mdata + be64(e + 24);
            c->clen = be64(e + 32);
            if (!c->nsectors) d->nchunks--;
            else if (c->sector + c->nsectors > d->total_sectors) d->total_sectors = c->sector + c->nsectors;
        }
    }
    plist_free(root);
    qsort(d->chunks, d->nchunks, sizeof(chunk), chunk_cmp);
    return d->nchunks ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* HFS+                                                               */

typedef struct { uint32_t start, count; } extent;

typedef struct {
    vdisk *d;
    uint64_t voff;           /* byte offset of the volume in the disk */
    uint32_t blocksize;
    extent *ext_ext;         /* extents of the extents-overflow file */
    int next_ext;
    uint8_t *extents_file;   /* whole extents-overflow file */
    size_t extents_len;
} hfs;

typedef struct { uint64_t size; int n; extent *e; } fork_t;

typedef struct {
    uint32_t id, parent;
    int type;                 /* 1 folder, 2 file */
    char *name;
    uint32_t ftype, fcreator, inode;
    uint8_t fork[80];
} centry;

static void fork_parse(fork_t *f, const uint8_t *fd)
{
    f->size = be64(fd);
    f->n = 0;
    f->e = xmalloc(8 * sizeof(extent));
    for (int i = 0; i < 8; i++) {
        extent e = { be32(fd + 16 + i * 8), be32(fd + 20 + i * 8) };
        if (e.count) f->e[f->n++] = e;
    }
}

/* Append overflow extents for (fileid, data fork) from the extents B-tree. */
static void fork_overflow(hfs *h, fork_t *f, uint32_t fileid)
{
    if (!h->extents_file || h->extents_len < 512) return;
    uint32_t nodesize = be16(h->extents_file + 32);
    uint32_t leaf = be32(h->extents_file + 24);
    if (!nodesize) return;
    while (leaf && (uint64_t)(leaf + 1) * nodesize <= h->extents_len) {
        const uint8_t *node = h->extents_file + (size_t)leaf * nodesize;
        uint16_t nrec = be16(node + 10);
        for (uint16_t r = 0; r < nrec; r++) {
            uint16_t off = be16(node + nodesize - 2 * (r + 1));
            if (off + 12 > nodesize) continue;
            const uint8_t *k = node + off;
            uint16_t klen = be16(k);
            if (k[2] != 0 || be32(k + 4) != fileid) continue;
            const uint8_t *rec = k + 2 + klen;
            if (rec + 64 > node + nodesize) continue;
            for (int i = 0; i < 8; i++) {
                extent e = { be32(rec + i * 8), be32(rec + 4 + i * 8) };
                if (!e.count) continue;
                f->e = xrealloc(f->e, (size_t)(f->n + 1) * sizeof(extent));
                f->e[f->n++] = e;
            }
        }
        leaf = be32(node);
    }
}

static int fork_read(hfs *h, fork_t *f, uint64_t off, uint8_t *buf, size_t n)
{
    uint64_t pos = 0;
    for (int i = 0; i < f->n && n; i++) {
        uint64_t elen = (uint64_t)f->e[i].count * h->blocksize;
        if (off < pos + elen) {
            uint64_t within = off - pos;
            size_t take = (size_t)min((uint64_t)n, elen - within);
            if (vread(h->d, h->voff + (uint64_t)f->e[i].start * h->blocksize + within, buf, take)) return -1;
            buf += take;
            off += take;
            n -= take;
        }
        pos += elen;
    }
    return n ? -1 : 0;
}

static char *hfs_name(const uint8_t *p, int len)
{
    wchar_t *w = xmalloc(((size_t)len + 1) * sizeof(wchar_t));
    for (int i = 0; i < len; i++) {
        wchar_t c = (wchar_t)be16(p + i * 2);
        if (c == ':' || c == '/' || c == '\\' || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*' || c < 32)
            c = '_';
        w[i] = c;
    }
    w[len] = 0;
    char *s = w_to_utf8(w);
    free(w);
    return s;
}

typedef struct {
    hfs *h;
    centry *e;
    size_t n, cap;
} catalog;

static int read_catalog(hfs *h, fork_t *cf, catalog *cat)
{
    uint8_t hdr[512];
    if (fork_read(h, cf, 0, hdr, 512)) return -1;
    uint32_t nodesize = be16(hdr + 14 + 18);
    uint32_t leaf = be32(hdr + 14 + 10);
    if (nodesize < 512) return -1;
    uint8_t *node = xmalloc(nodesize);
    uint32_t guard = 0;
    while (leaf && guard++ < 10000000) {
        if (fork_read(h, cf, (uint64_t)leaf * nodesize, node, nodesize)) break;
        if ((int8_t)node[8] != -1) break;
        uint16_t nrec = be16(node + 10);
        for (uint16_t r = 0; r < nrec; r++) {
            uint16_t off = be16(node + nodesize - 2 * (r + 1));
            if (off + 8 > nodesize) continue;
            const uint8_t *k = node + off;
            uint16_t klen = be16(k);
            const uint8_t *rec = k + 2 + klen;
            if (rec + 2 > node + nodesize) continue;
            uint32_t parent = be32(k + 2);
            uint16_t nlen = be16(k + 6);
            if (8 + (size_t)nlen * 2 > (size_t)klen + 2) continue;
            int16_t type = (int16_t)be16(rec);
            if (type != 1 && type != 2) continue;
            if (cat->n == cat->cap) { cat->cap = cat->cap ? cat->cap * 2 : 4096; cat->e = xrealloc(cat->e, cat->cap * sizeof(centry)); }
            centry *e = &cat->e[cat->n];
            memset(e, 0, sizeof *e);
            e->type = type;
            e->parent = parent;
            e->name = hfs_name(k + 8, nlen);
            e->id = be32(rec + 8);
            if (type == 2) {
                if (rec + 248 > node + nodesize) { free(e->name); continue; }
                e->inode = be32(rec + 44);
                e->ftype = be32(rec + 48);
                e->fcreator = be32(rec + 52);
                memcpy(e->fork, rec + 88, 80);
                if (rec[41] & 0x20) e->ftype = 0xFFFFFFFF;   /* UF_COMPRESSED */
            }
            cat->n++;
        }
        leaf = be32(node);
    }
    free(node);
    return 0;
}

static int id_cmp(const void *a, const void *b)
{
    uint32_t x = ((const centry *)a)->id, y = ((const centry *)b)->id;
    return x < y ? -1 : x > y;
}

static centry *find_id(catalog *c, uint32_t id, size_t nfolders)
{
    size_t lo = 0, hi = nfolders;
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (c->e[m].id < id) lo = m + 1;
        else hi = m;
    }
    return lo < nfolders && c->e[lo].id == id ? &c->e[lo] : NULL;
}

/* Path of a folder id, relative to the volume root. */
static char *folder_path(catalog *c, size_t nfolders, uint32_t id, int depth)
{
    if (id == 2 || id == 1 || depth > 512) return xstrdup("");
    centry *e = find_id(c, id, nfolders);
    if (!e) return NULL;
    char *p = folder_path(c, nfolders, e->parent, depth + 1);
    if (!p) return NULL;
    char *r = *p ? xasprintf("%s\\%s", p, e->name) : xstrdup(e->name);
    free(p);
    return r;
}

static int extract_file(hfs *h, centry *e, const char *dst, uint8_t *buf, size_t bufsz)
{
    fork_t f;
    fork_parse(&f, e->fork);
    if (f.n == 8) fork_overflow(h, &f, e->id);
    wchar_t *w = utf8_to_w(dst);
    HANDLE out = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (out == INVALID_HANDLE_VALUE) { free(f.e); return -1; }
    int rc = 0;
    for (uint64_t off = 0; off < f.size;) {
        size_t n = (size_t)min((uint64_t)bufsz, f.size - off);
        if (fork_read(h, &f, off, buf, n)) { rc = -1; break; }
        DWORD put;
        WriteFile(out, buf, (DWORD)n, &put, NULL);
        off += n;
    }
    CloseHandle(out);
    free(f.e);
    return rc;
}

typedef struct { char *link, *target; } symlink_t;

int dmg_extract(const char *dmg, const char *outdir, char *err, size_t errlen)
{
    vdisk d = {0};
    d.err = err;
    d.errlen = errlen;
    err[0] = 0;
    int rc = -1;
    catalog cat = {0};
    hfs h = {0};
    if (udif_open(&d, dmg)) goto done;

    /* find an HFS+ volume header: try every chunk-run boundary that could start a partition */
    uint8_t vh[512];
    int found = 0;
    uint64_t starts[64];
    int nstarts = 0;
    starts[nstarts++] = 0;
    for (size_t i = 0; i < d.nchunks && nstarts < 64; i++)
        if (i == 0 || d.chunks[i].sector != d.chunks[i - 1].sector + d.chunks[i - 1].nsectors || (d.chunks[i].sector % 8 == 0 && i < 8))
            starts[nstarts++] = d.chunks[i].sector * 512;
    /* GPT / APM partitions usually start at sector 40 or 64 */
    starts[nstarts++] = 40 * 512;
    starts[nstarts++] = 64 * 512;
    for (int i = 0; i < nstarts && !found; i++) {
        if (vread(&d, starts[i] + 1024, vh, 512)) {
            if (d.unsupported) goto done;
            continue;
        }
        if (vh[0] == 'H' && (vh[1] == '+' || vh[1] == 'X')) { h.voff = starts[i]; found = 1; }
    }
    if (!found) {
        uint8_t sb[64];
        for (int i = 0; i < nstarts; i++)
            if (!vread(&d, starts[i], sb, 64) && !memcmp(sb + 32, "NXSB", 4)) {
                snprintf(err, errlen, "disk image uses APFS");
                d.unsupported = 1;
                goto done;
            }
        if (!err[0]) snprintf(err, errlen, "no HFS+ volume found in disk image");
        d.unsupported = 1;
        goto done;
    }

    h.d = &d;
    h.blocksize = be32(vh + 40);
    if (!h.blocksize) goto done;
    fork_t ef, cf;
    fork_parse(&ef, vh + 192);
    fork_parse(&cf, vh + 272);
    if (ef.size && ef.size < (256u << 20)) {
        h.extents_len = (size_t)ef.size;
        h.extents_file = xmalloc(h.extents_len);
        if (fork_read(&h, &ef, 0, h.extents_file, h.extents_len)) { free(h.extents_file); h.extents_file = NULL; }
    }
    if (cf.n == 8) fork_overflow(&h, &cf, 4);
    if (read_catalog(&h, &cf, &cat)) {
        if (!err[0]) snprintf(err, errlen, "could not read the HFS+ catalog");
        goto done;
    }

    /* sort folders first (by id) so parent lookups are a binary search */
    size_t nf = 0;
    for (size_t i = 0; i < cat.n; i++)
        if (cat.e[i].type == 1) { centry t = cat.e[nf]; cat.e[nf] = cat.e[i]; cat.e[i] = t; nf++; }
    qsort(cat.e, nf, sizeof(centry), id_cmp);

    mkdirs(outdir);
    uint32_t private_dir = 0;
    for (size_t i = 0; i < nf; i++) {
        centry *e = &cat.e[i];
        if (e->parent == 2 && !strncmp(e->name, "____HFS+ Private Data", 21)) private_dir = e->id;
        char *p = folder_path(&cat, nf, e->id, 0);
        if (!p) continue;
        if (*p && strncmp(p, "____HFS+ Private Data", 21) && strncmp(p, ".HFS+ Private Directory Data", 28) &&
            strncmp(p, ".Trashes", 8) && strncmp(p, ".fseventsd", 10)) {
            char *full = path_join(outdir, p);
            mkdirs(full);
            free(full);
        }
        free(p);
    }

    size_t bufsz = 4u << 20;
    uint8_t *buf = xmalloc(bufsz);
    symlink_t *links = NULL;
    size_t nlinks = 0;
    int failures = 0, compressed = 0;
    for (size_t i = nf; i < cat.n; i++) {
        centry *e = &cat.e[i];
        if (e->parent == private_dir) continue;
        char *dirp = folder_path(&cat, nf, e->parent, 0);
        if (!dirp) continue;
        if (!strncmp(dirp, ".HFS+ Private", 13) || !strncmp(dirp, ".Trashes", 8) || !strncmp(dirp, ".fseventsd", 10)) { free(dirp); continue; }
        char *rel = *dirp ? xasprintf("%s\\%s", dirp, e->name) : xstrdup(e->name);
        char *dst = path_join(outdir, rel);
        centry *src = e;
        if (e->ftype == 0x686c6e6b && e->fcreator == 0x6866732b) {   /* 'hlnk' 'hfs+' hard link */
            char want[32];
            snprintf(want, sizeof want, "iNode%u", e->inode);
            for (size_t k = nf; k < cat.n; k++)
                if (cat.e[k].parent == private_dir && !strcmp(cat.e[k].name, want)) { src = &cat.e[k]; break; }
        }
        if (src->ftype == 0xFFFFFFFF) compressed++;
        if (e->ftype == 0x736c6e6b && e->fcreator == 0x72686170) {   /* 'slnk' 'rhap' symlink */
            fork_t f;
            fork_parse(&f, e->fork);
            char target[1024] = {0};
            if (f.size < sizeof target && !fork_read(&h, &f, 0, (uint8_t *)target, (size_t)f.size)) {
                links = xrealloc(links, (nlinks + 1) * sizeof *links);
                links[nlinks].link = dst;
                links[nlinks].target = xstrdup(target);
                nlinks++;
                dst = NULL;
            }
            free(f.e);
        } else if (extract_file(&h, src, dst, buf, bufsz)) {
            failures++;
        }
        free(dst);
        free(rel);
        free(dirp);
    }
    free(buf);

    /* Recreate symlinks: real symlinks when Windows allows it, copies otherwise. */
    for (int pass = 0; pass < 3; pass++) {
        for (size_t i = 0; i < nlinks; i++) {
            if (!links[i].target) continue;
            char *dir = path_dirname(links[i].link);
            char *tgt = links[i].target[0] == '/' ? path_join(outdir, links[i].target) : path_join(dir, links[i].target);
            free(dir);
            if (path_exists(tgt)) {
                wchar_t *wl = utf8_to_w(links[i].link), *wt = utf8_to_w(tgt);
                DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE | (is_dir(tgt) ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0);
                if (!CreateSymbolicLinkW(wl, wt, flags)) copy_tree(tgt, links[i].link, 1);
                free(wl);
                free(wt);
                free(links[i].target);
                links[i].target = NULL;
            }
            free(tgt);
        }
    }
    for (size_t i = 0; i < nlinks; i++) { free(links[i].link); free(links[i].target); }
    free(links);

    if (compressed) {
        snprintf(err, errlen, "%d files use HFS+ compression", compressed);
        d.unsupported = 1;
        goto done;
    }
    if (failures) vlog("%d files could not be extracted", failures);
    rc = 0;

done:
    for (size_t i = 0; i < cat.n; i++) free(cat.e[i].name);
    free(cat.e);
    free(h.extents_file);
    if (d.h && d.h != INVALID_HANDLE_VALUE) CloseHandle(d.h);
    free(d.chunks);
    free(d.cache);
    free(d.cbuf);
    if (rc && d.unsupported) return -2;   /* caller may retry with 7-Zip */
    return rc;
}
