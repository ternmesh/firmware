#include "scene.h"

#include "qr.h"
#include "tern/crypto.h"
#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define COLS 21     /* characters across the 128-pixel screen, six pixels each */
#define DOTS_COLS 4 /* a heading's end the page dots take: from column 108, and a gap before */
#define NO_ROW (-1)

void scene_clear(struct scene *s) {
    memset(s, 0, sizeof *s);
    s->battery = UI_BATTERY_UNKNOWN;
}

struct scene_item *scene_add(struct scene *s, enum scene_kind kind, enum scene_tone tone,
                             const char *fmt, ...) {
    static struct scene_item scratch;
    struct scene_item *it = s->n < SCENE_ITEMS ? &s->item[s->n++] : &scratch;
    memset(it, 0, sizeof *it);
    it->kind = (uint8_t)kind;
    it->tone = (uint8_t)tone;
    if (fmt != NULL) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(it->left, sizeof it->left, fmt, ap);
        va_end(ap);
    }
    return it;
}

struct scene_item *scene_stat(struct scene *s, enum scene_tone tone, const char *value,
                              const char *label) {
    static struct scene_item scratch;
    struct scene_item *at = &scratch;
    for (int i = 0; i < s->n; i++) {
        if (s->item[i].kind == SCENE_STATS) {
            at = &s->item[i];
        }
    }
    if (s->stats == 0) {
        at = scene_add(s, SCENE_STATS, TONE_TEXT, NULL);
    }
    if (s->stats < SCENE_STATS_MAX) {
        struct scene_stat *t = &s->stat[s->stats++];
        snprintf(t->value, sizeof t->value, "%s", value);
        snprintf(t->label, sizeof t->label, "%s", label);
        t->tone = (uint8_t)tone;
    }
    return at;
}

/* --- On a 128x64 monochrome screen ------------------------------------------------------------ */

/* UTF-8 as the screen's font can draw it: ASCII as it is, the few punctuation marks the pages use
 * as their nearest ASCII, anything else as '?', and line breaks and tabs as spaces. */
static void ascii(char *out, size_t size, const char *in) {
    size_t n = 0;
    const uint8_t *p = (const uint8_t *)in;
    while (*p != '\0' && n + 1 < size) {
        uint32_t c = *p++;
        if (c >= 0xC0) {
            int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            c &= 0x3Fu >> more;
            for (; more > 0 && (*p & 0xC0) == 0x80; more--) {
                c = c << 6 | (*p++ & 0x3Fu);
            }
        } else if (c >= 0x80) {
            continue; /* a stray continuation byte */
        }
        const char *as = NULL;
        switch (c) {
        case 0x2026:
            as = "...";
            break;
        case 0x203A:
            as = ">";
            break;
        case 0x00B7:
        case 0x2022:
        case 0x2013:
        case 0x2014:
            as = "-";
            break;
        case 0x2018:
        case 0x2019:
            as = "'";
            break;
        case 0x201C:
        case 0x201D:
            as = "\"";
            break;
        default:
            break;
        }
        if (as != NULL) {
            for (; *as != '\0' && n + 1 < size; as++) {
                out[n++] = *as;
            }
        } else if (c == '\n' || c == '\r' || c == '\t') {
            out[n++] = ' ';
        } else if ((c >= 0x20 && c < 0x7F) || c == (uint8_t)DISPLAY_BLUETOOTH) {
            out[n++] = (char)c;
        } else {
            out[n++] = '?';
        }
    }
    out[n] = '\0';
}

/* The picture as it is composed: its lines of text, and the rows a large line or a bar takes
 * instead. It is drawn whole once composed, so a page that comes out as it was is not sent. */
struct frame {
    char rows[DISPLAY_PAGES][COLS + 1];
    bool inverse[DISPLAY_PAGES];
    int big; /* the first of the two rows the large line takes, or NO_ROW */
    char big_text[DISPLAY_BIG_COLS + 1];
    int bar; /* the row the bar takes, or NO_ROW */
    int64_t part, whole;
};

/* A line of `cols` with `left` at its start and `right` at its end; `left` gives way if both do
 * not fit. Both are UTF-8. */
static void ends(char *out, int cols, const char *left, const char *right) {
    char l[64], r[32];
    ascii(l, sizeof l, left);
    ascii(r, sizeof r, right);
    int room = cols - (int)strlen(r);
    if (room < 0) {
        room = 0;
        r[cols] = '\0';
    }
    /* Where the left is cut, a space keeps it from running into the right. */
    int keep = (int)strlen(l) >= room && r[0] != '\0' && r[0] != ' ' && room > 0 ? room - 1 : room;
    snprintf(out, COLS + 1, "%-*.*s%s", room, keep, l, r);
}

/* Fills rows `first` to `last` with `text`, broken between words where it can be and within a
 * word longer than a line. Text that does not fit ends its last row with "...". */
static void wrap(struct frame *f, int first, int last, const char *utf8) {
    char text[SCENE_TEXT_MAX * 3];
    ascii(text, sizeof text, utf8);
    const char *p = text;
    for (int r = first; r <= last; r++) {
        while (*p == ' ') {
            p++;
        }
        size_t left = strlen(p);
        if (left == 0) {
            return;
        }
        size_t take = left;
        if (left > COLS) {
            take = COLS;
            while (take > 0 && p[take] != ' ') {
                take--;
            }
            if (take == 0) {
                take = COLS; /* one word wider than the screen */
            }
        }
        memcpy(f->rows[r], p, take);
        f->rows[r][take] = '\0';
        p += take;
        if (r == last) {
            while (*p == ' ') {
                p++;
            }
            if (*p != '\0') {
                size_t at = take > COLS - 3 ? COLS - 3 : take;
                memcpy(&f->rows[r][at], "...", 4);
            }
        }
    }
}

/* The rows an item takes on this screen. */
static int rows_of(const struct scene_item *it) {
    if (it->on == ON_COLOUR) {
        return 0;
    }
    return it->kind == SCENE_HERO ? 2 : 1;
}

/* The heading's end, where nothing else is put there: the battery, and a phone connected. */
static void status_text(const struct scene *s, char *out, size_t size) {
    out[0] = '\0';
    if (s->battery != UI_BATTERY_UNKNOWN) {
        const char *how = s->charging ? "Chg " : s->battery <= UI_BATTERY_LOW ? "Low " : "";
        snprintf(out, size, "%s%u%%", how, (unsigned)s->battery);
    }
}

/* The page dots, dark on the lit heading at its end: the page shown a block, the others a dot. */
static void dots(const struct scene *s, struct display *d) {
    for (int k = 0; k < s->dots; k++) {
        int x = DISPLAY_WIDTH - (s->dots - k) * 4;
        bool shown = k == s->dot;
        for (int dx = 0; dx < (shown ? 3 : 2); dx++) {
            for (int y = shown ? 2 : 3; y <= (shown ? 5 : 4); y++) {
                display_set(d, x + dx, y, false);
            }
        }
    }
}

/* Every picture is drawn whole here first, and then only what changed is sent. */
static struct display canvas;

#define QR_SCALE 2   /* pixels to a module: 58 of the screen's 64 rows */
#define QR_GROUND 70 /* the lit block the code sits in, from the left edge */
#define QR_LEFT ((QR_GROUND - QR_SIZE * QR_SCALE) / 2)
#define QR_TOP ((DISPLAY_PAGES * 8 - QR_SIZE * QR_SCALE) / 2)
#define QR_WORDS (QR_GROUND + 4) /* where the words beside it start */

/* A link as a QR code, dark on light, as every scanner reads it, which on this screen means a lit
 * block with the dark modules left unlit; the light margin round it is as wide as the screen
 * leaves. Beside it, the scene's `qr_words`. Drawn whole into a scratch picture, so only what
 * changed is sent; the picture and the code are wiped after, since a join code's is a group's
 * secret. */
static void code_page(const struct scene *s, struct display *d) {
    static struct qr code;
    display_init(&canvas);
    if (!qr_encode(&code, s->qr, QR_MASK_BEST)) {
        display_text(&canvas, 3, "No code to show", false);
        display_copy(d, &canvas);
        return;
    }
    for (int y = 0; y < DISPLAY_PAGES * 8; y++) {
        for (int x = 0; x < QR_GROUND; x++) {
            int mx = (x - QR_LEFT) / QR_SCALE, my = (y - QR_TOP) / QR_SCALE;
            bool inside = x >= QR_LEFT && y >= QR_TOP && mx < QR_SIZE && my < QR_SIZE;
            display_set(&canvas, x, y, !(inside && qr_dark(&code, mx, my)));
        }
    }
    for (int r = 0; r < DISPLAY_PAGES; r++) {
        if (s->qr_words[r] != NULL) {
            char w[COLS + 1];
            ascii(w, sizeof w, s->qr_words[r]);
            display_text_at(&canvas, r, QR_WORDS, w);
        }
    }
    display_copy(d, &canvas);
    tern_wipe(&code, sizeof code);
    tern_wipe(&canvas, sizeof canvas);
}

void mono_draw(const struct scene *s, struct display *d) {
    static struct frame f;
    if (s->qr[0] != '\0') {
        code_page(s, d);
        return;
    }
    display_init(&canvas);
    memset(&f, 0, sizeof f);
    f.big = NO_ROW;
    f.bar = NO_ROW;
    int row = 0;
    if (s->title[0] != '\0') {
        char left[32], right[24];
        int cols = s->dots > 0 ? COLS - DOTS_COLS : COLS;
        snprintf(left, sizeof left, "%s%s", s->title,
                 s->status && s->phone ? " " DISPLAY_BLUETOOTH_S : "");
        if (s->title_right[0] != '\0') {
            snprintf(right, sizeof right, "%s", s->title_right);
        } else if (s->status) {
            status_text(s, right, sizeof right);
        } else {
            right[0] = '\0';
        }
        ends(f.rows[0], cols, left, right);
        f.inverse[0] = true;
        row = 1;
    }

    /* The rows each item takes, and how many the text has: what the others leave it. */
    int fixed = 0, texts = 0;
    for (int i = 0; i < s->n; i++) {
        const struct scene_item *it = &s->item[i];
        if (it->on == ON_COLOUR) {
            continue;
        }
        if (it->kind == SCENE_TEXT) {
            texts++;
        } else {
            fixed += rows_of(it);
        }
    }
    int text_rows = DISPLAY_PAGES - row - fixed;
    if (texts > 0) {
        text_rows /= texts;
    }

    for (int i = 0; i < s->n && row < DISPLAY_PAGES; i++) {
        const struct scene_item *it = &s->item[i];
        if (it->on == ON_COLOUR) {
            continue;
        }
        switch (it->kind) {
        case SCENE_HERO:
            if (row + 1 < DISPLAY_PAGES) {
                f.big = row;
                ascii(f.big_text, sizeof f.big_text, it->left);
            }
            row += 2;
            break;
        case SCENE_TEXT:
            if (text_rows > 0) {
                wrap(&f, row, row + text_rows - 1, s->text);
                row += text_rows;
            }
            break;
        case SCENE_STATS: {
            char line[64] = "";
            for (int k = 0; k < s->stats; k++) {
                size_t at = strlen(line);
                snprintf(&line[at], sizeof line - at, "%s%s %s", k > 0 ? ", " : "",
                         s->stat[k].value, s->stat[k].label);
            }
            ends(f.rows[row++], COLS, line, "");
            break;
        }
        case SCENE_METER:
            f.bar = row++;
            f.part = it->part;
            f.whole = it->whole;
            break;
        case SCENE_GAP:
            row++;
            break;
        case SCENE_LINE: {
            char right[40];
            if (it->signal) {
                snprintf(right, sizeof right, " %+ddB %s", (int)it->snr_db, it->right);
            } else if (it->right[0] != '\0') {
                snprintf(right, sizeof right, " %s", it->right);
            } else {
                right[0] = '\0';
            }
            ends(f.rows[row], COLS, it->left, right);
            f.inverse[row++] = it->selected;
            break;
        }
        default: /* a small line, or a tile: a line of the menu */
            ends(f.rows[row], COLS, it->left, "");
            f.inverse[row++] = it->selected;
            break;
        }
    }
    /* What a hold does, at the foot, where the items left it room. */
    if (s->hint[0] != '\0' && f.rows[DISPLAY_PAGES - 1][0] == '\0' && f.big != DISPLAY_PAGES - 2 &&
        f.bar != DISPLAY_PAGES - 1) {
        ends(f.rows[DISPLAY_PAGES - 1], COLS, s->hint, "");
    }

    for (int i = 0; i < DISPLAY_PAGES; i++) {
        if (i == f.big) {
            display_big(&canvas, i, f.big_text);
            i++;
        } else if (i == f.bar) {
            display_bar(&canvas, i, f.part, f.whole);
        } else {
            display_text(&canvas, i, f.rows[i], f.inverse[i]);
        }
    }
    if (s->title[0] != '\0' && s->dots > 0) {
        dots(s, &canvas);
    }
    display_copy(d, &canvas);
}
