#include "status.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Writes a row, cut off at the screen's edge: a number too long for a line loses its end
 * rather than running off it. */
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static void
row(char *out, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, STATUS_COLS + 1, fmt, ap);
    va_end(ap);
}

/* "45s", "12m", "3h", "9d": a span in two or three characters, for lines that have no room. */
static void span(char *buf, size_t size, uint32_t s) {
    if (s < 60) {
        snprintf(buf, size, "%lus", (unsigned long)s);
    } else if (s < 3600) {
        snprintf(buf, size, "%lum", (unsigned long)(s / 60));
    } else if (s < 86400) {
        snprintf(buf, size, "%luh", (unsigned long)(s / 3600));
    } else {
        snprintf(buf, size, "%lud", (unsigned long)(s / 86400));
    }
}

/* The page's name, and where it is among the pages, at the right. */
static void title(char *out, const char *name, int page) {
    row(out, "%-*.*s%d/%d", STATUS_COLS - 3, STATUS_COLS - 3, name, page + 1, STATUS_PAGES);
}

/* Milliseconds as seconds with one decimal. */
static void seconds(char *buf, size_t size, int64_t ms) {
    snprintf(buf, size, "%lld.%lld", (long long)(ms / 1000), (long long)(ms / 100 % 10));
}

static void node(const struct node_status *s, char out[STATUS_ROWS][STATUS_COLS + 1]) {
    char name[32], up[16], used[24], limit[24];
    uint32_t h = s->uptime_s / 3600, m = s->uptime_s / 60 % 60;
    if (h > 0) {
        snprintf(up, sizeof up, "%luh%02lum", (unsigned long)(h > 999 ? 999 : h), (unsigned long)m);
    } else {
        snprintf(up, sizeof up, "%lum", (unsigned long)m);
    }
    snprintf(name, sizeof name, "Node, up %s", up);
    title(out[0], name, 0);
    row(out[1], "id %08lx %s", (unsigned long)s->id, s->relay ? "relay" : "leaf");
    row(out[2], "%s%s %lu.%03lu MHz", s->region, s->off_profile ? "*" : "",
        (unsigned long)(s->freq_hz / 1000000), (unsigned long)(s->freq_hz / 1000 % 1000));
    row(out[3], "%lu kHz SF%u %+d dBm", (unsigned long)(s->bw_hz / 1000), s->sf, s->dbm);
    row(out[4], "hears %u, %u both ways", s->heard, s->up);
    row(out[5], "routes to %u", s->routed);
    row(out[6], "frames %lu out %lu in", (unsigned long)s->frames_out, (unsigned long)s->frames_in);
    if (s->limited) {
        seconds(used, sizeof used, s->air_used_ms);
        snprintf(limit, sizeof limit, "%lld", (long long)(s->air_limit_ms / 1000));
        row(out[7], "air %s of %s s", used, limit);
    } else {
        seconds(used, sizeof used, s->air_total_ms);
        row(out[7], "air %s s, no limit", used);
    }
}

static void neighbours(const struct node_status *s, char out[STATUS_ROWS][STATUS_COLS + 1]) {
    title(out[0], "Neighbours", 1);
    if (s->n_neighbours == 0) {
        row(out[1], "none heard yet");
        return;
    }
    for (int i = 0; i < s->n_neighbours && i < STATUS_LISTED; i++) {
        const struct status_neighbour *n = &s->neighbours[i];
        char spare[16];
        if (n->spare_db == INT16_MIN) {
            snprintf(spare, sizeof spare, "  ?");
        } else {
            snprintf(spare, sizeof spare, "%+3d", n->spare_db);
        }
        row(out[1 + i], "%08lx %c %s %3d %s", (unsigned long)n->id, n->relay ? 'R' : 'L',
            n->up ? "up" : "dn", n->floor_dbm, spare);
    }
    if (s->heard > s->n_neighbours) {
        row(out[7], "and %u more", (unsigned)(s->heard - s->n_neighbours));
    } else {
        /* Relay or leaf, link up or down, dBm it needs from us, dB it hears us with to spare. */
        row(out[7], "R/L up/dn needs spare");
    }
}

static void routes(const struct node_status *s, char out[STATUS_ROWS][STATUS_COLS + 1]) {
    title(out[0], "Routes", 2);
    if (s->n_routes == 0) {
        row(out[1], "none yet");
        return;
    }
    for (int i = 0; i < s->n_routes && i < STATUS_LISTED; i++) {
        const struct status_route *r = &s->routes[i];
        row(out[1 + i], "%08lx %08lx %3u", (unsigned long)r->dest, (unsigned long)r->next,
            r->metric_ms > 999 ? 999u : r->metric_ms);
    }
    if (s->routed > s->n_routes) {
        row(out[7], "and %u more", (unsigned)(s->routed - s->n_routes));
    } else {
        /* To this node, by that neighbour, so many milliseconds on the air. */
        row(out[7], "to       by        ms");
    }
}

static void session(const struct node_status *s, char out[STATUS_ROWS][STATUS_COLS + 1]) {
    title(out[0], "Session", 3);
    if (s->contacting) {
        row(out[7], "contact under way");
    }
    if (!s->session) {
        row(out[1], "none yet: type");
        row(out[2], "contact <address>");
        row(out[3], "on the console");
        return;
    }
    if (s->peers > 1) {
        row(out[1], "to %08lx, of %u", (unsigned long)s->peer, s->peers);
    } else {
        row(out[1], "with %08lx", (unsigned long)s->peer);
    }
    row(out[2], "sent %lu, heard %lu", (unsigned long)s->sent, (unsigned long)s->received);
    if (!s->have_last) {
        return;
    }
    char ago[16];
    span(ago, sizeof ago, s->last_s);
    row(out[3], "last heard, %s ago:", ago);
    /* The message itself, cut into whole rows, with anything unprintable shown as '?'. */
    const char *end = memchr(s->last, '\0', sizeof s->last);
    size_t len = end != NULL ? (size_t)(end - s->last) : sizeof s->last;
    int last_row = s->contacting ? 6 : 7;
    for (int row = 4; row <= last_row && len > 0; row++) {
        size_t take = len < STATUS_COLS ? len : STATUS_COLS;
        const char *from = s->last + (size_t)(row - 4) * STATUS_COLS;
        for (size_t i = 0; i < take; i++) {
            char c = from[i];
            out[row][i] = c >= 0x20 && c < 0x7F ? c : '?';
        }
        out[row][take] = '\0';
        len -= take;
    }
}

void status_page(const struct node_status *s, int page, char out[STATUS_ROWS][STATUS_COLS + 1]) {
    for (int i = 0; i < STATUS_ROWS; i++) {
        out[i][0] = '\0';
    }
    switch (page) {
    case 0:
        node(s, out);
        break;
    case 1:
        neighbours(s, out);
        break;
    case 2:
        routes(s, out);
        break;
    default:
        session(s, out);
        break;
    }
}
