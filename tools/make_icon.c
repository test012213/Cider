/*
 * Renders Cider's icons from vector shapes (no image libraries needed).
 *
 *   make_icon <assets-dir>
 *
 * Writes cider.ico, dmg.ico (multi-size, PNG-compressed) and cider-512.png.
 * Design: a warm dark tile holding a glass of cider with foam, bubbles and
 * an apple leaf garnish. Drawn in a 256x256 design space.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float r, g, b, a; } rgba;   /* premultiplied */

static rgba over(rgba dst, float r, float g, float b, float a)
{
    rgba o;
    o.r = r * a + dst.r * (1 - a);
    o.g = g * a + dst.g * (1 - a);
    o.b = b * a + dst.b * (1 - a);
    o.a = a + dst.a * (1 - a);
    return o;
}

static float lerp(float a, float b, float t) { return a + (b - a) * t; }
static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static void hex(unsigned c, float *r, float *g, float *b) { *r = ((c >> 16) & 255) / 255.f; *g = ((c >> 8) & 255) / 255.f; *b = (c & 255) / 255.f; }
static void mixhex(unsigned c1, unsigned c2, float t, float *r, float *g, float *b)
{
    float r1, g1, b1, r2, g2, b2;
    hex(c1, &r1, &g1, &b1);
    hex(c2, &r2, &g2, &b2);
    t = clampf(t, 0, 1);
    *r = lerp(r1, r2, t); *g = lerp(g1, g2, t); *b = lerp(b1, b2, t);
}

/* ---- shapes (design space 0..256) ---- */

static int in_squircle(float x, float y, float cx, float cy, float half)
{
    float dx = fabsf(x - cx) / half, dy = fabsf(y - cy) / half;
    return powf(dx, 5) + powf(dy, 5) <= 1;
}

#define GLASS_TOP 66.f
#define GLASS_BOT 214.f
#define GLASS_HW_TOP 60.f
#define GLASS_HW_BOT 46.f
#define GLASS_CORNER 16.f

static float glass_hw(float y) { return lerp(GLASS_HW_TOP, GLASS_HW_BOT, (y - GLASS_TOP) / (GLASS_BOT - GLASS_TOP)); }

/* inside the glass silhouette, shrunk by `inset` */
static int in_glass(float x, float y, float inset)
{
    float top = GLASS_TOP + (inset > 0 ? 0 : inset), bot = GLASS_BOT - inset;
    if (y < top || y > bot) return 0;
    float hw = glass_hw(y) - inset;
    float dx = fabsf(x - 128);
    if (dx > hw) return 0;
    float rc = GLASS_CORNER - inset * 0.5f;
    if (rc < 2) rc = 2;
    if (y > bot - rc) {
        float cxx = glass_hw(bot - rc) - inset - rc;
        if (dx > cxx) {
            float ddx = dx - cxx, ddy = y - (bot - rc);
            if (ddx * ddx + ddy * ddy > rc * rc) return 0;
        }
    }
    return 1;
}

static int in_ellipse_ring(float x, float y, float cx, float cy, float rx, float ry, float t)
{
    float o = ((x - cx) * (x - cx)) / (rx * rx) + ((y - cy) * (y - cy)) / (ry * ry);
    float i = ((x - cx) * (x - cx)) / ((rx - t) * (rx - t)) + ((y - cy) * (y - cy)) / ((ry - t * 0.6f) * (ry - t * 0.6f));
    return o <= 1 && i > 1;
}

static int in_circle(float x, float y, float cx, float cy, float r) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; }

/* leaf: base (bx,by), angle (radians), length L, width W. returns 0/1, t along leaf in *u */
static int in_leaf(float x, float y, float bx, float by, float ang, float L, float W, float *u, float *v)
{
    float dx = x - bx, dy = y - by;
    float ca = cosf(ang), sa = sinf(ang);
    float a = dx * ca + dy * sa, b = -dx * sa + dy * ca;
    if (a < 0 || a > L) return 0;
    float half = W * 0.5f * powf(sinf((float)M_PI * a / L), 0.8f) * (a < L * 0.5f ? 1 : 1);
    if (fabsf(b) > half) return 0;
    *u = a / L;
    *v = b;
    return 1;
}

static float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float t = clampf((wx * vx + wy * vy) / (vx * vx + vy * vy), 0, 1);
    float dx = px - (ax + t * vx), dy = py - (ay + t * vy);
    return sqrtf(dx * dx + dy * dy);
}

/* ---- the cider glass, composited over `c` ---- */
static rgba draw_glass(rgba c, float x, float y, float size, float stroke, int detail)
{
    float r, g, b;
    /* glass tint */
    if (in_glass(x, y, 0)) c = over(c, 1, 1, 1, 0.07f);
    /* liquid */
    float surface = 104;
    if (in_glass(x, y, stroke) && y >= surface) {
        mixhex(0xFFCF4A, 0xE8590C, (y - surface) / (GLASS_BOT - surface), &r, &g, &b);
        c = over(c, r, g, b, 1);
        /* soft inner shading on the right */
        float hw = glass_hw(y) - stroke;
        float edge = clampf((x - 128) / hw, 0, 1);
        c = over(c, 0.55f, 0.12f, 0.0f, 0.22f * edge * edge);
    }
    /* bubbles */
    if (detail && in_glass(x, y, stroke)) {
        static const float bub[][3] = { { 108, 152, 6.5f }, { 147, 171, 5 }, { 124, 192, 4.5f }, { 157, 134, 3.8f }, { 101, 184, 3.2f }, { 136, 146, 2.8f } };
        for (int i = 0; i < 6; i++)
            if (in_circle(x, y, bub[i][0], bub[i][1], bub[i][2])) {
                float hl = in_circle(x, y, bub[i][0] - bub[i][2] * 0.35f, bub[i][1] - bub[i][2] * 0.35f, bub[i][2] * 0.45f);
                c = over(c, 1, 0.97f, 0.88f, hl ? 0.95f : 0.55f);
            }
    }
    /* foam */
    {
        static const float fc[][3] = { { 92, 103, 13 }, { 110, 98, 15 }, { 130, 96, 16 }, { 150, 99, 15 }, { 166, 104, 12 } };
        int foam = in_glass(x, y, stroke) && y >= 99 && y <= 112;
        for (int i = 0; i < 5 && !foam; i++)
            if (in_circle(x, y, fc[i][0], fc[i][1], fc[i][2]) && fabsf(x - 128) <= glass_hw(y) - stroke * 0.6f) foam = 1;
        if (foam) {
            mixhex(0xFFF8EA, 0xF2D3A2, clampf((y - 86) / 28, 0, 1), &r, &g, &b);
            c = over(c, r, g, b, 1);
        }
    }
    /* highlight stripe */
    if (detail) {
        float hx = 128 - glass_hw(y) + stroke + 9;
        if (y > 118 && y < 196 && x > hx && x < hx + 7 && in_glass(x, y, stroke)) {
            float fade = clampf(1 - fabsf(y - 157) / 39, 0, 1);
            c = over(c, 1, 1, 1, 0.38f * fade);
        }
    }
    /* glass outline (sides + bottom) */
    if (in_glass(x, y, 0) && !in_glass(x, y, stroke) && y > GLASS_TOP + 2) c = over(c, 0.97f, 0.95f, 0.93f, 0.96f);
    /* rim */
    if (in_ellipse_ring(x, y, 128, GLASS_TOP, GLASS_HW_TOP, 10, stroke)) c = over(c, 0.99f, 0.97f, 0.95f, 1);
    /* leaf garnish + stem */
    {
        float sw = stroke * 0.75f;
        if (seg_dist(x, y, 158, 74, 150, 92) < sw) c = over(c, 0.42f, 0.25f, 0.12f, 1);
        float u, v;
        if (in_leaf(x, y, 156, 76, -0.72f, 70, 32, &u, &v)) {
            mixhex(0x2F9E46, 0x8BE05A, u, &r, &g, &b);
            float shade = v > 0 ? 0.85f : 1.0f;
            c = over(c, r * shade, g * shade, b * shade, 1);
            if (detail && fabsf(v) < (size >= 48 ? 1.4f : 2.2f) && u > 0.06f && u < 0.85f) c = over(c, 0.12f, 0.45f, 0.18f, 0.75f);
        }
    }
    (void)size;
    return c;
}

static rgba shade_app(float x, float y, float size)
{
    rgba c = { 0, 0, 0, 0 };
    float stroke = fmaxf(6.5f, 1.15f * 256 / size);
    int detail = size >= 32;
    if (!in_squircle(x, y, 128, 128, 118)) return c;
    float r, g, b;
    mixhex(0x3B2316, 0x150B06, y / 256, &r, &g, &b);
    c = over(c, r, g, b, 1);
    /* warm glow behind the glass */
    float d = sqrtf((x - 128) * (x - 128) + (y - 150) * (y - 150) * 1.2f);
    float glow = clampf(1 - d / 120, 0, 1);
    c = over(c, 1.0f, 0.52f, 0.10f, 0.42f * glow * glow);
    /* top edge sheen */
    if (!in_squircle(x, y, 128, 130.5f, 118)) c = over(c, 1, 0.85f, 0.7f, 0.16f);
    return draw_glass(c, x, y, size, stroke, detail);
}

static rgba shade_dmg(float x, float y, float size)
{
    rgba c = { 0, 0, 0, 0 };
    float stroke = fmaxf(6.5f, 1.15f * 256 / size);
    float r, g, b;
    /* disk body */
    float x0 = 20, x1 = 236, y0 = 92, y1 = 226, rad = 26;
    int in = x >= x0 && x <= x1 && y >= y0 && y <= y1;
    if (in) {
        float cx = clampf(x, x0 + rad, x1 - rad), cy = clampf(y, y0 + rad, y1 - rad);
        in = (x - cx) * (x - cx) + (y - cy) * (y - cy) <= rad * rad;
    }
    if (in) {
        mixhex(0xF3EEEA, 0xB9AFA7, (y - y0) / (y1 - y0), &r, &g, &b);
        c = over(c, r, g, b, 1);
        if (y > 190 && y < 200 && x > 150 && x < 214) c = over(c, 0.25f, 0.2f, 0.17f, 0.85f);  /* slot */
        if (in_circle(x, y, 52, 196, 6)) c = over(c, 0.98f, 0.55f, 0.1f, 1);                    /* light */
    }
    /* small glass on top */
    float s = 0.62f, ox = 128 - 128 * s, oy = 6;
    float gx = (x - ox) / s, gy = (y - oy) / s;
    rgba g2 = draw_glass((rgba){ 0, 0, 0, 0 }, gx, gy, size * s, stroke / s * 0.8f, size >= 48);
    if (g2.a > 0) {
        c.r = g2.r + c.r * (1 - g2.a);
        c.g = g2.g + c.g * (1 - g2.a);
        c.b = g2.b + c.b * (1 - g2.a);
        c.a = g2.a + c.a * (1 - g2.a);
    }
    return c;
}

static uint8_t *render(int S, rgba (*shade)(float, float, float))
{
    uint8_t *px = malloc((size_t)S * S * 4);
    int n = S <= 64 ? 8 : S <= 128 ? 6 : 4;
    float k = 256.f / S;
    for (int j = 0; j < S; j++)
        for (int i = 0; i < S; i++) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < n; sy++)
                for (int sx = 0; sx < n; sx++) {
                    rgba c = shade((i + (sx + 0.5f) / n) * k, (j + (sy + 0.5f) / n) * k, (float)S);
                    r += c.r; g += c.g; b += c.b; a += c.a;
                }
            float inv = 1.f / (n * n);
            r *= inv; g *= inv; b *= inv; a *= inv;
            uint8_t *p = px + ((size_t)j * S + i) * 4;
            p[3] = (uint8_t)(a * 255 + 0.5f);
            p[0] = a > 0 ? (uint8_t)clampf(r / a * 255 + 0.5f, 0, 255) : 0;
            p[1] = a > 0 ? (uint8_t)clampf(g / a * 255 + 0.5f, 0, 255) : 0;
            p[2] = a > 0 ? (uint8_t)clampf(b / a * 255 + 0.5f, 0, 255) : 0;
        }
    return px;
}

/* ---- minimal deflate (fixed Huffman + LZ77) and PNG ---- */

typedef struct { uint8_t *d; size_t n, cap; uint32_t bb; int bc; } bw;

static void put_byte(bw *w, uint8_t b)
{
    if (w->n == w->cap) { w->cap = w->cap ? w->cap * 2 : 65536; w->d = realloc(w->d, w->cap); }
    w->d[w->n++] = b;
}
static void put_bits(bw *w, uint32_t v, int n)
{
    w->bb |= v << w->bc;
    w->bc += n;
    while (w->bc >= 8) { put_byte(w, (uint8_t)w->bb); w->bb >>= 8; w->bc -= 8; }
}
static uint32_t rev(uint32_t v, int n) { uint32_t r = 0; while (n--) { r = r << 1 | (v & 1); v >>= 1; } return r; }
static void put_sym(bw *w, int s)
{
    if (s < 144) put_bits(w, rev(0x30 + s, 8), 8);
    else if (s < 256) put_bits(w, rev(0x190 + s - 144, 9), 9);
    else if (s < 280) put_bits(w, rev(s - 256, 7), 7);
    else put_bits(w, rev(0xC0 + s - 280, 8), 8);
}

static const int LB[] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const int LE[] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const int DB[] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const int DE[] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static void deflate_fixed(bw *w, const uint8_t *in, size_t n)
{
    put_bits(w, 1, 1);
    put_bits(w, 1, 2);
    int *head = malloc(sizeof(int) * 65536), *prev = malloc(sizeof(int) * (n + 1));
    for (int i = 0; i < 65536; i++) head[i] = -1;
    size_t i = 0;
    while (i < n) {
        int best = 0, bdist = 0;
        if (i + 3 <= n) {
            unsigned h = (in[i] * 506832829u ^ in[i + 1] * 2654435761u ^ in[i + 2] * 40503u) & 0xFFFF;
            int cand = head[h], chain = 64;
            while (cand >= 0 && chain-- && i - (size_t)cand <= 32768) {
                int l = 0;
                while (l < 258 && i + l < n && in[cand + l] == in[i + l]) l++;
                if (l > best) { best = l; bdist = (int)(i - (size_t)cand); }
                cand = prev[cand];
            }
            prev[i] = head[h];
            head[h] = (int)i;
        }
        if (best >= 3) {
            int ls = 0;
            while (ls < 28 && LB[ls + 1] <= best) ls++;
            put_sym(w, 257 + ls);
            put_bits(w, (uint32_t)(best - LB[ls]), LE[ls]);
            int ds = 0;
            while (ds < 29 && DB[ds + 1] <= bdist) ds++;
            put_bits(w, rev((uint32_t)ds, 5), 5);
            put_bits(w, (uint32_t)(bdist - DB[ds]), DE[ds]);
            for (int k = 1; k < best; k++) {
                size_t p = i + k;
                if (p + 3 <= n) {
                    unsigned h = (in[p] * 506832829u ^ in[p + 1] * 2654435761u ^ in[p + 2] * 40503u) & 0xFFFF;
                    prev[p] = head[h];
                    head[h] = (int)p;
                }
            }
            i += (size_t)best;
        } else {
            put_sym(w, in[i]);
            i++;
        }
    }
    put_sym(w, 256);
    if (w->bc) put_bits(w, 0, 8 - w->bc);
    free(head);
    free(prev);
}

static uint32_t crc_table[256];
static uint32_t crc32b(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void be32w(bw *w, uint32_t v) { put_byte(w, v >> 24); put_byte(w, v >> 16); put_byte(w, v >> 8); put_byte(w, (uint8_t)v); }

static void chunk(bw *w, const char *type, const uint8_t *data, size_t n)
{
    be32w(w, (uint32_t)n);
    size_t start = w->n;
    for (int i = 0; i < 4; i++) put_byte(w, (uint8_t)type[i]);
    for (size_t i = 0; i < n; i++) put_byte(w, data[i]);
    be32w(w, crc32b(0, w->d + start, n + 4));
}

static bw make_png(const uint8_t *px, int S)
{
    size_t rawn = (size_t)S * (S * 4 + 1);
    uint8_t *raw = malloc(rawn);
    for (int y = 0; y < S; y++) {
        uint8_t *row = raw + (size_t)y * (S * 4 + 1);
        row[0] = 1;  /* Sub filter */
        const uint8_t *src = px + (size_t)y * S * 4;
        for (int x = 0; x < S * 4; x++) row[1 + x] = (uint8_t)(src[x] - (x >= 4 ? src[x - 4] : 0));
    }
    bw z = {0};
    put_byte(&z, 0x78);
    put_byte(&z, 0x01);
    deflate_fixed(&z, raw, rawn);
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < rawn; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    be32w(&z, b << 16 | a);
    free(raw);

    bw png = {0};
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    for (int i = 0; i < 8; i++) put_byte(&png, sig[i]);
    uint8_t ihdr[13] = { 0 };
    ihdr[0] = (uint8_t)(S >> 24); ihdr[1] = (uint8_t)(S >> 16); ihdr[2] = (uint8_t)(S >> 8); ihdr[3] = (uint8_t)S;
    memcpy(ihdr + 4, ihdr, 4);
    ihdr[8] = 8; ihdr[9] = 6;
    chunk(&png, "IHDR", ihdr, 13);
    chunk(&png, "IDAT", z.d, z.n);
    chunk(&png, "IEND", NULL, 0);
    free(z.d);
    return png;
}

static void write_ico(const char *path, rgba (*shade)(float, float, float))
{
    static const int sizes[] = { 16, 20, 24, 32, 40, 48, 64, 96, 128, 256 };
    const int N = sizeof sizes / sizeof *sizes;
    bw pngs[16];
    for (int i = 0; i < N; i++) {
        uint8_t *px = render(sizes[i], shade);
        pngs[i] = make_png(px, sizes[i]);
        free(px);
    }
    FILE *f = fopen(path, "wb");
    uint16_t hdr[3] = { 0, 1, (uint16_t)N };
    fwrite(hdr, 2, 3, f);
    uint32_t off = 6 + 16 * N;
    for (int i = 0; i < N; i++) {
        uint8_t e[16] = { 0 };
        e[0] = (uint8_t)(sizes[i] == 256 ? 0 : sizes[i]);
        e[1] = e[0];
        e[4] = 1;
        e[6] = 32;
        uint32_t len = (uint32_t)pngs[i].n;
        memcpy(e + 8, &len, 4);
        memcpy(e + 12, &off, 4);
        fwrite(e, 1, 16, f);
        off += len;
    }
    for (int i = 0; i < N; i++) { fwrite(pngs[i].d, 1, pngs[i].n, f); free(pngs[i].d); }
    fclose(f);
}

static void write_png(const char *path, int S, rgba (*shade)(float, float, float))
{
    uint8_t *px = render(S, shade);
    bw p = make_png(px, S);
    FILE *f = fopen(path, "wb");
    fwrite(p.d, 1, p.n, f);
    fclose(f);
    free(p.d);
    free(px);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[1024];
    snprintf(path, sizeof path, "%s/cider.ico", dir);
    write_ico(path, shade_app);
    snprintf(path, sizeof path, "%s/dmg.ico", dir);
    write_ico(path, shade_dmg);
    snprintf(path, sizeof path, "%s/cider-512.png", dir);
    write_png(path, 512, shade_app);
    snprintf(path, sizeof path, "%s/dmg-256.png", dir);
    write_png(path, 256, shade_dmg);
    printf("icons written to %s\n", dir);
    return 0;
}
