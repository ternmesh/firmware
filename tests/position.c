#include "tern/position.h"

#include <string.h>

#include "check.h"

/* Positions (draft/positions.md), against the specification's vectors: the grid, the bytes, what
 * a receiver holds, and when a position is due. Whom a node shares with, and the frames that
 * carry a position, are the port's, and are tested with it (tests/link.c). */

struct read_want {
    int32_t precision, row, column, lat, lon, south, west;
    int32_t altitude, accuracy, age; /* -1: not there */
};
struct read_result {
    bool some; /* else nothing read, or nothing held */
    struct read_want want;
};
struct grid_case {
    const char *name;
    int32_t lat, lon;
    uint8_t precision;
    uint32_t row, column;
    const uint8_t *where;
    size_t where_len;
    int32_t centre_lat, centre_lon, south, west;
};
struct position_case {
    const char *name;
    uint8_t precision;
    int32_t lat, lon;
    int32_t altitude, accuracy, age; /* -1: not given */
    const uint8_t *plaintext;
    size_t len;
    struct read_result read;
};
struct extended_case {
    const uint8_t *plaintext;
    size_t len;
    struct read_result read;
};
struct ignored_case {
    const char *why;
    const uint8_t *plaintext;
    size_t len;
};
struct age_case {
    uint32_t seconds;
    uint8_t byte;
    uint32_t read_seconds;
};
struct delivery {
    bool contact;
    uint32_t counter;
    const uint8_t *plaintext;
    size_t len;
    struct read_result holds;
};
struct receiving_case {
    const char *name;
    const struct delivery *deliveries;
    size_t count;
};
struct schedule_case {
    const char *note;
    int64_t interval;
    bool sent;
    int64_t last_at;
    bool changed;
    int64_t age, now;
    bool due;
};

#include "positions.h"

#define COUNT(a) (sizeof(a) / sizeof(a)[0])

/* That p is what the vectors say a reader gives. */
static void check_read(const struct tern_position *p, const struct read_want *w) {
    CHECK_EQ_I64(p->precision, w->precision);
    if (w->precision == 0) {
        CHECK_EQ_U64(p->fields, 0);
        return;
    }
    CHECK_EQ_U64(p->row, (uint32_t)w->row);
    CHECK_EQ_U64(p->column, (uint32_t)w->column);
    int32_t lat, lon, south, west;
    tern_position_centre(p, &lat, &lon);
    tern_position_corner(p, &south, &west);
    CHECK_EQ_I64(lat, w->lat);
    CHECK_EQ_I64(lon, w->lon);
    CHECK_EQ_I64(south, w->south);
    CHECK_EQ_I64(west, w->west);
    CHECK((p->fields & TERN_POSITION_ALTITUDE) == (w->altitude != -1 ? TERN_POSITION_ALTITUDE : 0));
    CHECK((p->fields & TERN_POSITION_ACCURACY) == (w->accuracy != -1 ? TERN_POSITION_ACCURACY : 0));
    CHECK((p->fields & TERN_POSITION_AGE) == (w->age != -1 ? TERN_POSITION_AGE : 0));
    if (w->altitude != -1) {
        CHECK_EQ_I64(p->altitude, w->altitude);
    }
    if (w->accuracy != -1) {
        CHECK_EQ_I64(p->accuracy, w->accuracy);
    }
    if (w->age != -1) {
        CHECK_EQ_I64(p->age, w->age);
    }
}

static void every_point_is_in_its_cell(void) {
    for (size_t i = 0; i < COUNT(grid); i++) {
        const struct grid_case *c = &grid[i];
        struct tern_position p = {0};
        tern_position_locate(&p, c->lat, c->lon, c->precision);
        CHECK_EQ_U64(p.row, c->row);
        CHECK_EQ_U64(p.column, c->column);

        uint8_t out[TERN_POSITION_MAX];
        size_t n = tern_position_write(&p, out);
        CHECK_EQ_U64(tern_position_where_len(c->precision), c->where_len);
        CHECK_EQ_U64(n, 2 + c->where_len);
        CHECK(memcmp(out + 2, c->where, c->where_len) == 0);

        struct tern_position back;
        CHECK(tern_position_read(&back, out, n));
        CHECK_EQ_U64(back.row, c->row);
        CHECK_EQ_U64(back.column, c->column);
        int32_t lat, lon, south, west;
        tern_position_centre(&back, &lat, &lon);
        tern_position_corner(&back, &south, &west);
        CHECK_EQ_I64(lat, c->centre_lat);
        CHECK_EQ_I64(lon, c->centre_lon);
        CHECK_EQ_I64(south, c->south);
        CHECK_EQ_I64(west, c->west);
    }
}

static void positions_are_written_and_read(void) {
    for (size_t i = 0; i < COUNT(positions); i++) {
        const struct position_case *c = &positions[i];
        struct tern_position p = {0};
        if (c->precision != 0) {
            tern_position_locate(&p, c->lat, c->lon, c->precision);
        }
        if (c->altitude != -1) {
            p.fields |= TERN_POSITION_ALTITUDE;
            p.altitude = (int16_t)c->altitude;
        }
        if (c->accuracy != -1) {
            p.fields |= TERN_POSITION_ACCURACY;
            p.accuracy = tern_position_accuracy_byte((uint32_t)c->accuracy);
        }
        if (c->age != -1) {
            p.fields |= TERN_POSITION_AGE;
            p.age = (uint32_t)c->age;
        }
        uint8_t out[TERN_POSITION_MAX];
        size_t n = tern_position_write(&p, out);
        CHECK_EQ_U64(n, c->len);
        CHECK(memcmp(out, c->plaintext, c->len) == 0);

        struct tern_position got;
        CHECK(tern_position_read(&got, c->plaintext, c->len));
        check_read(&got, &c->read.want);
    }
}

static void bytes_past_the_fields_are_read_past(void) {
    for (size_t i = 0; i < COUNT(extended); i++) {
        struct tern_position got;
        CHECK(tern_position_read(&got, extended[i].plaintext, extended[i].len));
        check_read(&got, &extended[i].read.want);
    }
}

static void ignored_positions_are_not_read(void) {
    for (size_t i = 0; i < COUNT(ignored); i++) {
        struct tern_position got;
        if (tern_position_read(&got, ignored[i].plaintext, ignored[i].len)) {
            fprintf(stderr, "read what should be ignored: %s\n", ignored[i].why);
            check_failures++;
        }
    }
}

static void ages_are_bytes_and_back(void) {
    for (size_t i = 0; i < COUNT(ages); i++) {
        CHECK_EQ_U64(tern_position_age_byte(ages[i].seconds), ages[i].byte);
        CHECK_EQ_U64(tern_position_age_seconds(ages[i].byte), ages[i].read_seconds);
    }
}

/* Nothing written that a reader would ignore. */
static void what_a_reader_ignores_is_not_written(void) {
    uint8_t out[TERN_POSITION_MAX];
    struct tern_position p = {.precision = 25};
    CHECK_EQ_U64(tern_position_write(&p, out), 0);
    p = (struct tern_position){.precision = 0, .fields = TERN_POSITION_AGE};
    CHECK_EQ_U64(tern_position_write(&p, out), 0);
    p = (struct tern_position){
        .precision = 12, .fields = TERN_POSITION_ALTITUDE, .altitude = TERN_POSITION_NO_ALTITUDE};
    CHECK_EQ_U64(tern_position_write(&p, out), 0);
    p = (struct tern_position){.precision = 12, .fields = TERN_POSITION_ACCURACY};
    CHECK_EQ_U64(tern_position_write(&p, out), 0);
    p = (struct tern_position){.precision = 12, .fields = 0x08};
    CHECK_EQ_U64(tern_position_write(&p, out), 0);
}

static void a_node_holds_the_newest_from_a_contact(void) {
    for (size_t i = 0; i < COUNT(receiving); i++) {
        const struct receiving_case *c = &receiving[i];
        struct tern_position_held h;
        tern_position_held_init(&h);
        for (size_t k = 0; k < c->count; k++) {
            const struct delivery *d = &c->deliveries[k];
            (void)tern_position_receive(&h, d->contact, d->counter, d->plaintext, d->len,
                                        TERN_S(1000 + (int64_t)k));
            if (h.holds != d->holds.some) {
                fprintf(stderr, "%s, delivery %zu: holds %d\n", c->name, k, h.holds);
                check_failures++;
            } else if (h.holds) {
                check_read(&h.position, &d->holds.want);
            }
        }
    }
}

static void a_received_position_is_forgotten_after_a_day(void) {
    static const uint8_t town[] = {0x02, 0x60, 0xc1, 0x30, 0x9c};
    struct tern_position_held h;
    tern_position_held_init(&h);
    CHECK(tern_position_receive(&h, true, 0, town, sizeof town, TERN_S(10)));
    CHECK(!tern_position_expire(&h, TERN_S(10) + TERN_POSITION_KEEP - 1));
    CHECK(h.holds);
    CHECK(tern_position_expire(&h, TERN_S(10) + TERN_POSITION_KEEP));
    CHECK(!h.holds);
    /* The counter stays: an older message brings back nothing. */
    CHECK(h.counted && h.counter == 0);
}

static void a_position_is_due_as_the_schedule_says(void) {
    for (size_t i = 0; i < COUNT(schedule); i++) {
        const struct schedule_case *c = &schedule[i];
        bool due = tern_position_due(TERN_S(c->interval), c->sent, TERN_S(c->last_at), c->changed,
                                     TERN_S(c->age), TERN_S(c->now));
        if (due != c->due) {
            fprintf(stderr, "schedule: %s: due %d\n", c->note, due);
            check_failures++;
        }
    }
}

/* A cell's edge: a quarter of a cell past it on any side is still the cell, and longitude wraps
 * at 180 degrees. */
static void a_fix_near_the_last_cell_is_in_it(void) {
    struct tern_position last;
    /* Precision 8: a cell is 360/256 degrees, 14062500 in 10^-7 degree, each way. */
    tern_position_locate(&last, 0, 0, 8);
    int32_t south, west;
    tern_position_corner(&last, &south, &west);
    const int32_t size = 14062500, quarter = size / 4;
    CHECK(tern_position_near(&last, south + size / 2, west + size / 2));
    CHECK(tern_position_near(&last, south - quarter + 1, west));
    CHECK(!tern_position_near(&last, south - quarter - size / 8, west));
    CHECK(tern_position_near(&last, south, west + size + quarter - 1));
    CHECK(!tern_position_near(&last, south, west + size + quarter + size / 8));
    CHECK(!tern_position_near(&last, south + 3 * size, west));

    /* The last column, at 180 degrees east, is next to the first. */
    tern_position_locate(&last, 0, -TERN_POSITION_LON_MAX, 8);
    CHECK_EQ_U64(last.column, 0);
    CHECK(tern_position_near(&last, 0, TERN_POSITION_LON_MAX - quarter / 2));
    CHECK(!tern_position_near(&last, 0, TERN_POSITION_LON_MAX - size));

    struct tern_position stopped = {0};
    CHECK(!tern_position_near(&stopped, 0, 0));
}

int main(void) {
    RUN(every_point_is_in_its_cell);
    RUN(positions_are_written_and_read);
    RUN(bytes_past_the_fields_are_read_past);
    RUN(ignored_positions_are_not_read);
    RUN(ages_are_bytes_and_back);
    RUN(what_a_reader_ignores_is_not_written);
    RUN(a_node_holds_the_newest_from_a_contact);
    RUN(a_received_position_is_forgotten_after_a_day);
    RUN(a_position_is_due_as_the_schedule_says);
    RUN(a_fix_near_the_last_cell_is_in_it);
    return CHECK_DONE();
}
