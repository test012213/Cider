/* Windows UI for ciderw.exe: progress popup, dialogs, drop-target launcher. */
#include "cider.h"
#include "../common/gfx.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <conio.h>
#include <io.h>
#include <fcntl.h>
#include "resource.h"

/* ------------------------------------------------------------------ */
/* Progress popup (runs on its own thread)                            */

static HWND g_prog;
static HANDLE g_prog_thread, g_prog_ready;
static wchar_t g_prog_title[256], g_prog_text[512];
static float g_phase;
static HICON g_icon48;

static LRESULT CALLBACK prog_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_TIMER:
        g_phase += 0.012f;
        if (g_phase > 1.4f) g_phase = -0.4f;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        gfx f;
        gfx_begin(&f, h, dc);
        float W = f.w / f.s, H = f.h / f.s;
        gfx_fill(&f, 0, 0, W, H, C_BG);
        gfx_glow(&f, 44, 46, 70, 60, 0x40F0700F);
        if (g_icon48) gfx_icon(&f, g_icon48, 20, 22, 48);
        gfx_text(&f, g_prog_title, 84, 22, W - 104, 26, 17, GFX_SEMIBOLD, C_TEXT, GFX_LEFT, 0);
        gfx_text(&f, g_prog_text, 84, 48, W - 104, 22, 13, GFX_REGULAR, C_TEXT2, GFX_LEFT, 0);
        float bx = 84, by = 82, bw = W - 104, bh = 6;
        gfx_round(&f, bx, by, bw, bh, 3, C_SURFACE2);
        float seg = bw * 0.35f, sx = bx + g_phase * bw;
        float x0 = sx < bx ? bx : sx, x1 = sx + seg > bx + bw ? bx + bw : sx + seg;
        if (x1 > x0) gfx_round_grad(&f, x0, by, x1 - x0, bh, 3, C_AMBER, C_ORANGE, 0);
        gfx_end(&f);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_APP:
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI prog_thread(LPVOID unused)
{
    (void)unused;
    gfx_startup();
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = prog_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_APPSTARTING);
    wc.lpszClassName = L"CiderProgress";
    wc.hIcon = gfx_load_icon(IDI_CIDER, 32);
    RegisterClassW(&wc);
    float s = gfx_scale(NULL);
    g_icon48 = gfx_load_icon(IDI_CIDER, (int)(48 * s));
    int w = (int)(440 * s), hgt = (int)(112 * s);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g_prog = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, g_prog_title, WS_POPUP,
                             (wa.left + wa.right - w) / 2, (wa.top + wa.bottom - hgt) / 2, w, hgt, NULL, NULL, wc.hInstance, NULL);
    gfx_dark_titlebar(g_prog, 0x16110E);
    ShowWindow(g_prog, SW_SHOWNOACTIVATE);
    SetTimer(g_prog, 1, 16, NULL);
    SetEvent(g_prog_ready);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

static void set_wtext(wchar_t *dst, size_t cap, const char *s)
{
    MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, dst, (int)cap);
}

void ui_progress_begin(const char *title, const char *text)
{
    if (!g_gui) return;
    set_wtext(g_prog_title, 256, title);
    set_wtext(g_prog_text, 512, text);
    if (g_prog) { PostMessageW(g_prog, WM_APP, 0, 0); return; }
    g_prog_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_prog_thread = CreateThread(NULL, 0, prog_thread, NULL, 0, NULL);
    WaitForSingleObject(g_prog_ready, 3000);
}

void ui_progress_text(const char *text)
{
    if (!g_prog) return;
    set_wtext(g_prog_text, 512, text);
    PostMessageW(g_prog, WM_APP, 0, 0);
}

void ui_progress_end(void)
{
    if (!g_prog) return;
    PostMessageW(g_prog, WM_CLOSE, 0, 0);
    WaitForSingleObject(g_prog_thread, 2000);
    g_prog = NULL;
}

/* ------------------------------------------------------------------ */
/* Dialogs                                                            */

void ui_message(int error, const char *title, const char *text, const char *details)
{
    if (!g_gui) {
        fprintf(error ? stderr : stdout, "%s\n%s\n", title, text);
        return;
    }
    wchar_t *wt = utf8_to_w(title), *wx = utf8_to_w(text), *wd = details ? utf8_to_w(details) : NULL;
    TASKDIALOGCONFIG tc = { sizeof tc };
    tc.hInstance = GetModuleHandleW(NULL);
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
    tc.pszWindowTitle = L"Cider";
    tc.pszMainIcon = error ? TD_WARNING_ICON : MAKEINTRESOURCEW(IDI_CIDER);
    tc.pszMainInstruction = wt;
    tc.pszContent = wx;
    tc.pszExpandedInformation = wd;
    tc.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    if (FAILED(TaskDialogIndirect(&tc, NULL, NULL, NULL)))
        MessageBoxW(NULL, wx, wt, MB_OK | (error ? MB_ICONWARNING : MB_ICONINFORMATION));
    free(wt);
    free(wx);
    free(wd);
}

int ui_choose(const char *title, const char *text, char **items, int n)
{
    if (!g_gui || n > 20) return 0;
    TASKDIALOG_BUTTON *b = xcalloc((size_t)n, sizeof *b);
    for (int i = 0; i < n; i++) {
        b[i].nButtonID = 100 + i;
        b[i].pszButtonText = utf8_to_w(items[i]);
    }
    wchar_t *wt = utf8_to_w(title), *wx = utf8_to_w(text);
    TASKDIALOGCONFIG tc = { sizeof tc };
    tc.hInstance = GetModuleHandleW(NULL);
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.pszWindowTitle = L"Cider";
    tc.pszMainIcon = MAKEINTRESOURCEW(IDI_CIDER);
    tc.pszMainInstruction = wt;
    tc.pszContent = wx;
    tc.cButtons = (UINT)n;
    tc.pButtons = b;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    int pressed = 0;
    TaskDialogIndirect(&tc, &pressed, NULL, NULL);
    return pressed >= 100 ? pressed - 100 : -1;
}

/* ------------------------------------------------------------------ */
/* Console for command-line macOS programs launched from Explorer     */

static int g_own_console;

void ui_ensure_console(const char *title)
{
    if (GetConsoleWindow()) return;
    if (!AllocConsole()) return;
    g_own_console = 1;
    wchar_t *wt = utf8_to_w(title);
    SetConsoleTitleW(wt);
    free(wt);
    FILE *f;
    freopen_s(&f, "CONIN$", "r", stdin);
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    _dup2(_fileno(stdin), 0);
    _dup2(_fileno(stdout), 1);
    _dup2(_fileno(stderr), 2);
    SetStdHandle(STD_INPUT_HANDLE, (HANDLE)_get_osfhandle(0));
    SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(1));
    SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(2));
}

void ui_console_pause(void)
{
    if (!g_own_console) return;
    fflush(NULL);
    fputs("\n\x1b[2m[Process completed - press any key to close]\x1b[0m", stdout);
    fflush(stdout);
    FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));
    _getch();
}

/* ------------------------------------------------------------------ */
/* Launcher: ciderw.exe with no arguments                             */

typedef struct { float x, y, w, h; } box;
static box b_dmg, b_app;
static int g_hover = -1;
static HICON g_icon96;
static char *g_chosen;

static int in_box(box b, float x, float y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }

static void pick(HWND h, int folder)
{
    wchar_t buf[MAX_PATH * 4] = {0};
    if (!folder) {
        OPENFILENAMEW of = { sizeof of };
        of.hwndOwner = h;
        of.lpstrFilter = L"macOS apps (*.dmg;*.zip)\0*.dmg;*.zip\0Mac programs (all files)\0*.*\0";
        of.lpstrFile = buf;
        of.nMaxFile = MAX_PATH * 4;
        of.Flags = OFN_FILEMUSTEXIST | OFN_EXPLORER;
        if (!GetOpenFileNameW(&of)) return;
    } else {
        BROWSEINFOW bi = {0};
        bi.hwndOwner = h;
        bi.lpszTitle = L"Choose a macOS .app folder";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON;
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        if (!pidl || !SHGetPathFromIDListW(pidl, buf)) return;
        CoTaskMemFree(pidl);
    }
    g_chosen = w_to_utf8(buf);
    DestroyWindow(h);
}

static LRESULT CALLBACK launch_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        gfx f;
        gfx_begin(&f, h, dc);
        float W = f.w / f.s, H = f.h / f.s;
        gfx_fill(&f, 0, 0, W, H, C_BG);
        gfx_glow(&f, W / 2, 86, 150, 110, 0x55F0700F);
        if (g_icon96) gfx_icon(&f, g_icon96, W / 2 - 48, 30, 96);
        gfx_text(&f, L"Cider", 0, 134, W, 40, 30, GFX_SEMIBOLD, C_TEXT, GFX_CENTER, 0);
        gfx_text(&f, L"Run macOS apps on Windows", 0, 172, W, 22, 14, GFX_REGULAR, C_TEXT2, GFX_CENTER, 0);
        float zx = 32, zy = 212, zw = W - 64, zh = 96;
        gfx_round(&f, zx, zy, zw, zh, 14, C_SURFACE);
        gfx_round_stroke(&f, zx + 0.5f, zy + 0.5f, zw - 1, zh - 1, 14, 0xFF5A4334, 1.5f, 1);
        gfx_text(&f, L"Drop a .dmg or .app here", zx, zy + 22, zw, 26, 16, GFX_SEMIBOLD, C_TEXT, GFX_CENTER, 0);
        gfx_text(&f, L"Intel Mac apps run natively · Electron and Java apps use Windows runtimes", zx, zy + 52, zw, 20, 12, GFX_REGULAR, C_TEXT3, GFX_CENTER, 0);
        float by = zy + zh + 20, bw = (zw - 12) / 2;
        b_dmg = (box){ zx, by, bw, 40 };
        b_app = (box){ zx + bw + 12, by, bw, 40 };
        if (g_hover == 0) gfx_round_grad(&f, b_dmg.x, b_dmg.y, b_dmg.w, b_dmg.h, 20, 0xFFFFC04E, 0xFFF5801F, 0);
        else gfx_round_grad(&f, b_dmg.x, b_dmg.y, b_dmg.w, b_dmg.h, 20, C_AMBER, C_ORANGE, 0);
        gfx_text(&f, L"Open disk image…", b_dmg.x, b_dmg.y, b_dmg.w, b_dmg.h, 14, GFX_SEMIBOLD, C_ON_ACCENT, GFX_CENTER, 0);
        gfx_round(&f, b_app.x, b_app.y, b_app.w, b_app.h, 20, g_hover == 1 ? 0xFF3A2D24 : C_SURFACE2);
        gfx_text(&f, L"Open .app folder…", b_app.x, b_app.y, b_app.w, b_app.h, 14, GFX_SEMIBOLD, C_TEXT, GFX_CENTER, 0);
        gfx_text(&f, L"Tip: right-click any .dmg or .app in File Explorer and choose “Open with Cider”.", 24, H - 40, W - 48, 20, 12, GFX_REGULAR, C_TEXT3, GFX_CENTER, 0);
        gfx_end(&f);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        float s = gfx_scale(h), x = (short)LOWORD(l) / s, y = (short)HIWORD(l) / s;
        int hv = in_box(b_dmg, x, y) ? 0 : in_box(b_app, x, y) ? 1 : -1;
        if (hv != g_hover) { g_hover = hv; InvalidateRect(h, NULL, FALSE); }
        SetCursor(LoadCursor(NULL, hv >= 0 ? IDC_HAND : IDC_ARROW));
        TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 };
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hover = -1;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        float s = gfx_scale(h), x = (short)LOWORD(l) / s, y = (short)HIWORD(l) / s;
        if (in_box(b_dmg, x, y)) pick(h, 0);
        else if (in_box(b_app, x, y)) pick(h, 1);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) return 1;
        break;
    case WM_DROPFILES: {
        wchar_t buf[MAX_PATH * 4];
        if (DragQueryFileW((HDROP)w, 0, buf, MAX_PATH * 4)) g_chosen = w_to_utf8(buf);
        DragFinish((HDROP)w);
        if (g_chosen) DestroyWindow(h);
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) DestroyWindow(h);
        return 0;
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

int ui_launcher(void)
{
    gfx_startup();
    CoInitialize(NULL);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = launch_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"CiderLauncher";
    wc.hIcon = gfx_load_icon(IDI_CIDER, 32);
    RegisterClassW(&wc);
    float s = gfx_scale(NULL);
    g_icon96 = gfx_load_icon(IDI_CIDER, (int)(96 * s));
    RECT r = { 0, 0, (int)(540 * s), (int)(440 * s) };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    HWND h = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"Cider", style, CW_USEDEFAULT, CW_USEDEFAULT, w, hgt,
                             NULL, NULL, wc.hInstance, NULL);
    gfx_dark_titlebar(h, 0x16110E);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    SetWindowPos(h, NULL, (wa.left + wa.right - w) / 2, (wa.top + wa.bottom - hgt) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(h, SW_SHOW);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (!g_chosen) return 0;
    wchar_t self[MAX_PATH * 4];
    GetModuleFileNameW(NULL, self, MAX_PATH * 4);
    char *s8 = w_to_utf8(self);
    char *cmd = xasprintf("\"%s\" \"%s\"", s8, g_chosen);
    run_process(s8, cmd, NULL, 0, NULL);
    return 0;
}
