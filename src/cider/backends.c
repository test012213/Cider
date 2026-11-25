/*
 * App bundle (.app) handling: figure out what kind of app it is and pick
 * the fastest way to run it on Windows.
 *
 *   native   x86_64 Mach-O code, loaded by Cider's own loader (macho.c)
 *   electron the app's JavaScript on the matching Windows build of Electron
 *   java     the app's jars on a Windows Java runtime
 *   script   shell/Python/etc. apps (incl. Platypus wrappers)
 */
#include "cider.h"
#include <urlmon.h>

typedef enum { KIND_UNKNOWN, KIND_NATIVE, KIND_ELECTRON, KIND_JAVA, KIND_SCRIPT } app_kind;

typedef struct {
    char *bundle, *contents, *name, *id, *version, *exe;
    plnode *info;
    app_kind kind;
    char *electron_version;
    char *script, *interp;
} app_t;

static const char *GUI_FRAMEWORKS[] = { "AppKit", "Cocoa", "UIKit", "SwiftUI", "Carbon", "QuartzCore", "Metal",
                                        "MetalKit", "WebKit", "SpriteKit", "SceneKit", "GLKit", "OpenGL", "AVKit", NULL };

static char *framework_name(const char *dylib)
{
    const char *f = strstr(dylib, ".framework/");
    if (!f) return NULL;
    const char *s = f;
    while (s > dylib && s[-1] != '/') s--;
    char *r = xmalloc((size_t)(f - s) + 1);
    memcpy(r, s, (size_t)(f - s));
    r[f - s] = 0;
    return r;
}

static char *first_line(const char *path)
{
    size_t n;
    uint8_t *b = read_file(path, &n);
    if (!b) return NULL;
    if (n < 2 || b[0] != '#' || b[1] != '!') { free(b); return NULL; }
    char *e = (char *)b;
    while (*e && *e != '\n' && *e != '\r') e++;
    *e = 0;
    char *r = xstrdup((char *)b + 2);
    free(b);
    return r;
}

static char *electron_framework_version(const char *contents)
{
    static const char *rel[] = {
        "Frameworks\\Electron Framework.framework\\Resources\\Info.plist",
        "Frameworks\\Electron Framework.framework\\Versions\\A\\Resources\\Info.plist",
        "Frameworks\\Electron Framework.framework\\Versions\\Current\\Resources\\Info.plist",
    };
    for (int i = 0; i < 3; i++) {
        char *p = path_join(contents, rel[i]);
        plnode *pl = plist_load(p);
        free(p);
        const char *v = pl_str(pl, "CFBundleVersion");
        if (!v) v = pl_str(pl, "CFBundleShortVersionString");
        char *r = v ? xstrdup(v) : NULL;
        plist_free(pl);
        if (r) return r;
    }
    return NULL;
}

static int load_app(const char *bundle, app_t *a)
{
    memset(a, 0, sizeof *a);
    a->bundle = xstrdup(bundle);
    size_t l = strlen(a->bundle);
    while (l && (a->bundle[l - 1] == '\\' || a->bundle[l - 1] == '/')) a->bundle[--l] = 0;
    a->contents = path_join(a->bundle, "Contents");
    char *ip = path_join(a->contents, "Info.plist");
    a->info = plist_load(ip);
    free(ip);
    const char *bn = path_basename(a->bundle);
    char *def = xstrdup(bn);
    if (ends_with_ci(def, ".app")) def[strlen(def) - 4] = 0;
    const char *nm = pl_str(a->info, "CFBundleDisplayName");
    if (!nm) nm = pl_str(a->info, "CFBundleName");
    a->name = xstrdup(nm && *nm ? nm : def);
    const char *id = pl_str(a->info, "CFBundleIdentifier");
    a->id = xstrdup(id ? id : def);
    const char *ver = pl_str(a->info, "CFBundleShortVersionString");
    a->version = xstrdup(ver ? ver : "");
    const char *exe = pl_str(a->info, "CFBundleExecutable");
    char *macos = path_join(a->contents, "MacOS");
    a->exe = path_join(macos, exe ? exe : def);
    free(macos);
    free(def);

    char *fw = path_join(a->contents, "Frameworks\\Electron Framework.framework");
    char *jdir = path_join(a->contents, "Java");
    char *jdir2 = path_join(a->contents, "Resources\\Java");
    char *jpk = path_join(a->contents, "app");
    char *platypus = path_join(a->contents, "Resources\\AppSettings.plist");
    char *shebang = is_file(a->exe) ? first_line(a->exe) : NULL;

    if (is_dir(fw)) {
        a->kind = KIND_ELECTRON;
        a->electron_version = electron_framework_version(a->contents);
    } else if (pl_get(a->info, "JVMMainClassName") || pl_get(a->info, "Java") || pl_get(a->info, "JavaX") ||
               is_dir(jdir) || is_dir(jdir2) || (is_dir(jpk) && !is_file(a->exe))) {
        a->kind = KIND_JAVA;
    } else if (shebang) {
        a->kind = KIND_SCRIPT;
        a->script = xstrdup(a->exe);
        a->interp = shebang;
        shebang = NULL;
    } else if (is_file(platypus)) {
        plnode *ps = plist_load(platypus);
        const char *interp = pl_str(ps, "InterpreterPath");
        if (!interp) interp = pl_str(ps, "Interpreter");
        char *scr = path_join(a->contents, "Resources\\script");
        if (is_file(scr)) {
            a->kind = KIND_SCRIPT;
            a->script = scr;
            a->interp = xstrdup(interp ? interp : "/bin/sh");
        } else {
            free(scr);
        }
        plist_free(ps);
    }
    if (a->kind == KIND_UNKNOWN && is_file(a->exe) && macho_is_macho_file(a->exe)) a->kind = KIND_NATIVE;
    free(shebang);
    free(fw);
    free(jdir);
    free(jdir2);
    free(jpk);
    free(platypus);
    return a->info || is_file(a->exe) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Native                                                             */

typedef struct { int gui, objc, swift; char gui_list[512], other_list[512]; } native_report;

static void report_native(const macho_probe_t *p, native_report *r)
{
    memset(r, 0, sizeof *r);
    for (int i = 0; i < p->ndylibs; i++) {
        const char *d = p->dylibs[i];
        if (strstr(d, "libobjc")) r->objc = 1;
        if (strstr(d, "libswift")) r->swift = 1;
        char *fw = framework_name(d);
        if (!fw) continue;
        int is_gui = 0;
        for (int k = 0; GUI_FRAMEWORKS[k]; k++) if (!strcmp(fw, GUI_FRAMEWORKS[k])) is_gui = 1;
        if (strncmp(d, "/System/", 8)) { free(fw); continue; }   /* bundled framework: loaded by Cider */
        char *list = is_gui ? r->gui_list : r->other_list;
        if (is_gui) r->gui = 1;
        if (strlen(list) + strlen(fw) + 3 < 512) {
            if (*list) strcat(list, ", ");
            strcat(list, fw);
        }
        free(fw);
    }
}

static int run_native(app_t *a, const char *exe, int argc, char **argv, int force)
{
    macho_probe_t p;
    if (macho_probe(exe, &p) || !p.is_macho) cider_die("Not a macOS program", "%s is not a macOS executable.", exe);
    if (!p.has_x86_64) {
        macho_probe_free(&p);
        cider_die("This app is Apple Silicon only",
                  "%s only contains Apple Silicon (arm64) code.\n\n"
                  "Cider runs Intel (x86_64) macOS code directly on your PC's processor. "
                  "Look for a \"Universal\" or \"Intel\" download of this app.",
                  a ? a->name : path_basename(exe));
    }
    native_report r;
    report_native(&p, &r);
    macho_probe_free(&p);
    if (r.gui && !force) {
        cider_die("This app needs the macOS desktop (Cocoa)",
                  "%s draws its windows with Apple's Cocoa frameworks (%s).\n\n"
                  "Cider can run command-line macOS programs natively, and Electron and Java apps through "
                  "their Windows runtimes, but it does not implement Cocoa yet.\n\n"
                  "Run with --force to try anyway.",
                  a ? a->name : path_basename(exe), r.gui_list);
    }
    if ((r.objc || r.swift || r.other_list[0]) && !force)
        vlog("warning: uses %s%s%s - some features may be missing",
             r.other_list[0] ? r.other_list : "", r.objc ? " objc" : "", r.swift ? " swift" : "");

    ui_progress_end();
    if (g_gui) ui_ensure_console(a ? a->name : path_basename(exe));

    char **nargv = xcalloc((size_t)argc + 2, sizeof(char *));
    char *mac_exe = xstrdup(exe);
    for (char *c = mac_exe; *c; c++) if (*c == '\\') *c = '/';
    nargv[0] = mac_exe;
    for (int i = 0; i < argc; i++) nargv[i + 1] = argv[i];
    if (a) SetCurrentDirectoryW(utf8_to_w(path_dirname(exe)));
    macho_run(exe, argc + 1, nargv);
    cider_die("Could not start the app", "Cider could not load %s. Run cider.exe -v for details.", exe);
}

/* ------------------------------------------------------------------ */
/* Electron                                                           */

static int download(const char *url, const char *dst)
{
    wchar_t *wu = utf8_to_w(url), *wd = utf8_to_w(dst);
    HRESULT hr = URLDownloadToFileW(NULL, wu, wd, 0, NULL);
    free(wu);
    free(wd);
    return SUCCEEDED(hr) ? 0 : -1;
}

static int extract_zip(const char *zip, const char *dst)
{
    mkdirs(dst);
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    char *s = w_to_utf8(sys);
    char *tar = path_join(s, "tar.exe");
    free(s);
    char *cmd = xasprintf("\"%s\" -xf \"%s\" -C \"%s\"", tar, zip, dst);
    DWORD code = 1;
    int r = run_process(tar, cmd, NULL, 1, &code);
    free(cmd);
    free(tar);
    return r == 0 && code == 0 ? 0 : -1;
}

static char *safe_name(const char *s)
{
    char *r = xstrdup(s);
    for (char *c = r; *c; c++)
        if (strchr("<>:\"/\\|?*", *c) || (unsigned char)*c < 32) *c = '_';
    return r;
}

typedef struct { int count; char first[260]; } native_mods;
static void find_node_cb(const char *path, int isdir, void *ctx)
{
    native_mods *m = ctx;
    if (!isdir && ends_with_ci(path, ".node")) {
        if (!m->count) snprintf(m->first, sizeof m->first, "%s", path_basename(path));
        m->count++;
    }
}

static int run_electron(app_t *a, int argc, char **argv)
{
    if (!a->electron_version)
        cider_die("Unknown Electron version", "Cider could not tell which Electron version %s was built with.", a->name);
    const char *v = a->electron_version;
    char *rtname = xasprintf("electron-v%s-win32-x64", v);
    char *runtimes = cider_data_dir("runtimes");
    char *rt = path_join(runtimes, rtname);
    char *rtexe = path_join(rt, "electron.exe");
    if (!is_file(rtexe)) {
        char *msg = xasprintf("Downloading Electron %s for Windows (one time)...", v);
        ui_progress_begin(a->name, msg);
        if (!g_gui) fprintf(stderr, "cider: %s\n", msg);
        free(msg);
        char *dl = cider_data_dir("downloads");
        char *zip = xasprintf("%s\\%s.zip", dl, rtname);
        char *url = xasprintf("https://github.com/electron/electron/releases/download/v%s/%s.zip", v, rtname);
        if (download(url, zip))
            cider_die("Download failed", "Cider could not download the Windows build of Electron %s from\n%s", v, url);
        ui_progress_text("Unpacking Electron...");
        char *tmp = xasprintf("%s.partial", rt);
        remove_tree(tmp);
        if (extract_zip(zip, tmp) || !MoveFileW(utf8_to_w(tmp), utf8_to_w(rt)))
            cider_die("Could not unpack Electron", "Unpacking %s failed.", zip);
        DeleteFileW(utf8_to_w(zip));
        free(tmp);
        free(url);
        free(zip);
        free(dl);
    }

    /* Per-app folder: hard links to the runtime + the app's own resources. */
    ui_progress_begin(a->name, "Preparing app...");
    char *apps = cider_data_dir("apps");
    char *sid = safe_name(a->id);
    char *appdir = path_join(apps, sid);
    char *stamp_path = path_join(appdir, ".cider-stamp");
    char *srcres = path_join(a->contents, "Resources");
    char *stamp = xasprintf("%s|%s|%s", a->bundle, v, a->version);
    size_t sl;
    char *old = (char *)read_file(stamp_path, &sl);
    char *exe_name = xasprintf("%s.exe", a->name);
    char *safe_exe = safe_name(exe_name);
    char *appexe = path_join(appdir, safe_exe);
    if (!old || strcmp(old, stamp) || !is_file(appexe)) {
        remove_tree(appdir);
        if (copy_tree(rt, appdir, 1)) cider_die("Could not prepare the app", "Copying the Electron runtime failed.");
        char *def = path_join(appdir, "resources\\default_app.asar");
        DeleteFileW(utf8_to_w(def));
        free(def);
        char *e1 = path_join(appdir, "electron.exe");
        MoveFileW(utf8_to_w(e1), utf8_to_w(appexe));
        free(e1);
        char *dstres = path_join(appdir, "resources");
        if (copy_tree(srcres, dstres, 1)) vlog("some resources could not be copied");
        free(dstres);
        write_file(stamp_path, stamp, strlen(stamp));
    }
    free(old);

    native_mods nm = {0};
    char *unpacked = path_join(srcres, "app.asar.unpacked");
    if (is_dir(unpacked)) walk_dir(unpacked, -1, find_node_cb, &nm);
    free(unpacked);
    if (nm.count)
        vlog("note: %d native Node module(s) (e.g. %s) are built for macOS and won't load; related features may not work",
             nm.count, nm.first);

    char *cmd = xasprintf("\"%s\"", appexe);
    for (int i = 0; i < argc; i++) {
        char *n = xasprintf("%s \"%s\"", cmd, argv[i]);
        free(cmd);
        cmd = n;
    }
    ui_progress_end();
    vlog("launching %s", cmd);
    if (run_process(appexe, cmd, appdir, 0, NULL))
        cider_die("Could not start the app", "Starting %s failed.", appexe);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Java                                                               */

static char *find_java(int gui)
{
    const char *exe = gui ? "javaw.exe" : "java.exe";
    char jh[1024];
    if (GetEnvironmentVariableA("JAVA_HOME", jh, sizeof jh)) {
        char *p = xasprintf("%s\\bin\\%s", jh, exe);
        if (is_file(p)) return p;
        free(p);
    }
    return find_on_path(exe);
}

typedef struct { char *cp; } cpctx;
static void jar_cb(const char *path, int isdir, void *ctx)
{
    cpctx *c = ctx;
    if (isdir || !ends_with_ci(path, ".jar")) return;
    char *n = c->cp ? xasprintf("%s;%s", c->cp, path) : xstrdup(path);
    free(c->cp);
    c->cp = n;
}

static char *expand_java_path(app_t *a, const char *s)
{
    char *javaroot = path_join(a->contents, "Resources\\Java");
    char *appdir = path_join(a->contents, "app");
    char *out = xstrdup("");
    while (*s) {
        const char *rep = NULL;
        size_t skip = 0;
        if (!strncmp(s, "$JAVAROOT", 9)) { rep = javaroot; skip = 9; }
        else if (!strncmp(s, "$APP_PACKAGE", 12)) { rep = a->bundle; skip = 12; }
        else if (!strncmp(s, "$APPDIR", 7)) { rep = appdir; skip = 7; }
        else if (!strncmp(s, "$ROOTDIR", 8)) { rep = a->contents; skip = 8; }
        char *n = rep ? xasprintf("%s%s", out, rep) : xasprintf("%s%c", out, *s == '/' ? '\\' : *s);
        free(out);
        out = n;
        s += rep ? skip : 1;
    }
    free(javaroot);
    free(appdir);
    return out;
}

static int run_java(app_t *a, int argc, char **argv)
{
    char *java = find_java(g_gui);
    if (!java)
        cider_die("Java is required", "%s is a Java app. Install a Java runtime (for example from adoptium.net) and try again.", a->name);
    char *mainclass = NULL, *cp = NULL, *mainjar = NULL, *opts = xstrdup("");

    /* jpackage: Contents/app/<Name>.cfg */
    char *appd = path_join(a->contents, "app");
    if (is_dir(appd)) {
        char *pat = path_join(appd, "*.cfg");
        WIN32_FIND_DATAW fd;
        wchar_t *wp = utf8_to_w(pat);
        HANDLE h = FindFirstFileW(wp, &fd);
        free(wp);
        if (h != INVALID_HANDLE_VALUE) {
            char *fn = w_to_utf8(fd.cFileName);
            char *cfgp = path_join(appd, fn);
            char *cfg = (char *)read_file(cfgp, NULL);
            for (char *line = cfg ? strtok(cfg, "\r\n") : NULL; line; line = strtok(NULL, "\r\n")) {
                if (!strncmp(line, "app.mainclass=", 14)) mainclass = xstrdup(line + 14);
                else if (!strncmp(line, "app.mainjar=", 12)) mainjar = expand_java_path(a, line + 12);
                else if (!strncmp(line, "app.classpath=", 14) && line[14]) {
                    char *e = expand_java_path(a, line + 14);
                    char *n = cp ? xasprintf("%s;%s", cp, e) : xstrdup(e);
                    free(cp);
                    free(e);
                    cp = n;
                } else if (!strncmp(line, "java-options=", 13)) {
                    char *e = expand_java_path(a, line + 13);
                    char *n = xasprintf("%s \"%s\"", opts, e);
                    free(opts);
                    free(e);
                    opts = n;
                }
            }
            free(cfg);
            free(cfgp);
            free(fn);
            FindClose(h);
        }
        free(pat);
    }
    /* Oracle appbundler */
    if (!mainclass && pl_str(a->info, "JVMMainClassName")) {
        mainclass = xstrdup(pl_str(a->info, "JVMMainClassName"));
        plnode *o = pl_get(a->info, "JVMOptions");
        for (int i = 0; o && i < o->n; i++)
            if (o->items[i]->t == PL_STRING && strncmp(o->items[i]->str, "-Xdock", 6)) {
                char *e = expand_java_path(a, o->items[i]->str);
                char *n = xasprintf("%s \"%s\"", opts, e);
                free(opts);
                free(e);
                opts = n;
            }
    }
    /* Apple's legacy Java dictionary */
    plnode *jd = pl_get(a->info, "Java");
    if (!jd) jd = pl_get(a->info, "JavaX");
    if (!mainclass && jd) {
        const char *mc = pl_str(jd, "MainClass");
        if (mc) mainclass = xstrdup(mc);
        plnode *c = pl_get(jd, "ClassPath");
        for (int i = 0; c && (c->t == PL_STRING ? i < 1 : i < c->n); i++) {
            plnode *item = c->t == PL_STRING ? c : c->items[i];
            if (item->t != PL_STRING) continue;
            char *e = expand_java_path(a, item->str);
            char *n = cp ? xasprintf("%s;%s", cp, e) : xstrdup(e);
            free(cp);
            free(e);
            cp = n;
        }
        const char *vmo = pl_str(jd, "VMOptions");
        if (vmo) { char *n = xasprintf("%s %s", opts, vmo); free(opts); opts = n; }
    }
    if (!cp) {
        cpctx c = {0};
        static const char *dirs[] = { "Java", "Resources\\Java", "app" };
        for (int i = 0; i < 3; i++) {
            char *d = path_join(a->contents, dirs[i]);
            if (is_dir(d)) walk_dir(d, 3, jar_cb, &c);
            free(d);
        }
        cp = c.cp;
    }
    if (!mainclass && !mainjar && cp && !strchr(cp, ';')) mainjar = xstrdup(cp);
    if (!mainclass && !mainjar)
        cider_die("Could not start Java app", "Cider could not find the main class of %s.", a->name);

    char *cmd = mainjar && !mainclass
        ? xasprintf("\"%s\"%s -jar \"%s\"", java, opts, mainjar)
        : xasprintf("\"%s\"%s -cp \"%s\" %s", java, opts, cp ? cp : ".", mainclass);
    for (int i = 0; i < argc; i++) {
        char *n = xasprintf("%s \"%s\"", cmd, argv[i]);
        free(cmd);
        cmd = n;
    }
    ui_progress_end();
    vlog("launching %s", cmd);
    char *cwd = path_join(a->contents, is_dir(appd) ? "app" : "Resources");
    if (!is_dir(cwd)) { free(cwd); cwd = xstrdup(a->contents); }
    DWORD code = 0;
    if (run_process(java, cmd, cwd, !g_gui, &code)) cider_die("Could not start Java", "Starting %s failed.", java);
    free(appd);
    return (int)code;
}

/* ------------------------------------------------------------------ */
/* Scripts                                                            */

static int run_script(app_t *a, int argc, char **argv)
{
    char *interp = a->interp;
    while (*interp == ' ') interp++;
    char *prog = NULL;
    const char *base = path_basename(interp);
    if (!strncmp(base, "env ", 4)) base += 4;
    char word[64];
    sscanf(base, "%63s", word);
    if (strstr(word, "python")) prog = find_on_path("python.exe");
    else if (!strcmp(word, "node")) prog = find_on_path("node.exe");
    else if (!strcmp(word, "perl")) prog = find_on_path("perl.exe");
    else if (!strcmp(word, "ruby")) prog = find_on_path("ruby.exe");
    else if (!strcmp(word, "sh") || !strcmp(word, "bash") || !strcmp(word, "zsh")) {
        static const char *bashes[] = { "C:\\Program Files\\Git\\bin\\bash.exe", "C:\\Program Files (x86)\\Git\\bin\\bash.exe" };
        for (int i = 0; i < 2 && !prog; i++) if (is_file(bashes[i])) prog = xstrdup(bashes[i]);
        if (!prog) prog = find_on_path("bash.exe");
    }
    if (!prog)
        cider_die("Interpreter missing", "%s is a script app that needs \"%s\". Install it for Windows and try again.", a->name, word);
    char *cmd = xasprintf("\"%s\" \"%s\"", prog, a->script);
    for (int i = 0; i < argc; i++) {
        char *n = xasprintf("%s \"%s\"", cmd, argv[i]);
        free(cmd);
        cmd = n;
    }
    ui_progress_end();
    if (g_gui) ui_ensure_console(a->name);
    char *cwd = path_join(a->contents, "Resources");
    DWORD code = 0;
    run_process(prog, cmd, cwd, 1, &code);
    if (g_gui) ui_console_pause();
    return (int)code;
}

/* ------------------------------------------------------------------ */

static const char *kind_name(app_kind k)
{
    switch (k) {
    case KIND_NATIVE: return "native macOS code (runs directly on the CPU)";
    case KIND_ELECTRON: return "Electron (runs on Electron for Windows)";
    case KIND_JAVA: return "Java (runs on a Windows JVM)";
    case KIND_SCRIPT: return "script";
    default: return "unknown";
    }
}

int run_app_bundle(const char *bundle, int argc, char **argv, int force)
{
    app_t a;
    if (load_app(bundle, &a)) cider_die("Not a macOS app", "%s doesn't look like a macOS app bundle.", bundle);
    vlog("%s (%s) - %s", a.name, a.id, kind_name(a.kind));
    switch (a.kind) {
    case KIND_ELECTRON: return run_electron(&a, argc, argv);
    case KIND_JAVA: return run_java(&a, argc, argv);
    case KIND_SCRIPT: return run_script(&a, argc, argv);
    case KIND_NATIVE: return run_native(&a, a.exe, argc, argv, force);
    default:
        cider_die("Unsupported app", "Cider doesn't know how to run %s: its executable is missing or isn't a macOS program.", a.name);
    }
}

int run_macho_file(const char *path, int argc, char **argv, int force)
{
    return run_native(NULL, path, argc, argv, force);
}

int describe_app_bundle(const char *bundle)
{
    app_t a;
    if (load_app(bundle, &a)) { printf("%s: not a macOS app bundle\n", bundle); return 1; }
    printf("Name:        %s\n", a.name);
    printf("Identifier:  %s\n", a.id);
    if (*a.version) printf("Version:     %s\n", a.version);
    printf("Executable:  %s\n", a.exe);
    printf("Type:        %s\n", kind_name(a.kind));
    if (a.kind == KIND_ELECTRON) printf("Electron:    %s\n", a.electron_version ? a.electron_version : "unknown");
    if (a.kind == KIND_NATIVE) {
        macho_probe_t p;
        macho_probe(a.exe, &p);
        printf("CPU:         %s%s%s\n", p.has_x86_64 ? "x86_64 " : "", p.has_arm64 ? "arm64 " : "", p.has_other ? "other" : "");
        native_report r;
        report_native(&p, &r);
        printf("Libraries:\n");
        for (int i = 0; i < p.ndylibs; i++) printf("  %s\n", p.dylibs[i]);
        printf("Verdict:     %s\n",
               !p.has_x86_64 ? "cannot run (Apple Silicon only)"
               : r.gui ? "needs Cocoa (not supported yet)"
               : r.objc || r.swift || r.other_list[0] ? "may run (uses some Apple frameworks)"
               : "should run");
        macho_probe_free(&p);
    } else {
        printf("Verdict:     should run\n");
    }
    return 0;
}
