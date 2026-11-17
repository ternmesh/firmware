#ifndef TERN_NODE_FONTS_H
#define TERN_NODE_FONTS_H

#include <stdint.h>

/* The typefaces and icons a colour screen draws with (colour.c), as 4-bit coverage: each pixel of a
 * glyph is how much of it the shape covers, 0 to 15, so its edges are smooth on any ground. Made by
 * tools/fontgen.py from Inter (the SIL Open Font License, fonts/OFL.txt) and Material Symbols
 * Rounded (the Apache License 2.0), and kept in flash. */

struct font_glyph {
    uint32_t bits; /* where its coverage starts in the font's `bits`, in nibbles */
    uint8_t w, h;  /* its box */
    int8_t x, y;   /* the box's left from the pen, and its top from the line's top */
    uint8_t adv;   /* how far the pen moves after it */
};

struct font {
    const struct font_glyph *glyph;
    const uint8_t *bits;
    const uint16_t *extra; /* the code points past Latin-1 it has, in order, after its ranges */
    uint8_t extras;
    uint8_t latin1; /* whether U+00A0 to U+00FF follow ASCII */
    uint8_t height; /* a line's height */
    uint8_t ascent; /* from a line's top to its baseline */
};

extern const struct font font_small; /* Inter SemiBold: headings, labels and hints */
extern const struct font font_body;  /* Inter Medium: what a page says */
extern const struct font font_bold;  /* Inter Bold: a figure in a tile */
extern const struct font font_hero;  /* Inter Bold, large: what a page is about, at a glance */

/* The icons, each in two sizes: ICON_SMALL for a line or the status bar, ICON_LARGE for a tile. */
enum icon {
    ICON_NONE,
    ICON_HOME,
    ICON_CHAT,
    ICON_PEOPLE,
    ICON_QR,
    ICON_MORE,
    ICON_AIR,
    ICON_GROUP,
    ICON_INFO,
    ICON_PHONE,
    ICON_ERASE,
    ICON_BACK,
    ICON_BLUETOOTH,
    ICON_SEND,
    ICON_CHECK,
    ICON_DONE_ALL,
    ICON_SCHEDULE,
    ICON_ERROR,
    ICON_ANTENNA,
    ICON_POWER,
    ICON_BOLT,
    ICON_KEY,
    ICON_DOWNLOAD,
    ICON_WARNING,
    ICON_COUNT
};

#define ICON_SMALL 16
#define ICON_LARGE 24

extern const struct font_glyph icon_small[ICON_COUNT], icon_large[ICON_COUNT];
extern const uint8_t icon_small_bits[], icon_large_bits[];

/* The glyph a font draws for code point `c`, or its '?' if it has none. */
const struct font_glyph *font_find(const struct font *f, uint32_t c);

#endif
