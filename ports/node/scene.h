#ifndef TERN_NODE_SCENE_H
#define TERN_NODE_SCENE_H

#include <stdbool.h>
#include <stdint.h>

#include "display.h"
#include "fonts.h"
#include "tern/companion.h"
#include "tern/group.h"

/* What a screen shows, apart from how any one panel draws it: docs/ui.md, "The node's own screen".
 *
 * ui.c says what each page is about as a scene: a heading, the node's status, what a hold of the
 * button does, where the page is among the others, and a column of items, each a kind of thing
 * (a line, a figure, a meter, a message's text). Each kind of screen then draws it as suits it:
 * mono_draw() on a 128x64 monochrome screen, as a line of text a row, and colour_draw() (colour.h)
 * on a colour panel, with type, icons and colour. A page is written once, for both. */

#define SCENE_ITEMS 12
#define SCENE_LINE_MAX 36 /* bytes of a line's text, UTF-8 */
#define SCENE_TEXT_MAX                                                                             \
    160 /* bytes of a scene's paragraph: a message's text, and a few more words */
#define SCENE_STATS_MAX 3
#define SCENE_QR_WORDS DISPLAY_PAGES

/* What an item means to its reader, which a colour panel shows as its colour. */
enum scene_tone {
    TONE_TEXT,   /* as words are */
    TONE_DIM,    /* by the way */
    TONE_ACCENT, /* Tern's own: new, or chosen */
    TONE_GOOD,   /* working, arrived */
    TONE_WARN,   /* waiting, or nearly out of something */
    TONE_BAD,    /* failed, or about to destroy something */
};

enum scene_kind {
    SCENE_HERO,  /* `left`, large: what the page is about */
    SCENE_LINE,  /* `icon`, `left` and `right` on one line */
    SCENE_SMALL, /* `left`, in small type: a row of the bench pages, or a detail */
    SCENE_TEXT,  /* the scene's `text`, wrapped, in as many lines as the screen leaves it */
    SCENE_STATS, /* the scene's `stat`s side by side */
    SCENE_METER, /* `part` of `whole`, as a bar */
    SCENE_TILE,  /* one of a menu's choices: `icon` and `left`, `selected` if it is the one */
    SCENE_GAP,   /* a little room */
};

/* Which screens draw an item: some are said one way on a line of 21 characters and another way
 * with room and colour. */
enum scene_on {
    ON_BOTH,
    ON_MONO,
    ON_COLOUR,
};

struct scene_item {
    uint8_t kind, tone, icon, on;
    bool selected; /* a line or tile: the one a press moves from and a hold chooses */
    bool bubble;   /* text: set in a bubble, as a message is; `tone` ACCENT for one sent */
    bool signal;   /* a line: `snr_db` says how well its node is heard */
    bool centre;   /* a line or text: set in the middle, as a page with nothing on it says so */
    bool tail;     /* text, on a colour panel: if it does not all fit, its end, not its start */
    int8_t snr_db;
    char left[SCENE_LINE_MAX];
    char right[16];
    int32_t part, whole; /* a meter's */
};

/* A figure with what it counts: "4" "nearby". */
struct scene_stat {
    char value[12];
    char label[16];
    uint8_t tone;
};

struct scene {
    char title[24];       /* the heading; empty for none */
    char title_right[16]; /* at the heading's end: which of how many, say */
    uint8_t title_icon;   /* beside the heading on a colour panel */

    /* The node's status, at the top of every page that wants it: on a colour panel, as icons. On a
     * monochrome screen, as text after the heading where `title_right` leaves room. */
    bool status;
    uint8_t battery; /* percent, or UI_BATTERY_UNKNOWN */
    bool charging, phone;
    uint8_t unread;

    /* Where the page is among the main ones: page `dot` of `dots`, or no dots if `dots` is 0. */
    uint8_t dot, dots;
    /* What the button does here, if not only showing the next page: "Hold: older". */
    char hint[28];

    uint8_t n;
    struct scene_item item[SCENE_ITEMS];
    uint8_t stats;
    struct scene_stat stat[SCENE_STATS_MAX];
    char text[SCENE_TEXT_MAX]; /* SCENE_TEXT's, UTF-8 */

    /* A QR code, which takes the screen's left: `qr` is the link it holds, empty for none. A
     * monochrome screen has room for nothing else, and says `qr_words` beside it, a line each from
     * the top; a colour panel shows the items beside it instead. A group's join code is its
     * secret, so whoever fills `qr` in wipes the scene once it is drawn. */
    char qr[TERN_GROUP_LINK_MAX + 1];
    const char *qr_words[SCENE_QR_WORDS];
};

/* An empty scene: no heading, no items. */
void scene_clear(struct scene *s);

/* Adds an item of `kind`, with `left` as printf would write it, and returns it, to be given more;
 * past SCENE_ITEMS, a scratch item that is not drawn. */
#if defined(__GNUC__)
__attribute__((format(printf, 4, 5)))
#endif
struct scene_item *
scene_add(struct scene *s, enum scene_kind kind, enum scene_tone tone, const char *fmt, ...);

/* Adds a figure to the scene's stats, and with the first, the item they are drawn at, which it
 * returns. */
struct scene_item *scene_stat(struct scene *s, enum scene_tone tone, const char *value,
                              const char *label);

/* Draws the scene on a 128x64 monochrome screen. */
void mono_draw(const struct scene *s, struct display *d);

#endif
