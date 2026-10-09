#include "status.h"

#include <string.h>

#include "check.h"
#include "display.h"

/* The ESP32 port's bench screen (ports/esp32/main/status.c and display.c), which has no
 * hardware in it: the pages it draws from a snapshot of the board, and the picture they are drawn
 * into. What matters is that no number, however large, runs a line off the screen, and that a
 * page is sent again only when it changed. */

static char page[STATUS_ROWS][STATUS_COLS + 1];

/* Two boards on a bench, a few minutes after they met. */
static struct node_status bench(void) {
    struct node_status s = {
        .id = 0x482fa614,
        .relay = true,
        .region = "US915",
        .freq_hz = 921250000,
        .bw_hz = 500000,
        .sf = 9,
        .dbm = 2,
        .uptime_s = 3725,
        .air_total_ms = 1234,
        .frames_out = 12,
        .frames_in = 30,
        .heard = 1,
        .up = 1,
        .routed = 1,
        .n_neighbours = 1,
        .n_routes = 1,
        .neighbours = {{0x3fa1b2c4, true, true, -21, 23}},
        .routes = {{0x3fa1b2c4, 0x3fa1b2c4, 70}},
        .session = true,
        .peer = 0x3fa1b2c4,
        .sent = 3,
        .received = 4,
        .have_last = true,
        .last = "pong to #2: -42 dBm, SNR 9.50 dB",
        .last_s = 12,
    };
    return s;
}

static bool fits(void) {
    for (int i = 0; i < STATUS_ROWS; i++) {
        if (strlen(page[i]) > STATUS_COLS) {
            return false;
        }
    }
    return true;
}

static void node_page_shows_the_board(void) {
    struct node_status s = bench();
    status_page(&s, 0, page);
    CHECK(strcmp(page[0], "Node, up 1h02m    1/4") == 0);
    CHECK(strcmp(page[1], "id 482fa614 relay") == 0);
    CHECK(strcmp(page[2], "US915 921.250 MHz") == 0);
    CHECK(strcmp(page[3], "500 kHz SF9 +2 dBm") == 0);
    CHECK(strcmp(page[4], "hears 1, 1 both ways") == 0);
    CHECK(strcmp(page[5], "routes to 1") == 0);
    CHECK(strcmp(page[6], "frames 12 out 30 in") == 0);
    CHECK(strcmp(page[7], "air 1.2 s, no limit") == 0);
    CHECK(fits());
}

static void limited_region_shows_its_limit(void) {
    struct node_status s = bench();
    s.region = "EU868";
    s.off_profile = true;
    s.limited = true;
    s.air_used_ms = 12345;
    s.air_limit_ms = 360000;
    status_page(&s, 0, page);
    CHECK(strcmp(page[2], "EU868* 921.250 MHz") == 0);
    CHECK(strcmp(page[7], "air 12.3 of 360 s") == 0);
}

static void neighbours_and_routes_are_listed(void) {
    struct node_status s = bench();
    status_page(&s, 1, page);
    CHECK(strcmp(page[0], "Neighbours        2/4") == 0);
    CHECK(strcmp(page[1], "3fa1b2c4 R up -21 +23") == 0);
    CHECK(strcmp(page[7], "R/L up/dn needs spare") == 0);
    s.neighbours[0].spare_db = INT16_MIN;
    s.neighbours[0].up = false;
    status_page(&s, 1, page);
    CHECK(strcmp(page[1], "3fa1b2c4 R dn -21   ?") == 0);

    status_page(&s, 2, page);
    CHECK(strcmp(page[0], "Routes            3/4") == 0);
    CHECK(strcmp(page[1], "3fa1b2c4 3fa1b2c4  70") == 0);
    CHECK(strcmp(page[7], "to       by        ms") == 0);
    CHECK(fits());
}

static void more_than_fit_are_counted(void) {
    struct node_status s = bench();
    s.heard = 40;
    s.routed = 90;
    s.n_neighbours = STATUS_LISTED;
    s.n_routes = STATUS_LISTED;
    status_page(&s, 1, page);
    CHECK(strcmp(page[7], "and 34 more") == 0);
    status_page(&s, 2, page);
    CHECK(strcmp(page[7], "and 84 more") == 0);
}

static void nothing_yet_says_so(void) {
    struct node_status s = {.region = "US915"};
    status_page(&s, 1, page);
    CHECK(strcmp(page[1], "none heard yet") == 0);
    status_page(&s, 2, page);
    CHECK(strcmp(page[1], "none yet") == 0);
    status_page(&s, 3, page);
    CHECK(strcmp(page[1], "none yet: type") == 0);
    CHECK(fits());
}

static void last_message_is_wrapped_and_cleaned(void) {
    struct node_status s = bench();
    status_page(&s, 3, page);
    CHECK(strcmp(page[0], "Session           4/4") == 0);
    CHECK(strcmp(page[1], "with 3fa1b2c4") == 0);
    CHECK(strcmp(page[2], "sent 3, heard 4") == 0);
    CHECK(strcmp(page[3], "last heard, 12s ago:") == 0);
    CHECK(strcmp(page[4], "pong to #2: -42 dBm, ") == 0);
    CHECK(strcmp(page[5], "SNR 9.50 dB") == 0);
    CHECK(page[6][0] == '\0');

    /* A full buffer with no NUL, and bytes the font has no glyph for. */
    memset(s.last, 'x', sizeof s.last);
    s.last[0] = '\n';
    s.last_s = 7200;
    s.contacting = true;
    status_page(&s, 3, page);
    CHECK(strcmp(page[3], "last heard, 2h ago:") == 0);
    CHECK(page[4][0] == '?' && page[4][1] == 'x');
    CHECK(strcmp(page[7], "contact under way") == 0);
    CHECK(fits());
}

static void huge_numbers_stay_on_the_screen(void) {
    struct node_status s = bench();
    s.id = 0xFFFFFFFF;
    s.uptime_s = UINT32_MAX;
    s.frames_out = s.frames_in = UINT32_MAX;
    s.heard = s.up = s.routed = UINT16_MAX;
    s.air_total_ms = INT64_MAX;
    s.region = "A REGION WITH A VERY LONG NAME";
    s.neighbours[0] = (struct status_neighbour){0xFFFFFFFF, false, false, INT16_MIN, INT16_MAX};
    s.routes[0].metric_ms = UINT16_MAX;
    s.sent = s.received = s.last_s = UINT32_MAX;
    for (int p = 0; p < STATUS_PAGES; p++) {
        status_page(&s, p, page);
        CHECK(fits());
    }
    s.limited = true;
    s.air_used_ms = s.air_limit_ms = INT64_MAX;
    status_page(&s, 0, page);
    CHECK(fits());
    status_page(&s, 2, page);
    CHECK(strcmp(page[1], "3fa1b2c4 3fa1b2c4 999") == 0);
}

static void only_changed_pages_are_sent(void) {
    struct display d;
    display_init(&d);
    for (int i = 0; i < DISPLAY_PAGES; i++) {
        CHECK_EQ_I64(display_take(&d), i);
    }
    CHECK_EQ_I64(display_take(&d), -1);

    display_text(&d, 3, "", false); /* already blank */
    CHECK_EQ_I64(display_take(&d), -1);
    display_text(&d, 3, "!", false);
    CHECK_EQ_I64(display_take(&d), 3);
    CHECK_EQ_I64(d.px[3][2], 0x5f); /* the '!', in its middle column */
    CHECK_EQ_I64(d.px[3][5], 0x00);
    display_text(&d, 3, "!", false);
    CHECK_EQ_I64(display_take(&d), -1);

    display_text(&d, 0, "!", true);
    CHECK_EQ_I64(display_take(&d), 0);
    CHECK_EQ_I64(d.px[0][2], 0xa0);
    CHECK_EQ_I64(d.px[0][127], 0xff);

    /* Off the screen, and off the end of a line, are ignored. */
    display_text(&d, -1, "x", false);
    display_text(&d, DISPLAY_PAGES, "x", false);
    CHECK_EQ_I64(display_take(&d), -1);
    display_text(&d, 7, "0123456789012345678901234", false);
    CHECK_EQ_I64(display_take(&d), 7);
    CHECK_EQ_I64(d.px[7][125], 0x00);
}

int main(void) {
    RUN(node_page_shows_the_board);
    RUN(limited_region_shows_its_limit);
    RUN(neighbours_and_routes_are_listed);
    RUN(more_than_fit_are_counted);
    RUN(nothing_yet_says_so);
    RUN(last_message_is_wrapped_and_cleaned);
    RUN(huge_numbers_stay_on_the_screen);
    RUN(only_changed_pages_are_sent);
    return CHECK_DONE();
}
