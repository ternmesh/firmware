#ifndef NODE_EPD_H
#define NODE_EPD_H

#include <stdbool.h>
#include <stdint.h>

#include "display.h"

/* The node's 128x64 picture (display.h) on a 296x128 e-paper panel driven by an SSD1680, the Vision
 * Master E290's: drawn twice the size, 256x128, in the middle of the panel's long side, dark on
 * light as paper is read.
 *
 * The panel's 128 pixels across its short side are the controller's sources, eight to a byte, the
 * first in the byte's top bit; its 296 along the long side are gates, one line of 16 bytes each
 * (Solomon Systech's SSD1680 datasheet, Rev 1.4, section 8.3 and table 6-5; DKE's DEPG0290BNS800F6
 * specification, pages 5, 6 and 21). A bit is 1 for white. The board sends the lines in the order
 * the panel's maker sets the controller up to take them, and this works out what each one holds,
 * so tests/epd.c checks it on a host. */

#define EPD_LINES 296
#define EPD_LINE_BYTES 16

/* The `n`th line the board sends, 0 to EPD_LINES - 1: column n of the panel, counted from the left
 * as the picture is seen, or, with flip, from the right with the picture upside down. */
void epd_line(const struct display *d, int n, bool flip, uint8_t out[EPD_LINE_BYTES]);

#endif
