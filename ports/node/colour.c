#include "colour.h"

#include "qr.h"
#include "tern/crypto.h"
#include "ui.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Tern's colours, as the phone apps have them in their dark theme: a deep blue-black ground, the
 * site's teal for what is Tern's own, and a colour each for good, waiting and bad. */
#define BG COLOUR_RGB(0x0A, 0x11, 0x16)
#define SURFACE COLOUR_RGB(0x15, 0x21, 0x2A)
#define RAISED COLOUR_RGB(0x22, 0x33, 0x3F)
#define TEXT COLOUR_RGB(0xE8, 0xEF, 0xF3)
#define DIM COLOUR_RGB(0x8A, 0x9E, 0xAB)
#define ACCENT COLOUR_RGB(0x6C, 0xC3, 0xE6)
#define DEEP COLOUR_RGB(0x0B, 0x4F, 0x6C)
#define GOOD COLOUR_RGB(0x7E, 0xDC, 0xA8)
#define WARN COLOUR_RGB(0xFF, 0xC8, 0x70)
#define BAD COLOUR_RGB(0xFF, 0x8A, 0x65)
#define WHITE COLOUR_RGB(0xFF, 0xFF, 0xFF)
#define BLACK COLOUR_RGB(0x00, 0x00, 0x00)

#define MARGIN 8
#define HEADER 22 /* the heading's rows, its rule included */
#define FOOTER 16 /* the dots' and the hint's */

/* The rows each kind of item takes. */
#define HERO_H 34
#define LINE_H 22
#define SMALL_H 16
#define STATS_H 40
#define METER_H 12
#define TILE_H 44
#define TILE_GAP 5
#define GAP_H 6
#define TEXT_LINE 19 /* a line of a paragraph */
#define BUBBLE_PAD 6

static uint16_t tone_colour(uint8_t tone) {
    switch (tone) {
    case TONE_DIM:
        return DIM;
    case TONE_ACCENT:
        return ACCENT;
    case TONE_GOOD:
        return GOOD;
    case TONE_WARN:
        return WARN;
    case TONE_BAD:
        return BAD;
    default:
        return TEXT;
    }
}

/* --- Pixels ----------------------------------------------------------------------------------- */

/* `fg` over `bg`, `a` sixteenths of the way: 0 is `bg` and 16 is `fg`. */
static uint16_t blend(uint16_t bg, uint16_t fg, unsigned a) {
    if (a >= 16) {
        return fg;
    }
    if (a == 0) {
        return bg;
    }
    unsigned r = ((bg >> 11) * (16 - a) + (fg >> 11) * a) / 16;
    unsigned g = (((bg >> 5) & 0x3F) * (16 - a) + ((fg >> 5) & 0x3F) * a) / 16;
    unsigned b = ((bg & 0x1F) * (16 - a) + (fg & 0x1F) * a) / 16;
    return (uint16_t)(r << 11 | g << 5 | b);
}

/* Paints (x, y) with `col`, `a` sixteenths opaque, if the band holds it. */
static void put(struct canvas *c, int x, int y, uint16_t col, unsigned a) {
    if (x < 0 || x >= c->w || y < c->y0 || y >= c->y0 + c->rows) {
        return;
    }
    uint16_t *p = &c->px[(y - c->y0) * c->w + x];
    *p = blend(*p, col, a);
}

static void fill(struct canvas *c, int x, int y, int w, int h, uint16_t col) {
    int y1 = y + h < c->y0 + c->rows ? y + h : c->y0 + c->rows;
    int x0 = x < 0 ? 0 : x, x1 = x + w < c->w ? x + w : c->w;
    for (int yy = y < c->y0 ? c->y0 : y; yy < y1; yy++) {
        uint16_t *p = &c->px[(yy - c->y0) * c->w];
        for (int xx = x0; xx < x1; xx++) {
            p[xx] = col;
        }
    }
}

/* How much of pixel (px, py), a unit square, a disc of radius `r` about (cx, cy) covers, in
 * sixteenths: near enough, from the distance of its centre to the circle. */
static unsigned disc(float px, float py, float cx, float cy, float r) {
    float dx = px + 0.5f - cx, dy = py + 0.5f - cy;
    float d = r + 0.5f - sqrtf(dx * dx + dy * dy);
    return d <= 0 ? 0 : d >= 1 ? 16 : (unsigned)(d * 16 + 0.5f);
}

/* A rectangle with corners rounded to radius `r`, smooth at their edges. */
static void round_rect(struct canvas *c, int x, int y, int w, int h, int r, uint16_t col) {
    if (r * 2 > h) {
        r = h / 2;
    }
    if (r * 2 > w) {
        r = w / 2;
    }
    int y0 = y < c->y0 ? c->y0 : y, y1 = y + h < c->y0 + c->rows ? y + h : c->y0 + c->rows;
    for (int yy = y0; yy < y1; yy++) {
        int in_top = yy < y + r, in_bottom = yy >= y + h - r;
        if (!in_top && !in_bottom) {
            fill(c, x, yy, w, 1, col);
            continue;
        }
        float cy = in_top ? (float)(y + r) : (float)(y + h - r);
        for (int xx = x; xx < x + w; xx++) {
            if (xx >= x + r && xx < x + w - r) {
                put(c, xx, yy, col, 16);
                continue;
            }
            float cx = xx < x + r ? (float)(x + r) : (float)(x + w - r);
            put(c, xx, yy, col, disc((float)xx, (float)yy, cx, cy, (float)r));
        }
    }
}

/* A ring: a rounded rectangle's outline, `t` pixels thick. */
static void round_outline(struct canvas *c, int x, int y, int w, int h, int r, int t, uint16_t col,
                          uint16_t inside) {
    round_rect(c, x, y, w, h, r, col);
    round_rect(c, x + t, y + t, w - 2 * t, h - 2 * t, r > t ? r - t : 0, inside);
}

static void dot(struct canvas *c, float cx, float cy, float r, uint16_t col) {
    int y0 = (int)(cy - r - 1), y1 = (int)(cy + r + 1);
    for (int y = y0; y <= y1; y++) {
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            put(c, x, y, col, disc((float)x, (float)y, cx, cy, r));
        }
    }
}

/* --- Type ------------------------------------------------------------------------------------- */

const struct font_glyph *font_find(const struct font *f, uint32_t ch) {
    if (ch >= 0x20 && ch < 0x7F) {
        return &f->glyph[ch - 0x20];
    }
    size_t at = 0x7F - 0x20;
    if (f->latin1) {
        if (ch >= 0xA0 && ch <= 0xFF) {
            return &f->glyph[at + ch - 0xA0];
        }
        at += 0x60;
    }
    for (size_t i = 0; i < f->extras; i++) {
        if (f->extra[i] == ch) {
            return &f->glyph[at + i];
        }
    }
    return &f->glyph['?' - 0x20];
}

/* The next character of UTF-8 text, and the bytes it took. */
static uint32_t next_char(const char *p, int *len) {
    const uint8_t *u = (const uint8_t *)p;
    uint32_t ch = u[0];
    int more = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : ch >= 0xC0 ? 1 : 0;
    *len = 1;
    if (ch >= 0x80 && ch < 0xC0) {
        return '?';
    }
    ch &= more == 0 ? 0x7Fu : 0x3Fu >> more;
    for (int i = 1; i <= more; i++) {
        if ((u[i] & 0xC0) != 0x80) {
            return '?';
        }
        ch = ch << 6 | (u[i] & 0x3Fu);
        *len = i + 1;
    }
    return ch == '\n' || ch == '\r' || ch == '\t' ? ' ' : ch;
}

/* The width of the first `n` bytes of `text`. */
static int width_of(const struct font *f, const char *text, size_t n) {
    int w = 0;
    for (size_t i = 0; i < n && text[i] != '\0';) {
        int len;
        w += font_find(f, next_char(&text[i], &len))->adv;
        i += (size_t)len;
    }
    return w;
}

static void glyph(struct canvas *c, const struct font_glyph *g, const uint8_t *bits, int x, int top,
                  uint16_t col) {
    top += g->y;
    if (top + g->h <= c->y0 || top >= c->y0 + c->rows) {
        return;
    }
    for (int j = 0; j < g->h; j++) {
        for (int i = 0; i < g->w; i++) {
            uint32_t k = g->bits + (uint32_t)(j * g->w + i);
            unsigned a = k & 1 ? bits[k / 2] & 0x0F : bits[k / 2] >> 4;
            if (a != 0) {
                put(c, x + g->x + i, top + j, col, a + (a >> 3)); /* 15 to 16 */
            }
        }
    }
}

/* Draws `n` bytes of `text` with its baseline at `base`, from `x`; returns where the pen ends. */
static int draw_n(struct canvas *c, const struct font *f, int x, int base, const char *text,
                  size_t n, uint16_t col) {
    int top = base - f->ascent;
    for (size_t i = 0; i < n && text[i] != '\0';) {
        int len;
        const struct font_glyph *g = font_find(f, next_char(&text[i], &len));
        glyph(c, g, f->bits, x, top, col);
        x += g->adv;
        i += (size_t)len;
    }
    return x;
}

/* Draws `text` from `x`, cut with an ellipsis to fit `maxw`; returns its width as drawn. */
static int draw(struct canvas *c, const struct font *f, int x, int base, const char *text, int maxw,
                uint16_t col) {
    size_t n = strlen(text);
    int w = width_of(f, text, n);
    if (w <= maxw) {
        draw_n(c, f, x, base, text, n, col);
        return w;
    }
    const char *dots = f->extras > 0 ? "\xE2\x80\xA6" : "...";
    int dw = width_of(f, dots, strlen(dots));
    while (n > 0) {
        n--;
        while (n > 0 && ((uint8_t)text[n] & 0xC0) == 0x80) {
            n--;
        }
        if (width_of(f, text, n) + dw <= maxw) {
            break;
        }
    }
    while (n > 0 && text[n - 1] == ' ') {
        n--;
    }
    int end = draw_n(c, f, x, base, text, n, col);
    end = draw_n(c, f, end, base, dots, strlen(dots), col);
    return end - x;
}

static void centred(struct canvas *c, const struct font *f, int x, int w, int base,
                    const char *text, uint16_t col) {
    int tw = width_of(f, text, strlen(text));
    if (tw > w) {
        draw(c, f, x, base, text, w, col);
        return;
    }
    draw_n(c, f, x + (w - tw) / 2, base, text, strlen(text), col);
}

static void icon(struct canvas *c, int size, uint8_t which, int x, int y, uint16_t col) {
    if (which == ICON_NONE || which >= ICON_COUNT) {
        return;
    }
    const struct font_glyph *g = size == ICON_LARGE ? &icon_large[which] : &icon_small[which];
    glyph(c, g, size == ICON_LARGE ? icon_large_bits : icon_small_bits, x, y, col);
}

/* The bytes of `p`, which starts a line, that the line takes at width `w`: up to the last word
 * that fits, or as much of a word wider than the line as fits. */
static size_t line_fit(const struct font *f, int w, const char *p) {
    size_t take = 0, fit = 0, len = strlen(p);
    while (take < len) {
        int cl;
        (void)next_char(&p[take], &cl);
        size_t next = take + (size_t)cl;
        if (width_of(f, p, next) > w) {
            break;
        }
        take = next;
        if (take == len || p[take] == ' ' || p[take] == '\n') {
            fit = take;
        }
    }
    return take == len || fit == 0 ? take : fit;
}

static const char *line_start(const char *p) {
    while (*p == ' ' || *p == '\n') {
        p++;
    }
    return p;
}

/* The lines `text` takes at width `w`, up to `most`. */
static int lines_of(const struct font *f, int w, int most, const char *text) {
    const char *p = line_start(text);
    int l = 0;
    while (*p != '\0' && l < most) {
        p = line_start(p + line_fit(f, w, p));
        l++;
    }
    return l;
}

/* Breaks `text` into lines that fit `w`, between words where it can be and within a word wider
 * than a line, and draws at most `lines` of them: the first, the last cut with an ellipsis if more
 * is left; or with `tail`, the last, as a message being typed shows where it is typed. */
static void paragraph(struct canvas *c, const struct font *f, int x, int y, int w, int lines,
                      const char *text, uint16_t col, bool centre, bool tail) {
    const char *p = line_start(text);
    if (tail) {
        for (int skip = lines_of(f, w, INT_MAX, p) - lines; skip > 0; skip--) {
            p = line_start(p + line_fit(f, w, p));
        }
    }
    for (int l = 0; l < lines && *p != '\0'; l++) {
        p = line_start(p);
        size_t fit = line_fit(f, w, p);
        int base = y + l * TEXT_LINE + f->ascent - 2;
        bool last = l == lines - 1 && p[fit] != '\0';
        char line[SCENE_TEXT_MAX];
        size_t n = last ? strlen(p) : fit;
        if (n >= sizeof line) {
            n = sizeof line - 1;
        }
        memcpy(line, p, n);
        line[n] = '\0';
        if (centre) {
            centred(c, f, x, w, base, line, col);
        } else {
            draw(c, f, x, base, line, w, col);
        }
        p += fit;
    }
}

/* --- The page's head and foot ----------------------------------------------------------------- */

/* A battery, filled as far as it is charged, with the charge beside it; returns its left. */
static int battery(struct canvas *c, const struct scene *s, int right) {
    if (s->battery == UI_BATTERY_UNKNOWN) {
        return right;
    }
    uint16_t col = s->charging ? GOOD : s->battery <= UI_BATTERY_LOW ? BAD : TEXT;
    int x = right - 21, y = 6;
    round_outline(c, x, y, 19, 10, 3, 1, DIM, BG);
    fill(c, x + 19, y + 3, 2, 4, DIM);
    int part = (int)s->battery * 15 / 100;
    if (part < 1) {
        part = 1;
    }
    round_rect(c, x + 2, y + 2, part, 6, 1, col);
    if (s->charging) {
        icon(c, ICON_SMALL, ICON_BOLT, x - 14, 3, GOOD);
        x -= 14;
    }
    char pc[8];
    pc[0] = (char)('0' + s->battery / 100 % 10);
    pc[1] = (char)('0' + s->battery / 10 % 10);
    pc[2] = (char)('0' + s->battery % 10);
    pc[3] = '%';
    pc[4] = '\0';
    const char *t = s->battery >= 100 ? pc : s->battery >= 10 ? &pc[1] : &pc[2];
    int w = width_of(&font_small, t, strlen(t));
    draw_n(c, &font_small, x - 3 - w, 15, t, strlen(t), s->battery <= UI_BATTERY_LOW ? BAD : DIM);
    return x - 3 - w;
}

static void header(struct canvas *c, const struct scene *s) {
    if (c->y0 >= HEADER) {
        return;
    }
    int x = MARGIN;
    if (s->title_icon != ICON_NONE) {
        uint16_t col = s->title_icon == ICON_WARNING || s->title_icon == ICON_ERROR ? WARN : ACCENT;
        icon(c, ICON_SMALL, s->title_icon, x - 1, 3, col);
        x += 20;
    }
    int right = c->w - MARGIN;
    if (s->status) {
        right = battery(c, s, right) - 6;
        if (s->phone) {
            icon(c, ICON_SMALL, ICON_BLUETOOTH, right - 14, 3, ACCENT);
            right -= 18;
        }
        if (s->unread > 0) {
            char n[4];
            unsigned u = s->unread > 99 ? 99 : s->unread;
            n[0] = (char)(u >= 10 ? '0' + u / 10 : '0' + u);
            n[1] = (char)(u >= 10 ? '0' + u % 10 : '\0');
            n[2] = '\0';
            int w = width_of(&font_small, n, strlen(n)) + 10;
            round_rect(c, right - w, 4, w, 14, 7, ACCENT);
            draw_n(c, &font_small, right - w + 5, 15, n, strlen(n), BG);
            right -= w + 6;
        }
    }
    if (s->title_right[0] != '\0') {
        int w = width_of(&font_small, s->title_right, strlen(s->title_right));
        draw_n(c, &font_small, right - w, 15, s->title_right, strlen(s->title_right), DIM);
        right -= w + 8;
    }
    draw(c, &font_small, x, 15, s->title, right - x, TEXT);
    fill(c, 0, HEADER - 1, c->w, 1, SURFACE);
}

static void footer(struct canvas *c, const struct scene *s, int left) {
    int top = c->h - FOOTER;
    if (c->y0 + c->rows <= top) {
        return;
    }
    float x = (float)left + 3;
    float cy = (float)top + FOOTER / 2.0f;
    for (int k = 0; k < s->dots; k++) {
        if (k == s->dot) {
            round_rect(c, (int)x - 3, (int)cy - 3, 16, 6, 3, ACCENT);
            x += 21;
        } else {
            dot(c, x, cy - 0.5f, 2.5f, RAISED);
            x += 10;
        }
    }
    if (s->hint[0] != '\0') {
        int w = width_of(&font_small, s->hint, strlen(s->hint));
        int at = c->w - MARGIN - w;
        int least = s->dots > 0 ? (int)x + 4 : left;
        draw(c, &font_small, at < least ? least : at, top + 12, s->hint, c->w - MARGIN - least,
             DIM);
    }
}

/* --- The items -------------------------------------------------------------------------------- */

static int height_of(const struct scene_item *it) {
    switch (it->kind) {
    case SCENE_HERO:
        return HERO_H;
    case SCENE_LINE:
        return LINE_H;
    case SCENE_SMALL:
        return SMALL_H;
    case SCENE_STATS:
        return STATS_H;
    case SCENE_METER:
        return METER_H;
    case SCENE_GAP:
        return GAP_H;
    default:
        return 0; /* text and tiles are measured with the others */
    }
}

/* Signal bars for how well a node is heard: four, lit as far as its SNR earns. */
static int bars(struct canvas *c, int right, int y, int8_t snr) {
    int lit = snr >= 5 ? 4 : snr >= -2 ? 3 : snr >= -9 ? 2 : 1;
    uint16_t col = lit >= 3 ? GOOD : lit == 2 ? WARN : BAD;
    int x = right - 4 * 5 + 2;
    for (int k = 0; k < 4; k++) {
        int h = 4 + 3 * k;
        round_rect(c, x + k * 5, y + 15 - h, 3, h, 1, k < lit ? col : RAISED);
    }
    return x;
}

static void line(struct canvas *c, const struct scene_item *it, int x, int w, int y) {
    uint16_t col = tone_colour(it->tone);
    if (it->centre) {
        int tw = width_of(&font_body, it->left, strlen(it->left));
        int iw = it->icon != ICON_NONE ? 22 : 0;
        int at = x + (w - tw - iw) / 2;
        if (at < x) {
            at = x;
        }
        icon(c, ICON_SMALL, it->icon, at, y + 3, it->tone == TONE_TEXT ? ACCENT : col);
        draw(c, &font_body, at + iw, y + 16, it->left, x + w - at - iw, col);
        return;
    }
    if (it->selected) {
        round_rect(c, x - 4, y, w + 8, LINE_H, 6, RAISED);
    }
    int at = x;
    if (it->icon != ICON_NONE) {
        icon(c, ICON_SMALL, it->icon, at, y + 3, it->tone == TONE_TEXT ? ACCENT : col);
        at += 22;
    }
    int right = x + w;
    if (it->right[0] != '\0') {
        int rw = width_of(&font_small, it->right, strlen(it->right));
        draw_n(c, &font_small, right - rw, y + 15, it->right, strlen(it->right), DIM);
        right -= rw + 6;
    }
    if (it->signal) {
        right = bars(c, right, y + 1, it->snr_db) - 6;
    }
    draw(c, &font_body, at, y + 16, it->left, right - at, col);
}

static void stats(struct canvas *c, const struct scene *s, int x, int w, int y) {
    if (s->stats == 0) {
        return;
    }
    int gap = 6, tw = (w - gap * (s->stats - 1)) / s->stats;
    for (int k = 0; k < s->stats; k++) {
        const struct scene_stat *t = &s->stat[k];
        int tx = x + k * (tw + gap);
        round_rect(c, tx, y, tw, STATS_H - 2, 8, SURFACE);
        centred(c, &font_bold, tx, tw, y + 20, t->value, tone_colour(t->tone));
        centred(c, &font_small, tx, tw, y + 33, t->label, DIM);
    }
}

static void meter(struct canvas *c, const struct scene_item *it, int x, int w, int y) {
    round_rect(c, x, y + 2, w, 8, 4, RAISED);
    if (it->part > 0 && it->whole > 0) {
        int64_t fillw = it->part >= it->whole ? w : (int64_t)it->part * w / it->whole;
        if (fillw < 8) {
            fillw = 8;
        }
        round_rect(c, x, y + 2, (int)fillw, 8, 4, tone_colour(it->tone));
    }
}

static void tile(struct canvas *c, const struct scene_item *it, int x, int w, int y) {
    uint16_t col = tone_colour(it->tone);
    if (it->selected) {
        round_outline(c, x, y, w, TILE_H, 9, 2, ACCENT, DEEP);
    } else {
        round_rect(c, x, y, w, TILE_H, 9, SURFACE);
    }
    const struct font_glyph *g = &icon_large[it->icon < ICON_COUNT ? it->icon : 0];
    icon(c, ICON_LARGE, it->icon, x + (w - ICON_LARGE) / 2, y + 3,
         it->tone == TONE_TEXT ? (it->selected ? WHITE : ACCENT) : col);
    (void)g;
    centred(c, &font_small, x + 2, w - 4, y + TILE_H - 7, it->left,
            it->selected            ? WHITE
            : it->tone == TONE_TEXT ? TEXT
                                    : col);
}

/* --- The page --------------------------------------------------------------------------------- */

static struct qr code;
static char code_of[TERN_GROUP_LINK_MAX + 1]; /* the link `code` holds, or empty */

void colour_forget(void) {
    tern_wipe(&code, sizeof code);
    tern_wipe(code_of, sizeof code_of);
}

/* The scene's QR code, dark on white, `scale` pixels a module, in a white square from (x, y). */
static void qr_code(struct canvas *c, const struct scene *s, int x, int y, int side, int scale) {
    if (strcmp(code_of, s->qr) != 0) {
        if (!qr_encode(&code, s->qr, QR_MASK_BEST)) {
            colour_forget();
            return;
        }
        memcpy(code_of, s->qr, sizeof code_of);
    }
    round_rect(c, x, y, side, side, 4, WHITE);
    int in = (side - QR_SIZE * scale) / 2;
    for (int my = 0; my < QR_SIZE; my++) {
        int py = y + in + my * scale;
        if (py + scale <= c->y0 || py >= c->y0 + c->rows) {
            continue;
        }
        for (int mx = 0; mx < QR_SIZE; mx++) {
            if (qr_dark(&code, mx, my)) {
                fill(c, x + in + mx * scale, py, scale, scale, BLACK);
            }
        }
    }
}

void colour_draw(const struct scene *s, struct canvas *c) {
    fill(c, 0, c->y0, c->w, c->rows, BG);
    bool head = s->title[0] != '\0';
    bool foot = s->dots > 0 || s->hint[0] != '\0';
    int top = head ? HEADER + 1 : 4;
    int bottom = c->h - (foot ? FOOTER : 4);
    int x = MARGIN, w = c->w - 2 * MARGIN;

    if (head) {
        header(c, s);
    }
    if (s->qr[0] != '\0') {
        /* As large as the panel's height allows, down past the foot, with a margin round it a
         * phone's camera finds it by; the foot goes beside it. */
        int side = c->h - top - 3;
        int scale = (side - 8) / QR_SIZE;
        qr_code(c, s, x - 3, top + 1, side, scale);
        x += side + 4;
        w = c->w - MARGIN - x;
        if (foot) {
            footer(c, s, x);
        }
        bottom = c->h - (foot ? FOOTER : 4);
    } else if (foot) {
        footer(c, s, MARGIN);
    }
    if (s->qr[0] == '\0' && code_of[0] != '\0') {
        colour_forget(); /* the code is no longer shown */
    }

    /* The height of what is fixed, the tiles' rows, and what is left for the paragraph. */
    int fixed = 0, tiles = 0, texts = 0;
    for (int i = 0; i < s->n; i++) {
        const struct scene_item *it = &s->item[i];
        if (it->on == ON_MONO) {
            continue;
        }
        tiles += it->kind == SCENE_TILE;
        texts += it->kind == SCENE_TEXT;
        fixed += height_of(it);
    }
    int tile_rows = (tiles + 2) / 3;
    fixed += tile_rows > 0 ? tile_rows * TILE_H + (tile_rows - 1) * TILE_GAP : 0;
    int room = bottom - top - fixed;
    int text_h = 0, text_lines = 0;
    bool bubble = false;
    for (int i = 0; i < s->n; i++) {
        if (s->item[i].kind == SCENE_TEXT && s->item[i].on != ON_MONO) {
            bubble = s->item[i].bubble;
        }
    }
    if (texts > 0) {
        int pad = bubble ? 2 * BUBBLE_PAD : 0;
        int most = (room - pad) / TEXT_LINE;
        int tw = bubble ? w - 2 * BUBBLE_PAD - 4 : w;
        text_lines = lines_of(&font_body, tw, most < 1 ? 1 : most, s->text);
        text_h = text_lines * TEXT_LINE + pad;
        room -= text_h;
    }
    /* What is left over: a page with little on it is set in the middle of its room. */
    int y = top + (room > 0 && tiles == 0 ? room / 2 : 0);
    bool centre = !head; /* a screen with no heading is a sign: centred */
    if (s->qr[0] != '\0') {
        y = top + 2 + (room > 0 ? room / 2 : 0);
    }

    int tile_k = 0, tile_w = (w - 2 * TILE_GAP) / 3;
    for (int i = 0; i < s->n; i++) {
        const struct scene_item *it = &s->item[i];
        if (it->on == ON_MONO) {
            continue;
        }
        int h = height_of(it);
        if (y >= c->y0 + c->rows) {
            break;
        }
        switch (it->kind) {
        case SCENE_HERO: {
            const struct font *f = s->qr[0] != '\0' ? &font_bold : &font_hero;
            int base = s->qr[0] != '\0' ? y + 22 : y + 28;
            if (s->qr[0] != '\0') {
                h = 26;
                draw(c, f, x, base, it->left, w, tone_colour(it->tone));
            } else {
                centred(c, f, x, w, base, it->left, tone_colour(it->tone));
            }
            break;
        }
        case SCENE_LINE:
            line(c, it, x, w, y);
            break;
        case SCENE_SMALL:
            if (centre) {
                centred(c, &font_small, x, w, y + 12, it->left, tone_colour(it->tone));
            } else {
                draw(c, &font_small, x, y + 12, it->left, w, tone_colour(it->tone));
            }
            break;
        case SCENE_STATS:
            stats(c, s, x, w, y);
            break;
        case SCENE_METER:
            meter(c, it, x, w, y);
            break;
        case SCENE_TEXT:
            h = text_h;
            if (it->bubble) {
                uint16_t ground = it->tone == TONE_ACCENT ? DEEP : SURFACE;
                round_rect(c, x, y, w, h, 10, ground);
                paragraph(c, &font_body, x + BUBBLE_PAD + 2, y + BUBBLE_PAD, w - 2 * BUBBLE_PAD - 4,
                          text_lines, s->text, TEXT, false, it->tail);
            } else {
                paragraph(c, &font_body, x, y, w, text_lines, s->text, tone_colour(it->tone),
                          centre || it->centre, it->tail);
            }
            break;
        case SCENE_TILE: {
            int col = tile_k % 3, row = tile_k / 3;
            tile(c, it, x + col * (tile_w + TILE_GAP), tile_w, y + row * (TILE_H + TILE_GAP));
            tile_k++;
            /* The tiles' rows are counted once, after the last. */
            h = tile_k == tiles ? tile_rows * TILE_H + (tile_rows - 1) * TILE_GAP : 0;
            break;
        }
        default:
            break;
        }
        y += h;
    }
}
