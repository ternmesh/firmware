#ifndef HELTEC_V3_DISPLAY_H
#define HELTEC_V3_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

/* A picture of a 128x64 monochrome screen, kept in the controller's own layout, and lines of text
 * drawn into it.
 *
 * The SSD1306 the Heltec V3 has, like most small OLED controllers, divides its 64 rows into eight
 * pages of eight, and takes a page as 128 bytes: one per column, its lowest bit at the top. Text
 * here is drawn a page at a time, six pixels a character, so each line of text is one page, and
 * only the pages that changed need sending. Sending a page is the board's (board_screen_page());
 * nothing here touches the hardware, so tests/status.c runs it on a host. */

#define DISPLAY_WIDTH 128
#define DISPLAY_PAGES 8

struct display {
    uint8_t px[DISPLAY_PAGES][DISPLAY_WIDTH];
    uint8_t dirty; /* a bit for each page changed since it was last taken */
};

/* Clears the picture and marks every page to be sent. */
void display_init(struct display *d);

/* Draws a line of text across page `page`, light on dark or, if inverse, dark on light, and
 * marks the page if that changed it. Printable ASCII is drawn; anything else is a blank. */
void display_text(struct display *d, int page, const char *text, bool inverse);

/* A page that has changed and not yet been sent, or -1 if there is none. It is then counted as
 * sent: take one page at a time and send it before taking another. */
int display_take(struct display *d);

#endif
