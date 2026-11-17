#ifndef TERN_NODE_COLOUR_H
#define TERN_NODE_COLOUR_H

#include <stdint.h>

#include "scene.h"

/* A scene drawn on a colour panel, such as the Heltec T114's 240x135: Tern's colours on a dark
 * ground, smooth type (fonts.h), icons, and the page's status at its head and its dots at its
 * foot.
 *
 * A whole picture of 240x135 in RGB565 is 64 KB, more than a small node's RAM can spare, so the
 * picture is drawn a band of rows at a time into a buffer the caller owns, and sent before the
 * next is drawn; a band that comes out as it was need not be sent at all. Nothing here touches the
 * hardware, so tests/colour.c draws every page on a host. */

/* An RGB565 colour, red in the top five bits, as the value is: the board sends it in the order
 * its panel takes. */
#define COLOUR_RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

/* The rows of a picture `w` by `h` that a band holds: from row `y0`, `rows` of them, each `w`
 * pixels in `px`. */
struct canvas {
    uint16_t *px;
    int w, h;
    int y0, rows;
};

/* Draws the rows of the scene that the canvas holds. */
void colour_draw(const struct scene *s, struct canvas *c);

/* Wipes what colour_draw() keeps between bands of a picture with a QR code in it: the code, which
 * for a group's join code is the group's secret. */
void colour_forget(void);

#endif
