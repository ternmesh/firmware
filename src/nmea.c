#include "tern/nmea.h"

#include <string.h>

/* GGA's fields, counted from the one after the sentence's name. */
enum {
    GGA_TIME = 1,
    GGA_LAT,
    GGA_NS,
    GGA_LON,
    GGA_EW,
    GGA_QUALITY,
    GGA_SATELLITES,
    GGA_HDOP,
    GGA_ALTITUDE,
    GGA_ALTITUDE_UNIT,
    GGA_SEPARATION,
    GGA_SEPARATION_UNIT,
    GGA_FIELDS,
};

#define E7 10000000LL

struct field {
    const char *s;
    size_t len;
};

void tern_nmea_init(struct tern_nmea *n) { memset(n, 0, sizeof *n); }

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* A decimal number, with an optional sign, as an integer scaled by 10^places: digits past those
 * places are dropped. False for an empty field, or for anything but digits and one point. */
static bool decimal(struct field f, unsigned places, int64_t *out) {
    size_t i = 0;
    bool negative = false;
    if (i < f.len && (f.s[i] == '-' || f.s[i] == '+')) {
        negative = f.s[i] == '-';
        i++;
    }
    int64_t v = 0;
    bool digits = false, point = false;
    unsigned after = 0;
    for (; i < f.len; i++) {
        char c = f.s[i];
        if (c == '.' && !point) {
            point = true;
            continue;
        }
        if (c < '0' || c > '9') {
            return false;
        }
        digits = true;
        if (point && after == places) {
            continue;
        }
        if (v > (INT64_MAX - 9) / 10) {
            return false;
        }
        v = v * 10 + (c - '0');
        after += point;
    }
    if (!digits) {
        return false;
    }
    for (; after < places; after++) {
        v *= 10;
    }
    *out = negative ? -v : v;
    return true;
}

/* A latitude (ddmm.mmmm) or longitude (dddmm.mmmm), with its hemisphere, in 10^-7 degree. */
static bool angle(struct field f, struct field side, char positive, char negative, int64_t most,
                  int32_t *out) {
    int64_t v; /* ddmm.mmmm, in units of 10^-7 */
    if (side.len != 1 || (side.s[0] != positive && side.s[0] != negative) || !decimal(f, 7, &v) ||
        v < 0) {
        return false;
    }
    int64_t degrees = v / (100 * E7), minutes = v % (100 * E7); /* minutes in 10^-7 */
    if (minutes >= 60 * E7) {
        return false;
    }
    int64_t e7 = degrees * E7 + (minutes + 30) / 60;
    if (e7 > most) {
        return false;
    }
    *out = (int32_t)(side.s[0] == negative ? -e7 : e7);
    return true;
}

/* The height above the ellipsoid, in whole metres: above sea level, and the geoid's separation
 * from the ellipsoid there. None if either is missing, or not in metres. */
static int16_t height(const struct field *f) {
    int64_t sea, separation;
    if (!decimal(f[GGA_ALTITUDE], 3, &sea) || !decimal(f[GGA_SEPARATION], 3, &separation) ||
        f[GGA_ALTITUDE_UNIT].len != 1 || f[GGA_ALTITUDE_UNIT].s[0] != 'M' ||
        f[GGA_SEPARATION_UNIT].len != 1 || f[GGA_SEPARATION_UNIT].s[0] != 'M') {
        return TERN_NMEA_NO_ALTITUDE;
    }
    int64_t mm = sea + separation;
    int64_t m = mm >= 0 ? (mm + 500) / 1000 : -((-mm + 500) / 1000);
    return m <= TERN_NMEA_NO_ALTITUDE || m > INT16_MAX ? TERN_NMEA_NO_ALTITUDE : (int16_t)m;
}

static enum tern_nmea_read gga(const struct field *f, size_t n, struct tern_nmea_fix *fix) {
    if (n < GGA_FIELDS) {
        return TERN_NMEA_NOTHING;
    }
    /* 0 is no fix. 1 to 5 are fixes made from the satellites, alone or with corrections; 6 and
     * up are estimates, a manual input and a simulator, none of them where the receiver is. */
    int64_t quality;
    if (!decimal(f[GGA_QUALITY], 0, &quality)) {
        return TERN_NMEA_NOTHING;
    }
    if (quality < 1 || quality > 5) {
        return TERN_NMEA_NO_FIX;
    }
    struct tern_nmea_fix x = {0};
    if (!angle(f[GGA_LAT], f[GGA_NS], 'N', 'S', 90 * E7, &x.lat) ||
        !angle(f[GGA_LON], f[GGA_EW], 'E', 'W', 180 * E7, &x.lon)) {
        return TERN_NMEA_NO_FIX;
    }
    x.altitude = height(f);
    int64_t satellites, hdop; /* hdop in hundredths */
    if (decimal(f[GGA_SATELLITES], 0, &satellites) && satellites >= 0) {
        x.satellites = satellites > UINT8_MAX ? UINT8_MAX : (uint8_t)satellites;
    }
    if (decimal(f[GGA_HDOP], 2, &hdop) && hdop > 0) {
        int64_t metres = (hdop * TERN_NMEA_UERE + 99) / 100;
        x.accuracy = metres > UINT16_MAX ? UINT16_MAX : (uint16_t)metres;
    }
    *fix = x;
    return TERN_NMEA_FIX;
}

enum tern_nmea_read tern_nmea_read(const char *s, size_t len, struct tern_nmea_fix *fix) {
    /* '$', the talker and the sentence's name, then fields, then '*' and two hex digits. */
    if (len < 9 || s[0] != '$' || s[len - 3] != '*') {
        return TERN_NMEA_NOTHING;
    }
    uint8_t sum = 0;
    for (size_t i = 1; i < len - 3; i++) {
        sum ^= (uint8_t)s[i];
    }
    int hi = hex_digit(s[len - 2]), lo = hex_digit(s[len - 1]);
    if (hi < 0 || lo < 0 || sum != (uint8_t)(hi << 4 | lo)) {
        return TERN_NMEA_NOTHING;
    }
    struct field f[GGA_FIELDS + 8];
    size_t n = 0, start = 1;
    for (size_t i = 1; i <= len - 3 && n < sizeof f / sizeof f[0]; i++) {
        if (i == len - 3 || s[i] == ',') {
            f[n++] = (struct field){s + start, i - start};
            start = i + 1;
        }
    }
    /* The name is the talker's two letters and three of its own, and a proprietary sentence's
     * begins with P: GGA from any talker. */
    if (f[0].len == 5 && f[0].s[0] != 'P' && memcmp(f[0].s + 2, "GGA", 3) == 0) {
        return gga(f, n, fix);
    }
    return TERN_NMEA_NOTHING;
}

enum tern_nmea_read tern_nmea_push(struct tern_nmea *n, uint8_t byte, struct tern_nmea_fix *fix) {
    if (byte == '$') {
        n->line[0] = '$';
        n->len = 1;
        n->in = true;
        return TERN_NMEA_NOTHING;
    }
    if (!n->in) {
        return TERN_NMEA_NOTHING;
    }
    if (byte == '\r' || byte == '\n') {
        n->in = false;
        return tern_nmea_read(n->line, n->len, fix);
    }
    /* NMEA is printable ASCII: anything else is noise, and so is a sentence too long. */
    if (byte < 0x20 || byte > 0x7E || n->len == sizeof n->line) {
        n->in = false;
        return TERN_NMEA_NOTHING;
    }
    n->line[n->len++] = (char)byte;
    return TERN_NMEA_NOTHING;
}
