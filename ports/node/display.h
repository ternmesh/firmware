#ifndef HELTEC_V3_DISPLAY_H
#define HELTEC_V3_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

/* A picture of a 128x64 monochrome screen, kept in the controller's own layout, and lines of text
 * drawn into it.
 *
 * The SSD1306 the Heltecs have, like most small OLED controllers, divides its 64 rows into eight
 * pages of eight, and takes a page as 128 bytes: one per column, its lowest bit at the top. Text
 * here is drawn a page at a time, six pixels a character, so each line of text is one page, and
 * only the pages that changed need sending. Sending a page is the board's (board_screen_page());
 * nothing here touches the hardware, so tests/status.c runs it on a host. */

#define DISPLAY_WIDTH 128
#define DISPLAY_PAGES 8

/* The one character drawn that is not ASCII: Bluetooth's rune, for a phone connected. */
#define DISPLAY_BLUETOOTH '\x01'
#define DISPLAY_BLUETOOTH_S "\x01"

struct display {
    uint8_t px[DISPLAY_PAGES][DISPLAY_WIDTH];
    uint8_t dirty; /* a bit for each page changed since it was last taken */
};

/* Clears the picture and marks every page to be sent. */
void display_init(struct display *d);

/* Draws a line of text across page `page`, light on dark or, if inverse, dark on light, and
 * marks the page if that changed it. Printable ASCII is drawn, and DISPLAY_BLUETOOTH; anything
 * else is a blank. */
void display_text(struct display *d, int page, const char *text, bool inverse);

/* Draws a line of text twice the size, ten pixels a character and twelve apart, across pages
 * `page` and `page + 1`, centred: at most DISPLAY_BIG_COLS characters, light on dark. */
#define DISPLAY_BIG_COLS 10
void display_big(struct display *d, int page, const char *text);

/* Draws a bar across page `page`: an outline the width of the screen, filled `part` of `whole` from
 * the left. A part that is not nothing always shows, and one past the whole fills it. */
void display_bar(struct display *d, int page, int64_t part, int64_t whole);

/* Lights pixel (x, y), or darkens it; off the screen is ignored. */
void display_set(struct display *d, int x, int y, bool on);

/* Draws text across page `page` from column `x`, light on dark, leaving the rest of the page as it
 * was: for a line beside a picture rather than across the screen. Cut at the right edge. */
void display_text_at(struct display *d, int page, int x, const char *text);

/* Makes the picture `from`, marking only the pages that differ: so a page can be drawn whole into a
 * scratch picture first, and only what changed is sent. */
void display_copy(struct display *d, const struct display *from);

/* A page that has changed and not yet been sent, or -1 if there is none. It is then counted as
 * sent: take one page at a time and send it before taking another. */
int display_take(struct display *d);

#endif
