#ifndef HELTEC_V3_STATUS_H
#define HELTEC_V3_STATUS_H

#include <stdbool.h>
#include <stdint.h>

/* What the board is doing, as one snapshot that a view reads and never reaches past.
 *
 * main.c fills it in from the radio, the router and the demo; the bench screen draws it. Nothing
 * that draws needs to know where a number came from, so when the demo is replaced, only the code
 * that fills this in changes. It is the first version of the node model in docs/ui.md, and the
 * part of the bench screen meant to outlast it.
 *
 * The pages it is drawn as are a developer's: routing ids, signal margins and frame counts. They
 * are what the serial console shows, on the board's display, and are not the user interface.
 *
 * Nothing here touches the hardware, so tests/status.c runs it on a host. */

#define STATUS_COLS 21 /* characters across the 128-pixel screen, six pixels each */
#define STATUS_ROWS 8  /* lines down its 64 */
#define STATUS_PAGES 4
#define STATUS_LISTED 6 /* neighbours or routes a page has room for */
#define STATUS_TEXT 64  /* bytes of the last message kept to show */

struct status_neighbour {
    uint32_t id;
    bool relay, up;
    int16_t floor_dbm; /* what it needs to be sent at to hear us */
    int16_t spare_db;  /* the margin it says it hears us with, or INT16_MIN if it has not said */
};

struct status_route {
    uint32_t dest, next;
    uint16_t metric_ms;
};

struct node_status {
    /* The node and its radio. */
    uint32_t id; /* its routing id */
    bool relay;
    const char *region;
    bool off_profile; /* the build moved it off the region's settings */
    uint32_t freq_hz, bw_hz;
    uint8_t sf;
    int8_t dbm;
    uint32_t uptime_s;

    /* Time on the air: what was sent since starting, and, where the region limits it, what is
     * counted against the limit and what the limit is. */
    int64_t air_total_ms;
    bool limited;
    int64_t air_used_ms, air_limit_ms;
    uint32_t window_s;

    /* Frames, of every kind. */
    uint32_t frames_out, frames_in;

    /* The mesh: how many of each there are, and the first STATUS_LISTED of them. */
    uint16_t heard, up, routed;
    uint8_t n_neighbours, n_routes;
    struct status_neighbour neighbours[STATUS_LISTED];
    struct status_route routes[STATUS_LISTED];

    /* The demo's sessions: how many, and the one 'send' goes to. */
    bool session, contacting;
    uint8_t peers;
    uint32_t peer; /* the first four bytes of its address */
    uint32_t sent, received;
    bool have_last; /* the last message heard is in `last` */
    char last[STATUS_TEXT];
    uint32_t last_s; /* how long ago it was heard */
};

/* Writes page `page` (0 to STATUS_PAGES - 1) as STATUS_ROWS lines of at most STATUS_COLS
 * characters, each ended with a NUL. Row 0 is the page's title. */
void status_page(const struct node_status *s, int page, char out[STATUS_ROWS][STATUS_COLS + 1]);

#endif
