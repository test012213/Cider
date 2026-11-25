/* Small helpers: memory, UTF-8 <-> UTF-16, files, processes. */
#include "cider.h"
#include <shlobj.h>
#include <shellapi.h>

int g_verbose = 0;
#ifdef CIDER_GUI
int g_gui = 1;
#else
int g_gui = 0;
#endif

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fputs("cider: out of memory\n", stderr); ExitProcess(1); }
    return p;
}

void *xcalloc(size_t a, size_t b)
{
    void *p = calloc(a ? a : 1, b ? b : 1);
    if (!p) { fputs("cider: out of memory\n", stderr); ExitProcess(1); }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) { fputs("cider: out of memory\n", stderr); ExitProcess(1); }
    return p;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    return memcpy(xmalloc(n), s, n);
}

char *xasprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *s = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}

wchar_t *utf8_to_w(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = xmalloc((size_t)n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

char *w_to_utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = xmalloc((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

uint8_t *read_file(const char *path, size_t *len)
{
    wchar_t *w = utf8_to_w(path);
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    uint8_t *buf = malloc((size_t)sz.QuadPart + 1);
    if (!buf) { CloseHandle(h); return NULL; }
    size_t done = 0;
    while (done < (size_t)sz.QuadPart) {
        DWORD chunk = (DWORD)min((size_t)sz.QuadPart - done, (size_t)1 << 30), got = 0;
        if (!ReadFile(h, buf + done, chunk, &got, NULL) || !got) break;
        done += got;
    }
    CloseHandle(h);
    buf[done] = 0;
    if (len) *len = done;
    return buf;
}

int write_file(const char *path, const void *data, size_t len)
{
    wchar_t *w = utf8_to_w(path);
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return -1;
    const uint8_t *p = data;
    while (len) {
        DWORD chunk = (DWORD)min(len, (size_t)1 << 30), put = 0;
        if (!WriteFile(h, p, chunk, &put, NULL) || !put) { CloseHandle(h); return -1; }
        p += put;
        len -= put;
    }
    CloseHandle(h);
    return 0;
}

static DWORD attrs(const char *p)
{
    wchar_t *w = utf8_to_w(p);
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a;
}

int path_exists(const char *p) { return attrs(p) != INVALID_FILE_ATTRIBUTES; }
int is_dir(const char *p) { DWORD a = attrs(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
int is_file(const char *p) { DWORD a = attrs(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

char *path_join(const char *a, const char *b)
{
    size_t la = strlen(a);
    while (*b == '/' || *b == '\\') b++;
    int sep = la && a[la - 1] != '\\' && a[la - 1] != '/';
    char *r = xasprintf("%s%s%s", a, sep ? "\\" : "", b);
    for (char *c = r; *c; c++) if (*c == '/') *c = '\\';
    return r;
}

char *path_dirname(const char *p)
{
    char *d = xstrdup(p);
    size_t n = strlen(d);
    while (n > 1 && (d[n - 1] == '\\' || d[n - 1] == '/')) d[--n] = 0;
    char *s = NULL;
    for (char *c = d; *c; c++) if (*c == '\\' || *c == '/') s = c;
    if (s) *s = 0; else strcpy(d, ".");
    return d;
}

const char *path_basename(const char *p)
{
    const char *b = p;
    for (const char *c = p; *c; c++)
        if ((*c == '\\' || *c == '/') && c[1]) b = c + 1;
    return b;
}

int ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    while (ls && (s[ls - 1] == '\\' || s[ls - 1] == '/')) ls--;
    return ls >= lx && _strnicmp(s + ls - lx, suffix, lx) == 0;
}

int mkdirs(const char *path)
{
    char *p = xstrdup(path);
    for (char *c = p; *c; c++) if (*c == '/') *c = '\\';
    for (char *c = p + 1; *c; c++) {
        if (*c != '\\' || c[-1] == ':') continue;
        *c = 0;
        wchar_t *w = utf8_to_w(p);
        CreateDirectoryW(w, NULL);
        free(w);
        *c = '\\';
    }
    wchar_t *w = utf8_to_w(p);
    BOOL ok = CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    free(w);
    free(p);
    return ok ? 0 : -1;
}

typedef struct { char *src, *dst; int hardlink; int fail; } copyctx;

static void walk_rec(const char *dir, int depth, int maxdepth, dir_cb cb, void *ctx)
{
    char *pat = path_join(dir, "*");
    wchar_t *w = utf8_to_w(pat);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    free(w);
    free(pat);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        char *name = w_to_utf8(fd.cFileName);
        char *full = path_join(dir, name);
        int isd = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        int isreparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        cb(full, isd, ctx);
        if (isd && !isreparse && (maxdepth < 0 || depth < maxdepth))
            walk_rec(full, depth + 1, maxdepth, cb, ctx);
        free(full);
        free(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void walk_dir(const char *dir, int maxdepth, dir_cb cb, void *ctx)
{
    walk_rec(dir, 1, maxdepth, cb, ctx);
}

int remove_tree(const char *path)
{
    /* SHFileOperation needs a double-NUL terminated list. */
    wchar_t *w = utf8_to_w(path);
    size_t n = wcslen(w);
    wchar_t *buf = xcalloc(n + 2, sizeof(wchar_t));
    wcscpy(buf, w);
    SHFILEOPSTRUCTW op = {0};
    op.wFunc = FO_DELETE;
    op.pFrom = buf;
    op.fFlags = FOF_NO_UI;
    int r = SHFileOperationW(&op);
    free(buf);
    free(w);
    return r;
}

static void copy_cb(const char *path, int isdir, void *vctx)
{
    copyctx *c = vctx;
    char *rel = (char *)path + strlen(c->src);
    char *dst = path_join(c->dst, rel);
    if (isdir) {
        mkdirs(dst);
    } else {
        wchar_t *ws = utf8_to_w(path), *wd = utf8_to_w(dst);
        DeleteFileW(wd);
        if (!(c->hardlink && CreateHardLinkW(wd, ws, NULL)) && !CopyFileW(ws, wd, FALSE))
            c->fail++;
        free(ws);
        free(wd);
    }
    free(dst);
}

int copy_tree(const char *src, const char *dst, int hardlink)
{
    copyctx c = { (char *)src, (char *)dst, hardlink, 0 };
    if (is_file(src)) {
        wchar_t *ws = utf8_to_w(src), *wd = utf8_to_w(dst);
        DeleteFileW(wd);
        int ok = (hardlink && CreateHardLinkW(wd, ws, NULL)) || CopyFileW(ws, wd, FALSE);
        free(ws);
        free(wd);
        return ok ? 0 : -1;
    }
    mkdirs(dst);
    walk_dir(src, -1, copy_cb, &c);
    return c.fail ? -1 : 0;
}

char *cider_data_dir(const char *sub)
{
    wchar_t *base = NULL;
    char *root;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &base))) {
        char *b = w_to_utf8(base);
        root = path_join(b, "Cider");
        free(b);
        CoTaskMemFree(base);
    } else {
        root = xstrdup("C:\\Cider");
    }
    char *r = sub ? path_join(root, sub) : xstrdup(root);
    free(root);
    mkdirs(r);
    return r;
}

char *self_dir(void)
{
    wchar_t buf[MAX_PATH * 4];
    GetModuleFileNameW(NULL, buf, MAX_PATH * 4);
    char *s = w_to_utf8(buf);
    char *d = path_dirname(s);
    free(s);
    return d;
}

uint64_t fnv1a(const void *data, size_t n, uint64_t h)
{
    const uint8_t *p = data;
    if (!h) h = 0xcbf29ce484222325ULL;
    while (n--) { h ^= *p++; h *= 0x100000001b3ULL; }
    return h;
}

int run_process(const char *exe, const char *cmdline, const char *cwd, int wait, DWORD *exitcode)
{
    wchar_t *wexe = exe ? utf8_to_w(exe) : NULL;
    wchar_t *wcmd = utf8_to_w(cmdline);
    wchar_t *wcwd = cwd ? utf8_to_w(cwd) : NULL;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(wexe, wcmd, NULL, NULL, TRUE, 0, NULL, wcwd, &si, &pi);
    free(wexe);
    free(wcmd);
    free(wcwd);
    if (!ok) return -1;
    if (wait) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        if (exitcode) GetExitCodeProcess(pi.hProcess, exitcode);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

char *find_on_path(const char *exe)
{
    wchar_t *w = utf8_to_w(exe);
    wchar_t buf[MAX_PATH * 2];
    DWORD n = SearchPathW(NULL, w, NULL, MAX_PATH * 2, buf, NULL);
    free(w);
    return n ? w_to_utf8(buf) : NULL;
}

void vlog(const char *fmt, ...)
{
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[cider] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void cider_die(const char *title, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[4096];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ui_progress_end();
    if (g_gui) ui_message(1, title, buf, NULL);
    else fprintf(stderr, "cider: %s\n%s\n", title, buf);
    ExitProcess(1);
}
