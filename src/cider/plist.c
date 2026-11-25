/* Property list reader: XML plists and binary (bplist00) plists. */
#include "cider.h"

static plnode *newnode(pltype t)
{
    plnode *n = xcalloc(1, sizeof *n);
    n->t = t;
    return n;
}

static void addchild(plnode *p, const char *key, plnode *c)
{
    p->items = xrealloc(p->items, sizeof(plnode *) * (p->n + 1));
    if (p->t == PL_DICT) {
        p->keys = xrealloc(p->keys, sizeof(char *) * (p->n + 1));
        p->keys[p->n] = xstrdup(key ? key : "");
    }
    p->items[p->n++] = c;
}

void plist_free(plnode *n)
{
    if (!n) return;
    for (int i = 0; i < n->n; i++) {
        plist_free(n->items[i]);
        if (n->keys) free(n->keys[i]);
    }
    free(n->items);
    free(n->keys);
    free(n->str);
    free(n);
}

plnode *pl_get(plnode *d, const char *key)
{
    if (!d || d->t != PL_DICT) return NULL;
    for (int i = 0; i < d->n; i++)
        if (!strcmp(d->keys[i], key)) return d->items[i];
    return NULL;
}

const char *pl_str(plnode *d, const char *key)
{
    plnode *n = pl_get(d, key);
    return n && n->t == PL_STRING ? n->str : NULL;
}

/* ---------------- XML ---------------- */

typedef struct { const char *p, *end; } xr;

static void skip_ws(xr *x)
{
    for (;;) {
        while (x->p < x->end && (*x->p == ' ' || *x->p == '\t' || *x->p == '\r' || *x->p == '\n')) x->p++;
        if (x->end - x->p >= 4 && !memcmp(x->p, "<!--", 4)) {
            const char *e = strstr(x->p, "-->");
            x->p = e ? e + 3 : x->end;
        } else if (x->end - x->p >= 2 && (!memcmp(x->p, "<?", 2) || !memcmp(x->p, "<!", 2))) {
            while (x->p < x->end && *x->p != '>') x->p++;
            if (x->p < x->end) x->p++;
        } else {
            return;
        }
    }
}

/* Reads "<name ...>" or "<name/>" ; returns 1 for open, 2 for self-closing, 3 for close tag. */
static int read_tag(xr *x, char *name, size_t cap)
{
    skip_ws(x);
    if (x->p >= x->end || *x->p != '<') return 0;
    x->p++;
    int close = 0;
    if (*x->p == '/') { close = 1; x->p++; }
    size_t i = 0;
    while (x->p < x->end && *x->p != '>' && *x->p != ' ' && *x->p != '/' && *x->p != '\t' && *x->p != '\n' && *x->p != '\r') {
        if (i + 1 < cap) name[i++] = *x->p;
        x->p++;
    }
    name[i] = 0;
    int self = 0;
    while (x->p < x->end && *x->p != '>') {
        if (*x->p == '/') self = 1;
        x->p++;
    }
    if (x->p < x->end) x->p++;
    return close ? 3 : self ? 2 : 1;
}

static char *read_text(xr *x, const char *tag)
{
    char endtag[64];
    snprintf(endtag, sizeof endtag, "</%s", tag);
    const char *start = x->p;
    const char *e = strstr(x->p, endtag);
    if (!e || e > x->end) e = x->end;
    size_t n = (size_t)(e - start);
    char *out = xmalloc(n + 1);
    size_t o = 0;
    for (const char *s = start; s < e;) {
        if (*s == '&') {
            const char *semi = memchr(s, ';', (size_t)(e - s));
            if (semi) {
                size_t l = (size_t)(semi - s - 1);
                const char *ent = s + 1;
                uint32_t cp = 0;
                if (l == 3 && !memcmp(ent, "amp", 3)) cp = '&';
                else if (l == 2 && !memcmp(ent, "lt", 2)) cp = '<';
                else if (l == 2 && !memcmp(ent, "gt", 2)) cp = '>';
                else if (l == 4 && !memcmp(ent, "quot", 4)) cp = '"';
                else if (l == 4 && !memcmp(ent, "apos", 4)) cp = '\'';
                else if (*ent == '#') cp = (uint32_t)(ent[1] == 'x' ? strtoul(ent + 2, NULL, 16) : strtoul(ent + 1, NULL, 10));
                if (cp) {
                    if (cp < 0x80) out[o++] = (char)cp;
                    else if (cp < 0x800) { out[o++] = (char)(0xC0 | cp >> 6); out[o++] = (char)(0x80 | (cp & 0x3F)); }
                    else { out[o++] = (char)(0xE0 | cp >> 12); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
                    s = semi + 1;
                    continue;
                }
            }
        }
        out[o++] = *s++;
    }
    out[o] = 0;
    x->p = e;
    char nm[64];
    read_tag(x, nm, sizeof nm);
    return out;
}

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static char *b64decode(const char *s, size_t *outlen)
{
    size_t n = strlen(s);
    char *out = xmalloc(n * 3 / 4 + 4);
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *s; s++) {
        int v = b64val(*s);
        if (v < 0) continue;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (char)(acc >> bits); }
    }
    *outlen = o;
    return out;
}

static plnode *xml_value(xr *x, int depth);

static plnode *xml_value_tag(xr *x, const char *tag, int kind, int depth)
{
    plnode *n;
    if (!strcmp(tag, "dict")) {
        n = newnode(PL_DICT);
        if (kind == 2) return n;
        char key[1024] = "";
        for (;;) {
            const char *save = x->p;
            char nm[64];
            int k = read_tag(x, nm, sizeof nm);
            if (!k || k == 3) break;
            if (!strcmp(nm, "key")) {
                if (k == 2) { key[0] = 0; continue; }
                char *t = read_text(x, "key");
                snprintf(key, sizeof key, "%s", t);
                free(t);
            } else {
                x->p = save;
                plnode *c = xml_value(x, depth + 1);
                if (!c) break;
                addchild(n, key, c);
            }
        }
        return n;
    }
    if (!strcmp(tag, "array")) {
        n = newnode(PL_ARRAY);
        if (kind == 2) return n;
        for (;;) {
            const char *save = x->p;
            char nm[64];
            int k = read_tag(x, nm, sizeof nm);
            if (!k || k == 3) break;
            x->p = save;
            plnode *c = xml_value(x, depth + 1);
            if (!c) break;
            addchild(n, NULL, c);
        }
        return n;
    }
    if (!strcmp(tag, "true") || !strcmp(tag, "false")) {
        n = newnode(PL_BOOL);
        n->i = tag[0] == 't';
        if (kind == 1) { char nm[64]; read_tag(x, nm, sizeof nm); }
        return n;
    }
    char *text = kind == 2 ? xstrdup("") : read_text(x, tag);
    if (!strcmp(tag, "integer")) { n = newnode(PL_INT); n->i = _strtoi64(text, NULL, 0); free(text); }
    else if (!strcmp(tag, "real")) { n = newnode(PL_REAL); n->r = strtod(text, NULL); free(text); }
    else if (!strcmp(tag, "data")) { n = newnode(PL_DATA); n->str = b64decode(text, &n->len); free(text); }
    else if (!strcmp(tag, "date")) { n = newnode(PL_DATE); n->str = text; }
    else { n = newnode(PL_STRING); n->str = text; n->len = strlen(text); }
    return n;
}

static plnode *xml_value(xr *x, int depth)
{
    if (depth > 256) return NULL;
    char tag[64];
    int k = read_tag(x, tag, sizeof tag);
    if (!k || k == 3) return NULL;
    if (!strcmp(tag, "plist")) return xml_value(x, depth + 1);
    return xml_value_tag(x, tag, k, depth);
}

/* ---------------- binary ---------------- */

typedef struct {
    const uint8_t *b;
    size_t len;
    int offsz, refsz;
    uint64_t nobj, offtab;
} bp;

static uint64_t be(const uint8_t *p, int n)
{
    uint64_t v = 0;
    while (n--) v = v << 8 | *p++;
    return v;
}

static plnode *bp_obj(bp *b, uint64_t idx, int depth);

static int bp_len(bp *b, size_t *pos, int lo, uint64_t *out)
{
    if (lo != 0xF) { *out = (uint64_t)lo; return 0; }
    if (*pos >= b->len) return -1;
    uint8_t m = b->b[(*pos)++];
    if ((m >> 4) != 1) return -1;
    int n = 1 << (m & 0xF);
    if (*pos + (size_t)n > b->len) return -1;
    *out = be(b->b + *pos, n);
    *pos += (size_t)n;
    return 0;
}

static plnode *bp_obj(bp *b, uint64_t idx, int depth)
{
    if (idx >= b->nobj || depth > 256) return NULL;
    size_t tp = (size_t)(b->offtab + idx * (uint64_t)b->offsz);
    if (tp + (size_t)b->offsz > b->len) return NULL;
    size_t pos = (size_t)be(b->b + tp, b->offsz);
    if (pos >= b->len) return NULL;
    uint8_t m = b->b[pos++];
    int hi = m >> 4, lo = m & 0xF;
    plnode *n;
    uint64_t cnt;
    switch (hi) {
    case 0x0:
        n = newnode(PL_BOOL);
        n->i = lo == 9;
        return n;
    case 0x1: {
        int sz = 1 << lo;
        if (pos + (size_t)sz > b->len) return NULL;
        n = newnode(PL_INT);
        n->i = (int64_t)be(b->b + pos, sz);
        return n;
    }
    case 0x2:
    case 0x3: {
        int sz = 1 << lo;
        if (pos + (size_t)sz > b->len) return NULL;
        n = newnode(hi == 2 ? PL_REAL : PL_DATE);
        uint64_t v = be(b->b + pos, sz);
        if (sz == 4) { float f; uint32_t u = (uint32_t)v; memcpy(&f, &u, 4); n->r = f; }
        else memcpy(&n->r, &v, 8);
        return n;
    }
    case 0x4:
    case 0x5:
    case 0x6:
        if (bp_len(b, &pos, lo, &cnt)) return NULL;
        if (hi == 0x6) {
            if (pos + cnt * 2 > b->len) return NULL;
            wchar_t *w = xmalloc((cnt + 1) * sizeof(wchar_t));
            for (uint64_t i = 0; i < cnt; i++) w[i] = (wchar_t)be(b->b + pos + i * 2, 2);
            w[cnt] = 0;
            n = newnode(PL_STRING);
            n->str = w_to_utf8(w);
            n->len = strlen(n->str);
            free(w);
            return n;
        }
        if (pos + cnt > b->len) return NULL;
        n = newnode(hi == 0x4 ? PL_DATA : PL_STRING);
        n->str = xmalloc(cnt + 1);
        memcpy(n->str, b->b + pos, cnt);
        n->str[cnt] = 0;
        n->len = cnt;
        return n;
    case 0x8:
        n = newnode(PL_INT);
        n->i = (int64_t)be(b->b + pos, lo + 1);
        return n;
    case 0xA:
    case 0xC:
    case 0xD: {
        if (bp_len(b, &pos, lo, &cnt)) return NULL;
        int isdict = hi == 0xD;
        if (pos + cnt * (uint64_t)b->refsz * (isdict ? 2 : 1) > b->len) return NULL;
        n = newnode(isdict ? PL_DICT : PL_ARRAY);
        for (uint64_t i = 0; i < cnt; i++) {
            plnode *k = NULL, *v;
            if (isdict) {
                k = bp_obj(b, be(b->b + pos + i * b->refsz, b->refsz), depth + 1);
                v = bp_obj(b, be(b->b + pos + (cnt + i) * b->refsz, b->refsz), depth + 1);
            } else {
                v = bp_obj(b, be(b->b + pos + i * b->refsz, b->refsz), depth + 1);
            }
            if (!v) v = newnode(PL_STRING), v->str = xstrdup("");
            addchild(n, k && k->str ? k->str : NULL, v);
            plist_free(k);
        }
        return n;
    }
    }
    return NULL;
}

plnode *plist_parse(const uint8_t *buf, size_t len)
{
    if (len >= 40 && !memcmp(buf, "bplist00", 8)) {
        const uint8_t *t = buf + len - 32;
        bp b = { buf, len, t[6], t[7], be(t + 8, 8), be(t + 24, 8) };
        if (!b.offsz || !b.refsz) return NULL;
        return bp_obj(&b, be(t + 16, 8), 0);
    }
    xr x = { (const char *)buf, (const char *)buf + len };
    return xml_value(&x, 0);
}

plnode *plist_load(const char *path)
{
    size_t len;
    uint8_t *b = read_file(path, &len);
    if (!b) return NULL;
    plnode *n = plist_parse(b, len);
    free(b);
    return n;
}
