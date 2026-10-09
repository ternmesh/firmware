#include "tern/position.h"

#include <string.h>

#define LAT_SPAN INT64_C(1800000000) /* 180 degrees, in 10^-7 degree */
#define LON_SPAN INT64_C(3600000000) /* 360 degrees */
#define LAT_MIN (-INT64_C(900000000))
#define LON_MIN (-INT64_C(1800000000))

void tern_position_locate(struct tern_position *p, int32_t lat, int32_t lon, uint8_t precision) {
    uint8_t b = precision;
    int64_t rows = INT64_C(1) << (b - 1);
    int64_t row = ((lat - LAT_MIN) << (b - 1)) / LAT_SPAN;
    p->precision = b;
    p->row = (uint32_t)(row < rows ? row : rows - 1); /* the north pole is in the top row */
    p->column = (uint32_t)((((lon - LON_MIN) << b) / LON_SPAN) % (INT64_C(1) << b));
}

void tern_position_centre(const struct tern_position *p, int32_t *lat, int32_t *lon) {
    uint8_t b = p->precision;
    *lat = (int32_t)((((2 * (int64_t)p->row + 1) * LAT_SPAN) >> b) + LAT_MIN);
    *lon = (int32_t)((((2 * (int64_t)p->column + 1) * LON_SPAN) >> (b + 1)) + LON_MIN);
}

void tern_position_corner(const struct tern_position *p, int32_t *south, int32_t *west) {
    uint8_t b = p->precision;
    *south = (int32_t)((((int64_t)p->row * LAT_SPAN) >> (b - 1)) + LAT_MIN);
    *west = (int32_t)((((int64_t)p->column * LON_SPAN) >> b) + LON_MIN);
}

/* In quarters of a cell: the cell a point is in covers four of them each way, and a quarter on
 * each side is near enough. Longitude wraps; latitude stops at the poles. */
bool tern_position_near(const struct tern_position *last, int32_t lat, int32_t lon) {
    uint8_t b = last->precision;
    if (b == 0) {
        return false;
    }
    int64_t q = ((lat - LAT_MIN) << (b + 1)) / LAT_SPAN;
    int64_t south = 4 * (int64_t)last->row;
    if (q < south - 1 || q > south + 4) {
        return false;
    }
    int64_t around = INT64_C(4) << b;
    int64_t qc = (((lon - LON_MIN) << (b + 2)) / LON_SPAN) % around;
    int64_t d = ((qc - 4 * (int64_t)last->column) % around + around) % around;
    return d <= 4 || d >= around - 1;
}

size_t tern_position_where_len(uint8_t precision) {
    return precision == 0 ? 0 : (2u * precision - 1 + 7) / 8;
}

uint8_t tern_position_age_byte(uint32_t seconds) {
    if (seconds < 60) {
        return (uint8_t)seconds;
    }
    uint32_t minutes = seconds / 60;
    return minutes >= 255 - 59 ? 255 : (uint8_t)(59 + minutes);
}

uint32_t tern_position_age_seconds(uint8_t byte) {
    return byte < 60 ? byte : (uint32_t)(byte - 59) * 60;
}

uint8_t tern_position_accuracy_byte(uint32_t metres) {
    return metres < 1 ? 1 : metres > 255 ? 255 : (uint8_t)metres;
}

size_t tern_position_write(const struct tern_position *p, uint8_t *out) {
    uint8_t b = p->precision;
    if (b > TERN_POSITION_PRECISION_MAX || (p->fields & ~TERN_POSITION_FIELDS) != 0 ||
        (b == 0 && p->fields != 0)) {
        return 0;
    }
    if (((p->fields & TERN_POSITION_ALTITUDE) && p->altitude == TERN_POSITION_NO_ALTITUDE) ||
        ((p->fields & TERN_POSITION_ACCURACY) && p->accuracy == 0)) {
        return 0;
    }
    out[0] = TERN_POSITION_KIND;
    out[1] = (uint8_t)(b << 3 | p->fields);
    size_t at = 2;
    if (b == 0) {
        return at;
    }
    size_t n = tern_position_where_len(b);
    unsigned pad = (unsigned)(8 * n - (2u * b - 1));
    uint64_t where = ((uint64_t)p->row << b | p->column) << pad;
    for (size_t i = 0; i < n; i++) {
        out[at + i] = (uint8_t)(where >> (8 * (n - 1 - i)));
    }
    at += n;
    if (p->fields & TERN_POSITION_ALTITUDE) {
        uint16_t a = (uint16_t)p->altitude;
        out[at++] = (uint8_t)(a >> 8);
        out[at++] = (uint8_t)a;
    }
    if (p->fields & TERN_POSITION_ACCURACY) {
        out[at++] = p->accuracy;
    }
    if (p->fields & TERN_POSITION_AGE) {
        out[at++] = tern_position_age_byte(p->age);
    }
    return at;
}

bool tern_position_read(struct tern_position *p, const uint8_t *plaintext, size_t len) {
    if (len < 2 || plaintext[0] != TERN_POSITION_KIND) {
        return false;
    }
    uint8_t b = plaintext[1] >> 3, fields = plaintext[1] & TERN_POSITION_FIELDS;
    if (b > TERN_POSITION_PRECISION_MAX || (b == 0 && fields != 0)) {
        return false;
    }
    struct tern_position got = {.precision = b, .fields = fields};
    if (b == 0) {
        *p = got;
        return true;
    }
    size_t n = tern_position_where_len(b);
    size_t need = 2 + n + (fields & TERN_POSITION_ALTITUDE ? 2 : 0) +
                  (fields & TERN_POSITION_ACCURACY ? 1 : 0) + (fields & TERN_POSITION_AGE ? 1 : 0);
    if (len < need) {
        return false;
    }
    uint64_t where = 0;
    for (size_t i = 0; i < n; i++) {
        where = where << 8 | plaintext[2 + i];
    }
    unsigned pad = (unsigned)(8 * n - (2u * b - 1));
    if (where & ((UINT64_C(1) << pad) - 1)) {
        return false;
    }
    where >>= pad;
    got.row = (uint32_t)(where >> b);
    got.column = (uint32_t)(where & ((UINT64_C(1) << b) - 1));
    size_t at = 2 + n;
    if (fields & TERN_POSITION_ALTITUDE) {
        got.altitude = (int16_t)(uint16_t)(plaintext[at] << 8 | plaintext[at + 1]);
        if (got.altitude == TERN_POSITION_NO_ALTITUDE) {
            return false;
        }
        at += 2;
    }
    if (fields & TERN_POSITION_ACCURACY) {
        got.accuracy = plaintext[at++];
        if (got.accuracy == 0) {
            return false;
        }
    }
    if (fields & TERN_POSITION_AGE) {
        got.age = tern_position_age_seconds(plaintext[at]);
    }
    *p = got;
    return true;
}

bool tern_position_due(tern_time interval, bool sent, tern_time last, bool changed, tern_time age,
                       tern_time now) {
    if (age > TERN_POSITION_STALE) {
        return false;
    }
    if (!sent) {
        return true;
    }
    if (now - last < interval) {
        return false;
    }
    return changed || now - last >= TERN_POSITION_REFRESH;
}

void tern_position_held_init(struct tern_position_held *h) { memset(h, 0, sizeof *h); }

bool tern_position_receive(struct tern_position_held *h, bool contact, uint32_t counter,
                           const uint8_t *plaintext, size_t len, tern_time now) {
    struct tern_position p;
    if (!contact || !tern_position_read(&p, plaintext, len)) {
        return false;
    }
    if (h->counted && counter < h->counter) {
        return false; /* overtaken on the way by one already taken */
    }
    h->counter = counter;
    h->counted = true;
    if (p.precision == 0) {
        bool had = h->holds;
        h->holds = false;
        memset(&h->position, 0, sizeof h->position);
        return had;
    }
    h->position = p;
    h->at = now;
    h->holds = true;
    return true;
}

bool tern_position_expire(struct tern_position_held *h, tern_time now) {
    if (!h->holds || now - h->at < TERN_POSITION_KEEP) {
        return false;
    }
    h->holds = false;
    memset(&h->position, 0, sizeof h->position);
    return true;
}
