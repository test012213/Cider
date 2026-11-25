#include "gfx.h"
#include <dwmapi.h>

static ULONG_PTR g_token;

void gfx_startup(void)
{
    if (g_token) return;
    GdiplusStartupInput in = { 1, NULL, FALSE, FALSE };
    GdiplusStartup(&g_token, &in, NULL);
}

float gfx_scale(HWND hwnd)
{
    UINT dpi = 96;
    HMODULE u = GetModuleHandleW(L"user32.dll");
    UINT(WINAPI * fn)(HWND) = u ? (void *)GetProcAddress(u, "GetDpiForWindow") : NULL;
    if (fn && hwnd) dpi = fn(hwnd);
    else {
        HDC dc = GetDC(NULL);
        dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(NULL, dc);
    }
    return dpi / 96.0f;
}

void gfx_dark_titlebar(HWND hwnd, unsigned rgb)
{
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    COLORREF cap = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof cap);
    int corner = 2; /* DWMWCP_ROUND */
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof corner);
}

void gfx_begin(gfx *f, HWND hwnd, HDC hdc)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    f->hwnd = hwnd;
    f->hdc = hdc;
    f->w = rc.right;
    f->h = rc.bottom;
    f->s = gfx_scale(hwnd);
    f->mem = CreateCompatibleDC(hdc);
    f->bmp = CreateCompatibleBitmap(hdc, f->w ? f->w : 1, f->h ? f->h : 1);
    f->oldbmp = SelectObject(f->mem, f->bmp);
    GdipCreateFromHDC(f->mem, &f->g);
    GdipSetSmoothingMode(f->g, SmoothingModeAntiAlias8x8);
    GdipSetPixelOffsetMode(f->g, PixelOffsetModeHalf);
    GdipSetTextRenderingHint(f->g, TextRenderingHintClearTypeGridFit);
    GdipSetCompositingQuality(f->g, CompositingQualityHighQuality);
    GdipSetInterpolationMode(f->g, InterpolationModeHighQualityBicubic);
}

void gfx_end(gfx *f)
{
    GdipDeleteGraphics(f->g);
    BitBlt(f->hdc, 0, 0, f->w, f->h, f->mem, 0, 0, SRCCOPY);
    SelectObject(f->mem, f->oldbmp);
    DeleteObject(f->bmp);
    DeleteDC(f->mem);
}

static GpPath *round_path(gfx *f, float x, float y, float w, float h, float r)
{
    float s = f->s;
    x *= s; y *= s; w *= s; h *= s; r *= s;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    GpPath *p;
    GdipCreatePath(FillModeAlternate, &p);
    if (r <= 0.5f) {
        GdipAddPathRectangle(p, x, y, w, h);
        return p;
    }
    float d = r * 2;
    GdipAddPathArc(p, x, y, d, d, 180, 90);
    GdipAddPathArc(p, x + w - d, y, d, d, 270, 90);
    GdipAddPathArc(p, x + w - d, y + h - d, d, d, 0, 90);
    GdipAddPathArc(p, x, y + h - d, d, d, 90, 90);
    GdipClosePathFigure(p);
    return p;
}

void gfx_fill(gfx *f, float x, float y, float w, float h, unsigned argb)
{
    GpSolidFill *b;
    GdipCreateSolidFill(argb, &b);
    GdipFillRectangle(f->g, (GpBrush *)b, x * f->s, y * f->s, w * f->s, h * f->s);
    GdipDeleteBrush((GpBrush *)b);
}

void gfx_round(gfx *f, float x, float y, float w, float h, float r, unsigned argb)
{
    GpPath *p = round_path(f, x, y, w, h, r);
    GpSolidFill *b;
    GdipCreateSolidFill(argb, &b);
    GdipFillPath(f->g, (GpBrush *)b, p);
    GdipDeleteBrush((GpBrush *)b);
    GdipDeletePath(p);
}

void gfx_round_grad(gfx *f, float x, float y, float w, float h, float r, unsigned a1, unsigned a2, int vertical)
{
    GpPath *p = round_path(f, x, y, w, h, r);
    GpLineGradient *b;
    GpPointF p1 = { x * f->s, y * f->s };
    GpPointF p2 = { (vertical ? x : x + w) * f->s, (vertical ? y + h : y) * f->s };
    if (vertical) p2.Y += 1; else p2.X += 1;
    GdipCreateLineBrush(&p1, &p2, a1, a2, WrapModeTileFlipXY, &b);
    GdipFillPath(f->g, (GpBrush *)b, p);
    GdipDeleteBrush((GpBrush *)b);
    GdipDeletePath(p);
}

void gfx_round_stroke(gfx *f, float x, float y, float w, float h, float r, unsigned argb, float width, int dashed)
{
    GpPath *p = round_path(f, x, y, w, h, r);
    GpPen *pen;
    GdipCreatePen1(argb, width * f->s, UnitPixel, &pen);
    if (dashed) {
        REAL dash[2] = { 4, 3 };
        GdipSetPenDashArray(pen, dash, 2);
    }
    GdipDrawPath(f->g, pen, p);
    GdipDeletePen(pen);
    GdipDeletePath(p);
}

void gfx_glow(gfx *f, float cx, float cy, float rx, float ry, unsigned center)
{
    GpPath *p;
    GdipCreatePath(FillModeAlternate, &p);
    GdipAddPathEllipse(p, (cx - rx) * f->s, (cy - ry) * f->s, rx * 2 * f->s, ry * 2 * f->s);
    GpPathGradient *b;
    GdipCreatePathGradientFromPath(p, &b);
    GdipSetPathGradientCenterColor(b, center);
    ARGB edge = center & 0x00FFFFFF;
    INT n = 1;
    GdipSetPathGradientSurroundColorsWithCount(b, &edge, &n);
    GdipFillPath(f->g, (GpBrush *)b, p);
    GdipDeleteBrush((GpBrush *)b);
    GdipDeletePath(p);
}

void gfx_circle(gfx *f, float cx, float cy, float r, unsigned argb)
{
    GpSolidFill *b;
    GdipCreateSolidFill(argb, &b);
    GdipFillEllipse(f->g, (GpBrush *)b, (cx - r) * f->s, (cy - r) * f->s, r * 2 * f->s, r * 2 * f->s);
    GdipDeleteBrush((GpBrush *)b);
}

void gfx_check(gfx *f, float x, float y, float size, unsigned argb)
{
    GpPen *pen;
    GdipCreatePen1(argb, size * 0.16f * f->s, UnitPixel, &pen);
    GdipSetPenStartCap(pen, LineCapRound);
    GdipSetPenEndCap(pen, LineCapRound);
    GdipSetPenLineJoin(pen, LineJoinRound);
    GpPointF pts[3] = {
        { (x + size * 0.18f) * f->s, (y + size * 0.52f) * f->s },
        { (x + size * 0.42f) * f->s, (y + size * 0.76f) * f->s },
        { (x + size * 0.84f) * f->s, (y + size * 0.26f) * f->s },
    };
    GdipDrawLines(f->g, pen, pts, 3);
    GdipDeletePen(pen);
}

static GpFontFamily *family(int weight)
{
    static GpFontFamily *fam[3];
    if (fam[weight]) return fam[weight];
    static const wchar_t *names[3][2] = {
        { L"Segoe UI Variable Text", L"Segoe UI" },
        { L"Segoe UI Variable Display Semib", L"Segoe UI Semibold" },
        { L"Segoe UI Variable Display", L"Segoe UI" },
    };
    if (GdipCreateFontFamilyFromName(names[weight][0], NULL, &fam[weight]) != Ok)
        GdipCreateFontFamilyFromName(names[weight][1], NULL, &fam[weight]);
    return fam[weight];
}

void gfx_text(gfx *f, const wchar_t *s, float x, float y, float w, float h, float size, int weight, unsigned argb, int align, int wrap)
{
    GpFont *font;
    GdipCreateFont(family(weight), size * f->s, weight == GFX_BOLD ? FontStyleBold : FontStyleRegular, UnitPixel, &font);
    GpStringFormat *fmt;
    GdipCreateStringFormat(wrap ? 0 : StringFormatFlagsNoWrap, LANG_NEUTRAL, &fmt);
    GdipSetStringFormatAlign(fmt, align == GFX_CENTER ? StringAlignmentCenter : align == GFX_RIGHT ? StringAlignmentFar : StringAlignmentNear);
    GdipSetStringFormatLineAlign(fmt, wrap ? StringAlignmentNear : StringAlignmentCenter);
    GdipSetStringFormatTrimming(fmt, StringTrimmingEllipsisCharacter);
    GpSolidFill *b;
    GdipCreateSolidFill(argb, &b);
    RectF r = { x * f->s, y * f->s, w * f->s, h * f->s };
    GdipDrawString(f->g, s, -1, font, &r, fmt, (GpBrush *)b);
    GdipDeleteBrush((GpBrush *)b);
    GdipDeleteStringFormat(fmt);
    GdipDeleteFont(font);
}

void gfx_icon(gfx *f, HICON icon, float x, float y, float size)
{
    HDC dc;
    if (GdipGetDC(f->g, &dc) != Ok) return;
    int px = (int)(size * f->s + 0.5f);
    DrawIconEx(dc, (int)(x * f->s + 0.5f), (int)(y * f->s + 0.5f), icon, px, px, 0, NULL, DI_NORMAL);
    GdipReleaseDC(f->g, dc);
}

HICON gfx_load_icon(int id, int px)
{
    return (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(id), IMAGE_ICON, px, px, LR_DEFAULTCOLOR);
}
