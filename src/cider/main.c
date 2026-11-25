/*
 * Cider entry point.
 *
 *   cider  [options] <file.dmg | App.app | archive.zip | macho-binary> [app args...]
 *
 * cider.exe is the console build, ciderw.exe the windowed build used by the
 * Explorer context menu (errors become dialogs, progress is shown in a popup).
 */
#include "cider.h"
#include <shellapi.h>

int run_macho_file(const char *path, int argc, char **argv, int force);

static void usage(void)
{
    printf("Cider %s - run macOS apps on Windows\n\n"
           "Usage:\n"
           "  cider [options] <target> [arguments for the app...]\n\n"
           "Targets:\n"
           "  App.dmg        a macOS disk image (the app inside is found automatically)\n"
           "  App.app        an extracted macOS app bundle\n"
           "  App.zip        a zip containing a .app\n"
           "  program        a macOS command-line program (Mach-O)\n\n"
           "Options:\n"
           "  --info               describe the app and whether Cider can run it\n"
           "  --extract <folder>   unpack a .dmg into <folder> and exit\n"
           "  --app <name>         pick which app to run when a disk image has several\n"
           "  --force              try to run apps that need unsupported frameworks\n"
           "  --fresh              re-extract the disk image instead of using the cache\n"
           "  -v, --verbose        print what Cider is doing\n"
           "  --version            print the version\n",
           CIDER_VERSION);
}

static char *find_7z(void)
{
    static const wchar_t *keys[] = { L"SOFTWARE\\7-Zip", L"SOFTWARE\\WOW6432Node\\7-Zip" };
    for (int i = 0; i < 2; i++) {
        wchar_t buf[MAX_PATH];
        DWORD n = sizeof buf;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, keys[i], L"Path", RRF_RT_REG_SZ, NULL, buf, &n) == ERROR_SUCCESS ||
            RegGetValueW(HKEY_CURRENT_USER, keys[i], L"Path", RRF_RT_REG_SZ, NULL, buf, &n) == ERROR_SUCCESS) {
            char *d = w_to_utf8(buf);
            char *p = path_join(d, "7z.exe");
            free(d);
            if (is_file(p)) return p;
            free(p);
        }
    }
    static const char *fixed[] = { "C:\\Program Files\\7-Zip\\7z.exe", "C:\\Program Files (x86)\\7-Zip\\7z.exe" };
    for (int i = 0; i < 2; i++) if (is_file(fixed[i])) return xstrdup(fixed[i]);
    return find_on_path("7z.exe");
}

typedef struct { char **items; int n; } strlist;

static void add_str(strlist *l, const char *s)
{
    l->items = xrealloc(l->items, sizeof(char *) * (size_t)(l->n + 1));
    l->items[l->n++] = xstrdup(s);
}

static void inner_image_cb(const char *path, int isdir, void *ctx)
{
    if (isdir) return;
    if (ends_with_ci(path, ".hfs") || ends_with_ci(path, ".hfsx") || ends_with_ci(path, ".apfs") || ends_with_ci(path, ".img"))
        add_str(ctx, path);
}

static int run_7z(const char *archive, const char *out)
{
    char *sz = find_7z();
    if (!sz) return -1;
    char *cmd = xasprintf("\"%s\" x -y -bso0 -bsp0 -bse0 \"-o%s\" \"%s\"", sz, out, archive);
    wchar_t *wc = utf8_to_w(cmd), *wx = utf8_to_w(sz);
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    DWORD code = 2;
    if (CreateProcessW(wx, wc, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    free(wc);
    free(wx);
    free(cmd);
    free(sz);
    return code <= 1 ? 0 : -1;
}

static void app_cb(const char *path, int isdir, void *ctx)
{
    if (!isdir || !ends_with_ci(path, ".app")) return;
    /* skip helper apps nested inside other bundles */
    if (strstr(path, ".app\\")) return;
    add_str(ctx, path);
}

static int extract_image(const char *image, const char *out, char *err, size_t errlen)
{
    int r = -1;
    if (ends_with_ci(image, ".zip")) {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        char *s = w_to_utf8(sys);
        char *tar = path_join(s, "tar.exe");
        mkdirs(out);
        char *cmd = xasprintf("\"%s\" -xf \"%s\" -C \"%s\"", tar, image, out);
        DWORD code = 1;
        r = run_process(tar, cmd, NULL, 1, &code) == 0 && code == 0 ? 0 : -1;
        if (r) r = run_7z(image, out);
        if (r) snprintf(err, errlen, "could not unpack the archive");
        free(cmd);
        free(tar);
        free(s);
        return r;
    }
    r = dmg_extract(image, out, err, errlen);
    if (r == 0) return 0;
    vlog("built-in reader: %s - trying 7-Zip", err);
    remove_tree(out);
    mkdirs(out);
    if (run_7z(image, out) == 0) {
        /* 7-Zip may stop at the partition level (e.g. "4.apfs"); unpack one level further */
        strlist inner = {0};
        walk_dir(out, 2, inner_image_cb, &inner);
        for (int i = 0; i < inner.n; i++) {
            char *sub = xasprintf("%s.d", inner.items[i]);
            if (run_7z(inner.items[i], sub) == 0) DeleteFileW(utf8_to_w(inner.items[i]));
            free(sub);
        }
        return 0;
    }
    if (!find_7z())
        snprintf(err + strlen(err), errlen - strlen(err),
                 ".\n\nInstall 7-Zip (7-zip.org) and Cider will use it to open this kind of disk image");
    return -1;
}

static char *cache_dir_for(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA ad = {0};
    wchar_t *w = utf8_to_w(path);
    GetFileAttributesExW(w, GetFileExInfoStandard, &ad);
    free(w);
    uint64_t h = fnv1a(path, strlen(path), 0);
    h = fnv1a(&ad.nFileSizeLow, 4, h);
    h = fnv1a(&ad.nFileSizeHigh, 4, h);
    h = fnv1a(&ad.ftLastWriteTime, sizeof ad.ftLastWriteTime, h);
    char *name = xstrdup(path_basename(path));
    char *dot = strrchr(name, '.');
    if (dot) *dot = 0;
    char *vols = cider_data_dir("volumes");
    char *leaf = xasprintf("%s-%08x", name, (unsigned)(h ^ (h >> 32)));
    char *r = path_join(vols, leaf);
    free(leaf);
    free(vols);
    free(name);
    return r;
}

static char *open_image(const char *image, const char *want, int fresh)
{
    char *dir = cache_dir_for(image);
    char *marker = path_join(dir, ".cider-complete");
    if (fresh || !is_file(marker)) {
        char *title = xasprintf("Opening %s", path_basename(image));
        ui_progress_begin(title, "Unpacking disk image...");
        free(title);
        if (!g_gui) fprintf(stderr, "cider: unpacking %s...\n", path_basename(image));
        remove_tree(dir);
        char err[512];
        if (extract_image(image, dir, err, sizeof err)) {
            remove_tree(dir);
            cider_die("Could not open the disk image", "Cider couldn't read %s: %s.", path_basename(image), err);
        }
        write_file(marker, "ok", 2);
    }
    strlist apps = {0};
    walk_dir(dir, 4, app_cb, &apps);
    if (!apps.n) {
        ui_progress_end();
        cider_die("No app found", "%s doesn't contain a macOS app (.app). If it contains an installer package (.pkg), "
                  "Cider can't run installers yet.", path_basename(image));
    }
    int pick = 0;
    if (want) {
        pick = -1;
        for (int i = 0; i < apps.n; i++)
            if (strstr(path_basename(apps.items[i]), want)) { pick = i; break; }
        if (pick < 0) cider_die("App not found", "No app named \"%s\" in %s.", want, path_basename(image));
    } else if (apps.n > 1) {
        if (g_gui) {
            ui_progress_end();
            char **names = xcalloc((size_t)apps.n, sizeof(char *));
            for (int i = 0; i < apps.n; i++) names[i] = (char *)path_basename(apps.items[i]);
            pick = ui_choose("Which app do you want to open?", path_basename(image), names, apps.n);
            if (pick < 0) ExitProcess(0);
        } else {
            fprintf(stderr, "cider: %s contains several apps; running the first (use --app to choose):\n", path_basename(image));
            for (int i = 0; i < apps.n; i++) fprintf(stderr, "  %s\n", path_basename(apps.items[i]));
        }
    }
    vlog("app: %s", apps.items[pick]);
    return apps.items[pick];
}

static int cider_main(int argc, char **argv)
{
    int info = 0, force = 0, fresh = 0, extract_here = 0;
    const char *extract_to = NULL, *want = NULL, *target = NULL;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) g_verbose = 1;
        else if (!strcmp(a, "--info")) info = 1;
        else if (!strcmp(a, "--force")) force = 1;
        else if (!strcmp(a, "--fresh")) fresh = 1;
        else if (!strcmp(a, "--extract") && i + 1 < argc) extract_to = argv[++i];
        else if (!strcmp(a, "--extract-here")) extract_here = 1;
        else if (!strcmp(a, "--app") && i + 1 < argc) want = argv[++i];
        else if (!strcmp(a, "--version")) { printf("Cider %s\n", CIDER_VERSION); return 0; }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help") || !strcmp(a, "/?")) { usage(); return 0; }
        else if (!strcmp(a, "--")) { i++; break; }
        else if (a[0] == '-' && a[1] == '-') { fprintf(stderr, "cider: unknown option %s\n", a); return 2; }
        else break;
    }
    if (i < argc) target = argv[i++];
    int app_argc = argc - i;
    char **app_argv = argv + i;

    if (!target) {
        if (g_gui) return ui_launcher();
        usage();
        return 1;
    }

    wchar_t full[MAX_PATH * 4];
    wchar_t *wt = utf8_to_w(target);
    GetFullPathNameW(wt, MAX_PATH * 4, full, NULL);
    free(wt);
    char *path = w_to_utf8(full);
    size_t pl = strlen(path);
    while (pl > 3 && (path[pl - 1] == '\\' || path[pl - 1] == '/')) path[--pl] = 0;
    if (!path_exists(path)) cider_die("File not found", "%s does not exist.", path);

    if (extract_here) {
        /* "Extract with Cider" from Explorer: App.dmg -> App\ (or "App (2)\" if taken) */
        char *base = xstrdup(path);
        char *dot = strrchr(base, '.');
        if (dot && dot > strrchr(base, '\\')) *dot = 0;
        char *dest = xstrdup(base);
        for (int n = 2; path_exists(dest); n++) {
            free(dest);
            dest = xasprintf("%s (%d)", base, n);
        }
        extract_to = dest;
    }
    if (extract_to) {
        char err[512];
        char *title = xasprintf("Extracting %s", path_basename(path));
        ui_progress_begin(title, "Unpacking disk image...");
        free(title);
        if (extract_image(path, extract_to, err, sizeof err)) cider_die("Extraction failed", "%s", err);
        ui_progress_end();
        printf("Extracted to %s\n", extract_to);
        if (extract_here) {
            wchar_t *w = utf8_to_w(extract_to);
            ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
            free(w);
        }
        return 0;
    }

    char *bundle = NULL;
    if (is_dir(path)) {
        if (ends_with_ci(path, ".app")) bundle = path;
        else {
            /* a folder like Contents/ or something containing an .app */
            char *parent = path_dirname(path);
            if (ends_with_ci(parent, ".app")) bundle = parent;
            else {
                strlist apps = {0};
                walk_dir(path, 2, app_cb, &apps);
                if (!apps.n) cider_die("No app found", "%s doesn't contain a macOS app.", path);
                bundle = apps.items[0];
            }
        }
    } else if (ends_with_ci(path, ".dmg") || ends_with_ci(path, ".zip") || ends_with_ci(path, ".img") ||
               ends_with_ci(path, ".cdr") || ends_with_ci(path, ".iso")) {
        bundle = open_image(path, want, fresh);
    } else if (macho_is_macho_file(path)) {
        if (info) {
            macho_probe_t p;
            macho_probe(path, &p);
            printf("Mach-O: %s%s%s\n", p.has_x86_64 ? "x86_64 " : "", p.has_arm64 ? "arm64 " : "", p.has_other ? "other" : "");
            for (int k = 0; k < p.ndylibs; k++) printf("  %s\n", p.dylibs[k]);
            return 0;
        }
        return run_macho_file(path, app_argc, app_argv, force);
    } else {
        cider_die("Unsupported file", "%s isn't a macOS app, disk image or program.", path_basename(path));
    }

    if (info) {
        ui_progress_end();
        return describe_app_bundle(bundle);
    }
    return run_app_bundle(bundle, app_argc, app_argv, force);
}

static char **utf8_argv(int *argc)
{
    wchar_t **w = CommandLineToArgvW(GetCommandLineW(), argc);
    char **a = xcalloc((size_t)*argc + 1, sizeof(char *));
    for (int i = 0; i < *argc; i++) a[i] = w_to_utf8(w[i]);
    LocalFree(w);
    return a;
}

#ifdef CIDER_GUI
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cmd, int show)
{
    (void)hi; (void)hp; (void)cmd; (void)show;
    int argc;
    char **argv = utf8_argv(&argc);
    int r = cider_main(argc, argv);
    ui_progress_end();
    return r;
}
#else
int wmain(void)
{
    int argc;
    char **argv = utf8_argv(&argc);
    SetConsoleOutputCP(CP_UTF8);
    return cider_main(argc, argv);
}
#endif
