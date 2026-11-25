/*
 * Cider - run macOS apps on Windows.
 * Shared declarations for the runtime (cider.exe / ciderw.exe).
 */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define CIDER_VERSION "1.0.0"

/* Calling convention used by macOS x86_64 code. */
#define SV __attribute__((sysv_abi))

/* ---- util.c ---- */
extern int g_verbose;
extern int g_gui;            /* built as ciderw.exe */

void *xmalloc(size_t n);
void *xcalloc(size_t a, size_t b);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xasprintf(const char *fmt, ...);

wchar_t *utf8_to_w(const char *s);
char *w_to_utf8(const wchar_t *w);

uint8_t *read_file(const char *path, size_t *len);
int write_file(const char *path, const void *data, size_t len);
int path_exists(const char *p);
int is_dir(const char *p);
int is_file(const char *p);
char *path_join(const char *a, const char *b);
char *path_dirname(const char *p);
const char *path_basename(const char *p);
int ends_with_ci(const char *s, const char *suffix);
int mkdirs(const char *path);
int remove_tree(const char *path);
int copy_tree(const char *src, const char *dst, int hardlink);
char *cider_data_dir(const char *sub);   /* %LOCALAPPDATA%\Cider\<sub> */
char *self_dir(void);
uint64_t fnv1a(const void *data, size_t n, uint64_t h);
int run_process(const char *exe, const char *cmdline, const char *cwd, int wait, DWORD *exitcode);
char *find_on_path(const char *exe);

typedef void (*dir_cb)(const char *path, int isdir, void *ctx);
void walk_dir(const char *dir, int maxdepth, dir_cb cb, void *ctx);

void vlog(const char *fmt, ...);                       /* verbose log */
void cider_die(const char *title, const char *fmt, ...) __attribute__((noreturn));

/* ---- plist.c ---- */
typedef enum { PL_DICT, PL_ARRAY, PL_STRING, PL_INT, PL_REAL, PL_BOOL, PL_DATA, PL_DATE } pltype;
typedef struct plnode {
    pltype t;
    char *str;           /* string value, or raw bytes for PL_DATA */
    size_t len;          /* byte length for PL_DATA */
    int64_t i;           /* PL_INT / PL_BOOL */
    double r;            /* PL_REAL / PL_DATE */
    int n;               /* number of children */
    char **keys;         /* PL_DICT keys */
    struct plnode **items;
} plnode;

plnode *plist_parse(const uint8_t *buf, size_t len);
plnode *plist_load(const char *path);
plnode *pl_get(plnode *dict, const char *key);
const char *pl_str(plnode *dict, const char *key);
void plist_free(plnode *n);

/* ---- macho.c ---- */
#define MAX_PROBE_DYLIBS 128
typedef struct {
    int is_macho;
    int has_x86_64, has_arm64, has_other;
    int filetype;
    int ndylibs;
    char *dylibs[MAX_PROBE_DYLIBS];
} macho_probe_t;

int macho_probe(const char *path, macho_probe_t *out);
void macho_probe_free(macho_probe_t *p);
int macho_is_macho_file(const char *path);
int macho_run(const char *path, int argc, char **argv);  /* never returns on success */

/* ---- libsystem.c ---- */
void *shim_lookup(const char *name);
void libsystem_init(int argc, char **argv, const char *exe_path);
SV void shim_exit(int code) __attribute__((noreturn));
void libsystem_register_image(void *base, const char *path);

/* ---- dmg.c ---- */
int dmg_extract(const char *dmg, const char *outdir, char *err, size_t errlen);
int inflate_zlib(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen, size_t *produced);

/* ---- backends.c ---- */
int run_app_bundle(const char *bundle, int argc, char **argv, int force);
int describe_app_bundle(const char *bundle);

/* ---- ui.c ---- */
void ui_progress_begin(const char *title, const char *text);
void ui_progress_text(const char *text);
void ui_progress_end(void);
void ui_message(int error, const char *title, const char *text, const char *details);
int ui_choose(const char *title, const char *text, char **items, int n);
int ui_launcher(void);
void ui_ensure_console(const char *title);
void ui_console_pause(void);
