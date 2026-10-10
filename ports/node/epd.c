#include "epd.h"

#include <string.h>

#define EPD_HEIGHT (EPD_LINE_BYTES * 8)
#define SCALE 2
#define MARGIN ((EPD_LINES - DISPLAY_WIDTH * SCALE) / 2) /* each side of the picture: 20 */

static bool lit(const struct display *d, int col, int row) {
    return (d->px[row / 8][col] >> (row % 8)) & 1u;
}

void epd_line(const struct display *d, int n, bool flip, uint8_t out[EPD_LINE_BYTES]) {
    memset(out, 0xFF, EPD_LINE_BYTES); /* white */
    int x = flip ? EPD_LINES - 1 - n : n;
    if (x < MARGIN || x >= MARGIN + DISPLAY_WIDTH * SCALE) {
        return;
    }
    int col = (x - MARGIN) / SCALE;
    for (int s = 0; s < EPD_HEIGHT; s++) {
        int y = flip ? EPD_HEIGHT - 1 - s : s;
        if (lit(d, col, y / SCALE)) {
            out[s / 8] &= (uint8_t) ~(0x80u >> (s % 8)); /* black */
        }
    }
}
