/*
 * Cider's libSystem: the macOS C library, implemented on Windows.
 *
 * Every function here is called directly by macOS code, so it uses the
 * System V calling convention (SV). Data layouts that differ between macOS
 * and Windows (FILE, struct stat, struct tm, dirent, termios, long being
 * 64-bit, ...) are translated at this boundary.
 */
#define _CRT_RAND_S
#include "cider.h"
#include <io.h>
#include <locale.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <direct.h>
#include <process.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#include <float.h>

/* ------------------------------------------------------------------ */
/* Process state                                                      */

char **cider_environ;
static int g_argc;
static char **g_argv;
static const char *g_progname = "app";
static char *g_exe_path;
static uint64_t g_stack_chk_guard = 0x2b992ddfa23249d6ULL;
static int g_mb_cur_max = 4;

typedef struct { void *base; const char *path; } reg_image;
static reg_image g_reg[256];
static int g_nreg;

void libsystem_register_image(void *base, const char *path)
{
    if (g_nreg < 256) { g_reg[g_nreg].base = base; g_reg[g_nreg].path = path; g_nreg++; }
}

void *macho_dlsym(const char *name);

/* Map well-known macOS paths onto Windows equivalents. */
static const char *mp(const char *p, char *buf, size_t cap)
{
    if (!p) return NULL;
    if (!strcmp(p, "/dev/null")) return "NUL";
    if (!strcmp(p, "/dev/tty")) return "CON";
    static const char *tmps[] = { "/tmp", "/private/tmp", "/var/tmp" };
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(tmps[i]);
        if (!strncmp(p, tmps[i], n) && (p[n] == 0 || p[n] == '/')) {
            char t[MAX_PATH];
            DWORD l = GetTempPathA(MAX_PATH, t);
            if (l && (t[l - 1] == '\\' || t[l - 1] == '/')) t[--l] = 0;
            snprintf(buf, cap, "%s%s", t, p + n);
            return buf;
        }
    }
    return p;
}
#define MP(p) mp((p), _mpbuf, sizeof _mpbuf)
#define MPDECL char _mpbuf[1024]

static void slashify(char *s)
{
    for (; s && *s; s++) if (*s == '\\') *s = '/';
}

/* ------------------------------------------------------------------ */
/* System V va_list access                                            */

typedef struct { uint32_t gp, fp; uint8_t *ov; uint8_t *rsa; } sysv_va;

static uint64_t va_g(sysv_va *v)
{
    uint64_t x;
    if (v->gp < 48) { memcpy(&x, v->rsa + v->gp, 8); v->gp += 8; }
    else { memcpy(&x, v->ov, 8); v->ov += 8; }
    return x;
}

static double va_f(sysv_va *v)
{
    double x;
    if (v->fp < 176) { memcpy(&x, v->rsa + v->fp, 8); v->fp += 16; }
    else { memcpy(&x, v->ov, 8); v->ov += 8; }
    return x;
}

static long double va_ld(sysv_va *v)
{
    uintptr_t p = ((uintptr_t)v->ov + 15) & ~(uintptr_t)15;
    long double x = 0;
    memcpy(&x, (void *)p, 10);
    v->ov = (uint8_t *)(p + 16);
    return x;
}

/* Rewrites a macOS printf/scanf format for the Windows CRT and collects the
 * arguments into an MS x64 style va_list (an array of 8-byte slots). */
typedef struct {
    char *f;
    size_t fl, fc;
    uint64_t *s;
    size_t sl, sc;
    char *tmp[16];
    int ntmp;
} conv;

static void cvc(conv *c, char ch)
{
    if (c->fl + 2 > c->fc) { c->fc = c->fc ? c->fc * 2 : 256; c->f = xrealloc(c->f, c->fc); }
    c->f[c->fl++] = ch;
    c->f[c->fl] = 0;
}
static void cvs(conv *c, const char *s) { while (*s) cvc(c, *s++); }
static void cvslot(conv *c, uint64_t v)
{
    if (c->sl == c->sc) { c->sc = c->sc ? c->sc * 2 : 16; c->s = xrealloc(c->s, c->sc * 8); }
    c->s[c->sl++] = v;
}
static void cvfree(conv *c)
{
    free(c->f);
    free(c->s);
    for (int i = 0; i < c->ntmp; i++) free(c->tmp[i]);
}

static const char *utf32_str(conv *c, const uint32_t *w)
{
    size_t n = 0;
    while (w[n]) n++;
    char *o = xmalloc(n * 4 + 1), *p = o;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = w[i];
        if (cp < 0x80) *p++ = (char)cp;
        else if (cp < 0x800) { *p++ = (char)(0xC0 | cp >> 6); *p++ = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { *p++ = (char)(0xE0 | cp >> 12); *p++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *p++ = (char)(0x80 | (cp & 0x3F)); }
        else { *p++ = (char)(0xF0 | cp >> 18); *p++ = (char)(0x80 | ((cp >> 12) & 0x3F)); *p++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *p++ = (char)(0x80 | (cp & 0x3F)); }
    }
    *p = 0;
    if (c->ntmp < 16) c->tmp[c->ntmp++] = o;
    return o;
}

static int parse_len(const char **pf)
{
    const char *f = *pf;
    int len = 0;
    for (;;) {
        if (*f == 'h') { len = len == 2 ? 1 : 2; f++; }
        else if (*f == 'l') { len = len == 3 ? 4 : 3; f++; }
        else if (*f == 'q' || *f == 'j' || *f == 'z' || *f == 't') { len = 4; f++; }
        else if (*f == 'L') { len = 5; f++; }
        else break;
    }
    *pf = f;
    return len;
}

static void conv_printf(conv *c, const char *f, sysv_va *va)
{
    cvs(c, "");
    if (!c->f) cvc(c, 0), c->fl = 0;
    while (f && *f) {
        if (*f != '%') { cvc(c, *f++); continue; }
        f++;
        if (*f == '%') { cvs(c, "%%"); f++; continue; }
        char spec[64];
        int n = 0;
        spec[n++] = '%';
        while (*f && strchr("-+ #0'", *f)) { if (*f != '\'' && n < 40) spec[n++] = *f; f++; }
        if (*f == '*') { spec[n++] = '*'; cvslot(c, (uint64_t)(int64_t)(int32_t)va_g(va)); f++; }
        else while (*f >= '0' && *f <= '9') { if (n < 40) spec[n++] = *f; f++; }
        if (*f == '.') {
            spec[n++] = *f++;
            if (*f == '*') { spec[n++] = '*'; cvslot(c, (uint64_t)(int64_t)(int32_t)va_g(va)); f++; }
            else while (*f >= '0' && *f <= '9') { if (n < 50) spec[n++] = *f; f++; }
        }
        spec[n] = 0;
        int len = parse_len(&f);
        char cv = *f;
        if (!cv) break;
        f++;
        switch (cv) {
        case 'D': len = 3; cv = 'd'; /* fallthrough */
        case 'd': case 'i': {
            uint64_t v = va_g(va);
            int64_t x = len == 1 ? (int8_t)v : len == 2 ? (int16_t)v : (len == 3 || len == 4) ? (int64_t)v : (int32_t)v;
            cvs(c, spec); cvs(c, "ll"); cvc(c, cv); cvslot(c, (uint64_t)x);
            break;
        }
        case 'U': case 'O': len = 3; cv = (char)tolower(cv); /* fallthrough */
        case 'u': case 'o': case 'x': case 'X': {
            uint64_t v = va_g(va);
            uint64_t x = len == 1 ? (uint8_t)v : len == 2 ? (uint16_t)v : (len == 3 || len == 4) ? v : (uint32_t)v;
            cvs(c, spec); cvs(c, "ll"); cvc(c, cv); cvslot(c, x);
            break;
        }
        case 'c': case 'C':
            cvs(c, spec); cvc(c, 'c'); cvslot(c, (uint8_t)va_g(va));
            break;
        case 's': case 'S': {
            const char *p = (const char *)va_g(va);
            if ((len == 3 || cv == 'S') && p) p = utf32_str(c, (const uint32_t *)p);
            cvs(c, spec); cvc(c, 's'); cvslot(c, (uint64_t)p);
            break;
        }
        case 'p':
            cvs(c, "0x%llx"); cvslot(c, va_g(va));
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double d = len == 5 ? (double)va_ld(va) : va_f(va);
            uint64_t bits;
            memcpy(&bits, &d, 8);
            cvs(c, spec); cvc(c, cv); cvslot(c, bits);
            break;
        }
        case 'n':
            va_g(va);
            break;
        default:
            cvs(c, spec); cvc(c, cv);
        }
    }
}

static void conv_scanf(conv *c, const char *f, sysv_va *va)
{
    cvs(c, "");
    if (!c->f) cvc(c, 0), c->fl = 0;
    while (f && *f) {
        if (*f != '%') { cvc(c, *f++); continue; }
        cvc(c, *f++);
        if (*f == '%') { cvc(c, *f++); continue; }
        int suppress = 0;
        if (*f == '*') { suppress = 1; cvc(c, *f++); }
        while (*f >= '0' && *f <= '9') cvc(c, *f++);
        int len = parse_len(&f);
        char cv = *f;
        if (!cv) break;
        f++;
        if (cv == '[') {
            cvc(c, '[');
            if (*f == '^') cvc(c, *f++);
            if (*f == ']') cvc(c, *f++);
            while (*f && *f != ']') cvc(c, *f++);
            if (*f) cvc(c, *f++);
        } else {
            if (strchr("diouxXn", cv)) cvs(c, len == 1 ? "hh" : len == 2 ? "h" : len >= 3 ? "ll" : "");
            else if (strchr("eEfgGaA", cv) && len >= 3) cvc(c, 'l');
            cvc(c, cv);
        }
        if (!suppress) cvslot(c, va_g(va));
    }
}

/* ------------------------------------------------------------------ */
/* FILE: macOS code may peek at FILE fields (getc/putc macros), so it  */
/* gets a macOS-shaped struct that wraps the real CRT stream.          */

typedef struct macFILE {
    unsigned char *_p;
    int _r, _w;
    short _flags, _file;
    struct { unsigned char *_base; int _size; } _bf;
    int _lbfsize;
    void *_cookie;
    uint8_t pad[152 - 56];
    FILE *real;
    int is_std, is_popen;
} macFILE;

static macFILE mac_stdin_s, mac_stdout_s, mac_stderr_s;
static macFILE *mac_stdinp = &mac_stdin_s, *mac_stdoutp = &mac_stdout_s, *mac_stderrp = &mac_stderr_s;

static FILE *R(macFILE *f) { return f ? f->real : NULL; }

static macFILE *wrapf(FILE *f)
{
    if (!f) return NULL;
    macFILE *m = xcalloc(1, sizeof *m);
    m->real = f;
    m->_file = (short)_fileno(f);
    return m;
}

static int do_vfprintf(FILE *fp, const char *fmt, sysv_va *va)
{
    conv c = {0};
    conv_printf(&c, fmt, va);
    int r = vfprintf(fp, c.f, (va_list)c.s);
    cvfree(&c);
    return r;
}

static int do_vsnprintf(char *buf, size_t n, const char *fmt, sysv_va *va)
{
    conv c = {0};
    conv_printf(&c, fmt, va);
    int r = vsnprintf(buf, n, c.f, (va_list)c.s);
    cvfree(&c);
    return r;
}

static char *do_vasprintf(const char *fmt, sysv_va *va, int *len)
{
    conv c = {0};
    conv_printf(&c, fmt, va);
    int n = vsnprintf(NULL, 0, c.f, (va_list)c.s);
    char *out = malloc((size_t)(n < 0 ? 0 : n) + 1);
    if (out) vsnprintf(out, (size_t)n + 1, c.f, (va_list)c.s);
    cvfree(&c);
    if (len) *len = n;
    return out;
}

/* Called from the variadic thunks in thunks.S (MS ABI). */
int vimpl_printf(uint64_t *g, sysv_va *va) { return do_vfprintf(R(mac_stdoutp), (const char *)g[0], va); }
int vimpl_fprintf(uint64_t *g, sysv_va *va) { return do_vfprintf(R((macFILE *)g[0]), (const char *)g[1], va); }
int vimpl_sprintf(uint64_t *g, sysv_va *va) { return do_vsnprintf((char *)g[0], (size_t)INT_MAX, (const char *)g[1], va); }
int vimpl_snprintf(uint64_t *g, sysv_va *va) { return do_vsnprintf((char *)g[0], (size_t)g[1], (const char *)g[2], va); }
int vimpl_sprintf_chk(uint64_t *g, sysv_va *va) { return do_vsnprintf((char *)g[0], (size_t)g[2], (const char *)g[3], va); }
int vimpl_snprintf_chk(uint64_t *g, sysv_va *va) { return do_vsnprintf((char *)g[0], (size_t)g[1], (const char *)g[4], va); }
int vimpl_asprintf(uint64_t *g, sysv_va *va)
{
    int n;
    *(char **)g[0] = do_vasprintf((const char *)g[1], va, &n);
    return n;
}
int vimpl_dprintf(uint64_t *g, sysv_va *va)
{
    int n;
    char *s = do_vasprintf((const char *)g[1], va, &n);
    if (s && n > 0) n = _write((int)g[0], s, (unsigned)n);
    free(s);
    return n;
}
static int do_vsscanf(const char *str, const char *fmt, sysv_va *va)
{
    conv c = {0};
    conv_scanf(&c, fmt, va);
    int r = vsscanf(str, c.f, (va_list)c.s);
    cvfree(&c);
    return r;
}
static int do_vfscanf(FILE *f, const char *fmt, sysv_va *va)
{
    conv c = {0};
    conv_scanf(&c, fmt, va);
    int r = vfscanf(f, c.f, (va_list)c.s);
    cvfree(&c);
    return r;
}
int vimpl_sscanf(uint64_t *g, sysv_va *va) { return do_vsscanf((const char *)g[0], (const char *)g[1], va); }
int vimpl_fscanf(uint64_t *g, sysv_va *va) { return do_vfscanf(R((macFILE *)g[0]), (const char *)g[1], va); }
int vimpl_scanf(uint64_t *g, sysv_va *va) { return do_vfscanf(R(mac_stdinp), (const char *)g[0], va); }
int vimpl_syslog(uint64_t *g, sysv_va *va)
{
    do_vfprintf(stderr, (const char *)g[1], va);
    fputc('\n', stderr);
    return 0;
}

static void verr(int with_errno, const char *fmt, sysv_va *va)
{
    int e = errno;
    fflush(R(mac_stdoutp));
    fprintf(stderr, "%s: ", g_progname);
    if (fmt) do_vfprintf(stderr, fmt, va);
    if (with_errno) fprintf(stderr, "%s%s", fmt ? ": " : "", strerror(e));
    fputc('\n', stderr);
}
int vimpl_err(uint64_t *g, sysv_va *va) { verr(1, (const char *)g[1], va); shim_exit((int)g[0]); }
int vimpl_errx(uint64_t *g, sysv_va *va) { verr(0, (const char *)g[1], va); shim_exit((int)g[0]); }
int vimpl_warn(uint64_t *g, sysv_va *va) { verr(1, (const char *)g[0], va); return 0; }
int vimpl_warnx(uint64_t *g, sysv_va *va) { verr(0, (const char *)g[0], va); return 0; }

/* macOS open() flags -> CRT */
static int oflags(int m)
{
    int w = m & 3;
    if (m & 0x8) w |= _O_APPEND;
    if (m & 0x200) w |= _O_CREAT;
    if (m & 0x400) w |= _O_TRUNC;
    if (m & 0x800) w |= _O_EXCL;
    return w | _O_BINARY;
}

static int do_open(const char *path, int flags, int mode)
{
    MPDECL;
    int pm = (mode & 0222) || !(flags & 0x200) ? (_S_IREAD | _S_IWRITE) : _S_IREAD;
    return _open(MP(path), oflags(flags), pm);
}
int vimpl_open(uint64_t *g, sysv_va *va)
{
    int flags = (int)g[1];
    int mode = (flags & 0x200) ? (int)va_g(va) : 0;
    return do_open((const char *)g[0], flags, mode);
}
int vimpl_openat(uint64_t *g, sysv_va *va)
{
    int flags = (int)g[2];
    int mode = (flags & 0x200) ? (int)va_g(va) : 0;
    return do_open((const char *)g[1], flags, mode);
}
int vimpl_fcntl(uint64_t *g, sysv_va *va)
{
    int fd = (int)g[0], cmd = (int)g[1];
    switch (cmd) {
    case 0: case 67: return _dup(fd);   /* F_DUPFD, F_DUPFD_CLOEXEC */
    case 3: return 2;                   /* F_GETFL -> O_RDWR */
    default: (void)va; return 0;
    }
}
int vimpl_ioctl(uint64_t *g, sysv_va *va)
{
    int fd = (int)g[0];
    uint64_t req = (uint32_t)g[1];
    if (req == 0x40087468) {            /* TIOCGWINSZ */
        uint16_t *ws = (uint16_t *)va_g(va);
        CONSOLE_SCREEN_BUFFER_INFO ci;
        HANDLE h = (HANDLE)_get_osfhandle(fd);
        if (!GetConsoleScreenBufferInfo(h, &ci) && !GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &ci)) {
            errno = ENOTTY;
            return -1;
        }
        ws[0] = (uint16_t)(ci.srWindow.Bottom - ci.srWindow.Top + 1);
        ws[1] = (uint16_t)(ci.srWindow.Right - ci.srWindow.Left + 1);
        ws[2] = ws[3] = 0;
        return 0;
    }
    if (req == 0x4004667f) { *(int *)va_g(va) = 0; return 0; }  /* FIONREAD */
    errno = ENOTTY;
    return -1;
}

/* ------------------------------------------------------------------ */
/* exit / atexit                                                      */

typedef SV void (*vfn)(void);
typedef SV void (*vfn1)(void *);
static struct { void *fn, *arg; int cxa; } g_atexit[256];
static int g_natexit;
static CRITICAL_SECTION g_exit_lock;

SV static int sh_atexit(void *fn)
{
    if (g_natexit >= 256) return -1;
    g_atexit[g_natexit].fn = fn;
    g_atexit[g_natexit].arg = NULL;
    g_atexit[g_natexit++].cxa = 0;
    return 0;
}
SV static int sh_cxa_atexit(void *fn, void *arg, void *dso)
{
    (void)dso;
    if (g_natexit >= 256) return -1;
    g_atexit[g_natexit].fn = fn;
    g_atexit[g_natexit].arg = arg;
    g_atexit[g_natexit++].cxa = 1;
    return 0;
}

SV void shim_exit(int code)
{
    EnterCriticalSection(&g_exit_lock);
    while (g_natexit > 0) {
        g_natexit--;
        if (g_atexit[g_natexit].cxa) ((vfn1)g_atexit[g_natexit].fn)(g_atexit[g_natexit].arg);
        else ((vfn)g_atexit[g_natexit].fn)();
    }
    fflush(NULL);
    if (g_gui) ui_console_pause();
    ExitProcess((UINT)code);
}
SV static void sh__exit(int code) { fflush(NULL); ExitProcess((UINT)code); }
SV static void sh_abort(void)
{
    fflush(NULL);
    fputs("Abort trap: 6\n", stderr);
    if (g_gui) ui_console_pause();
    ExitProcess(134);
}
SV static void sh_stack_chk_fail(void)
{
    fputs("[cider] stack buffer overflow detected; terminating\n", stderr);
    sh_abort();
}
SV static void sh_assert_rtn(const char *func, const char *file, int line, const char *expr)
{
    fflush(NULL);
    fprintf(stderr, "Assertion failed: (%s), function %s, file %s, line %d.\n", expr, func, file, line);
    sh_abort();
}

/* ------------------------------------------------------------------ */
/* Memory                                                             */

static SRWLOCK g_align_lock = SRWLOCK_INIT;
static void **g_aligned;
static size_t g_naligned_cap, g_naligned;

static void aligned_add(void *p)
{
    AcquireSRWLockExclusive(&g_align_lock);
    if ((g_naligned + 1) * 2 > g_naligned_cap) {
        size_t nc = g_naligned_cap ? g_naligned_cap * 2 : 64;
        void **n = calloc(nc, sizeof(void *));
        for (size_t i = 0; i < g_naligned_cap; i++) {
            void *q = g_aligned[i];
            if (!q || q == (void *)1) continue;
            size_t h = ((uintptr_t)q >> 4) & (nc - 1);
            while (n[h]) h = (h + 1) & (nc - 1);
            n[h] = q;
        }
        free(g_aligned);
        g_aligned = n;
        g_naligned_cap = nc;
    }
    size_t h = ((uintptr_t)p >> 4) & (g_naligned_cap - 1);
    while (g_aligned[h] && g_aligned[h] != (void *)1) h = (h + 1) & (g_naligned_cap - 1);
    g_aligned[h] = p;
    g_naligned++;
    ReleaseSRWLockExclusive(&g_align_lock);
}

static int aligned_take(void *p)
{
    if (!g_naligned) return 0;
    int found = 0;
    AcquireSRWLockExclusive(&g_align_lock);
    size_t h = ((uintptr_t)p >> 4) & (g_naligned_cap - 1);
    while (g_aligned[h]) {
        if (g_aligned[h] == p) { g_aligned[h] = (void *)1; g_naligned--; found = 1; break; }
        h = (h + 1) & (g_naligned_cap - 1);
    }
    ReleaseSRWLockExclusive(&g_align_lock);
    return found;
}

SV static void *sh_malloc(size_t n) { return malloc(n ? n : 1); }
SV static void *sh_calloc(size_t a, size_t b) { return calloc(a ? a : 1, b ? b : 1); }
SV static void sh_free(void *p)
{
    if (!p) return;
    if (aligned_take(p)) _aligned_free(p);
    else free(p);
}
SV static void *sh_realloc(void *p, size_t n)
{
    if (p && g_naligned && aligned_take(p)) {
        size_t old = _aligned_msize(p, 64, 0);
        void *q = malloc(n ? n : 1);
        if (q) memcpy(q, p, old < n ? old : n);
        _aligned_free(p);
        return q;
    }
    return realloc(p, n ? n : 1);
}
SV static void *sh_reallocf(void *p, size_t n)
{
    void *q = sh_realloc(p, n);
    if (!q) sh_free(p);
    return q;
}
SV static int sh_posix_memalign(void **out, size_t align, size_t n)
{
    if (align <= 16) { *out = malloc(n ? n : 1); return *out ? 0 : ENOMEM; }
    if (align < 64) align = 64;
    void *p = _aligned_malloc(n ? n : 1, align);
    if (!p) return ENOMEM;
    aligned_add(p);
    *out = p;
    return 0;
}
SV static void *sh_aligned_alloc(size_t align, size_t n)
{
    void *p = NULL;
    return sh_posix_memalign(&p, align, n) ? NULL : p;
}
SV static void *sh_valloc(size_t n) { return sh_aligned_alloc(4096, n); }
SV static size_t sh_malloc_size(void *p)
{
    if (!p) return 0;
    if (g_naligned) {
        AcquireSRWLockShared(&g_align_lock);
        size_t h = ((uintptr_t)p >> 4) & (g_naligned_cap - 1);
        int is = 0;
        while (g_aligned[h]) { if (g_aligned[h] == p) { is = 1; break; } h = (h + 1) & (g_naligned_cap - 1); }
        ReleaseSRWLockShared(&g_align_lock);
        if (is) return _aligned_msize(p, 64, 0);
    }
    return _msize(p);
}
SV static size_t sh_malloc_good_size(size_t n) { return (n + 15) & ~(size_t)15; }

/* C++ runtime basics */
SV static void *sh_new(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fputs("[cider] operator new: out of memory\n", stderr); sh_abort(); }
    return p;
}
SV static void *sh_new_nothrow(size_t n, void *tag) { (void)tag; return malloc(n ? n : 1); }
SV static void sh_delete(void *p) { free(p); }
SV static void sh_delete_sized(void *p, size_t n) { (void)n; free(p); }
SV static int sh_cxa_guard_acquire(uint64_t *g) { return *(uint8_t *)g == 0; }
SV static void sh_cxa_guard_release(uint64_t *g) { *(uint8_t *)g = 1; }
SV static void sh_cxa_guard_abort(uint64_t *g) { (void)g; }
SV static void sh_cxa_pure_virtual(void)
{
    fputs("[cider] pure virtual function called\n", stderr);
    sh_abort();
}
SV static void sh_cxx_throw(void)
{
    fputs("[cider] this program threw a C++ exception; C++ exceptions are not supported yet\n", stderr);
    sh_abort();
}

/* ------------------------------------------------------------------ */
/* Strings                                                            */

#define W1(r, n, t1) SV static r sh_##n(t1 a) { return n(a); }
#define W2(r, n, t1, t2) SV static r sh_##n(t1 a, t2 b) { return n(a, b); }
#define W3(r, n, t1, t2, t3) SV static r sh_##n(t1 a, t2 b, t3 c) { return n(a, b, c); }
#define W4(r, n, t1, t2, t3, t4) SV static r sh_##n(t1 a, t2 b, t3 c, t4 d) { return n(a, b, c, d); }

W1(size_t, strlen, const char *)
W2(size_t, strnlen, const char *, size_t)
W2(int, strcmp, const char *, const char *)
W3(int, strncmp, const char *, const char *, size_t)
W2(char *, strcpy, char *, const char *)
W3(char *, strncpy, char *, const char *, size_t)
W2(char *, strcat, char *, const char *)
W3(char *, strncat, char *, const char *, size_t)
W2(char *, strchr, const char *, int)
W2(char *, strrchr, const char *, int)
W2(char *, strstr, const char *, const char *)
W2(size_t, strspn, const char *, const char *)
W2(size_t, strcspn, const char *, const char *)
W2(char *, strpbrk, const char *, const char *)
W2(char *, strtok, char *, const char *)
W2(int, strcoll, const char *, const char *)
W3(size_t, strxfrm, char *, const char *, size_t)
W1(char *, strdup, const char *)
W3(void *, memcpy, void *, const void *, size_t)
W3(void *, memmove, void *, const void *, size_t)
W3(void *, memset, void *, int, size_t)
W3(int, memcmp, const void *, const void *, size_t)
W3(void *, memchr, const void *, int, size_t)
W1(char *, strerror, int)
W1(int, atoi, const char *)
W1(double, atof, const char *)
W2(double, strtod, const char *, char **)
W2(float, strtof, const char *, char **)
W3(long long, strtoll, const char *, char **, int)
W3(unsigned long long, strtoull, const char *, char **, int)
W1(int, abs, int)
W1(long long, llabs, long long)
W1(int, toupper, int)
W1(int, tolower, int)
W1(int, isalpha, int)
W1(int, isdigit, int)
W1(int, isalnum, int)
W1(int, isspace, int)
W1(int, isupper, int)
W1(int, islower, int)
W1(int, isxdigit, int)
W1(int, isprint, int)
W1(int, ispunct, int)
W1(int, iscntrl, int)
W1(int, isgraph, int)

SV static long long sh_atol(const char *s) { return _atoi64(s); }
SV static long long sh_labs(long long a) { return llabs(a); }
SV static int sh_isascii(int c) { return (unsigned)c < 128; }
SV static int sh_isblank(int c) { return c == ' ' || c == '\t'; }
SV static int sh_digittoint(int c) { return isdigit(c) ? c - '0' : isxdigit(c) ? (tolower(c) - 'a' + 10) : 0; }
SV static char *sh_strndup(const char *s, size_t n)
{
    size_t l = strnlen(s, n);
    char *d = malloc(l + 1);
    if (d) { memcpy(d, s, l); d[l] = 0; }
    return d;
}
SV static int sh_strcasecmp(const char *a, const char *b) { return _stricmp(a, b); }
SV static int sh_strncasecmp(const char *a, const char *b, size_t n) { return _strnicmp(a, b, n); }
SV static char *sh_strtok_r(char *s, const char *d, char **save) { return strtok_s(s, d, save); }
SV static size_t sh_strlcpy(char *d, const char *s, size_t n)
{
    size_t l = strlen(s);
    if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; }
    return l;
}
SV static size_t sh_strlcat(char *d, const char *s, size_t n)
{
    size_t dl = strnlen(d, n);
    if (dl == n) return n + strlen(s);
    return dl + sh_strlcpy(d + dl, s, n - dl);
}
SV static char *sh_strsep(char **sp, const char *delim)
{
    char *s = *sp;
    if (!s) return NULL;
    char *e = s + strcspn(s, delim);
    if (*e) { *e = 0; *sp = e + 1; } else *sp = NULL;
    return s;
}
SV static char *sh_strcasestr(const char *h, const char *n)
{
    size_t l = strlen(n);
    for (; *h; h++) if (!_strnicmp(h, n, l)) return (char *)h;
    return l ? NULL : (char *)h;
}
SV static void *sh_memmem(const void *h, size_t hl, const void *n, size_t nl)
{
    if (!nl) return (void *)h;
    for (size_t i = 0; i + nl <= hl; i++)
        if (!memcmp((const char *)h + i, n, nl)) return (char *)h + i;
    return NULL;
}
SV static char *sh_stpcpy(char *d, const char *s) { size_t l = strlen(s); memcpy(d, s, l + 1); return d + l; }
SV static char *sh_stpncpy(char *d, const char *s, size_t n)
{
    size_t l = strnlen(s, n);
    memcpy(d, s, l);
    memset(d + l, 0, n - l);
    return d + l;
}
SV static void sh_bzero(void *p, size_t n) { memset(p, 0, n); }
SV static void sh_bcopy(const void *s, void *d, size_t n) { memmove(d, s, n); }
SV static int sh_bcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n) != 0; }
SV static char *sh_index(const char *s, int c) { return strchr(s, c); }
SV static char *sh_rindex(const char *s, int c) { return strrchr(s, c); }
SV static int sh_ffs(int v) { unsigned long i; return _BitScanForward(&i, (unsigned long)v) ? (int)i + 1 : 0; }
SV static void sh_memset_pattern(void *b, const void *pat, size_t len, size_t plen)
{
    for (size_t i = 0; i < len; i++) ((uint8_t *)b)[i] = ((const uint8_t *)pat)[i % plen];
}
SV static void sh_memset_pattern4(void *b, const void *p, size_t n) { sh_memset_pattern(b, p, n, 4); }
SV static void sh_memset_pattern8(void *b, const void *p, size_t n) { sh_memset_pattern(b, p, n, 8); }
SV static void sh_memset_pattern16(void *b, const void *p, size_t n) { sh_memset_pattern(b, p, n, 16); }
SV static int sh_strerror_r(int e, char *buf, size_t n) { return strerror_s(buf, n, e); }
SV static long long sh_strtol(const char *s, char **e, int b) { return strtoll(s, e, b); }
SV static unsigned long long sh_strtoul(const char *s, char **e, int b) { return strtoull(s, e, b); }
SV static long double sh_strtold(const char *s, char **e) { return strtod(s, e); }
SV static char *sh_strsignal(int s) { static char b[32]; snprintf(b, sizeof b, "Signal %d", s); return b; }

/* checked variants emitted by _FORTIFY_SOURCE */
SV static void *sh_memcpy_chk(void *d, const void *s, size_t n, size_t dl) { (void)dl; return memcpy(d, s, n); }
SV static void *sh_memmove_chk(void *d, const void *s, size_t n, size_t dl) { (void)dl; return memmove(d, s, n); }
SV static void *sh_memset_chk(void *d, int c, size_t n, size_t dl) { (void)dl; return memset(d, c, n); }
SV static char *sh_strcpy_chk(char *d, const char *s, size_t dl) { (void)dl; return strcpy(d, s); }
SV static char *sh_strcat_chk(char *d, const char *s, size_t dl) { (void)dl; return strcat(d, s); }
SV static char *sh_strncpy_chk(char *d, const char *s, size_t n, size_t dl) { (void)dl; return strncpy(d, s, n); }
SV static char *sh_strncat_chk(char *d, const char *s, size_t n, size_t dl) { (void)dl; return strncat(d, s, n); }
SV static char *sh_stpcpy_chk(char *d, const char *s, size_t dl) { (void)dl; return sh_stpcpy(d, s); }
SV static size_t sh_strlcpy_chk(char *d, const char *s, size_t n, size_t dl) { (void)dl; return sh_strlcpy(d, s, n); }
SV static size_t sh_strlcat_chk(char *d, const char *s, size_t n, size_t dl) { (void)dl; return sh_strlcat(d, s, n); }
SV static int sh_vsnprintf(char *b, size_t n, const char *f, sysv_va *va) { return do_vsnprintf(b, n, f, va); }
SV static int sh_vsprintf(char *b, const char *f, sysv_va *va) { return do_vsnprintf(b, (size_t)INT_MAX, f, va); }
SV static int sh_vsprintf_chk(char *b, int fl, size_t sl, const char *f, sysv_va *va) { (void)fl; return do_vsnprintf(b, sl, f, va); }
SV static int sh_vsnprintf_chk(char *b, size_t n, int fl, size_t sl, const char *f, sysv_va *va) { (void)fl; (void)sl; return do_vsnprintf(b, n, f, va); }
SV static int sh_vprintf(const char *f, sysv_va *va) { return do_vfprintf(R(mac_stdoutp), f, va); }
SV static int sh_vfprintf(macFILE *fp, const char *f, sysv_va *va) { return do_vfprintf(R(fp), f, va); }
SV static int sh_vasprintf(char **out, const char *f, sysv_va *va) { int n; *out = do_vasprintf(f, va, &n); return n; }
SV static int sh_vsscanf(const char *s, const char *f, sysv_va *va) { return do_vsscanf(s, f, va); }
SV static int sh_vfscanf(macFILE *fp, const char *f, sysv_va *va) { return do_vfscanf(R(fp), f, va); }
SV static int sh_vdprintf(int fd, const char *f, sysv_va *va)
{
    int n;
    char *s = do_vasprintf(f, va, &n);
    if (s && n > 0) n = _write(fd, s, (unsigned)n);
    free(s);
    return n;
}

/* macOS ctype tables: <ctype.h> inlines lookups into _DefaultRuneLocale. */
#define CT_A 0x100
#define CT_C 0x200
#define CT_D 0x400
#define CT_G 0x800
#define CT_L 0x1000
#define CT_P 0x2000
#define CT_S 0x4000
#define CT_U 0x8000
#define CT_X 0x10000
#define CT_B 0x20000
#define CT_R 0x40000
#define CT_SW1 0x40000000

static struct {
    char magic[8], encoding[32];
    void *sgetrune, *sputrune;
    int32_t invalid_rune;
    uint32_t runetype[256];
    int32_t maplower[256], mapupper[256];
    uint8_t rest[256];
} g_rune;

static uint32_t rune_mask(int c)
{
    uint32_t m = 0;
    if (c >= 128) return 0;
    if (isalpha(c)) m |= CT_A;
    if (iscntrl(c)) m |= CT_C;
    if (isdigit(c)) m |= CT_D | (uint32_t)(c - '0');
    if (isgraph(c)) m |= CT_G;
    if (islower(c)) m |= CT_L;
    if (ispunct(c)) m |= CT_P;
    if (isspace(c)) m |= CT_S;
    if (isupper(c)) m |= CT_U;
    if (isxdigit(c)) { m |= CT_X; if (!isdigit(c)) m |= (uint32_t)(tolower(c) - 'a' + 10); }
    if (c == ' ' || c == '\t') m |= CT_B;
    if (isprint(c)) m |= CT_R | CT_SW1;
    return m;
}

static void init_rune(void)
{
    memcpy(g_rune.magic, "RuneMagA", 8);
    strcpy(g_rune.encoding, "UTF-8");
    g_rune.invalid_rune = 0xFFFD;
    for (int c = 0; c < 256; c++) {
        g_rune.runetype[c] = rune_mask(c);
        g_rune.maplower[c] = c < 128 ? tolower(c) : c;
        g_rune.mapupper[c] = c < 128 ? toupper(c) : c;
    }
}
SV static uint32_t sh_maskrune(int c, unsigned long long m) { return (uint32_t)(rune_mask(c) & m); }
SV static int sh_mac_toupper(int c) { return c >= 0 && c < 128 ? toupper(c) : c; }
SV static int sh_mac_tolower(int c) { return c >= 0 && c < 128 ? tolower(c) : c; }

/* ------------------------------------------------------------------ */
/* stdio                                                              */

static char *fmode(const char *m, char *buf)
{
    size_t n = 0;
    int hasb = 0;
    for (; *m && n < 14; m++) {
        if (*m == 'e') continue;   /* O_CLOEXEC */
        if (*m == 'b') hasb = 1;
        buf[n++] = *m;
    }
    if (!hasb) buf[n++] = 'b';
    buf[n] = 0;
    return buf;
}

SV static macFILE *sh_fopen(const char *p, const char *m)
{
    MPDECL;
    char mb[16];
    return wrapf(fopen(MP(p), fmode(m, mb)));
}
SV static macFILE *sh_fdopen(int fd, const char *m) { char mb[16]; return wrapf(_fdopen(fd, fmode(m, mb))); }
SV static macFILE *sh_freopen(const char *p, const char *m, macFILE *f)
{
    MPDECL;
    char mb[16];
    FILE *r = p ? freopen(MP(p), fmode(m, mb), R(f)) : R(f);
    if (!r) return NULL;
    f->real = r;
    return f;
}
SV static int sh_fclose(macFILE *f)
{
    if (!f) return -1;
    int r = f->is_popen ? _pclose(f->real) : fclose(f->real);
    if (!f->is_std) free(f);
    return r;
}
SV static size_t sh_fread(void *p, size_t s, size_t n, macFILE *f) { return fread(p, s, n, R(f)); }
SV static size_t sh_fwrite(const void *p, size_t s, size_t n, macFILE *f) { return fwrite(p, s, n, R(f)); }
SV static char *sh_fgets(char *b, int n, macFILE *f) { return fgets(b, n, R(f)); }
SV static int sh_fputs(const char *s, macFILE *f) { return fputs(s, R(f)); }
SV static int sh_fputc(int c, macFILE *f) { return fputc(c, R(f)); }
SV static int sh_fgetc(macFILE *f) { return fgetc(R(f)); }
SV static int sh_ungetc(int c, macFILE *f) { return ungetc(c, R(f)); }
SV static int sh_fflush(macFILE *f) { return fflush(f ? R(f) : NULL); }
SV static int sh_fseek(macFILE *f, long long o, int w) { return _fseeki64(R(f), o, w); }
SV static long long sh_ftell(macFILE *f) { return _ftelli64(R(f)); }
SV static void sh_rewind(macFILE *f) { rewind(R(f)); }
SV static int sh_feof(macFILE *f) { return feof(R(f)); }
SV static int sh_ferror(macFILE *f) { return ferror(R(f)); }
SV static void sh_clearerr(macFILE *f) { clearerr(R(f)); }
SV static int sh_fileno(macFILE *f) { return _fileno(R(f)); }
SV static int sh_fgetpos(macFILE *f, fpos_t *p) { return fgetpos(R(f), p); }
SV static int sh_fsetpos(macFILE *f, const fpos_t *p) { return fsetpos(R(f), p); }
SV static int sh_setvbuf(macFILE *f, char *b, int mode, size_t n)
{
    int m = mode == 0 ? _IOFBF : mode == 1 ? _IOLBF : _IONBF;
    if (m == _IOLBF) { m = _IOFBF; if (!n) n = 4096; }
    if (m != _IONBF && n < 2) n = 2;
    return setvbuf(R(f), b, m, n);
}
SV static void sh_setbuf(macFILE *f, char *b) { setbuf(R(f), b); }
SV static void sh_setlinebuf(macFILE *f) { (void)f; }
SV static int sh_puts(const char *s) { return puts(s); }
SV static int sh_putchar(int c) { return fputc(c, R(mac_stdoutp)); }
SV static int sh_getchar(void) { return fgetc(R(mac_stdinp)); }
SV static void sh_perror(const char *s)
{
    if (s && *s) fprintf(stderr, "%s: %s\n", s, strerror(errno));
    else fprintf(stderr, "%s\n", strerror(errno));
}
SV static int sh_remove(const char *p) { MPDECL; return remove(MP(p)); }
SV static int sh_rename(const char *a, const char *b)
{
    char b1[1024], b2[1024];
    const char *x = mp(a, b1, sizeof b1), *y = mp(b, b2, sizeof b2);
    wchar_t *wa = utf8_to_w(x), *wb = utf8_to_w(y);
    BOOL ok = MoveFileExW(wa, wb, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    free(wa);
    free(wb);
    if (!ok) { errno = ENOENT; return -1; }
    return 0;
}
SV static macFILE *sh_tmpfile(void) { return wrapf(tmpfile()); }
SV static macFILE *sh_popen(const char *cmd, const char *m)
{
    char mb[16];
    macFILE *f = wrapf(_popen(cmd, fmode(m, mb)));
    if (f) f->is_popen = 1;
    return f;
}
SV static int sh_pclose(macFILE *f) { return sh_fclose(f); }
SV static void sh_flockfile(macFILE *f) { _lock_file(R(f)); }
SV static void sh_funlockfile(macFILE *f) { _unlock_file(R(f)); }
SV static int sh_srget(macFILE *f) { f->_r = 0; return fgetc(R(f)); }
SV static int sh_swbuf(int c, macFILE *f) { f->_w = 0; return fputc(c, R(f)); }
SV static int sh_fpurge(macFILE *f) { (void)f; return 0; }
SV static long long sh_getdelim(char **lp, size_t *n, int delim, macFILE *mf)
{
    FILE *f = R(mf);
    if (!lp || !n) { errno = EINVAL; return -1; }
    if (!*lp || !*n) { *n = 128; *lp = realloc(*lp, *n); }
    size_t i = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (i + 2 > *n) { *n *= 2; *lp = realloc(*lp, *n); }
        (*lp)[i++] = (char)c;
        if (c == delim) break;
    }
    if (!i) return -1;
    (*lp)[i] = 0;
    return (long long)i;
}
SV static long long sh_getline(char **lp, size_t *n, macFILE *f) { return sh_getdelim(lp, n, '\n', f); }

/* ------------------------------------------------------------------ */
/* POSIX I/O and filesystem                                           */

SV static long long sh_read(int fd, void *b, size_t n) { return _read(fd, b, (unsigned)min(n, (size_t)INT_MAX)); }
SV static long long sh_write(int fd, const void *b, size_t n) { return _write(fd, b, (unsigned)min(n, (size_t)INT_MAX)); }
SV static long long sh_pread(int fd, void *b, size_t n, long long off)
{
    long long cur = _lseeki64(fd, 0, SEEK_CUR);
    _lseeki64(fd, off, SEEK_SET);
    long long r = _read(fd, b, (unsigned)n);
    _lseeki64(fd, cur, SEEK_SET);
    return r;
}
SV static long long sh_pwrite(int fd, const void *b, size_t n, long long off)
{
    long long cur = _lseeki64(fd, 0, SEEK_CUR);
    _lseeki64(fd, off, SEEK_SET);
    long long r = _write(fd, b, (unsigned)n);
    _lseeki64(fd, cur, SEEK_SET);
    return r;
}
SV static int sh_close(int fd) { return _close(fd); }
SV static long long sh_lseek(int fd, long long o, int w) { return _lseeki64(fd, o, w); }
SV static int sh_dup(int fd) { return _dup(fd); }
SV static int sh_dup2(int a, int b) { return _dup2(a, b) == 0 ? b : -1; }
SV static int sh_pipe(int *fds) { return _pipe(fds, 65536, _O_BINARY); }
SV static int sh_isatty(int fd) { return _isatty(fd); }
SV static int sh_unlink(const char *p) { MPDECL; return _unlink(MP(p)); }
SV static int sh_rmdir(const char *p) { MPDECL; return _rmdir(MP(p)); }
SV static int sh_mkdir(const char *p, int mode) { MPDECL; (void)mode; return _mkdir(MP(p)); }
SV static int sh_chdir(const char *p) { MPDECL; return _chdir(MP(p)); }
SV static int sh_access(const char *p, int m) { MPDECL; return _access(MP(p), m & 6); }
SV static int sh_chmod(const char *p, int m) { MPDECL; return _chmod(MP(p), (m & 0222) ? (_S_IREAD | _S_IWRITE) : _S_IREAD); }
SV static int sh_fchmod(int fd, int m) { (void)fd; (void)m; return 0; }
SV static int sh_chown(const char *p, int u, int g) { (void)p; (void)u; (void)g; return 0; }
SV static int sh_umask(int m) { (void)m; return 022; }
SV static int sh_fsync(int fd) { return _commit(fd); }
SV static int sh_ftruncate(int fd, long long n) { return _chsize_s(fd, n) ? -1 : 0; }
SV static int sh_truncate(const char *p, long long n)
{
    MPDECL;
    int fd = _open(MP(p), _O_RDWR | _O_BINARY);
    if (fd < 0) return -1;
    int r = _chsize_s(fd, n) ? -1 : 0;
    _close(fd);
    return r;
}
SV static int sh_link(const char *a, const char *b)
{
    wchar_t *wa = utf8_to_w(a), *wb = utf8_to_w(b);
    BOOL ok = CreateHardLinkW(wb, wa, NULL);
    free(wa);
    free(wb);
    return ok ? 0 : (errno = EACCES, -1);
}
SV static int sh_symlink(const char *target, const char *link)
{
    wchar_t *wa = utf8_to_w(target), *wb = utf8_to_w(link);
    BOOL ok = CreateSymbolicLinkW(wb, wa, SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
    free(wa);
    free(wb);
    return ok ? 0 : (errno = EACCES, -1);
}
SV static long long sh_readlink(const char *p, char *b, size_t n) { (void)p; (void)b; (void)n; errno = EINVAL; return -1; }
SV static char *sh_getcwd(char *buf, size_t n)
{
    char *r = buf ? _getcwd(buf, (int)n) : _getcwd(NULL, 0);
    slashify(r);
    return r;
}
SV static char *sh_realpath(const char *p, char *out)
{
    MPDECL;
    const char *q = MP(p);
    if (!path_exists(q)) { errno = ENOENT; return NULL; }
    char *r = _fullpath(out, q, 1024);
    slashify(r);
    return r;
}

struct mac_stat {
    int32_t dev;
    uint16_t mode, nlink;
    uint64_t ino;
    uint32_t uid, gid;
    int32_t rdev, pad;
    int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns, btime, btime_ns;
    int64_t size, blocks;
    int32_t blksize;
    uint32_t flags, gen;
    int32_t lspare;
    int64_t qspare[2];
};
_Static_assert(sizeof(struct mac_stat) == 144, "macOS struct stat layout");

static void fill_stat(struct mac_stat *m, const struct _stat64 *s)
{
    memset(m, 0, sizeof *m);
    m->dev = (int32_t)s->st_dev;
    uint16_t perm = s->st_mode & 0700;
    m->mode = (uint16_t)((s->st_mode & 0xF000) | perm | (perm >> 3 & 0050) | (perm >> 6 & 0005));
    m->nlink = (uint16_t)(s->st_nlink ? s->st_nlink : 1);
    m->ino = (uint64_t)s->st_ino;
    m->uid = 501;
    m->gid = 20;
    m->atime = s->st_atime;
    m->mtime = s->st_mtime;
    m->ctime = s->st_ctime;
    m->btime = s->st_ctime;
    m->size = s->st_size;
    m->blksize = 4096;
    m->blocks = (s->st_size + 511) / 512;
}
SV static int sh_stat(const char *p, struct mac_stat *m)
{
    MPDECL;
    struct _stat64 s;
    char buf[1024];
    const char *q = MP(p);
    size_t l = strlen(q);
    /* the CRT rejects trailing slashes on directories */
    if (l > 1 && l < sizeof buf && (q[l - 1] == '/' || q[l - 1] == '\\') && q[l - 2] != ':') {
        memcpy(buf, q, l - 1);
        buf[l - 1] = 0;
        q = buf;
    }
    int r = _stat64(q, &s);
    if (!r) fill_stat(m, &s);
    return r;
}
SV static int sh_fstat(int fd, struct mac_stat *m)
{
    struct _stat64 s;
    int r = _fstat64(fd, &s);
    if (!r) fill_stat(m, &s);
    return r;
}

struct mac_dirent { uint64_t ino, seekoff; uint16_t reclen, namlen; uint8_t type; char name[1024]; };
typedef struct { HANDLE h; WIN32_FIND_DATAW fd; int first; struct mac_dirent ent; int fdnum; } MDIR;

SV static MDIR *sh_opendir(const char *p)
{
    MPDECL;
    char *pat = path_join(MP(p), "*");
    wchar_t *w = utf8_to_w(pat);
    free(pat);
    MDIR *d = xcalloc(1, sizeof *d);
    d->h = FindFirstFileW(w, &d->fd);
    free(w);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); errno = ENOENT; return NULL; }
    d->first = 1;
    return d;
}
SV static struct mac_dirent *sh_readdir(MDIR *d)
{
    if (!d->first && !FindNextFileW(d->h, &d->fd)) return NULL;
    d->first = 0;
    WideCharToMultiByte(CP_UTF8, 0, d->fd.cFileName, -1, d->ent.name, sizeof d->ent.name, NULL, NULL);
    d->ent.namlen = (uint16_t)strlen(d->ent.name);
    d->ent.type = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 4 : 8;
    d->ent.ino++;
    d->ent.reclen = (uint16_t)(21 + d->ent.namlen + 1);
    return &d->ent;
}
SV static int sh_closedir(MDIR *d) { if (d) { FindClose(d->h); free(d); } return 0; }
SV static void sh_rewinddir(MDIR *d) { (void)d; }
SV static int sh_dirfd(MDIR *d) { (void)d; return -1; }

/* ------------------------------------------------------------------ */
/* Environment, process, user                                         */

SV static char *sh_getenv(const char *n) { return getenv(n); }
SV static int sh_setenv(const char *n, const char *v, int ow)
{
    if (!ow && getenv(n)) return 0;
    return _putenv_s(n, v) ? -1 : 0;
}
SV static int sh_unsetenv(const char *n) { return _putenv_s(n, "") ? -1 : 0; }
SV static int sh_putenv(char *s) { return _putenv(s); }
SV static int sh_system(const char *c) { fflush(NULL); return system(c); }
SV static int sh_getpid(void) { return (int)GetCurrentProcessId(); }
SV static int sh_getppid(void) { return 1; }
SV static unsigned sh_getuid(void) { return 501; }
SV static unsigned sh_getgid(void) { return 20; }
SV static int sh_setuid(unsigned u) { (void)u; return 0; }
SV static int sh_fork(void) { errno = ENOSYS; return -1; }
SV static int sh_execvp(const char *f, char *const *a) { (void)f; (void)a; errno = ENOSYS; return -1; }
SV static int sh_waitpid(int p, int *s, int o) { (void)p; (void)s; (void)o; errno = ECHILD; return -1; }
SV static int sh_kill(int pid, int sig);
SV static unsigned sh_sleep(unsigned s) { Sleep(s * 1000); return 0; }
SV static int sh_usleep(unsigned us) { Sleep((us + 999) / 1000); return 0; }
SV static int sh_getpagesize(void) { return 4096; }
SV static int sh_getdtablesize(void) { return 2048; }

static char g_username[256] = "user";
static char g_home[1024];

SV static char *sh_getlogin(void) { return g_username; }
struct mac_passwd { char *name, *passwd; uint32_t uid, gid; int64_t change; char *cls, *gecos, *dir, *shell; int64_t expire; int32_t fields; };
static struct mac_passwd g_pw;
SV static struct mac_passwd *sh_getpwuid(unsigned uid)
{
    (void)uid;
    g_pw.name = g_username;
    g_pw.passwd = "*";
    g_pw.uid = 501;
    g_pw.gid = 20;
    g_pw.cls = "";
    g_pw.gecos = g_username;
    g_pw.dir = g_home;
    g_pw.shell = "/bin/zsh";
    return &g_pw;
}
SV static struct mac_passwd *sh_getpwnam(const char *n) { (void)n; return sh_getpwuid(501); }

struct mac_utsname { char sysname[256], nodename[256], release[256], version[256], machine[256]; };
SV static int sh_uname(struct mac_utsname *u)
{
    memset(u, 0, sizeof *u);
    strcpy(u->sysname, "Darwin");
    DWORD n = 255;
    if (!GetComputerNameA(u->nodename, &n)) strcpy(u->nodename, "localhost");
    strcpy(u->release, "23.6.0");
    strcpy(u->version, "Darwin Kernel Version 23.6.0 (Cider)");
    strcpy(u->machine, "x86_64");
    return 0;
}
SV static int sh_gethostname(char *b, size_t n)
{
    DWORD l = (DWORD)n;
    return GetComputerNameA(b, &l) ? 0 : -1;
}

static int ncpu(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }
static uint64_t memsize(void) { MEMORYSTATUSEX m = { sizeof m }; GlobalMemoryStatusEx(&m); return m.ullTotalPhys; }

SV static long long sh_sysconf(int name)
{
    switch (name) {
    case 1: return 262144;          /* _SC_ARG_MAX */
    case 3: return 100;             /* _SC_CLK_TCK */
    case 5: return 2048;            /* _SC_OPEN_MAX */
    case 29: return 4096;           /* _SC_PAGESIZE */
    case 57: case 58: return ncpu();
    case 200: return (long long)(memsize() / 4096);
    }
    errno = EINVAL;
    return -1;
}

static int put_sysctl(void *old, size_t *oldlen, const void *v, size_t n)
{
    if (old && oldlen) {
        if (*oldlen < n) { errno = ENOMEM; return -1; }
        memcpy(old, v, n);
    }
    if (oldlen) *oldlen = n;
    return 0;
}
SV static int sh_sysctlbyname(const char *name, void *old, size_t *oldlen, void *nw, size_t nl)
{
    (void)nw; (void)nl;
    if (!strcmp(name, "hw.ncpu") || !strcmp(name, "hw.logicalcpu") || !strcmp(name, "hw.physicalcpu") ||
        !strcmp(name, "hw.activecpu") || !strcmp(name, "hw.logicalcpu_max") || !strcmp(name, "hw.physicalcpu_max")) {
        int v = ncpu();
        return put_sysctl(old, oldlen, &v, sizeof v);
    }
    if (!strcmp(name, "hw.memsize")) { uint64_t v = memsize(); return put_sysctl(old, oldlen, &v, 8); }
    if (!strcmp(name, "hw.pagesize")) { int v = 4096; return put_sysctl(old, oldlen, &v, 4); }
    if (!strcmp(name, "kern.osrelease")) return put_sysctl(old, oldlen, "23.6.0", 7);
    if (!strcmp(name, "kern.ostype")) return put_sysctl(old, oldlen, "Darwin", 7);
    if (!strcmp(name, "kern.osproductversion")) return put_sysctl(old, oldlen, "14.6", 5);
    if (!strcmp(name, "hw.machine")) return put_sysctl(old, oldlen, "x86_64", 7);
    if (!strcmp(name, "hw.model")) return put_sysctl(old, oldlen, "Cider1,1", 9);
    if (!strcmp(name, "sysctl.proc_translated")) { int v = 0; return put_sysctl(old, oldlen, &v, 4); }
    if (!strcmp(name, "machdep.cpu.brand_string")) {
        char b[128] = "Intel CPU";
        DWORD n = sizeof b;
        RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     "ProcessorNameString", RRF_RT_REG_SZ, NULL, b, &n);
        return put_sysctl(old, oldlen, b, strlen(b) + 1);
    }
    errno = ENOENT;
    return -1;
}
SV static int sh_sysctl(int *mib, unsigned n, void *old, size_t *oldlen, void *nw, size_t nl)
{
    if (n >= 2 && mib[0] == 6) {
        if (mib[1] == 3 || mib[1] == 25) return sh_sysctlbyname("hw.ncpu", old, oldlen, nw, nl);
        if (mib[1] == 24) return sh_sysctlbyname("hw.memsize", old, oldlen, nw, nl);
        if (mib[1] == 7) return sh_sysctlbyname("hw.pagesize", old, oldlen, nw, nl);
        if (mib[1] == 1) return sh_sysctlbyname("hw.machine", old, oldlen, nw, nl);
    }
    if (n >= 2 && mib[0] == 1 && mib[1] == 2) return sh_sysctlbyname("kern.osrelease", old, oldlen, nw, nl);
    errno = ENOENT;
    return -1;
}

SV static int sh_NSGetExecutablePath(char *buf, uint32_t *size)
{
    size_t n = strlen(g_exe_path) + 1;
    if (*size < n) { *size = (uint32_t)n; return -1; }
    memcpy(buf, g_exe_path, n);
    return 0;
}
SV static int *sh_NSGetArgc(void) { return &g_argc; }
SV static char ***sh_NSGetArgv(void) { return &g_argv; }
SV static char ***sh_NSGetEnviron(void) { return &cider_environ; }
SV static const char **sh_NSGetProgname(void) { return &g_progname; }
SV static const char *sh_getprogname(void) { return g_progname; }
SV static void sh_setprogname(const char *p) { g_progname = p; }

SV static uint32_t sh_dyld_image_count(void) { return (uint32_t)g_nreg; }
SV static void *sh_dyld_get_image_header(uint32_t i) { return i < (uint32_t)g_nreg ? g_reg[i].base : NULL; }
SV static const char *sh_dyld_get_image_name(uint32_t i) { return i < (uint32_t)g_nreg ? g_reg[i].path : NULL; }
SV static intptr_t sh_dyld_get_image_vmaddr_slide(uint32_t i) { (void)i; return 0; }

SV static void *sh_dlopen(const char *p, int mode) { (void)mode; return p ? (void *)macho_dlsym : (void *)-2; }
SV static void *sh_dlsym(void *h, const char *name)
{
    (void)h;
    char buf[512];
    snprintf(buf, sizeof buf, "_%s", name);
    void *p = macho_dlsym(buf);
    return p ? p : shim_lookup(buf);
}
SV static const char *sh_dlerror(void) { return "dlopen of macOS libraries is not supported by Cider"; }
SV static int sh_dlclose(void *h) { (void)h; return 0; }
SV static int sh_dladdr(const void *a, void *info) { (void)a; (void)info; return 0; }

/* rlimit / rusage */
SV static int sh_getrlimit(int r, uint64_t *rl)
{
    rl[0] = rl[1] = r == 8 ? 2048 : 0x7FFFFFFFFFFFFFFFULL;
    if (r == 3) rl[0] = 8 << 20;   /* RLIMIT_STACK */
    return 0;
}
SV static int sh_setrlimit(int r, const uint64_t *rl) { (void)r; (void)rl; return 0; }
SV static int sh_getrusage(int who, int64_t *ru)
{
    (void)who;
    memset(ru, 0, 144);
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    uint64_t ut = ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime) / 10;
    uint64_t kt = ((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) / 10;
    ru[0] = (int64_t)(ut / 1000000); ru[1] = (int64_t)(ut % 1000000);
    ru[2] = (int64_t)(kt / 1000000); ru[3] = (int64_t)(kt % 1000000);
    return 0;
}

/* termios: just enough for raw-mode terminal programs */
SV static int sh_tcgetattr(int fd, uint64_t *t)
{
    memset(t, 0, 72);
    DWORD mode = 0;
    if (!GetConsoleMode((HANDLE)_get_osfhandle(fd), &mode)) { errno = ENOTTY; return -1; }
    t[3] = 0x8 | 0x100 | 0x80;    /* ECHO | ICANON | ISIG */
    if (!(mode & ENABLE_ECHO_INPUT)) t[3] &= ~0x8ULL;
    if (!(mode & ENABLE_LINE_INPUT)) t[3] &= ~0x100ULL;
    return 0;
}
SV static int sh_tcsetattr(int fd, int act, const uint64_t *t)
{
    (void)act;
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) { errno = ENOTTY; return -1; }
    if (t[3] & 0x8) mode |= ENABLE_ECHO_INPUT; else mode &= ~ENABLE_ECHO_INPUT;
    if (t[3] & 0x100) mode |= ENABLE_LINE_INPUT; else mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    if (t[3] & 0x80) mode |= ENABLE_PROCESSED_INPUT; else mode &= ~ENABLE_PROCESSED_INPUT;
    mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    SetConsoleMode(h, mode);
    return 0;
}
SV static char *sh_ttyname(int fd) { return _isatty(fd) ? "/dev/ttys000" : NULL; }

/* getopt / getopt_long */
static char *g_optarg;
static int g_optind = 1, g_opterr = 1, g_optopt = '?', g_optreset;
static char *g_nextchar;
struct mac_option { const char *name; int has_arg; int *flag; int val; };

static int getopt_impl(int argc, char *const *argv, const char *os, const struct mac_option *lo, int *li)
{
    if (g_optreset || g_optind == 0) { g_optreset = 0; if (!g_optind) g_optind = 1; g_nextchar = NULL; }
    int colon = 0;
    if (*os == '+' || *os == '-') os++;
    if (*os == ':') { colon = 1; os++; }
    g_optarg = NULL;
    if (!g_nextchar || !*g_nextchar) {
        if (g_optind >= argc) return -1;
        char *a = argv[g_optind];
        if (!a || a[0] != '-' || !a[1]) return -1;
        if (!strcmp(a, "--")) { g_optind++; return -1; }
        if (a[1] == '-' && lo) {
            const char *name = a + 2, *eq = strchr(name, '=');
            size_t nl = eq ? (size_t)(eq - name) : strlen(name);
            int match = -1, ambiguous = 0;
            for (int i = 0; lo[i].name; i++) {
                if (strncmp(lo[i].name, name, nl)) continue;
                if (strlen(lo[i].name) == nl) { match = i; ambiguous = 0; break; }
                if (match >= 0) ambiguous = 1; else match = i;
            }
            g_optind++;
            if (match < 0 || ambiguous) {
                if (g_opterr) fprintf(stderr, "%s: %s option `%s'\n", g_progname, ambiguous ? "ambiguous" : "unrecognized", a);
                g_optopt = 0;
                return '?';
            }
            const struct mac_option *o = &lo[match];
            if (o->has_arg == 1) {
                if (eq) g_optarg = (char *)eq + 1;
                else if (g_optind < argc) g_optarg = argv[g_optind++];
                else {
                    if (g_opterr && !colon) fprintf(stderr, "%s: option `--%s' requires an argument\n", g_progname, o->name);
                    g_optopt = o->val;
                    return colon ? ':' : '?';
                }
            } else if (o->has_arg == 2) {
                g_optarg = eq ? (char *)eq + 1 : NULL;
            } else if (eq) {
                if (g_opterr) fprintf(stderr, "%s: option `--%s' doesn't allow an argument\n", g_progname, o->name);
                g_optopt = o->val;
                return '?';
            }
            if (li) *li = match;
            if (o->flag) { *o->flag = o->val; return 0; }
            return o->val;
        }
        g_nextchar = a + 1;
    }
    int ch = (unsigned char)*g_nextchar++;
    const char *p = strchr(os, ch);
    if (!p || ch == ':') {
        g_optopt = ch;
        if (!*g_nextchar) g_optind++;
        if (g_opterr && !colon) fprintf(stderr, "%s: illegal option -- %c\n", g_progname, ch);
        return '?';
    }
    if (p[1] == ':') {
        if (*g_nextchar) { g_optarg = g_nextchar; g_optind++; }
        else if (p[2] == ':') { g_optind++; }
        else if (g_optind + 1 < argc) { g_optarg = argv[g_optind + 1]; g_optind += 2; }
        else {
            g_optopt = ch;
            g_optind++;
            g_nextchar = NULL;
            if (g_opterr && !colon) fprintf(stderr, "%s: option requires an argument -- %c\n", g_progname, ch);
            return colon ? ':' : '?';
        }
        g_nextchar = NULL;
    } else if (!*g_nextchar) {
        g_optind++;
        g_nextchar = NULL;
    }
    return ch;
}
SV static int sh_getopt(int argc, char *const *argv, const char *os) { return getopt_impl(argc, argv, os, NULL, NULL); }
SV static int sh_getopt_long(int argc, char *const *argv, const char *os, const struct mac_option *lo, int *li)
{
    return getopt_impl(argc, argv, os, lo, li);
}

/* ------------------------------------------------------------------ */
/* Sorting                                                            */

typedef SV int (*cmp_fn)(const void *, const void *);
typedef SV int (*cmpr_fn)(void *, const void *, const void *);

typedef struct { cmp_fn f; cmpr_fn fr; void *th; } cmpctx;
static int callcmp(cmpctx *c, const void *a, const void *b) { return c->f ? c->f(a, b) : c->fr(c->th, a, b); }

static void msort(char **a, char **t, size_t n, cmpctx *c)
{
    if (n < 2) return;
    size_t m = n / 2;
    msort(a, t, m, c);
    msort(a + m, t, n - m, c);
    size_t i = 0, j = m, k = 0;
    while (i < m && j < n) t[k++] = callcmp(c, a[j], a[i]) < 0 ? a[j++] : a[i++];
    while (i < m) t[k++] = a[i++];
    while (j < n) t[k++] = a[j++];
    memcpy(a, t, n * sizeof(char *));
}

static void do_sort(void *base, size_t n, size_t w, cmpctx *c)
{
    if (n < 2 || !w) return;
    char **p = xmalloc(n * sizeof(char *)), **t = xmalloc(n * sizeof(char *));
    for (size_t i = 0; i < n; i++) p[i] = (char *)base + i * w;
    msort(p, t, n, c);
    char *out = xmalloc(n * w);
    for (size_t i = 0; i < n; i++) memcpy(out + i * w, p[i], w);
    memcpy(base, out, n * w);
    free(out);
    free(p);
    free(t);
}
SV static void sh_qsort(void *b, size_t n, size_t w, cmp_fn f) { cmpctx c = { f, NULL, NULL }; do_sort(b, n, w, &c); }
SV static void sh_qsort_r(void *b, size_t n, size_t w, void *th, cmpr_fn f) { cmpctx c = { NULL, f, th }; do_sort(b, n, w, &c); }
SV static int sh_heapsort(void *b, size_t n, size_t w, cmp_fn f) { sh_qsort(b, n, w, f); return 0; }
SV static void *sh_bsearch(const void *key, const void *b, size_t n, size_t w, cmp_fn f)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const char *e = (const char *)b + mid * w;
        int r = f(key, e);
        if (!r) return (void *)e;
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Random numbers                                                     */

static uint64_t g_rng = 0x853c49e6748fea9bULL;
static int32_t rng31(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (int32_t)(g_rng >> 33);
}
SV static int sh_rand(void) { return rng31(); }
SV static void sh_srand(unsigned s) { g_rng = 0x9E3779B97F4A7C15ULL * (s + 1); }
SV static long long sh_random(void) { return rng31(); }
SV static void sh_srandom(unsigned s) { sh_srand(s); }
SV static uint32_t sh_arc4random(void) { unsigned v; rand_s(&v); return v; }
SV static uint32_t sh_arc4random_uniform(uint32_t n)
{
    if (n < 2) return 0;
    uint32_t lim = (uint32_t)(-n) % n, v;
    do v = sh_arc4random(); while (v < lim);
    return v % n;
}
SV static void sh_arc4random_buf(void *b, size_t n)
{
    for (size_t i = 0; i < n; i += 4) {
        uint32_t v = sh_arc4random();
        memcpy((char *)b + i, &v, n - i < 4 ? n - i : 4);
    }
}
SV static int sh_getentropy(void *b, size_t n) { sh_arc4random_buf(b, n); return 0; }
SV static double sh_drand48(void) { return (double)(uint32_t)rng31() / 2147483648.0; }
SV static void sh_srand48(long long s) { sh_srand((unsigned)s); }
SV static long long sh_lrand48(void) { return rng31(); }

/* ------------------------------------------------------------------ */
/* Time                                                               */

struct mac_tm { int sec, min, hour, mday, mon, year, wday, yday, isdst, pad; int64_t gmtoff; char *zone; };
static _Thread_local struct mac_tm t_tm;
static char g_tzname[2][64];

static struct mac_tm *to_mac_tm(const struct tm *t, struct mac_tm *m, int utc)
{
    memcpy(m, t, 9 * sizeof(int));
    if (utc) { m->gmtoff = 0; m->zone = "UTC"; return m; }
    long tz = 0;
    _get_timezone(&tz);
    int dst = t->tm_isdst > 0;
    m->gmtoff = -tz + (dst ? 3600 : 0);
    m->zone = g_tzname[dst];
    return m;
}
static void from_mac_tm(const struct mac_tm *m, struct tm *t) { memcpy(t, m, 9 * sizeof(int)); }

SV static int64_t sh_time(int64_t *t) { int64_t v = _time64(NULL); if (t) *t = v; return v; }
SV static struct mac_tm *sh_localtime_r(const int64_t *t, struct mac_tm *out)
{
    struct tm w;
    __time64_t tt = *t;
    if (_localtime64_s(&w, &tt)) return NULL;
    return to_mac_tm(&w, out, 0);
}
SV static struct mac_tm *sh_gmtime_r(const int64_t *t, struct mac_tm *out)
{
    struct tm w;
    __time64_t tt = *t;
    if (_gmtime64_s(&w, &tt)) return NULL;
    return to_mac_tm(&w, out, 1);
}
SV static struct mac_tm *sh_localtime(const int64_t *t) { return sh_localtime_r(t, &t_tm); }
SV static struct mac_tm *sh_gmtime(const int64_t *t) { return sh_gmtime_r(t, &t_tm); }
SV static int64_t sh_mktime(struct mac_tm *m)
{
    struct tm t;
    from_mac_tm(m, &t);
    int64_t r = _mktime64(&t);
    to_mac_tm(&t, m, 0);
    return r;
}
SV static int64_t sh_timegm(struct mac_tm *m)
{
    struct tm t;
    from_mac_tm(m, &t);
    int64_t r = _mkgmtime64(&t);
    to_mac_tm(&t, m, 1);
    return r;
}
SV static size_t sh_strftime(char *buf, size_t max, const char *fmt, const struct mac_tm *m)
{
    struct tm t;
    from_mac_tm(m, &t);
    char f2[1024];
    size_t o = 0;
    for (const char *p = fmt; *p && o < sizeof f2 - 64; p++) {
        if (*p != '%') { f2[o++] = *p; continue; }
        p++;
        if (*p == 'E' || *p == 'O') p++;
        if (!*p) break;
        switch (*p) {
        case 's': {
            struct tm c = t;
            o += (size_t)snprintf(f2 + o, 32, "%lld", (long long)_mktime64(&c));
            break;
        }
        case 'k': f2[o++] = '%'; f2[o++] = 'H'; break;
        case 'l': f2[o++] = '%'; f2[o++] = 'I'; break;
        case 'P': f2[o++] = '%'; f2[o++] = 'p'; break;
        case '+': memcpy(f2 + o, "%a %b %e %H:%M:%S %Z %Y", 23); o += 23; break;
        case 'z': {
            int64_t off = m->gmtoff;
            o += (size_t)snprintf(f2 + o, 16, "%c%02d%02d", off < 0 ? '-' : '+', (int)(llabs(off) / 3600), (int)(llabs(off) % 3600 / 60));
            break;
        }
        case 'Z': o += (size_t)snprintf(f2 + o, 64, "%s", m->zone ? m->zone : ""); break;
        default: f2[o++] = '%'; f2[o++] = *p;
        }
    }
    f2[o] = 0;
    return strftime(buf, max, f2, &t);
}
SV static char *sh_asctime(const struct mac_tm *m) { struct tm t; from_mac_tm(m, &t); return asctime(&t); }
SV static char *sh_ctime(const int64_t *t) { __time64_t tt = *t; return _ctime64(&tt); }
SV static double sh_difftime(int64_t a, int64_t b) { return (double)(a - b); }
SV static void sh_tzset(void) { _tzset(); }

static uint64_t qpc_ns(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((__int128)c.QuadPart * 1000000000 / f.QuadPart);
}
static uint64_t unix_ns(void)
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ULL;
    return t * 100;
}
static uint64_t cpu_ns(int thread)
{
    FILETIME c, e, k, u;
    if (thread) GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
    else GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return (((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime)) * 100;
}
static uint64_t clock_ns(int id)
{
    switch (id) {
    case 0: return unix_ns();
    case 12: return cpu_ns(0);
    case 16: return cpu_ns(1);
    default: return qpc_ns();
    }
}
SV static uint64_t sh_clock(void) { return cpu_ns(0) / 1000; }
SV static int sh_gettimeofday(int64_t *tv, void *tz)
{
    (void)tz;
    if (tv) {
        uint64_t ns = unix_ns();
        tv[0] = (int64_t)(ns / 1000000000);
        ((int32_t *)tv)[2] = (int32_t)(ns % 1000000000 / 1000);
    }
    return 0;
}
SV static int sh_clock_gettime(int id, int64_t *ts)
{
    uint64_t ns = clock_ns(id);
    ts[0] = (int64_t)(ns / 1000000000);
    ts[1] = (int64_t)(ns % 1000000000);
    return 0;
}
SV static int sh_clock_getres(int id, int64_t *ts) { (void)id; if (ts) { ts[0] = 0; ts[1] = 1000; } return 0; }
SV static uint64_t sh_clock_gettime_nsec_np(int id) { return clock_ns(id); }
SV static uint64_t sh_mach_absolute_time(void) { return qpc_ns(); }
SV static int sh_mach_timebase_info(uint32_t *i) { i[0] = 1; i[1] = 1; return 0; }
SV static int sh_nanosleep(const int64_t *req, int64_t *rem)
{
    uint64_t ms = (uint64_t)req[0] * 1000 + (uint64_t)(req[1] + 999999) / 1000000;
    Sleep((DWORD)ms);
    if (rem) rem[0] = rem[1] = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Signals                                                            */

static void *g_sig[64];
static void sig_tramp(int s)
{
    int mac = s == SIGABRT ? 6 : s;
    signal(s, sig_tramp);
    void *h = g_sig[mac];
    if (h && h != (void *)1) ((SV void (*)(int))h)(mac);
}
static int win_sig(int s)
{
    switch (s) {
    case 2: return SIGINT;
    case 4: return SIGILL;
    case 6: return SIGABRT;
    case 8: return SIGFPE;
    case 11: return SIGSEGV;
    case 15: return SIGTERM;
    }
    return -1;
}
SV static void *sh_signal(int sig, void *h)
{
    if (sig <= 0 || sig >= 64) { errno = EINVAL; return (void *)-1; }
    void *old = g_sig[sig];
    g_sig[sig] = h;
    int ws = win_sig(sig);
    if (ws >= 0) signal(ws, h == NULL ? SIG_DFL : h == (void *)1 ? SIG_IGN : sig_tramp);
    return old;
}
SV static int sh_sigaction(int sig, const void **act, void **oact)
{
    if (sig <= 0 || sig >= 64) { errno = EINVAL; return -1; }
    if (oact) { memset(oact, 0, 16); oact[0] = g_sig[sig]; }
    if (act) sh_signal(sig, (void *)act[0]);
    return 0;
}
SV static int sh_raise(int sig)
{
    int ws = win_sig(sig);
    if (g_sig[sig] && g_sig[sig] != (void *)1) { ((SV void (*)(int))g_sig[sig])(sig); return 0; }
    if (ws >= 0) return raise(ws);
    return 0;
}
SV static int sh_kill(int pid, int sig)
{
    if (pid == (int)GetCurrentProcessId() || pid == 0) return sh_raise(sig);
    errno = EPERM;
    return -1;
}
SV static int sh_sigzero(uint32_t *s) { *s = 0; return 0; }
SV static int sh_sigfill(uint32_t *s) { *s = 0xFFFFFFFF; return 0; }
SV static int sh_sigadd(uint32_t *s, int n) { *s |= 1u << (n - 1); return 0; }
SV static int sh_sigdel(uint32_t *s, int n) { *s &= ~(1u << (n - 1)); return 0; }
SV static int sh_sigismember(const uint32_t *s, int n) { return (*s >> (n - 1)) & 1; }
SV static int sh_sigprocmask(int h, const void *s, void *o) { (void)h; (void)s; if (o) *(uint32_t *)o = 0; return 0; }
SV static unsigned sh_alarm(unsigned s) { (void)s; return 0; }

/* ------------------------------------------------------------------ */
/* Threads                                                            */

typedef struct cthread { HANDLE h; void *fn, *arg, *ret; DWORD id; } cthread;
static DWORD g_tls_self = TLS_OUT_OF_INDEXES;

static DWORD WINAPI thread_tramp(LPVOID p)
{
    cthread *t = p;
    TlsSetValue(g_tls_self, t);
    t->ret = ((SV void *(*)(void *))t->fn)(t->arg);
    return 0;
}
SV static int sh_pthread_create(cthread **out, void *attr, void *fn, void *arg)
{
    (void)attr;
    cthread *t = xcalloc(1, sizeof *t);
    t->fn = fn;
    t->arg = arg;
    t->h = CreateThread(NULL, 8u << 20, thread_tramp, t, STACK_SIZE_PARAM_IS_A_RESERVATION, &t->id);
    if (!t->h) { free(t); return EAGAIN; }
    *out = t;
    return 0;
}
SV static int sh_pthread_join(cthread *t, void **ret)
{
    WaitForSingleObject(t->h, INFINITE);
    if (ret) *ret = t->ret;
    CloseHandle(t->h);
    return 0;
}
SV static int sh_pthread_detach(cthread *t) { (void)t; return 0; }
SV static cthread *sh_pthread_self(void)
{
    cthread *t = TlsGetValue(g_tls_self);
    if (!t) { t = xcalloc(1, sizeof *t); t->id = GetCurrentThreadId(); TlsSetValue(g_tls_self, t); }
    return t;
}
SV static int sh_pthread_equal(void *a, void *b) { return a == b; }
SV static void sh_pthread_exit(void *r) { cthread *t = sh_pthread_self(); t->ret = r; ExitThread(0); }
SV static int sh_pthread_threadid_np(cthread *t, uint64_t *id) { *id = t ? t->id : GetCurrentThreadId(); return 0; }
SV static int sh_pthread_zero(void) { return 0; }
SV static int sh_pthread_attr_getstacksize(void *a, size_t *s) { (void)a; *s = 8u << 20; return 0; }

static CRITICAL_SECTION *mutex_cs(void *m)
{
    CRITICAL_SECTION **slot = (CRITICAL_SECTION **)((char *)m + 8);
    CRITICAL_SECTION *cs = *slot;
    if (cs) return cs;
    cs = xmalloc(sizeof *cs);
    InitializeCriticalSection(cs);
    if (InterlockedCompareExchangePointer((void **)slot, cs, NULL)) {
        DeleteCriticalSection(cs);
        free(cs);
    }
    return *slot;
}
SV static int sh_mutex_init(void *m, void *a) { (void)a; memset(m, 0, 64); *(uint64_t *)m = 0x32AAABA7; mutex_cs(m); return 0; }
SV static int sh_mutex_lock(void *m) { EnterCriticalSection(mutex_cs(m)); return 0; }
SV static int sh_mutex_trylock(void *m) { return TryEnterCriticalSection(mutex_cs(m)) ? 0 : EBUSY; }
SV static int sh_mutex_unlock(void *m) { LeaveCriticalSection(mutex_cs(m)); return 0; }
SV static int sh_mutex_destroy(void *m)
{
    CRITICAL_SECTION **slot = (CRITICAL_SECTION **)((char *)m + 8);
    if (*slot) { DeleteCriticalSection(*slot); free(*slot); *slot = NULL; }
    return 0;
}
SV static int sh_cond_init(void *c, void *a) { (void)a; memset(c, 0, 48); return 0; }
SV static int sh_cond_wait(void *c, void *m)
{
    SleepConditionVariableCS((PCONDITION_VARIABLE)((char *)c + 8), mutex_cs(m), INFINITE);
    return 0;
}
SV static int sh_cond_timedwait(void *c, void *m, const int64_t *abst)
{
    int64_t now = (int64_t)(unix_ns() / 1000000);
    int64_t when = abst[0] * 1000 + abst[1] / 1000000;
    DWORD ms = when > now ? (DWORD)(when - now) : 0;
    if (!SleepConditionVariableCS((PCONDITION_VARIABLE)((char *)c + 8), mutex_cs(m), ms))
        return GetLastError() == ERROR_TIMEOUT ? 60 : EINVAL;
    return 0;
}
SV static int sh_cond_signal(void *c) { WakeConditionVariable((PCONDITION_VARIABLE)((char *)c + 8)); return 0; }
SV static int sh_cond_broadcast(void *c) { WakeAllConditionVariable((PCONDITION_VARIABLE)((char *)c + 8)); return 0; }
SV static int sh_pthread_once(void *o, void *fn)
{
    volatile LONG *st = (volatile LONG *)((char *)o + 8);
    if (*st == 2) return 0;
    if (InterlockedCompareExchange(st, 1, 0) == 0) { ((SV void (*)(void))fn)(); *st = 2; }
    else while (*st != 2) SwitchToThread();
    return 0;
}
SV static int sh_key_create(uint64_t *k, void *d) { (void)d; DWORD i = TlsAlloc(); if (i == TLS_OUT_OF_INDEXES) return EAGAIN; *k = i; return 0; }
SV static int sh_key_delete(uint64_t k) { TlsFree((DWORD)k); return 0; }
SV static void *sh_getspecific(uint64_t k) { return TlsGetValue((DWORD)k); }
SV static int sh_setspecific(uint64_t k, const void *v) { TlsSetValue((DWORD)k, (void *)v); return 0; }
SV static int sh_rw_init(void *l, void *a) { (void)a; memset(l, 0, 32); return 0; }
SV static int sh_rw_rdlock(void *l) { AcquireSRWLockShared((PSRWLOCK)((char *)l + 8)); return 0; }
SV static int sh_rw_wrlock(void *l) { AcquireSRWLockExclusive((PSRWLOCK)((char *)l + 8)); *(int *)((char *)l + 16) = 1; return 0; }
SV static int sh_rw_unlock(void *l)
{
    int *w = (int *)((char *)l + 16);
    if (*w) { *w = 0; ReleaseSRWLockExclusive((PSRWLOCK)((char *)l + 8)); }
    else ReleaseSRWLockShared((PSRWLOCK)((char *)l + 8));
    return 0;
}
SV static int sh_sched_yield(void) { SwitchToThread(); return 0; }
SV static void sh_unfair_lock(volatile LONG *l) { while (InterlockedCompareExchange(l, 1, 0)) SwitchToThread(); }
SV static void sh_unfair_unlock(volatile LONG *l) { InterlockedExchange(l, 0); }
SV static int sh_unfair_trylock(volatile LONG *l) { return InterlockedCompareExchange(l, 1, 0) == 0; }

/* ------------------------------------------------------------------ */
/* Memory mapping                                                     */

static DWORD win_prot(int p)
{
    if (p & 4) return (p & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    if (p & 2) return PAGE_READWRITE;
    if (p & 1) return PAGE_READONLY;
    return PAGE_NOACCESS;
}
SV static void *sh_mmap(void *addr, size_t len, int prot, int flags, int fd, long long off)
{
    (void)addr;
    void *p = VirtualAlloc(NULL, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) { errno = ENOMEM; return (void *)-1; }
    if (!(flags & 0x1000) && fd >= 0) {
        long long cur = _lseeki64(fd, 0, SEEK_CUR);
        _lseeki64(fd, off, SEEK_SET);
        size_t done = 0;
        while (done < len) {
            int r = _read(fd, (char *)p + done, (unsigned)min(len - done, (size_t)1 << 30));
            if (r <= 0) break;
            done += (size_t)r;
        }
        _lseeki64(fd, cur, SEEK_SET);
    }
    DWORD old;
    if (prot != 3) VirtualProtect(p, len, win_prot(prot), &old);
    return p;
}
SV static int sh_munmap(void *p, size_t len) { (void)len; VirtualFree(p, 0, MEM_RELEASE); return 0; }
SV static int sh_mprotect(void *p, size_t len, int prot) { DWORD old; return VirtualProtect(p, len, win_prot(prot), &old) ? 0 : -1; }
SV static int sh_madvise(void *p, size_t l, int a) { (void)p; (void)l; (void)a; return 0; }

/* ------------------------------------------------------------------ */
/* Math                                                               */

#define M1(n) SV static double sh_##n(double x) { return n(x); }
#define M2(n) SV static double sh_##n(double x, double y) { return n(x, y); }
#define F1(n) SV static float sh_##n(float x) { return n(x); }
#define F2(n) SV static float sh_##n(float x, float y) { return n(x, y); }
M1(sin) M1(cos) M1(tan) M1(asin) M1(acos) M1(atan) M1(sinh) M1(cosh) M1(tanh) M1(asinh) M1(acosh) M1(atanh)
M1(exp) M1(exp2) M1(expm1) M1(log) M1(log2) M1(log10) M1(log1p) M1(sqrt) M1(cbrt) M1(fabs) M1(floor)
M1(ceil) M1(round) M1(trunc) M1(rint) M1(nearbyint) M1(erf) M1(erfc) M1(tgamma) M1(lgamma) M1(logb)
M2(pow) M2(atan2) M2(fmod) M2(hypot) M2(fmin) M2(fmax) M2(copysign) M2(remainder) M2(nextafter) M2(fdim)
F1(sinf) F1(cosf) F1(tanf) F1(asinf) F1(acosf) F1(atanf) F1(expf) F1(exp2f) F1(logf) F1(log2f) F1(log10f)
F1(sqrtf) F1(cbrtf) F1(fabsf) F1(floorf) F1(ceilf) F1(roundf) F1(truncf) F1(sinhf) F1(coshf) F1(tanhf) F1(rintf)
F2(powf) F2(atan2f) F2(fmodf) F2(hypotf) F2(fminf) F2(fmaxf) F2(copysignf)
SV static double sh_ldexp(double x, int e) { return ldexp(x, e); }
SV static float sh_ldexpf(float x, int e) { return ldexpf(x, e); }
SV static double sh_frexp(double x, int *e) { return frexp(x, e); }
SV static double sh_modf(double x, double *i) { return modf(x, i); }
SV static float sh_modff(float x, float *i) { return modff(x, i); }
SV static double sh_fma(double a, double b, double c) { return fma(a, b, c); }
SV static long long sh_lround(double x) { return llround(x); }
SV static long long sh_lrint(double x) { return llrint(x); }
SV static long long sh_lroundf(float x) { return llroundf(x); }
SV static double sh_exp10(double x) { return pow(10.0, x); }
SV static double sh_sinpi(double x) { return sin(M_PI * x); }
SV static double sh_cospi(double x) { return cos(M_PI * x); }
SV static int sh_isnan(double x) { return isnan(x); }
SV static int sh_isinf(double x) { return isinf(x); }
typedef struct { double s, c; } sincos_ret;
typedef struct { float s, c; } sincosf_ret;
SV static sincos_ret sh_sincos_stret(double x) { sincos_ret r = { sin(x), cos(x) }; return r; }
SV static sincosf_ret sh_sincosf_stret(float x) { sincosf_ret r = { sinf(x), cosf(x) }; return r; }
typedef struct { int q, r; } div_ret;
typedef struct { long long q, r; } ldiv_ret;
SV static div_ret sh_div(int a, int b) { div_ret r = { a / b, a % b }; return r; }
SV static ldiv_ret sh_ldiv(long long a, long long b) { ldiv_ret r = { a / b, a % b }; return r; }

SV static int *sh_error(void) { return _errno(); }
SV static char *sh_setlocale(int c, const char *l) { (void)c; (void)l; return "en_US.UTF-8"; }
SV static void *sh_localeconv(void) { return localeconv(); }
SV static char *sh_basename(char *p)
{
    static _Thread_local char b[1024];
    if (!p || !*p) return ".";
    snprintf(b, sizeof b, "%s", p);
    size_t n = strlen(b);
    while (n > 1 && b[n - 1] == '/') b[--n] = 0;
    char *s = strrchr(b, '/');
    return s && s[1] ? s + 1 : b;
}
SV static char *sh_dirname(char *p)
{
    static _Thread_local char b[1024];
    if (!p || !*p) return ".";
    snprintf(b, sizeof b, "%s", p);
    size_t n = strlen(b);
    while (n > 1 && b[n - 1] == '/') b[--n] = 0;
    char *s = strrchr(b, '/');
    if (!s) return ".";
    if (s == b) s[1] = 0; else *s = 0;
    return b;
}
SV static int sh_backtrace(void **b, int n) { (void)b; (void)n; return 0; }
SV static char **sh_backtrace_symbols(void **b, int n) { (void)b; (void)n; return NULL; }
SV static int sh_noop(void) { return 0; }

/* in thunks.S */
extern void th_printf(void), th_fprintf(void), th_sprintf(void), th_snprintf(void), th_asprintf(void),
    th_dprintf(void), th_sprintf_chk(void), th_snprintf_chk(void), th_sscanf(void), th_fscanf(void),
    th_scanf(void), th_syslog(void), th_err(void), th_errx(void), th_warn(void), th_warnx(void),
    th_open(void), th_openat(void), th_fcntl(void), th_ioctl(void), cider_chkstk_darwin(void),
    cider_setjmp(void), cider_longjmp(void), cider_tlv_thunk(void);

/* ------------------------------------------------------------------ */
/* Symbol table                                                       */

typedef struct { const char *name; void *addr; } shim;
#define S(n) { "_" #n, (void *)sh_##n }
#define SA(name, fn) { name, (void *)(fn) }

static shim g_shims[] = {
    /* stdio */
    SA("_printf", th_printf), SA("_fprintf", th_fprintf), SA("_sprintf", th_sprintf), SA("_snprintf", th_snprintf),
    SA("_asprintf", th_asprintf), SA("_dprintf", th_dprintf), SA("___sprintf_chk", th_sprintf_chk),
    SA("___snprintf_chk", th_snprintf_chk), SA("_sscanf", th_sscanf), SA("_fscanf", th_fscanf), SA("_scanf", th_scanf),
    SA("_syslog", th_syslog), SA("_err", th_err), SA("_errx", th_errx), SA("_warn", th_warn), SA("_warnx", th_warnx),
    S(vprintf), S(vfprintf), S(vsprintf), S(vsnprintf), S(vasprintf), S(vsscanf), S(vfscanf), S(vdprintf),
    SA("___vsprintf_chk", sh_vsprintf_chk), SA("___vsnprintf_chk", sh_vsnprintf_chk),
    S(fopen), S(fdopen), S(freopen), S(fclose), S(fread), S(fwrite), S(fgets), S(fputs), S(fputc), S(fgetc),
    SA("_putc", sh_fputc), SA("_getc", sh_fgetc), SA("_putc_unlocked", sh_fputc), SA("_getc_unlocked", sh_fgetc),
    SA("_putchar_unlocked", sh_putchar), SA("_getchar_unlocked", sh_getchar),
    S(ungetc), S(fflush), S(fseek), SA("_fseeko", sh_fseek), S(ftell), SA("_ftello", sh_ftell), S(rewind), S(feof),
    S(ferror), S(clearerr), S(fileno), S(fgetpos), S(fsetpos), S(setvbuf), S(setbuf), S(setlinebuf), S(puts),
    S(putchar), S(getchar), S(perror), S(remove), S(rename), S(tmpfile), S(popen), S(pclose), S(flockfile),
    S(funlockfile), SA("___srget", sh_srget), SA("___swbuf", sh_swbuf), S(fpurge), S(getline), S(getdelim),
    SA("___stdinp", &mac_stdinp), SA("___stdoutp", &mac_stdoutp), SA("___stderrp", &mac_stderrp),
    /* process */
    SA("_exit", shim_exit), SA("__exit", sh__exit), SA("__Exit", sh__exit), S(abort), S(atexit),
    SA("___cxa_atexit", sh_cxa_atexit), SA("___cxa_finalize", sh_noop), SA("___stack_chk_fail", sh_stack_chk_fail),
    SA("___stack_chk_guard", &g_stack_chk_guard), SA("___assert_rtn", sh_assert_rtn), SA("___error", sh_error),
    SA("___chkstk_darwin", cider_chkstk_darwin), SA("_environ", &cider_environ), SA("_NXArgc", &g_argc),
    SA("_NXArgv", &g_argv), SA("___progname", &g_progname), SA("___mb_cur_max", &g_mb_cur_max),
    SA("__NSGetExecutablePath", sh_NSGetExecutablePath), SA("__NSGetArgc", sh_NSGetArgc),
    SA("__NSGetArgv", sh_NSGetArgv), SA("__NSGetEnviron", sh_NSGetEnviron), SA("__NSGetProgname", sh_NSGetProgname),
    S(getprogname), S(setprogname), SA("__dyld_image_count", sh_dyld_image_count),
    SA("__dyld_get_image_header", sh_dyld_get_image_header), SA("__dyld_get_image_name", sh_dyld_get_image_name),
    SA("__dyld_get_image_vmaddr_slide", sh_dyld_get_image_vmaddr_slide),
    S(dlopen), S(dlsym), S(dlerror), S(dlclose), S(dladdr),
    S(getenv), S(setenv), S(unsetenv), S(putenv), S(system), S(getpid), S(getppid), S(getuid), SA("_geteuid", sh_getuid),
    S(getgid), SA("_getegid", sh_getgid), S(setuid), SA("_setgid", sh_setuid), S(fork), SA("_vfork", sh_fork),
    S(execvp), SA("_execv", sh_execvp), SA("_execve", sh_execvp), S(waitpid), SA("_wait", sh_waitpid),
    S(sleep), S(usleep), S(nanosleep), S(getpagesize), S(getdtablesize), S(getlogin), S(getpwuid), S(getpwnam),
    S(uname), S(gethostname), S(sysconf), S(sysctl), S(sysctlbyname), S(getrlimit), S(setrlimit), S(getrusage),
    S(tcgetattr), S(tcsetattr), S(ttyname), S(getopt), S(getopt_long), SA("_getopt_long_only", sh_getopt_long),
    SA("_optarg", &g_optarg), SA("_optind", &g_optind), SA("_opterr", &g_opterr), SA("_optopt", &g_optopt),
    SA("_optreset", &g_optreset), S(backtrace), S(backtrace_symbols),
    /* memory */
    S(malloc), S(calloc), S(realloc), S(reallocf), S(free), S(posix_memalign), S(aligned_alloc), S(valloc),
    S(malloc_size), S(malloc_good_size), S(mmap), S(munmap), S(mprotect), S(madvise),
    SA("__Znwm", sh_new), SA("__Znam", sh_new), SA("__ZdlPv", sh_delete), SA("__ZdaPv", sh_delete),
    SA("__ZdlPvm", sh_delete_sized), SA("__ZdaPvm", sh_delete_sized),
    SA("__ZnwmRKSt9nothrow_t", sh_new_nothrow), SA("__ZnamRKSt9nothrow_t", sh_new_nothrow),
    SA("___cxa_guard_acquire", sh_cxa_guard_acquire), SA("___cxa_guard_release", sh_cxa_guard_release),
    SA("___cxa_guard_abort", sh_cxa_guard_abort), SA("___cxa_pure_virtual", sh_cxa_pure_virtual),
    SA("___cxa_throw", sh_cxx_throw), SA("___cxa_allocate_exception", sh_cxx_throw), SA("__ZSt9terminatev", sh_abort),
    SA("___gxx_personality_v0", sh_noop), SA("__tlv_atexit", sh_noop), SA("dyld_stub_binder", sh_noop),
    SA("__tlv_bootstrap", cider_tlv_thunk),
    /* strings */
    S(strlen), S(strnlen), S(strcmp), S(strncmp), S(strcpy), S(strncpy), S(strcat), S(strncat), S(strchr), S(strrchr),
    S(strstr), S(strspn), S(strcspn), S(strpbrk), S(strtok), S(strtok_r), S(strcoll), S(strxfrm), S(strdup),
    S(strndup), S(strcasecmp), S(strncasecmp), S(strlcpy), S(strlcat), S(strsep), S(strcasestr), S(memmem),
    S(stpcpy), S(stpncpy), S(memcpy), S(memmove), S(memset), S(memcmp), S(memchr), S(bzero), SA("___bzero", sh_bzero),
    S(bcopy), S(bcmp), S(index), S(rindex), S(ffs), S(memset_pattern4), S(memset_pattern8), S(memset_pattern16),
    S(strerror), S(strerror_r), S(strsignal), S(atoi), S(atol), SA("_atoll", sh_atol), S(atof), S(strtol), S(strtoul),
    S(strtoll), S(strtoull), SA("_strtoq", sh_strtoll), SA("_strtouq", sh_strtoull), SA("_strtoimax", sh_strtoll),
    SA("_strtoumax", sh_strtoull), S(strtod), S(strtof), S(strtold), S(abs), S(labs), S(llabs),
    SA("___memcpy_chk", sh_memcpy_chk), SA("___memmove_chk", sh_memmove_chk), SA("___memset_chk", sh_memset_chk),
    SA("___strcpy_chk", sh_strcpy_chk), SA("___strcat_chk", sh_strcat_chk), SA("___strncpy_chk", sh_strncpy_chk),
    SA("___strncat_chk", sh_strncat_chk), SA("___stpcpy_chk", sh_stpcpy_chk), SA("___strlcpy_chk", sh_strlcpy_chk),
    SA("___strlcat_chk", sh_strlcat_chk),
    /* ctype */
    S(toupper), S(tolower), S(isalpha), S(isdigit), S(isalnum), S(isspace), S(isupper), S(islower), S(isxdigit),
    S(isprint), S(ispunct), S(iscntrl), S(isgraph), S(isascii), S(isblank), S(digittoint),
    SA("___maskrune", sh_maskrune), SA("___toupper", sh_mac_toupper), SA("___tolower", sh_mac_tolower),
    SA("__DefaultRuneLocale", &g_rune), SA("_setlocale", sh_setlocale), SA("_localeconv", sh_localeconv),
    /* fs / io */
    SA("_open", th_open), SA("_openat", th_openat), SA("_fcntl", th_fcntl), SA("_ioctl", th_ioctl),
    S(read), S(write), S(pread), S(pwrite), S(close), S(lseek), S(dup), S(dup2), S(pipe), S(isatty), S(unlink),
    S(rmdir), S(mkdir), S(chdir), S(access), S(chmod), S(fchmod), S(chown), SA("_lchown", sh_chown), S(umask),
    S(fsync), S(ftruncate), S(truncate), S(link), S(symlink), S(readlink), S(getcwd), S(realpath),
    S(stat), SA("_lstat", sh_stat), S(fstat), S(opendir), S(readdir), S(closedir), S(rewinddir), S(dirfd),
    S(basename), S(dirname),
    /* sorting / random */
    S(qsort), S(qsort_r), S(heapsort), SA("_mergesort", sh_heapsort), S(bsearch), S(rand), S(srand), S(random),
    S(srandom), S(arc4random), S(arc4random_uniform), S(arc4random_buf), S(getentropy), S(drand48), S(srand48),
    S(lrand48),
    /* time */
    S(time), S(localtime), S(localtime_r), S(gmtime), S(gmtime_r), S(mktime), S(timegm), S(strftime), S(asctime),
    S(ctime), S(difftime), S(tzset), S(clock), S(gettimeofday), S(clock_gettime), S(clock_getres),
    S(clock_gettime_nsec_np), S(mach_absolute_time), S(mach_timebase_info), SA("_mach_continuous_time", sh_mach_absolute_time),
    /* signals */
    S(signal), S(sigaction), S(raise), S(kill), SA("_sigemptyset", sh_sigzero), SA("_sigfillset", sh_sigfill),
    SA("_sigaddset", sh_sigadd), SA("_sigdelset", sh_sigdel), S(sigismember), S(sigprocmask),
    SA("_pthread_sigmask", sh_sigprocmask), S(alarm),
    SA("_setjmp", cider_setjmp), SA("__setjmp", cider_setjmp), SA("_sigsetjmp", cider_setjmp),
    SA("_longjmp", cider_longjmp), SA("__longjmp", cider_longjmp), SA("_siglongjmp", cider_longjmp),
    /* threads */
    S(pthread_create), S(pthread_join), S(pthread_detach), S(pthread_self), S(pthread_equal), S(pthread_exit),
    S(pthread_threadid_np), SA("_pthread_attr_init", sh_pthread_zero), SA("_pthread_attr_destroy", sh_pthread_zero),
    SA("_pthread_attr_setstacksize", sh_pthread_zero), SA("_pthread_attr_setdetachstate", sh_pthread_zero),
    SA("_pthread_attr_getstacksize", sh_pthread_attr_getstacksize), SA("_pthread_setname_np", sh_pthread_zero),
    SA("_pthread_mutexattr_init", sh_pthread_zero), SA("_pthread_mutexattr_settype", sh_pthread_zero),
    SA("_pthread_mutexattr_destroy", sh_pthread_zero), SA("_pthread_condattr_init", sh_pthread_zero),
    SA("_pthread_condattr_destroy", sh_pthread_zero),
    SA("_pthread_mutex_init", sh_mutex_init), SA("_pthread_mutex_lock", sh_mutex_lock),
    SA("_pthread_mutex_trylock", sh_mutex_trylock), SA("_pthread_mutex_unlock", sh_mutex_unlock),
    SA("_pthread_mutex_destroy", sh_mutex_destroy), SA("_pthread_cond_init", sh_cond_init),
    SA("_pthread_cond_wait", sh_cond_wait), SA("_pthread_cond_timedwait", sh_cond_timedwait),
    SA("_pthread_cond_signal", sh_cond_signal), SA("_pthread_cond_broadcast", sh_cond_broadcast),
    SA("_pthread_cond_destroy", sh_pthread_zero), S(pthread_once), SA("_pthread_key_create", sh_key_create),
    SA("_pthread_key_delete", sh_key_delete), SA("_pthread_getspecific", sh_getspecific),
    SA("_pthread_setspecific", sh_setspecific), SA("_pthread_rwlock_init", sh_rw_init),
    SA("_pthread_rwlock_rdlock", sh_rw_rdlock), SA("_pthread_rwlock_wrlock", sh_rw_wrlock),
    SA("_pthread_rwlock_unlock", sh_rw_unlock), SA("_pthread_rwlock_destroy", sh_pthread_zero),
    S(sched_yield), SA("_os_unfair_lock_lock", sh_unfair_lock), SA("_os_unfair_lock_unlock", sh_unfair_unlock),
    SA("_os_unfair_lock_trylock", sh_unfair_trylock), SA("_os_unfair_lock_assert_owner", sh_noop),
    /* math */
    S(sin), S(cos), S(tan), S(asin), S(acos), S(atan), S(sinh), S(cosh), S(tanh), S(asinh), S(acosh), S(atanh),
    S(exp), S(exp2), S(expm1), S(log), S(log2), S(log10), S(log1p), S(sqrt), S(cbrt), S(fabs), S(floor), S(ceil),
    S(round), S(trunc), S(rint), S(nearbyint), S(erf), S(erfc), S(tgamma), S(lgamma), S(logb), S(pow), S(atan2),
    S(fmod), S(hypot), S(fmin), S(fmax), S(copysign), S(remainder), S(nextafter), S(fdim), S(sinf), S(cosf),
    S(tanf), S(asinf), S(acosf), S(atanf), S(expf), S(exp2f), S(logf), S(log2f), S(log10f), S(sqrtf), S(cbrtf),
    S(fabsf), S(floorf), S(ceilf), S(roundf), S(truncf), S(sinhf), S(coshf), S(tanhf), S(rintf), S(powf), S(atan2f),
    S(fmodf), S(hypotf), S(fminf), S(fmaxf), S(copysignf), S(ldexp), S(ldexpf), S(frexp), S(modf), S(modff), S(fma),
    S(lround), SA("_llround", sh_lround), S(lrint), SA("_llrint", sh_lrint), S(lroundf),
    SA("___exp10", sh_exp10), SA("___sinpi", sh_sinpi), SA("___cospi", sh_cospi),
    SA("___sincos_stret", sh_sincos_stret), SA("___sincosf_stret", sh_sincosf_stret),
    SA("___isnand", sh_isnan), SA("___isinfd", sh_isinf), S(div), S(ldiv), SA("_lldiv", sh_ldiv),
};

static int shim_cmp(const void *a, const void *b) { return strcmp(((const shim *)a)->name, ((const shim *)b)->name); }
static int g_sorted;

void *shim_lookup(const char *name)
{
    if (!g_sorted) {
        qsort(g_shims, sizeof g_shims / sizeof *g_shims, sizeof(shim), shim_cmp);
        g_sorted = 1;
    }
    shim key = { name, NULL };
    shim *s = bsearch(&key, g_shims, sizeof g_shims / sizeof *g_shims, sizeof(shim), shim_cmp);
    if (s) return s->addr;
    /* "_stat$INODE64", "_fopen$DARWIN_EXTSN", "_fputs$UNIX2003", ... */
    const char *d = strchr(name, '$');
    if (d && d > name) {
        char buf[256];
        size_t n = (size_t)(d - name);
        if (n >= sizeof buf) return NULL;
        memcpy(buf, name, n);
        buf[n] = 0;
        return shim_lookup(buf);
    }
    return NULL;
}

static void invalid_param(const wchar_t *e, const wchar_t *f, const wchar_t *file, unsigned line, uintptr_t r)
{
    (void)e; (void)f; (void)file; (void)line; (void)r;
}

void libsystem_init(int argc, char **argv, const char *exe_path)
{
    InitializeCriticalSection(&g_exit_lock);
    _set_invalid_parameter_handler(invalid_param);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    g_tls_self = TlsAlloc();
    init_rune();
    _tzset();
    size_t tzl;
    _get_tzname(&tzl, g_tzname[0], sizeof g_tzname[0], 0);
    _get_tzname(&tzl, g_tzname[1], sizeof g_tzname[1], 1);

    g_argc = argc;
    g_argv = argv;
    g_exe_path = xstrdup(exe_path);
    slashify(g_exe_path);
    g_progname = path_basename(argv[0]);
    unsigned r1, r2;
    rand_s(&r1);
    rand_s(&r2);
    g_stack_chk_guard = ((uint64_t)r1 << 32 | r2) & ~0xFFULL;
    sh_srand(r1);

    _setmode(0, _O_BINARY);
    _setmode(1, _O_BINARY);
    _setmode(2, _O_BINARY);
    mac_stdin_s.real = stdin; mac_stdin_s._file = 0; mac_stdin_s.is_std = 1;
    mac_stdout_s.real = stdout; mac_stdout_s._file = 1; mac_stdout_s.is_std = 1;
    mac_stderr_s.real = stderr; mac_stderr_s._file = 2; mac_stderr_s.is_std = 1;
    if (_isatty(1)) setvbuf(stdout, NULL, _IONBF, 0);

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (GetConsoleMode(ho, &mode)) SetConsoleMode(ho, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    HANDLE he = GetStdHandle(STD_ERROR_HANDLE);
    if (GetConsoleMode(he, &mode)) SetConsoleMode(he, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    DWORD un = sizeof g_username;
    GetUserNameA(g_username, &un);
    const char *home = getenv("HOME");
    if (!home || !*home) home = getenv("USERPROFILE");
    snprintf(g_home, sizeof g_home, "%s", home ? home : "C:/");
    slashify(g_home);
    _putenv_s("HOME", g_home);
    if (!getenv("USER")) _putenv_s("USER", g_username);
    if (!getenv("SHELL")) _putenv_s("SHELL", "/bin/zsh");
    if (!getenv("LANG")) _putenv_s("LANG", "en_US.UTF-8");
    if (!getenv("TMPDIR")) {
        char t[MAX_PATH];
        GetTempPathA(MAX_PATH, t);
        slashify(t);
        _putenv_s("TMPDIR", t);
    }
    cider_environ = _environ;
}
