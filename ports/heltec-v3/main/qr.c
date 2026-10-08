#include "qr.h"

#include <string.h>

#define DATA_CODEWORDS 55 /* version 3 at level L: one block of 55, and 15 to correct them */
#define EC_CODEWORDS 15
#define ALIGNMENT 22 /* the one alignment pattern's centre, in both directions */
#define FORMAT_L 1   /* level L's two bits in the format information */

static const char alphanumeric[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";

static int value_of(char c) {
    const char *at = c != '\0' ? strchr(alphanumeric, c) : NULL;
    return at != NULL ? (int)(at - alphanumeric) : -1;
}

/* --- The codewords -----------------------------------------------------------------------------
 */

struct bits {
    uint8_t *out;
    size_t at; /* in bits */
};

static void put(struct bits *b, unsigned value, int n) {
    for (int i = n - 1; i >= 0; i--) {
        if (value >> i & 1u) {
            b->out[b->at / 8] |= (uint8_t)(0x80u >> (b->at % 8));
        }
        b->at++;
    }
}

/* Multiplies in GF(256), as the standard builds it on x^8 + x^4 + x^3 + x^2 + 1. */
static uint8_t gf_mul(uint8_t a, uint8_t b) {
    unsigned r = 0;
    for (int i = 7; i >= 0; i--) {
        r = (r << 1) ^ ((r >> 7) * 0x11Du);
        r ^= (b >> i & 1u) * a;
    }
    return (uint8_t)r;
}

/* The error correction codewords: what is left when the data, as a polynomial, is divided by the
 * generator whose roots are the first fifteen powers of 2. */
static void correct(const uint8_t data[DATA_CODEWORDS], uint8_t ec[EC_CODEWORDS]) {
    uint8_t gen[EC_CODEWORDS] = {0};
    uint8_t root = 1;
    gen[EC_CODEWORDS - 1] = 1;
    for (int i = 0; i < EC_CODEWORDS; i++) {
        for (int j = 0; j < EC_CODEWORDS; j++) {
            gen[j] = gf_mul(gen[j], root);
            if (j + 1 < EC_CODEWORDS) {
                gen[j] ^= gen[j + 1];
            }
        }
        root = gf_mul(root, 2);
    }
    memset(ec, 0, EC_CODEWORDS);
    for (int i = 0; i < DATA_CODEWORDS; i++) {
        uint8_t factor = data[i] ^ ec[0];
        memmove(ec, ec + 1, EC_CODEWORDS - 1);
        ec[EC_CODEWORDS - 1] = 0;
        for (int j = 0; j < EC_CODEWORDS; j++) {
            ec[j] ^= gf_mul(gen[j], factor);
        }
    }
}

/* The text as the data codewords: mode, count, the characters two at a time, then the
 * terminator and the padding the standard gives. */
static bool codewords(const char *text, uint8_t out[DATA_CODEWORDS + EC_CODEWORDS]) {
    size_t n = strlen(text);
    if (n > QR_TEXT_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (value_of(text[i]) < 0) {
            return false;
        }
    }
    memset(out, 0, DATA_CODEWORDS + EC_CODEWORDS);
    struct bits b = {out, 0};
    put(&b, 0x2, 4); /* alphanumeric */
    put(&b, (unsigned)n, 9);
    for (size_t i = 0; i + 1 < n; i += 2) {
        put(&b, (unsigned)(value_of(text[i]) * 45 + value_of(text[i + 1])), 11);
    }
    if (n % 2 != 0) {
        put(&b, (unsigned)value_of(text[n - 1]), 6);
    }
    size_t room = DATA_CODEWORDS * 8;
    size_t end = b.at + 4 < room ? b.at + 4 : room; /* the terminator, as much of it as fits */
    b.at = (end + 7) / 8 * 8;
    for (uint8_t pad = 0xEC; b.at < room; pad ^= 0xEC ^ 0x11) {
        put(&b, pad, 8);
    }
    correct(out, out + DATA_CODEWORDS);
    return true;
}

/* --- The symbol ------------------------------------------------------------------------------- */

struct grid {
    uint32_t dark[QR_SIZE];
    uint32_t fixed[QR_SIZE]; /* the function patterns: not data, and not masked */
};

static void set(struct grid *g, int x, int y, bool dark) {
    if (dark) {
        g->dark[y] |= 1u << x;
    } else {
        g->dark[y] &= ~(1u << x);
    }
    g->fixed[y] |= 1u << x;
}

static bool is_dark(const uint32_t rows[QR_SIZE], int x, int y) { return (rows[y] >> x & 1u) != 0; }

/* A finder pattern and the light separator round it, centred on (cx, cy). */
static void finder(struct grid *g, int cx, int cy) {
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -4; dx <= 4; dx++) {
            int x = cx + dx, y = cy + dy;
            int d = dx < 0 ? -dx : dx;
            int e = dy < 0 ? -dy : dy;
            int ring = d > e ? d : e;
            if (x >= 0 && x < QR_SIZE && y >= 0 && y < QR_SIZE) {
                set(g, x, y, ring != 2 && ring != 4);
            }
        }
    }
}

static void format(struct grid *g, int mask) {
    unsigned data = FORMAT_L << 3 | (unsigned)mask;
    unsigned rem = data;
    for (int i = 0; i < 10; i++) {
        rem = (rem << 1) ^ ((rem >> 9) * 0x537u);
    }
    unsigned bits = (data << 10 | rem) ^ 0x5412u;
    for (int i = 0; i <= 5; i++) {
        set(g, 8, i, bits >> i & 1u);
    }
    set(g, 8, 7, bits >> 6 & 1u);
    set(g, 8, 8, bits >> 7 & 1u);
    set(g, 7, 8, bits >> 8 & 1u);
    for (int i = 9; i < 15; i++) {
        set(g, 14 - i, 8, bits >> i & 1u);
    }
    for (int i = 0; i < 8; i++) {
        set(g, QR_SIZE - 1 - i, 8, bits >> i & 1u);
    }
    for (int i = 8; i < 15; i++) {
        set(g, 8, QR_SIZE - 15 + i, bits >> i & 1u);
    }
    set(g, 8, QR_SIZE - 8, true); /* the dark module, always dark */
}

static void patterns(struct grid *g) {
    memset(g, 0, sizeof *g);
    for (int i = 0; i < QR_SIZE; i++) {
        set(g, 6, i, i % 2 == 0);
        set(g, i, 6, i % 2 == 0);
    }
    finder(g, 3, 3);
    finder(g, QR_SIZE - 4, 3);
    finder(g, 3, QR_SIZE - 4);
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            int d = dx < 0 ? -dx : dx;
            int e = dy < 0 ? -dy : dy;
            set(g, ALIGNMENT + dx, ALIGNMENT + dy, (d > e ? d : e) != 1);
        }
    }
    format(g, 0); /* only to reserve its modules: written again once the mask is chosen */
}

/* Lays the codewords' bits in the standard's zigzag: two columns at a time from the right, up
 * then down, around the function patterns. The few modules left over stay light. */
static void place(struct grid *g, const uint8_t *cw, size_t len) {
    size_t i = 0;
    for (int right = QR_SIZE - 1; right >= 1; right -= 2) {
        if (right == 6) {
            right = 5; /* the vertical timing pattern has a column to itself */
        }
        bool up = ((right + 1) & 2) == 0;
        for (int v = 0; v < QR_SIZE; v++) {
            int y = up ? QR_SIZE - 1 - v : v;
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                if (is_dark(g->fixed, x, y)) {
                    continue;
                }
                if (i < len * 8 && (cw[i / 8] >> (7 - i % 8) & 1u)) {
                    g->dark[y] |= 1u << x;
                }
                i++;
            }
        }
    }
}

static bool masked(int mask, int x, int y) {
    switch (mask) {
    case 0:
        return (x + y) % 2 == 0;
    case 1:
        return y % 2 == 0;
    case 2:
        return x % 3 == 0;
    case 3:
        return (x + y) % 3 == 0;
    case 4:
        return (x / 3 + y / 2) % 2 == 0;
    case 5:
        return x * y % 2 + x * y % 3 == 0;
    case 6:
        return (x * y % 2 + x * y % 3) % 2 == 0;
    default:
        return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

static void apply(struct grid *g, int mask) {
    for (int y = 0; y < QR_SIZE; y++) {
        for (int x = 0; x < QR_SIZE; x++) {
            if (!is_dark(g->fixed, x, y) && masked(mask, x, y)) {
                g->dark[y] ^= 1u << x;
            }
        }
    }
}

/* --- Choosing the mask ------------------------------------------------------------------------ */

/* A module along a line: row `line` when across, column `line` when not. Outside is light. */
static bool along(const uint32_t rows[QR_SIZE], bool across, int line, int i) {
    if (i < 0 || i >= QR_SIZE) {
        return false;
    }
    return across ? is_dark(rows, i, line) : is_dark(rows, line, i);
}

/* The standard's four penalties: runs of five or more alike, 2x2 blocks alike, anything like a
 * finder pattern with light on one side of it, and dark modules far from half. */
static long penalty(const uint32_t rows[QR_SIZE]) {
    static const bool like_finder[7] = {1, 0, 1, 1, 1, 0, 1};
    long score = 0;
    int dark = 0;
    for (int across = 0; across < 2; across++) {
        for (int line = 0; line < QR_SIZE; line++) {
            int run = 1;
            for (int i = 1; i <= QR_SIZE; i++) {
                if (i < QR_SIZE &&
                    along(rows, across, line, i) == along(rows, across, line, i - 1)) {
                    run++;
                    continue;
                }
                if (run >= 5) {
                    score += 3 + (run - 5);
                }
                run = 1;
            }
            /* Dark, light, three dark, light, dark, with four light before it or after it:
             * once for each such pattern, whichever side its light is on. */
            for (int i = 0; i + 7 <= QR_SIZE; i++) {
                bool core = true, before = true, after = true;
                for (int k = 0; k < 7; k++) {
                    core = core && along(rows, across, line, i + k) == like_finder[k];
                }
                for (int k = 1; k <= 4; k++) {
                    before = before && !along(rows, across, line, i - k);
                    after = after && !along(rows, across, line, i + 6 + k);
                }
                score += core && (before || after) ? 40 : 0;
            }
        }
    }
    for (int y = 0; y < QR_SIZE; y++) {
        for (int x = 0; x < QR_SIZE; x++) {
            dark += is_dark(rows, x, y);
            if (x + 1 < QR_SIZE && y + 1 < QR_SIZE) {
                bool c = is_dark(rows, x, y);
                if (c == is_dark(rows, x + 1, y) && c == is_dark(rows, x, y + 1) &&
                    c == is_dark(rows, x + 1, y + 1)) {
                    score += 3;
                }
            }
        }
    }
    int total = QR_SIZE * QR_SIZE;
    int off = dark * 20 - total * 10; /* twenty times how far the dark share is from half */
    off = off < 0 ? -off : off;
    score += 10 * (off / total);
    return score;
}

bool qr_encode(struct qr *q, const char *text, int mask) {
    uint8_t cw[DATA_CODEWORDS + EC_CODEWORDS];
    struct grid base, g;
    if (mask < QR_MASK_BEST || mask > 7 || !codewords(text, cw)) {
        return false;
    }
    patterns(&base);
    place(&base, cw, sizeof cw);
    int best = mask;
    if (mask == QR_MASK_BEST) {
        long least = 0;
        for (int m = 0; m < 8; m++) {
            g = base;
            apply(&g, m);
            format(&g, m);
            long p = penalty(g.dark);
            if (m == 0 || p < least) {
                least = p;
                best = m;
            }
        }
    }
    g = base;
    apply(&g, best);
    format(&g, best);
    memcpy(q->rows, g.dark, sizeof q->rows);
    return true;
}
