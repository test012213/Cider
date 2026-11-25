/*
 * CiderSetup.exe - installs Cider for the current user (no admin rights).
 *
 *   CiderSetup.exe                 interactive install
 *   CiderSetup.exe /S              silent install with default options
 *   uninstall.exe /uninstall [/S]  remove Cider
 *
 * Installs to %LOCALAPPDATA%\Programs\Cider and registers:
 *   - "Open with Cider" / "Extract with Cider" on .dmg files
 *   - "Open with Cider" on folders named *.app
 *   - Cider as the default handler for .dmg (optional)
 *   - the install folder on the user's PATH (optional)
 *   - a Start menu shortcut and an Apps & features uninstall entry
 */
#define COBJMACROS
#include "../common/gfx.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "setup_res.h"

#define VERSION L"1.0.0"
#define UNINSTALL_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Cider"
#define CIDER_KEY L"Software\\Cider"
#define CLASSES L"Software\\Classes\\"

enum { PAGE_WELCOME, PAGE_PROGRESS, PAGE_DONE, PAGE_UNINSTALL, PAGE_UNINSTALL_DONE, PAGE_ERROR };
enum { OPT_MENU, OPT_DEFAULT, OPT_PATH, OPT_SHORTCUT, OPT_LAUNCH, OPT_PURGE, NOPTS };
enum { HIT_NONE = -1, HIT_PRIMARY = 100, HIT_SECONDARY = 101 };

static HWND g_hwnd;
static int g_page;
static int g_opt[NOPTS] = { 1, 1, 1, 1, 1, 1 };
static int g_hover = HIT_NONE;
static float g_progress, g_progress_target;
static wchar_t g_status[256] = L"Preparing…";
static wchar_t g_error[512];
static wchar_t g_dir[MAX_PATH];
static int g_silent, g_uninstall, g_updating, g_need_default, g_uninstalled;
static HICON g_icon128, g_icon20;

typedef struct { float x, y, w, h; int id; } hit;
static hit g_hits[16];
static int g_nhits;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */

static void install_dir(wchar_t *out)
{
    wchar_t *la = NULL;
    SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &la);
    swprintf(out, MAX_PATH, L"%ls\\Programs\\Cider", la);
    CoTaskMemFree(la);
}

static int reg_set(HKEY root, const wchar_t *key, const wchar_t *name, const wchar_t *val)
{
    HKEY h;
    if (RegCreateKeyExW(root, key, 0, NULL, 0, KEY_WRITE, NULL, &h, NULL)) return -1;
    LSTATUS r = RegSetValueExW(h, name, 0, REG_SZ, (const BYTE *)val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return r ? -1 : 0;
}

static int reg_set_dword(HKEY root, const wchar_t *key, const wchar_t *name, DWORD v)
{
    HKEY h;
    if (RegCreateKeyExW(root, key, 0, NULL, 0, KEY_WRITE, NULL, &h, NULL)) return -1;
    LSTATUS r = RegSetValueExW(h, name, 0, REG_DWORD, (const BYTE *)&v, sizeof v);
    RegCloseKey(h);
    return r ? -1 : 0;
}

static int reg_get(HKEY root, const wchar_t *key, const wchar_t *name, wchar_t *out, DWORD cap)
{
    DWORD n = cap * sizeof(wchar_t);
    out[0] = 0;
    return RegGetValueW(root, key, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, NULL, out, &n) ? -1 : 0;
}

static void reg_del_tree(const wchar_t *key) { RegDeleteTreeW(HKEY_CURRENT_USER, key); RegDeleteKeyW(HKEY_CURRENT_USER, key); }

static int write_resource(int id, const wchar_t *path)
{
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    if (!r) return -1;
    HGLOBAL g = LoadResource(NULL, r);
    const void *data = LockResource(g);
    DWORD size = SizeofResource(NULL, r);
    for (int attempt = 0; attempt < 2; attempt++) {
        HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD put = 0;
            BOOL ok = WriteFile(h, data, size, &put, NULL) && put == size;
            CloseHandle(h);
            return ok ? 0 : -1;
        }
        /* file in use (Cider running): move it aside and try again */
        wchar_t old[MAX_PATH + 16];
        swprintf(old, MAX_PATH + 16, L"%ls.old%u", path, GetTickCount());
        if (!MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING)) return -1;
        MoveFileExW(old, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    return -1;
}

static void broadcast_env(void)
{
    DWORD_PTR r;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 2000, &r);
}

/* Adds or removes `dir` from the user PATH. */
static void edit_path(const wchar_t *dir, int add)
{
    wchar_t cur[8192] = L"", out[8192] = L"";
    reg_get(HKEY_CURRENT_USER, L"Environment", L"Path", cur, 8192);
    int found = 0;
    wchar_t *ctx = NULL;
    wchar_t copy[8192];
    wcscpy(copy, cur);
    for (wchar_t *tok = wcstok(copy, L";", &ctx); tok; tok = wcstok(NULL, L";", &ctx)) {
        if (!_wcsicmp(tok, dir)) { found = 1; if (!add) continue; }
        if (*out) wcscat(out, L";");
        wcscat(out, tok);
    }
    if (add && !found) {
        if (*out) wcscat(out, L";");
        wcscat(out, dir);
    }
    if ((add && found) || (!add && !found)) return;
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_WRITE, &h) == ERROR_SUCCESS) {
        RegSetValueExW(h, L"Path", 0, REG_EXPAND_SZ, (const BYTE *)out, (DWORD)((wcslen(out) + 1) * sizeof(wchar_t)));
        RegCloseKey(h);
        broadcast_env();
    }
}

static void shortcut_path(wchar_t *out)
{
    wchar_t *pr = NULL;
    SHGetKnownFolderPath(&FOLDERID_Programs, 0, NULL, &pr);
    swprintf(out, MAX_PATH, L"%ls\\Cider.lnk", pr);
    CoTaskMemFree(pr);
}

static int make_shortcut(const wchar_t *target, const wchar_t *lnk)
{
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) return -1;
    IShellLinkW_SetPath(sl, target);
    IShellLinkW_SetDescription(sl, L"Run macOS apps on Windows");
    IShellLinkW_SetIconLocation(sl, target, 0);
    wchar_t wd[MAX_PATH];
    wcscpy(wd, target);
    *wcsrchr(wd, L'\\') = 0;
    IShellLinkW_SetWorkingDirectory(sl, wd);
    HRESULT hr = IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf);
    if (SUCCEEDED(hr)) {
        hr = IPersistFile_Save(pf, lnk, TRUE);
        IPersistFile_Release(pf);
    }
    IShellLinkW_Release(sl);
    return SUCCEEDED(hr) ? 0 : -1;
}

static void delete_matching(const wchar_t *dir, const wchar_t *pattern)
{
    wchar_t q[MAX_PATH + 16], p[MAX_PATH * 2];
    swprintf(q, MAX_PATH + 16, L"%ls\\%ls", dir, pattern);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(q, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        swprintf(p, MAX_PATH * 2, L"%ls\\%ls", dir, fd.cFileName);
        DeleteFileW(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static BOOL CALLBACK close_window_cb(HWND h, LPARAM pid)
{
    DWORD p;
    GetWindowThreadProcessId(h, &p);
    if (p == (DWORD)pid) PostMessageW(h, WM_CLOSE, 0, 0);
    return TRUE;
}

/* Closes Cider processes running from `dir` so their files can be removed. */
static void close_running(const wchar_t *dir)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = { sizeof pe };
    size_t dl = wcslen(dir);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == GetCurrentProcessId()) continue;
        HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
        if (!ph) continue;
        wchar_t img[MAX_PATH * 2];
        DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(ph, 0, img, &n) && !_wcsnicmp(img, dir, dl) && img[dl] == L'\\' &&
            _wcsicmp(img + dl + 1, L"uninstall.exe")) {   /* that one is waiting for us */
            EnumWindows(close_window_cb, (LPARAM)pe.th32ProcessID);
            if (WaitForSingleObject(ph, 2000) != WAIT_OBJECT_0) TerminateProcess(ph, 1);
            WaitForSingleObject(ph, 2000);
        }
        CloseHandle(ph);
    }
    CloseHandle(snap);
}

/* Windows only lets the user pick default apps, so check what .dmg opens with now. */
static int dmg_default_is_cider(void)
{
    wchar_t exe[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    if (FAILED(AssocQueryStringW(0, ASSOCSTR_EXECUTABLE, L".dmg", L"open", exe, &n))) return 0;
    const wchar_t *base = wcsrchr(exe, L'\\');
    return !_wcsicmp(base ? base + 1 : exe, L"ciderw.exe");
}

static void status(const wchar_t *s, float pct)
{
    if (g_silent) return;
    wcsncpy(g_status, s, 255);
    g_progress_target = pct;
    InvalidateRect(g_hwnd, NULL, FALSE);
    Sleep(140);   /* lets each step register visually; the whole install takes about a second */
}

/* ------------------------------------------------------------------ */
/* Install / uninstall                                                */

static int do_install(void)
{
    wchar_t p[MAX_PATH + 64], cmd[MAX_PATH * 2], icon[MAX_PATH + 16], ciderw[MAX_PATH + 32];
    status(L"Creating install folder…", 0.08f);
    SHCreateDirectoryExW(NULL, g_dir, NULL);
    if (GetFileAttributesW(g_dir) == INVALID_FILE_ATTRIBUTES) {
        swprintf(g_error, 512, L"Couldn't create %ls.", g_dir);
        return -1;
    }

    status(L"Copying Cider runtime…", 0.25f);
    delete_matching(g_dir, L"*.old*");   /* left behind by earlier updates */
    swprintf(p, MAX_PATH + 64, L"%ls\\cider.exe", g_dir);
    if (write_resource(IDR_CIDER, p)) { swprintf(g_error, 512, L"Couldn't write %ls.", p); return -1; }
    swprintf(ciderw, MAX_PATH + 32, L"%ls\\ciderw.exe", g_dir);
    if (write_resource(IDR_CIDERW, ciderw)) { swprintf(g_error, 512, L"Couldn't write %ls.", ciderw); return -1; }
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    swprintf(p, MAX_PATH + 64, L"%ls\\uninstall.exe", g_dir);
    if (_wcsicmp(self, p)) CopyFileW(self, p, FALSE);

    swprintf(icon, MAX_PATH + 16, L"\"%ls\",0", ciderw);
    status(L"Adding “Open with Cider” to File Explorer…", 0.45f);
    reg_del_tree(CLASSES L"SystemFileAssociations\\.dmg\\shell\\Cider");
    reg_del_tree(CLASSES L"SystemFileAssociations\\.dmg\\shell\\CiderExtract");
    reg_del_tree(CLASSES L"Directory\\shell\\Cider");
    if (g_opt[OPT_MENU]) {
        swprintf(cmd, MAX_PATH * 2, L"\"%ls\" \"%%1\"", ciderw);
        reg_set(HKEY_CURRENT_USER, CLASSES L"SystemFileAssociations\\.dmg\\shell\\Cider", L"MUIVerb", L"Open with Cider");
        reg_set(HKEY_CURRENT_USER, CLASSES L"SystemFileAssociations\\.dmg\\shell\\Cider", L"Icon", icon);
        reg_set(HKEY_CURRENT_USER, CLASSES L"SystemFileAssociations\\.dmg\\shell\\Cider\\command", NULL, cmd);
        swprintf(cmd, MAX_PATH * 2, L"\"%ls\" --extract-here \"%%1\"", ciderw);
        reg_set(HKEY_CURRENT_USER, CLASSES L"SystemFileAssociations\\.dmg\\shell\\CiderExtract", L"MUIVerb", L"Extract with Cider");
        reg_set(HKEY_CURRENT_USER, CLASSES L"SystemFileAssociations\\.dmg\\shell\\CiderExtract", L"Icon", icon);
        reg_set(HKEY_CURRENT_USER, CLASSES L"SystemFileAssociations\\.dmg\\shell\\CiderExtract\\command", NULL, cmd);
        swprintf(cmd, MAX_PATH * 2, L"\"%ls\" \"%%1\"", ciderw);
        reg_set(HKEY_CURRENT_USER, CLASSES L"Directory\\shell\\Cider", L"MUIVerb", L"Open with Cider");
        reg_set(HKEY_CURRENT_USER, CLASSES L"Directory\\shell\\Cider", L"Icon", icon);
        reg_set(HKEY_CURRENT_USER, CLASSES L"Directory\\shell\\Cider", L"AppliesTo", L"System.ItemName:~\"*.app\"");
        reg_set(HKEY_CURRENT_USER, CLASSES L"Directory\\shell\\Cider\\command", NULL, cmd);
    }

    status(L"Registering .dmg files…", 0.6f);
    swprintf(cmd, MAX_PATH * 2, L"\"%ls\" \"%%1\"", ciderw);
    wchar_t dmgicon[MAX_PATH + 16];
    swprintf(dmgicon, MAX_PATH + 16, L"\"%ls\",-2", ciderw);
    reg_set(HKEY_CURRENT_USER, CLASSES L"Cider.DiskImage", NULL, L"macOS Disk Image");
    reg_set(HKEY_CURRENT_USER, CLASSES L"Cider.DiskImage", L"FriendlyTypeName", L"macOS Disk Image");
    reg_set(HKEY_CURRENT_USER, CLASSES L"Cider.DiskImage\\DefaultIcon", NULL, dmgicon);
    reg_set(HKEY_CURRENT_USER, CLASSES L"Cider.DiskImage\\shell\\open", L"MUIVerb", L"Open with Cider");
    reg_set(HKEY_CURRENT_USER, CLASSES L"Cider.DiskImage\\shell\\open\\command", NULL, cmd);
    reg_set(HKEY_CURRENT_USER, CLASSES L".dmg\\OpenWithProgids", L"Cider.DiskImage", L"");
    swprintf(p, MAX_PATH + 64, CLASSES L"Applications\\ciderw.exe\\shell\\open\\command");
    reg_set(HKEY_CURRENT_USER, p, NULL, cmd);
    reg_set(HKEY_CURRENT_USER, CLASSES L"Applications\\ciderw.exe", L"FriendlyAppName", L"Cider");
    reg_set(HKEY_CURRENT_USER, CLASSES L"Applications\\ciderw.exe\\SupportedTypes", L".dmg", L"");
    /* lists Cider under Settings > Default apps */
    reg_set(HKEY_CURRENT_USER, CIDER_KEY L"\\Capabilities", L"ApplicationName", L"Cider");
    reg_set(HKEY_CURRENT_USER, CIDER_KEY L"\\Capabilities", L"ApplicationDescription", L"Run macOS apps on Windows");
    reg_set(HKEY_CURRENT_USER, CIDER_KEY L"\\Capabilities", L"ApplicationIcon", icon);
    reg_set(HKEY_CURRENT_USER, CIDER_KEY L"\\Capabilities\\FileAssociations", L".dmg", L"Cider.DiskImage");
    reg_set(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", L"Cider", CIDER_KEY L"\\Capabilities");
    if (g_opt[OPT_DEFAULT]) {
        wchar_t prev[256];
        if (!reg_get(HKEY_CURRENT_USER, CLASSES L".dmg", NULL, prev, 256) && *prev && wcscmp(prev, L"Cider.DiskImage"))
            reg_set(HKEY_CURRENT_USER, CIDER_KEY, L"PreviousDmgProgId", prev);
        reg_set(HKEY_CURRENT_USER, CLASSES L".dmg", NULL, L"Cider.DiskImage");
    }

    status(L"Updating PATH…", 0.72f);
    edit_path(g_dir, g_opt[OPT_PATH]);

    status(L"Creating Start menu shortcut…", 0.82f);
    shortcut_path(p);
    if (g_opt[OPT_SHORTCUT]) make_shortcut(ciderw, p);
    else DeleteFileW(p);

    status(L"Registering uninstaller…", 0.92f);
    swprintf(cmd, MAX_PATH * 2, L"\"%ls\\uninstall.exe\" /uninstall", g_dir);
    reg_set(HKEY_CURRENT_USER, UNINSTALL_KEY, L"DisplayName", L"Cider");
    reg_set(HKEY_CURRENT_USER, UNINSTALL_KEY, L"DisplayVersion", VERSION);
    reg_set(HKEY_CURRENT_USER, UNINSTALL_KEY, L"Publisher", L"Cider");
    reg_set(HKEY_CURRENT_USER, UNINSTALL_KEY, L"DisplayIcon", icon);
    reg_set(HKEY_CURRENT_USER, UNINSTALL_KEY, L"InstallLocation", g_dir);
    reg_set(HKEY_CURRENT_USER, UNINSTALL_KEY, L"UninstallString", cmd);
    wcscat(cmd, L" /S");
    reg_set(HKEY_CURRENT_USER, UNINSTALL_KEY, L"QuietUninstallString", cmd);
    reg_set_dword(HKEY_CURRENT_USER, UNINSTALL_KEY, L"NoModify", 1);
    reg_set_dword(HKEY_CURRENT_USER, UNINSTALL_KEY, L"NoRepair", 1);
    reg_set_dword(HKEY_CURRENT_USER, UNINSTALL_KEY, L"EstimatedSize", 4096);
    reg_set(HKEY_CURRENT_USER, CIDER_KEY, L"InstallDir", g_dir);
    reg_set(HKEY_CURRENT_USER, CIDER_KEY, L"Version", VERSION);
    reg_set_dword(HKEY_CURRENT_USER, CIDER_KEY, L"AddedToPath", (DWORD)g_opt[OPT_PATH]);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    g_need_default = g_opt[OPT_DEFAULT] && !dmg_default_is_cider();
    status(L"Done", 1.0f);
    return 0;
}

static void delete_dir(const wchar_t *dir)
{
    wchar_t buf[MAX_PATH + 2] = { 0 };
    wcsncpy(buf, dir, MAX_PATH);
    SHFILEOPSTRUCTW op = { 0 };
    op.wFunc = FO_DELETE;
    op.pFrom = buf;
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

static int do_uninstall(void)
{
    wchar_t p[MAX_PATH + 64], prev[256];
    status(L"Removing File Explorer entries…", 0.2f);
    reg_del_tree(CLASSES L"SystemFileAssociations\\.dmg\\shell\\Cider");
    reg_del_tree(CLASSES L"SystemFileAssociations\\.dmg\\shell\\CiderExtract");
    reg_del_tree(CLASSES L"Directory\\shell\\Cider");
    reg_del_tree(CLASSES L"Cider.DiskImage");
    reg_del_tree(CLASSES L"Applications\\ciderw.exe");
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, CLASSES L".dmg\\OpenWithProgids", 0, KEY_WRITE, &h) == ERROR_SUCCESS) {
        RegDeleteValueW(h, L"Cider.DiskImage");
        RegCloseKey(h);
    }
    wchar_t cur[256];
    if (!reg_get(HKEY_CURRENT_USER, CLASSES L".dmg", NULL, cur, 256) && !wcscmp(cur, L"Cider.DiskImage")) {
        if (!reg_get(HKEY_CURRENT_USER, CIDER_KEY, L"PreviousDmgProgId", prev, 256) && *prev)
            reg_set(HKEY_CURRENT_USER, CLASSES L".dmg", NULL, prev);
        else if (RegOpenKeyExW(HKEY_CURRENT_USER, CLASSES L".dmg", 0, KEY_WRITE, &h) == ERROR_SUCCESS) {
            RegDeleteValueW(h, NULL);
            RegCloseKey(h);
        }
    }
    status(L"Removing from PATH…", 0.4f);
    edit_path(g_dir, 0);
    status(L"Removing shortcut…", 0.55f);
    shortcut_path(p);
    DeleteFileW(p);
    status(L"Closing Cider…", 0.65f);
    close_running(g_dir);
    status(L"Removing files…", 0.75f);
    delete_dir(g_dir);
    if (g_opt[OPT_PURGE]) {
        wchar_t *la = NULL;
        SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &la);
        swprintf(p, MAX_PATH + 64, L"%ls\\Cider", la);
        CoTaskMemFree(la);
        close_running(p);   /* Electron apps Cider set up */
        delete_dir(p);
    }
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", 0, KEY_WRITE, &h) == ERROR_SUCCESS) {
        RegDeleteValueW(h, L"Cider");
        RegCloseKey(h);
    }
    reg_del_tree(UNINSTALL_KEY);
    reg_del_tree(CIDER_KEY);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    g_uninstalled = 1;
    status(L"Done", 1.0f);
    return 0;
}

static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    CoInitialize(NULL);
    int r = g_uninstall ? do_uninstall() : do_install();
    Sleep(250);
    PostMessageW(g_hwnd, WM_APP, (WPARAM)r, 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                            */

static void add_hit(float x, float y, float w, float h, int id)
{
    if (g_nhits < 16) g_hits[g_nhits++] = (hit){ x, y, w, h, id };
}

static void button(gfx *f, float x, float y, float w, float h, const wchar_t *label, int id, int style)
{
    int hov = g_hover == id;
    if (style == 0) gfx_round_grad(f, x, y, w, h, h / 2, hov ? 0xFFFFC456 : C_AMBER, hov ? 0xFFF7831F : C_ORANGE, 0);
    else if (style == 2) gfx_round(f, x, y, w, h, h / 2, hov ? 0xFFF05A5E : C_DANGER);
    else {
        gfx_round(f, x, y, w, h, h / 2, hov ? 0xFF3A2D24 : C_SURFACE2);
        gfx_round_stroke(f, x + 0.5f, y + 0.5f, w - 1, h - 1, h / 2, C_BORDER, 1, 0);
    }
    gfx_text(f, label, x, y, w, h, 14, GFX_SEMIBOLD, style == 0 ? C_ON_ACCENT : C_TEXT, GFX_CENTER, 0);
    add_hit(x, y, w, h, id);
}

static void toggle(gfx *f, float x, float y, int on, int id)
{
    float w = 40, h = 22;
    if (on) gfx_round_grad(f, x, y, w, h, h / 2, C_AMBER, C_ORANGE, 0);
    else {
        gfx_round(f, x, y, w, h, h / 2, C_SURFACE2);
        gfx_round_stroke(f, x + 0.5f, y + 0.5f, w - 1, h - 1, h / 2, 0xFF54443A, 1, 0);
    }
    float kx = on ? x + w - h + 3 : x + 3;
    gfx_circle(f, kx + (h - 6) / 2, y + h / 2, (h - 6) / 2, on ? 0xFFFFF8F0 : 0xFF9C8B7F);
    (void)id;
}

static void option_row(gfx *f, float x, float y, float w, const wchar_t *title, const wchar_t *sub, int opt)
{
    if (g_hover == opt) gfx_round(f, x - 12, y - 6, w + 24, 52, 10, 0x14FFFFFF);
    gfx_text(f, title, x, y, w - 60, 22, 14, GFX_SEMIBOLD, C_TEXT, GFX_LEFT, 0);
    gfx_text(f, sub, x, y + 21, w - 60, 20, 12, GFX_REGULAR, C_TEXT3, GFX_LEFT, 0);
    toggle(f, x + w - 40, y + 9, g_opt[opt], opt);
    add_hit(x - 12, y - 6, w + 24, 52, opt);
}

static void hero(gfx *f, float H)
{
    float W = 280;
    gfx_round_grad(f, 0, 0, W, H, 0, 0xFF2B170C, 0xFF110A07, 1);
    gfx_glow(f, W / 2, 168, 170, 150, 0x70F0700F);
    gfx_glow(f, W / 2, 168, 90, 80, 0x30FFB02E);
    if (g_icon128) gfx_icon(f, g_icon128, W / 2 - 64, 92, 128);
    gfx_text(f, L"Cider", 0, 234, W, 44, 36, GFX_SEMIBOLD, C_TEXT, GFX_CENTER, 0);
    gfx_text(f, L"macOS apps on Windows", 0, 280, W, 22, 14, GFX_REGULAR, C_TEXT2, GFX_CENTER, 0);
    static const wchar_t *feat[] = { L"Intel Mac code runs natively", L"Electron and Java apps supported",
                                     L"Opens .dmg and .app directly" };
    for (int i = 0; i < 3; i++) {
        float y = 332 + i * 26;
        gfx_circle(f, 48, y + 10, 3, C_AMBER);
        gfx_text(f, feat[i], 60, y, W - 70, 20, 12.5f, GFX_REGULAR, C_TEXT2, GFX_LEFT, 0);
    }
    gfx_text(f, L"Version " VERSION, 0, H - 34, W, 18, 11, GFX_REGULAR, C_TEXT3, GFX_CENTER, 0);
    gfx_fill(f, W - 1, 0, 1, H, 0xFF2A201A);
}

static void fake_menu(gfx *f, float x, float y)
{
    gfx_round(f, x + 3, y + 5, 300, 132, 10, 0x50000000);
    gfx_round(f, x, y, 300, 132, 10, 0xFF2B2B2B);
    gfx_round_stroke(f, x + 0.5f, y + 0.5f, 299, 131, 10, 0xFF3C3C3C, 1, 0);
    static const wchar_t *rows[] = { L"Open", L"Open with Cider", L"Extract with Cider", L"Copy as path" };
    for (int i = 0; i < 4; i++) {
        float ry = y + 8 + i * 29;
        if (i == 1) gfx_round(f, x + 6, ry, 288, 27, 6, 0xFF3D3D3D);
        if ((i == 1 || i == 2) && g_icon20) gfx_icon(f, g_icon20, x + 16, ry + 4, 18);
        gfx_text(f, rows[i], x + 44, ry, 240, 27, 13, i == 1 ? GFX_SEMIBOLD : GFX_REGULAR, i == 1 ? 0xFFFFFFFF : 0xFFD6D6D6, GFX_LEFT, 0);
    }
}

static void paint(HWND h, HDC dc)
{
    gfx f;
    gfx_begin(&f, h, dc);
    float W = f.w / f.s, H = f.h / f.s;
    g_nhits = 0;
    gfx_fill(&f, 0, 0, W, H, C_BG);
    hero(&f, H);
    float x = 320, cw = W - x - 40;
    float by = H - 64;

    switch (g_page) {
    case PAGE_WELCOME: {
        gfx_text(&f, g_updating ? L"Update Cider" : L"Install Cider", x, 36, cw, 40, 27, GFX_SEMIBOLD, C_TEXT, GFX_LEFT, 0);
        gfx_text(&f, L"Run macOS apps from File Explorer. After installing, right-click any .dmg or .app and choose Open with Cider.",
                 x, 80, cw, 44, 13.5f, GFX_REGULAR, C_TEXT2, GFX_LEFT, 1);
        option_row(&f, x, 142, cw, L"Add “Open with Cider” to right-click menus", L"For .dmg disk images and .app folders", OPT_MENU);
        option_row(&f, x, 198, cw, L"Open .dmg files with Cider", L"Double-click a disk image to start the app inside", OPT_DEFAULT);
        option_row(&f, x, 254, cw, L"Add cider to PATH", L"Run Mac command-line tools from any terminal", OPT_PATH);
        option_row(&f, x, 310, cw, L"Start menu shortcut", L"A drop window for .dmg and .app files", OPT_SHORTCUT);
        gfx_text(&f, L"Installs just for you, no administrator rights needed", x, by - 52, cw, 18, 11.5f, GFX_REGULAR, C_TEXT3, GFX_LEFT, 0);
        gfx_text(&f, g_dir, x, by - 34, cw, 18, 11.5f, GFX_REGULAR, C_TEXT3, GFX_LEFT, 0);
        button(&f, W - 40 - 120, by, 120, 40, g_updating ? L"Update" : L"Install", HIT_PRIMARY, 0);
        button(&f, W - 40 - 120 - 12 - 100, by, 100, 40, L"Cancel", HIT_SECONDARY, 1);
        break;
    }
    case PAGE_PROGRESS: {
        gfx_text(&f, g_uninstall ? L"Uninstalling…" : L"Installing…", x, 36, cw, 40, 27, GFX_SEMIBOLD, C_TEXT, GFX_LEFT, 0);
        gfx_text(&f, g_status, x, 196, cw, 22, 14, GFX_REGULAR, C_TEXT2, GFX_LEFT, 0);
        gfx_round(&f, x, 228, cw, 10, 5, C_SURFACE2);
        float pw = cw * g_progress;
        if (pw > 10) gfx_round_grad(&f, x, 228, pw, 10, 5, C_AMBER, C_ORANGE, 0);
        wchar_t pct[16];
        swprintf(pct, 16, L"%d%%", (int)(g_progress * 100 + 0.5f));
        gfx_text(&f, pct, x, 246, cw, 20, 12, GFX_REGULAR, C_TEXT3, GFX_RIGHT, 0);
        break;
    }
    case PAGE_DONE: {
        gfx_round_grad(&f, x, 36, 44, 44, 22, C_AMBER, C_ORANGE, 1);
        gfx_check(&f, x + 10, 46, 24, C_ON_ACCENT);
        gfx_text(&f, g_updating ? L"Cider is up to date" : L"Cider is ready", x, 92, cw, 40, 27, GFX_SEMIBOLD, C_TEXT, GFX_LEFT, 0);
        gfx_text(&f, L"Right-click a .dmg file or a .app folder in File Explorer and choose Open with Cider.",
                 x, 136, cw, 44, 13.5f, GFX_REGULAR, C_TEXT2, GFX_LEFT, 1);
        fake_menu(&f, x, 196);
        if (g_need_default)
            gfx_text(&f, L"Another app is set to open .dmg files. When you click Finish, Windows Settings opens so you can choose Cider.",
                     x, 344, cw, 40, 12, GFX_REGULAR, C_TEXT3, GFX_LEFT, 1);
        toggle(&f, x, by + 9, g_opt[OPT_LAUNCH], OPT_LAUNCH);
        gfx_text(&f, L"Open Cider now", x + 50, by, 160, 40, 13.5f, GFX_REGULAR, C_TEXT, GFX_LEFT, 0);
        add_hit(x - 6, by, 200, 40, OPT_LAUNCH);
        button(&f, W - 40 - 120, by, 120, 40, L"Finish", HIT_PRIMARY, 0);
        break;
    }
    case PAGE_UNINSTALL:
        gfx_text(&f, L"Uninstall Cider", x, 36, cw, 40, 27, GFX_SEMIBOLD, C_TEXT, GFX_LEFT, 0);
        gfx_text(&f, L"This removes Cider, its right-click menu entries, file associations and Start menu shortcut.",
                 x, 80, cw, 44, 13.5f, GFX_REGULAR, C_TEXT2, GFX_LEFT, 1);
        option_row(&f, x, 150, cw, L"Also remove downloaded runtimes and cached apps",
                   L"Electron runtimes and unpacked disk images", OPT_PURGE);
        button(&f, W - 40 - 120, by, 120, 40, L"Uninstall", HIT_PRIMARY, 2);
        button(&f, W - 40 - 120 - 12 - 100, by, 100, 40, L"Cancel", HIT_SECONDARY, 1);
        break;
    case PAGE_UNINSTALL_DONE:
        gfx_text(&f, L"Cider was removed", x, 36, cw, 40, 27, GFX_SEMIBOLD, C_TEXT, GFX_LEFT, 0);
        gfx_text(&f, L"Thanks for trying Cider.", x, 80, cw, 24, 13.5f, GFX_REGULAR, C_TEXT2, GFX_LEFT, 0);
        button(&f, W - 40 - 120, by, 120, 40, L"Close", HIT_PRIMARY, 0);
        break;
    case PAGE_ERROR:
        gfx_text(&f, L"Something went wrong", x, 36, cw, 40, 27, GFX_SEMIBOLD, C_TEXT, GFX_LEFT, 0);
        gfx_text(&f, g_error, x, 80, cw, 120, 13.5f, GFX_REGULAR, C_TEXT2, GFX_LEFT, 1);
        button(&f, W - 40 - 120, by, 120, 40, L"Close", HIT_PRIMARY, 0);
        break;
    }
    gfx_end(&f);
}

/* ------------------------------------------------------------------ */
/* Window                                                             */

static void start_work(void)
{
    g_page = PAGE_PROGRESS;
    g_progress = g_progress_target = 0;
    SetTimer(g_hwnd, 1, 16, NULL);
    CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void launch_cider(void)
{
    wchar_t p[MAX_PATH + 32];
    swprintf(p, MAX_PATH + 32, L"%ls\\ciderw.exe", g_dir);
    ShellExecuteW(NULL, L"open", p, NULL, g_dir, SW_SHOWNORMAL);
}

static void activate(int id)
{
    if (id >= 0 && id < NOPTS) {
        g_opt[id] = !g_opt[id];
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    switch (g_page) {
    case PAGE_WELCOME:
    case PAGE_UNINSTALL:
        if (id == HIT_PRIMARY) start_work();
        else if (id == HIT_SECONDARY) DestroyWindow(g_hwnd);
        break;
    case PAGE_DONE:
        if (id == HIT_PRIMARY) {
            if (g_need_default)
                ShellExecuteW(NULL, L"open", L"ms-settings:defaultapps?registeredAppUser=Cider", NULL, NULL, SW_SHOWNORMAL);
            if (g_opt[OPT_LAUNCH]) launch_cider();
            DestroyWindow(g_hwnd);
        }
        break;
    case PAGE_UNINSTALL_DONE:
    case PAGE_ERROR:
        if (id == HIT_PRIMARY) DestroyWindow(g_hwnd);
        break;
    }
}

static int hit_test(LPARAM l)
{
    float s = gfx_scale(g_hwnd), x = (short)LOWORD(l) / s, y = (short)HIWORD(l) / s;
    for (int i = g_nhits - 1; i >= 0; i--)
        if (x >= g_hits[i].x && x < g_hits[i].x + g_hits[i].w && y >= g_hits[i].y && y < g_hits[i].y + g_hits[i].h)
            return g_hits[i].id;
    return HIT_NONE;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        if (g_progress < g_progress_target) {
            g_progress += (g_progress_target - g_progress) * 0.18f + 0.002f;
            if (g_progress > g_progress_target) g_progress = g_progress_target;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_APP:
        KillTimer(h, 1);
        if ((int)w != 0) g_page = PAGE_ERROR;
        else g_page = g_uninstall ? PAGE_UNINSTALL_DONE : PAGE_DONE;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        int id = hit_test(l);
        if (id != g_hover) { g_hover = id; InvalidateRect(h, NULL, FALSE); }
        SetCursor(LoadCursor(NULL, id != HIT_NONE ? IDC_HAND : IDC_ARROW));
        TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 };
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hover = HIT_NONE;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) return 1;
        break;
    case WM_LBUTTONUP:
        activate(hit_test(l));
        return 0;
    case WM_KEYDOWN:
        if (w == VK_RETURN) activate(HIT_PRIMARY);
        else if (w == VK_ESCAPE && g_page != PAGE_PROGRESS) DestroyWindow(h);
        return 0;
    case WM_CLOSE:
        if (g_page == PAGE_PROGRESS) return 0;
        break;
    case WM_DPICHANGED: {
        RECT *r = (RECT *)l;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static HWND create_window(int visible)
{
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = gfx_load_icon(IDI_CIDER, 32);
    wc.lpszClassName = L"CiderSetup";
    RegisterClassW(&wc);
    float s = gfx_scale(NULL);
    g_icon128 = gfx_load_icon(IDI_CIDER, (int)(128 * s));
    g_icon20 = gfx_load_icon(IDI_CIDER, (int)(20 * s));
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = { 0, 0, (int)(780 * s), (int)(500 * s) };
    AdjustWindowRect(&r, style, FALSE);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    HWND h = CreateWindowExW(0, wc.lpszClassName, g_uninstall ? L"Uninstall Cider" : L"Cider Setup", style,
                             (wa.left + wa.right - w) / 2, (wa.top + wa.bottom - hgt) / 2, w, hgt, NULL, NULL, wc.hInstance, NULL);
    gfx_dark_titlebar(h, 0x16110E);
    if (visible) ShowWindow(h, SW_SHOW);
    return h;
}

/* Renders a page to a PNG (used to check the UI without clicking through it). */
static int snapshot(int page, const wchar_t *out)
{
    g_hwnd = create_window(0);
    g_page = page;
    g_progress = 0.62f;
    g_need_default = 1;
    wcscpy(g_dir, L"C:\\Users\\you\\AppData\\Local\\Programs\\Cider");   /* keeps real usernames out of screenshots */
    wcscpy(g_status, L"Adding “Open with Cider” to File Explorer…");
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    HDC wdc = GetDC(g_hwnd), mem = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    paint(g_hwnd, mem);
    SelectObject(mem, old);
    GpBitmap *gb;
    GdipCreateBitmapFromHBITMAP(bmp, NULL, &gb);
    CLSID png = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
    GpStatus st = GdipSaveImageToFile((GpImage *)gb, out, &png, NULL);
    GdipDisposeImage((GpImage *)gb);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(g_hwnd, wdc);
    return st == Ok ? 0 : 1;
}

/* The uninstaller runs from a temp copy so it can delete the install folder. */
static int relaunch_from_temp(void)
{
    wchar_t self[MAX_PATH], tmp[MAX_PATH], dst[MAX_PATH + 64], cmd[MAX_PATH * 3];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    GetTempPathW(MAX_PATH, tmp);
    swprintf(dst, MAX_PATH + 64, L"%lscider-uninstall-%u.exe", tmp, GetCurrentProcessId());
    if (!CopyFileW(self, dst, FALSE)) return -1;
    swprintf(cmd, MAX_PATH * 3, L"\"%ls\" /uninstall /from \"%ls\"%ls", dst, g_dir, g_silent ? L" /S" : L"");
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(dst, cmd, NULL, NULL, FALSE, 0, NULL, tmp, &si, &pi)) return -1;
    if (g_silent) WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
}

/* Deletes this temp copy once it exits, and after an uninstall also the
   install folder's uninstall.exe, which stays locked until the original
   process that launched us has exited too. */
static void delete_self_later(void)
{
    wchar_t self[MAX_PATH], cmd[MAX_PATH * 4], sys[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    GetSystemDirectoryW(sys, MAX_PATH);
    int n = swprintf(cmd, MAX_PATH * 4, L"\"%ls\\cmd.exe\" /c ping -n 3 127.0.0.1 >nul & del /f /q \"%ls\"", sys, self);
    if (g_uninstalled)
        swprintf(cmd + n, MAX_PATH * 4 - n, L" & del /f /q \"%ls\\uninstall.exe\" & rmdir \"%ls\"", g_dir, g_dir);
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, sys, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cmdline, int show)
{
    (void)hi; (void)hp; (void)cmdline; (void)show;
    CoInitialize(NULL);
    gfx_startup();
    install_dir(g_dir);
    int argc, from_temp = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        if (!_wcsicmp(argv[i], L"/S") || !_wcsicmp(argv[i], L"--silent")) g_silent = 1;
        else if (!_wcsicmp(argv[i], L"/uninstall") || !_wcsicmp(argv[i], L"--uninstall")) g_uninstall = 1;
        else if (!_wcsicmp(argv[i], L"/from") && i + 1 < argc) { wcsncpy(g_dir, argv[++i], MAX_PATH - 1); from_temp = 1; }
        else if (!_wcsicmp(argv[i], L"/snapshot") && i + 2 < argc) return snapshot(_wtoi(argv[i + 1]), argv[i + 2]);
    }
    wchar_t existing[MAX_PATH];
    if (!reg_get(HKEY_CURRENT_USER, CIDER_KEY, L"InstallDir", existing, MAX_PATH) && *existing) {
        if (!from_temp) wcsncpy(g_dir, existing, MAX_PATH - 1);
        g_updating = !g_uninstall;
    }

    if (g_uninstall && !from_temp) {
        /* running from the install folder: hand over to a temp copy */
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(NULL, self, MAX_PATH);
        if (!_wcsnicmp(self, g_dir, wcslen(g_dir))) return relaunch_from_temp() ? 1 : 0;
    }

    if (g_silent) {
        int r = g_uninstall ? do_uninstall() : do_install();
        if (from_temp) delete_self_later();
        return r ? 1 : 0;
    }

    g_page = g_uninstall ? PAGE_UNINSTALL : PAGE_WELCOME;
    g_hwnd = create_window(1);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (from_temp) delete_self_later();
    return 0;
}
