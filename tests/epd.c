#include "epd.h"

#include <string.h>

#include "check.h"

/* The picture on the e-paper panel: where a pixel of the 128x64 picture lands among the bits the
 * panel is sent, worked out by hand from the layout in epd.h. */

static bool white(const uint8_t line[EPD_LINE_BYTES]) {
    for (int i = 0; i < EPD_LINE_BYTES; i++) {
        if (line[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

static void a_blank_picture_is_white(void) {
    static struct display d;
    display_init(&d);
    uint8_t line[EPD_LINE_BYTES];
    for (int n = 0; n < EPD_LINES; n++) {
        epd_line(&d, n, false, line);
        CHECK(white(line));
        epd_line(&d, n, true, line);
        CHECK(white(line));
    }
}

static void a_pixel_is_two_by_two_and_black(void) {
    static struct display d;
    display_init(&d);
    display_set(&d, 0, 0, true); /* the top left */
    uint8_t line[EPD_LINE_BYTES];

    /* 20 lines of margin, then the picture's first column, twice. */
    epd_line(&d, 19, false, line);
    CHECK(white(line));
    for (int n = 20; n <= 21; n++) {
        epd_line(&d, n, false, line);
        CHECK(line[0] == 0x3F); /* the first two sources black, top bits first */
        for (int i = 1; i < EPD_LINE_BYTES; i++) {
            CHECK(line[i] == 0xFF);
        }
    }
    epd_line(&d, 22, false, line);
    CHECK(white(line));

    /* Upside down, the same pixel is at the far end of both. */
    epd_line(&d, EPD_LINES - 21, true, line);
    CHECK(line[EPD_LINE_BYTES - 1] == 0xFC);
    epd_line(&d, 21, true, line);
    CHECK(white(line));
}

static void the_bottom_right_reaches_the_last_source(void) {
    static struct display d;
    display_init(&d);
    display_set(&d, 127, 63, true);
    uint8_t line[EPD_LINE_BYTES];
    /* Column 127 is lines 20 + 254 and 255; the margin after it starts at 276. */
    epd_line(&d, 275, false, line);
    CHECK(line[EPD_LINE_BYTES - 1] == 0xFC);
    epd_line(&d, 276, false, line);
    CHECK(white(line));
    epd_line(&d, 20, true, line); /* flipped, it is the first column */
    CHECK(line[0] == 0x3F);
}

int main(void) {
    RUN(a_blank_picture_is_white);
    RUN(a_pixel_is_two_by_two_and_black);
    RUN(the_bottom_right_reaches_the_last_source);
    return CHECK_DONE();
}
