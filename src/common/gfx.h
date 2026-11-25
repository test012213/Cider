/* Tiny GDI+ drawing layer shared by Cider's windows and the installer. */
#pragma once
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

/* Palette (ARGB) */
#define C_BG        0xFF16110Eu
#define C_SURFACE   0xFF221A15u
#define C_SURFACE2  0xFF2C221Bu
#define C_BORDER    0xFF3B2D24u
#define C_TEXT      0xFFF6EEE7u
#define C_TEXT2     0xFFB9A89Bu
#define C_TEXT3     0xFF7F6F63u
#define C_AMBER     0xFFFFB02Eu
#define C_ORANGE    0xFFF0700Fu
#define C_ON_ACCENT 0xFF1D0F05u
#define C_DANGER    0xFFE5484Du

typedef struct {
    HWND hwnd;
    HDC hdc, mem;
    HBITMAP bmp, oldbmp;
    GpGraphics *g;
    int w, h;          /* physical pixels */
    float s;           /* DPI scale */
} gfx;

enum { GFX_LEFT = 0, GFX_CENTER = 1, GFX_RIGHT = 2 };
enum { GFX_REGULAR = 0, GFX_SEMIBOLD = 1, GFX_BOLD = 2 };

void gfx_startup(void);
float gfx_scale(HWND hwnd);
void gfx_dark_titlebar(HWND hwnd, unsigned rgb_caption);
void gfx_begin(gfx *f, HWND hwnd, HDC hdc);
void gfx_end(gfx *f);
void gfx_fill(gfx *f, float x, float y, float w, float h, unsigned argb);
void gfx_round(gfx *f, float x, float y, float w, float h, float r, unsigned argb);
void gfx_round_grad(gfx *f, float x, float y, float w, float h, float r, unsigned a1, unsigned a2, int vertical);
void gfx_round_stroke(gfx *f, float x, float y, float w, float h, float r, unsigned argb, float width, int dashed);
void gfx_glow(gfx *f, float cx, float cy, float rx, float ry, unsigned center);
void gfx_text(gfx *f, const wchar_t *s, float x, float y, float w, float h, float size, int weight, unsigned argb, int align, int wrap);
void gfx_icon(gfx *f, HICON icon, float x, float y, float size);
void gfx_circle(gfx *f, float cx, float cy, float r, unsigned argb);
void gfx_check(gfx *f, float x, float y, float size, unsigned argb);
HICON gfx_load_icon(int id, int px);
