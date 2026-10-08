#include "ui.h"

#include "qr.h"
#include "tern/share.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define COLS 21 /* characters across the 128-pixel screen, six pixels each */
#define NO_ROW (-1)

/* A page as it is composed: its lines of text, and the rows a large line or a bar takes instead.
 * It is drawn whole once it is composed, so a page that comes out as it was is not sent again. */
struct frame {
    char rows[DISPLAY_PAGES][COLS + 1];
    int big;           /* the first of the two rows the large line takes, or NO_ROW */
    char big_text[24]; /* drawn as far as DISPLAY_BIG_COLS of it */
    int bar;           /* the row the bar takes, or NO_ROW */
    int64_t part, whole;
};

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static void
line(char *out, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, COLS + 1, fmt, ap);
    va_end(ap);
}

/* A line with `left` at its start and `right` at its end; `left` gives way if both do not fit. */
static void ends(char *out, const char *left, const char *right) {
    int room = COLS - (int)strlen(right);
    if (room < 0) {
        room = 0;
    }
    line(out, "%-*.*s%s", room, room, left, right);
}

/* "45s", "12m", "3h", "9d": a span in two or three characters. */
static void span(char *buf, size_t size, uint32_t s) {
    if (s < 60) {
        snprintf(buf, size, "%lus", (unsigned long)s);
    } else if (s < 3600) {
        snprintf(buf, size, "%lum", (unsigned long)(s / 60));
    } else if (s < 86400) {
        snprintf(buf, size, "%luh", (unsigned long)(s / 3600));
    } else {
        snprintf(buf, size, "%lud", (unsigned long)(s / 86400));
    }
}

/* UTF-8 as the font can draw it: one character for each character of the text, ASCII as it is,
 * anything else as '?', and line breaks and tabs as spaces. Returns the length written. */
static size_t ascii(char *out, size_t size, const uint8_t *in, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len && n + 1 < size; i++) {
        uint8_t c = in[i];
        if (c >= 0x80 && c < 0xC0) {
            continue; /* the rest of a character already written */
        }
        if (c == '\n' || c == '\r' || c == '\t') {
            out[n++] = ' ';
        } else if (c >= 0x20 && c < 0x7F) {
            out[n++] = (char)c;
        } else {
            out[n++] = '?';
        }
    }
    out[n] = '\0';
    return n;
}

/* The length of a name, which ends at its NUL or at UI_NAME bytes, whichever comes first. */
static size_t name_len(const char *name) {
    const char *end = memchr(name, '\0', UI_NAME + 1);
    return end != NULL ? (size_t)(end - name) : UI_NAME + 1;
}

/* Fills rows `first` to `last` with `text`, broken between words where it can be and within a
 * word longer than a line. Text that does not fit ends its last row with "...". */
static void wrap(struct frame *f, int first, int last, const char *text) {
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

/* The share of the air left, as a whole percentage, rounded down so that it never says more is
 * left than is. */
static unsigned left_percent(const struct ui_node *n) {
    if (n->air_limit_ms <= 0 || n->air_used_ms >= n->air_limit_ms) {
        return 0;
    }
    int64_t left = n->air_limit_ms - n->air_used_ms;
    if (n->air_limit_ms <= INT64_MAX / 100) {
        return (unsigned)(left * 100 / n->air_limit_ms);
    }
    return (unsigned)(left / (n->air_limit_ms / 100));
}

static void home(const struct ui_node *n, struct frame *f) {
    char wait[16], right[24];
    if (n->battery != UI_BATTERY_UNKNOWN) {
        snprintf(right, sizeof right, "%u%% %s", (unsigned)n->battery, n->region);
    } else {
        snprintf(right, sizeof right, "%s", n->region);
    }
    ends(f->rows[0], "Tern", right);
    f->big = 2;
    if (n->bench) {
        snprintf(f->big_text, sizeof f->big_text, "Bench mode");
    } else if (n->unread > 0) {
        snprintf(f->big_text, sizeof f->big_text, "%u new", (unsigned)n->unread);
    } else if (n->nearby == 0) {
        snprintf(f->big_text, sizeof f->big_text, "Listening");
    } else {
        snprintf(f->big_text, sizeof f->big_text, "%u nearby", (unsigned)n->nearby);
    }

    /* Below it, what it does not say: never the same thing twice. */
    bool says_nearby = !n->bench && n->unread == 0 && n->nearby > 0;
    if (n->nearby == 0) {
        line(f->rows[5], "No nodes heard yet");
    } else if (says_nearby) {
        line(f->rows[5], "Reaches %u nodes", (unsigned)n->reachable);
    } else {
        line(f->rows[5], "%u nearby, reaches %u", (unsigned)n->nearby, (unsigned)n->reachable);
    }
    if (n->unread > 0) {
        char from[UI_NAME + 1];
        ascii(from, sizeof from, (const uint8_t *)n->from, name_len(n->from));
        line(f->rows[6], "Last from %s", from);
    } else if (n->waiting > 0) {
        line(f->rows[6], "%u awaiting delivery", (unsigned)n->waiting);
    } else {
        line(f->rows[6], "No new messages");
    }
    if (!n->limited) {
        line(f->rows[7], "Air: no limit");
    } else if (n->wait_ms > 0) {
        span(wait, sizeof wait, (n->wait_ms + 999) / 1000);
        line(f->rows[7], "Air: full for %s", wait);
    } else {
        line(f->rows[7], "Air: %u%% left", left_percent(n));
    }
}

/* What became of a message this node sent, in the words of draft/companion.md's states. */
static void delivery(const struct ui_message *m, char *out) {
    char wait[16] = "";
    if (m->wait_s > 0) {
        span(wait, sizeof wait, m->wait_s);
    }
    switch (m->state) {
    case TERN_C_SENT:
        line(out, "Sent");
        return;
    case TERN_C_DELIVERED:
        line(out, "Delivered");
        return;
    case TERN_C_NOT_DELIVERED:
        line(out, "Not delivered");
        return;
    default:
        break;
    }
    switch (m->reason) {
    case TERN_C_WAIT_ROUTE:
        line(out, "Waiting: no route yet");
        break;
    case TERN_C_WAIT_SESSION:
        line(out, "Making contact...");
        break;
    case TERN_C_WAIT_REGION:
        if (wait[0] != '\0') {
            line(out, "Air limit, %s wait", wait);
        } else {
            line(out, "Waiting: air limit");
        }
        break;
    case TERN_C_WAIT_BUDGET:
        line(out, "Waiting: air budget");
        break;
    case TERN_C_WAIT_RADIO:
        line(out, "Waiting: radio busy");
        break;
    default:
        line(out, "Waiting");
        break;
    }
}

static void messages(const struct ui_node *n, struct frame *f) {
    char where[16] = "";
    if (n->messages == 0) {
        line(f->rows[0], "Messages");
        line(f->rows[3], "No messages yet");
        line(f->rows[5], "Send one from your");
        line(f->rows[6], "phone or computer");
        return;
    }
    snprintf(where, sizeof where, "%u/%u", (unsigned)n->shown + 1, (unsigned)n->messages);
    ends(f->rows[0], "Messages", where);

    const struct ui_message *m = &n->message;
    char name[UI_NAME + 1], who[40], ago[16] = "";
    ascii(name, sizeof name, (const uint8_t *)m->who, name_len(m->who));
    snprintf(who, sizeof who, "%s %s", !m->received ? "To" : m->unread ? "New from" : "From", name);
    if (m->aged) {
        if (m->ago_s < 60) {
            snprintf(ago, sizeof ago, " now");
        } else {
            char s[8];
            span(s, sizeof s, m->ago_s);
            snprintf(ago, sizeof ago, " %s ago", s);
        }
    }
    ends(f->rows[1], who, ago);

    char text[TERN_COMPANION_TEXT_MAX + 1];
    size_t len = m->text_len < TERN_COMPANION_TEXT_MAX ? m->text_len : TERN_COMPANION_TEXT_MAX;
    ascii(text, sizeof text, m->text, len);
    if (m->received) {
        wrap(f, 2, 7, text);
    } else {
        wrap(f, 2, 6, text);
        delivery(m, f->rows[7]);
    }
}

static void air(const struct ui_node *n, struct frame *f) {
    char used[24], wait[16];
    ends(f->rows[0], "Air", n->region);
    if (!n->limited) {
        line(f->rows[2], "%s sets no limit on", n->region);
        line(f->rows[3], "time on the air.");
        int64_t ms = n->air_total_ms < 0 ? 0 : n->air_total_ms;
        line(f->rows[5], "Sent for %lld.%lld s", (long long)(ms / 1000),
             (long long)(ms / 100 % 10));
        line(f->rows[6], "since it started");
        return;
    }
    int64_t ms = n->air_used_ms < 0 ? 0 : n->air_used_ms;
    snprintf(used, sizeof used, "%lld.%lld", (long long)(ms / 1000), (long long)(ms / 100 % 10));
    line(f->rows[1], "Used %s of %lld s", used, (long long)(n->air_limit_ms / 1000));
    if (n->window_s == 3600) {
        line(f->rows[2], "in any hour");
    } else if (n->window_s % 60 == 0) {
        line(f->rows[2], "in any %lu minutes", (unsigned long)(n->window_s / 60));
    } else {
        line(f->rows[2], "in any %lu s", (unsigned long)n->window_s);
    }
    f->bar = 4;
    f->part = n->air_used_ms;
    f->whole = n->air_limit_ms;
    if (n->wait_ms > 0) {
        span(wait, sizeof wait, (n->wait_ms + 999) / 1000);
        line(f->rows[6], "Next send in %s", wait);
    } else {
        line(f->rows[6], "Free to send now");
    }
    if (n->window_s > 0 && n->air_limit_ms > 0) {
        line(f->rows[7], "%s allows %lld%%", n->region,
             (long long)(n->air_limit_ms / 10 / (int64_t)n->window_s));
    }
}

#define QR_SCALE 2   /* pixels to a module: 58 of the screen's 64 rows */
#define QR_GROUND 70 /* the lit block the code sits in, from the left edge */
#define QR_LEFT ((QR_GROUND - QR_SIZE * QR_SCALE) / 2)
#define QR_TOP ((DISPLAY_PAGES * 8 - QR_SIZE * QR_SCALE) / 2)
#define QR_WORDS (QR_GROUND + 4) /* where the words beside it start */

/* The code dark on light, as every scanner reads it, which on this screen means a lit block with
 * the dark modules left unlit; the light margin round it is as wide as the screen leaves. Drawn
 * whole into a scratch picture, so only what changed is sent. */
static void share(const struct ui_node *n, struct display *d) {
    static struct display canvas;
    static struct qr code;
    char link[TERN_ADDRESS_LINK_LEN + 1], sc[TERN_SHORT_CODE_LEN + 1];
    display_init(&canvas);
    /* The link (draft/sharing.md): a web address a phone's camera opens, with the address in it in
     * base32, which the console's 'contact' also takes as it is. */
    tern_address_link(n->address, link);
    tern_short_code(n->address, sc);
    if (!qr_encode(&code, link, QR_MASK_BEST)) {
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
    /* Beside it, the short code, for whoever scanned it to check against their phone's. */
    sc[9] = '\0'; /* "5358 3737" on one line, "3382" on the next */
    /* Where the code leads: a phone with no Tern app opens a page on the site (draft/sharing.md).
     */
    display_text_at(&canvas, 1, QR_WORDS, "Scan to");
    display_text_at(&canvas, 2, QR_WORDS, "open its");
    display_text_at(&canvas, 3, QR_WORDS, "web page");
    display_text_at(&canvas, 5, QR_WORDS, "Its code:");
    display_text_at(&canvas, 6, QR_WORDS, sc);
    display_text_at(&canvas, 7, QR_WORDS, &sc[10]);
    display_copy(d, &canvas);
}

static void node(const struct ui_node *n, struct frame *f) {
    char text[TERN_ADDRESS_TEXT_LEN + 1], sc[TERN_SHORT_CODE_LEN + 1];
    tern_address_text(n->address, text);
    tern_short_code(n->address, sc);
    line(f->rows[0], "This node");
    line(f->rows[1], "Code %s", sc);
    /* Its sixty-four digits, sixteen a line in two groups of eight, as a person reads them out. */
    for (int r = 0; r < 4; r++) {
        line(f->rows[2 + r], "  %.8s %.8s", &text[16 * r], &text[16 * r + 8]);
    }
    line(f->rows[6], "%s, %s, %+d dBm", n->relay ? "Relay" : "Leaf", n->region, n->dbm);
    line(f->rows[7], "Tern %s", n->version);
}

static void draw(const struct frame *f, struct display *d) {
    for (int i = 0; i < DISPLAY_PAGES; i++) {
        if (i == f->big) {
            display_big(d, i, f->big_text);
            i++;
        } else if (i == f->bar) {
            display_bar(d, i, f->part, f->whole);
        } else {
            display_text(d, i, f->rows[i], i == 0);
        }
    }
}

static void blank(struct frame *f) {
    memset(f, 0, sizeof *f);
    f->big = NO_ROW;
    f->bar = NO_ROW;
}

void ui_draw(const struct ui_node *n, int page, struct display *d) {
    struct frame f;
    blank(&f);
    switch (page) {
    case UI_MESSAGES:
        messages(n, &f);
        break;
    case UI_AIR:
        air(n, &f);
        break;
    case UI_SHARE:
        share(n, d);
        return;
    case UI_NODE:
        node(n, &f);
        break;
    default:
        home(n, &f);
        break;
    }
    draw(&f, d);
}

void ui_pairing(uint32_t passkey, struct display *d) {
    struct frame f;
    blank(&f);
    line(f.rows[0], "Bluetooth pairing");
    line(f.rows[2], "Type this passkey");
    line(f.rows[3], "into your phone:");
    f.big = 5;
    snprintf(f.big_text, sizeof f.big_text, "%06lu", (unsigned long)(passkey % 1000000));
    draw(&f, d);
}
