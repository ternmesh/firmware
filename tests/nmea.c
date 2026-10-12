#include "tern/nmea.h"

#include <string.h>

#include "check.h"

/* A satellite receiver's sentences (src/nmea.c): the fix GGA gives, from any talker, in the units
 * a node's fix is kept in, and every way a sentence is not one to take a fix from. The sentences
 * are written here from NMEA 0183's description of GGA, with their checksums worked out below
 * rather than copied, so that a mistake in one is a mistake in the reader. */

/* A sentence: '$', the body, '*' and its checksum, the XOR of the body's bytes. */
static size_t sentence(const char *body, char *out) {
    uint8_t sum = 0;
    size_t n = strlen(body);
    for (size_t i = 0; i < n; i++) {
        sum ^= (uint8_t)body[i];
    }
    static const char hex[] = "0123456789ABCDEF";
    out[0] = '$';
    memcpy(out + 1, body, n);
    out[n + 1] = '*';
    out[n + 2] = hex[sum >> 4];
    out[n + 3] = hex[sum & 15];
    out[n + 4] = '\0';
    return n + 4;
}

static enum tern_nmea_read read_body(const char *body, struct tern_nmea_fix *fix) {
    char s[128];
    size_t len = sentence(body, s);
    return tern_nmea_read(s, len, fix);
}

/* The summit the specification's vectors stand on, 45.8325 N 6.8644 E, as a receiver gives it:
 * degrees and minutes, its height above sea level and the geoid's above the ellipsoid. */
static const char summit[] =
    "GNGGA,092750.000,4549.9500,N,00651.8640,E,1,09,0.62,4757.2,M,48.4,M,,";

static void a_fix_is_read_in_tenths_of_a_microdegree(void) {
    struct tern_nmea_fix fix;
    CHECK(read_body(summit, &fix) == TERN_NMEA_FIX);
    CHECK_EQ_I64(fix.lat, 458325000);
    CHECK_EQ_I64(fix.lon, 68644000);
    /* 4757.2 m above the sea, and the geoid 48.4 m above the ellipsoid: 4805.6, to the metre. */
    CHECK_EQ_I64(fix.altitude, 4806);
    /* A dilution of 0.62, at five metres each: 3.1 m, rounded up. */
    CHECK_EQ_U64(fix.accuracy, 4);
    CHECK_EQ_U64(fix.satellites, 9);
}

static void south_and_west_are_negative(void) {
    struct tern_nmea_fix fix;
    CHECK(read_body("GPGGA,120000,3337.0980,S,07027.1020,W,2,12,1.0,-12.3,M,-30.1,M,1.2,0000",
                    &fix) == TERN_NMEA_FIX);
    CHECK_EQ_I64(fix.lat, -336183000);
    CHECK_EQ_I64(fix.lon, -704517000);
    CHECK_EQ_I64(fix.altitude, -42); /* -42.4 */
    CHECK_EQ_U64(fix.accuracy, 5);
    CHECK_EQ_U64(fix.satellites, 12);
}

/* Minutes to the seventh decimal of a degree are rounded to the nearest: 0.00001' is 1.67e-7. */
static void minutes_are_rounded_to_the_nearest(void) {
    struct tern_nmea_fix fix;
    CHECK(read_body("GPGGA,0,0000.00001,N,00000.00002,E,1,4,1,0,M,0,M,,", &fix) == TERN_NMEA_FIX);
    CHECK_EQ_I64(fix.lat, 2);
    CHECK_EQ_I64(fix.lon, 3);
    /* And places past the seventh are dropped. */
    CHECK(read_body("GPGGA,0,8959.999999999,N,17959.99999999,W,1,4,1,0,M,0,M,,", &fix) ==
          TERN_NMEA_FIX);
    CHECK_EQ_I64(fix.lat, 900000000);
    CHECK_EQ_I64(fix.lon, -1800000000);
}

/* Each constellation's talker sends GGA alike: Galileo's, BeiDou's and GLONASS's too. */
static void any_talker_gives_a_fix(void) {
    static const char *talkers[] = {"GP", "GN", "GL", "GA", "GB", "BD"};
    for (size_t i = 0; i < sizeof talkers / sizeof talkers[0]; i++) {
        char body[sizeof summit];
        memcpy(body, summit, sizeof summit);
        memcpy(body, talkers[i], 2);
        struct tern_nmea_fix fix;
        CHECK(read_body(body, &fix) == TERN_NMEA_FIX);
    }
}

/* A receiver without a fix says so, as quality 0, with its other fields empty or not. So do
 * estimates of every kind, which are not where the receiver is. */
static void no_fix_is_said_and_estimates_are_not_fixes(void) {
    struct tern_nmea_fix fix = {.lat = 7};
    CHECK(read_body("GPGGA,,,,,,0,00,99.99,,,,,,", &fix) == TERN_NMEA_NO_FIX);
    CHECK(read_body("GNGGA,092750.000,4549.9500,N,00651.8640,E,0,03,9.9,4757.2,M,48.4,M,,", &fix) ==
          TERN_NMEA_NO_FIX);
    CHECK(read_body("GNGGA,092750.000,4549.9500,N,00651.8640,E,6,09,0.62,4757.2,M,48.4,M,,",
                    &fix) == TERN_NMEA_NO_FIX);
    CHECK(read_body("GNGGA,092750.000,4549.9500,N,00651.8640,E,8,09,0.62,4757.2,M,48.4,M,,",
                    &fix) == TERN_NMEA_NO_FIX);
    CHECK_EQ_I64(fix.lat, 7); /* untouched */
}

/* A fix that claims a place that is not one is no fix. */
static void a_place_that_is_not_one_is_no_fix(void) {
    struct tern_nmea_fix fix;
    CHECK(read_body("GPGGA,0,4560.0000,N,00651.8640,E,1,09,0.62,0,M,0,M,,", &fix) ==
          TERN_NMEA_NO_FIX);
    CHECK(read_body("GPGGA,0,9000.0001,N,00651.8640,E,1,09,0.62,0,M,0,M,,", &fix) ==
          TERN_NMEA_NO_FIX);
    CHECK(read_body("GPGGA,0,4549.9500,X,00651.8640,E,1,09,0.62,0,M,0,M,,", &fix) ==
          TERN_NMEA_NO_FIX);
    CHECK(read_body("GPGGA,0,,N,00651.8640,E,1,09,0.62,0,M,0,M,,", &fix) == TERN_NMEA_NO_FIX);
    CHECK(read_body("GPGGA,0,45x9.9500,N,00651.8640,E,1,09,0.62,0,M,0,M,,", &fix) ==
          TERN_NMEA_NO_FIX);
}

/* Without the geoid's separation the height above the ellipsoid cannot be had, and the fix has
 * none; nor without the height itself, nor in a unit that is not metres. Without a dilution
 * there is no accuracy. */
static void what_is_missing_is_left_out(void) {
    struct tern_nmea_fix fix;
    CHECK(read_body("GPGGA,0,4549.9500,N,00651.8640,E,1,09,,4757.2,M,,M,,", &fix) == TERN_NMEA_FIX);
    CHECK_EQ_I64(fix.altitude, TERN_NMEA_NO_ALTITUDE);
    CHECK_EQ_U64(fix.accuracy, 0);
    CHECK(read_body("GPGGA,0,4549.9500,N,00651.8640,E,1,,0.5,,M,48.4,M,,", &fix) == TERN_NMEA_FIX);
    CHECK_EQ_I64(fix.altitude, TERN_NMEA_NO_ALTITUDE);
    CHECK_EQ_U64(fix.accuracy, 3);
    CHECK_EQ_U64(fix.satellites, 0);
    CHECK(read_body("GPGGA,0,4549.9500,N,00651.8640,E,1,09,0.62,15600,F,159,F,,", &fix) ==
          TERN_NMEA_FIX);
    CHECK_EQ_I64(fix.altitude, TERN_NMEA_NO_ALTITUDE);
    /* A height past what a fix carries is none either. */
    CHECK(read_body("GPGGA,0,4549.9500,N,00651.8640,E,1,09,0.62,40000,M,0,M,,", &fix) ==
          TERN_NMEA_FIX);
    CHECK_EQ_I64(fix.altitude, TERN_NMEA_NO_ALTITUDE);
}

/* A sentence is read only whole and as sent: its checksum right, in either case of hex. */
static void a_checksum_must_be_right(void) {
    struct tern_nmea_fix fix;
    char s[128];
    size_t len = sentence(summit, s);
    CHECK(tern_nmea_read(s, len, &fix) == TERN_NMEA_FIX);
    s[len - 1] = s[len - 1] == '0' ? '1' : '0';
    CHECK(tern_nmea_read(s, len, &fix) == TERN_NMEA_NOTHING);
    len = sentence(summit, s);
    s[20] = '8'; /* a digit of the latitude changed on the way */
    CHECK(tern_nmea_read(s, len, &fix) == TERN_NMEA_NOTHING);
    /* Lower-case hex is hex. */
    len = sentence(summit, s);
    for (size_t i = len - 2; i < len; i++) {
        if (s[i] >= 'A' && s[i] <= 'F') {
            s[i] = (char)(s[i] - 'A' + 'a');
        }
    }
    CHECK(tern_nmea_read(s, len, &fix) == TERN_NMEA_FIX);
    /* With no checksum at all, it is not taken. */
    char bare[sizeof summit + 1] = "$";
    memcpy(bare + 1, summit, sizeof summit);
    CHECK(tern_nmea_read(bare, strlen(bare), &fix) == TERN_NMEA_NOTHING);
}

/* Other sentences say nothing of a fix here, a proprietary one named like GGA included. */
static void other_sentences_say_nothing(void) {
    struct tern_nmea_fix fix;
    CHECK(read_body("GNRMC,092750.000,A,4549.9500,N,00651.8640,E,0.02,31.66,121026,,,A", &fix) ==
          TERN_NMEA_NOTHING);
    CHECK(read_body("GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00", &fix) ==
          TERN_NMEA_NOTHING);
    CHECK(read_body("PQGGA,092750.000,4549.9500,N,00651.8640,E,1,09,0.62,4757.2,M,48.4,M,,",
                    &fix) == TERN_NMEA_NOTHING);
    CHECK(read_body("GPGGA,092750.000,4549.9500,N", &fix) == TERN_NMEA_NOTHING);
    CHECK(tern_nmea_read("", 0, &fix) == TERN_NMEA_NOTHING);
}

static enum tern_nmea_read push_all(struct tern_nmea *n, const char *bytes, size_t len,
                                    struct tern_nmea_fix *fix, unsigned *fixes) {
    enum tern_nmea_read last = TERN_NMEA_NOTHING;
    for (size_t i = 0; i < len; i++) {
        enum tern_nmea_read r = tern_nmea_push(n, (uint8_t)bytes[i], fix);
        *fixes += r == TERN_NMEA_FIX;
        last = r != TERN_NMEA_NOTHING ? r : last;
    }
    return last;
}

/* From the serial port, byte by byte: noise before a sentence and between them, a sentence cut
 * short by the next one's '$', one too long for NMEA, and the line's end as CR, LF or both. */
static void sentences_are_found_in_a_stream(void) {
    struct tern_nmea n;
    struct tern_nmea_fix fix;
    unsigned fixes = 0;
    char s[128];
    size_t len = sentence(summit, s);
    tern_nmea_init(&n);

    CHECK(push_all(&n, "\xff\x00garbage", 9, &fix, &fixes) == TERN_NMEA_NOTHING);
    push_all(&n, s, len, &fix, &fixes);
    CHECK_EQ_U64(fixes, 0); /* not until its line ends */
    CHECK(push_all(&n, "\r\n", 2, &fix, &fixes) == TERN_NMEA_FIX);
    CHECK_EQ_U64(fixes, 1);
    CHECK_EQ_I64(fix.lat, 458325000);

    /* Cut short: the next '$' begins again, and the whole one after it is read. */
    push_all(&n, s, len / 2, &fix, &fixes);
    push_all(&n, s, len, &fix, &fixes);
    CHECK(push_all(&n, "\n", 1, &fix, &fixes) == TERN_NMEA_FIX);
    CHECK_EQ_U64(fixes, 2);

    /* A byte that is not printable spoils the sentence it is in. */
    push_all(&n, s, 10, &fix, &fixes);
    push_all(&n, "\x01", 1, &fix, &fixes);
    CHECK(push_all(&n, s + 10, len - 10, &fix, &fixes) == TERN_NMEA_NOTHING);
    CHECK(push_all(&n, "\r", 1, &fix, &fixes) == TERN_NMEA_NOTHING);
    CHECK_EQ_U64(fixes, 2);

    /* Longer than a sentence may be: not read, and the next is. */
    char longer[200] = "$GPGGA,";
    memset(longer + 7, '0', 150);
    CHECK(push_all(&n, longer, 157, &fix, &fixes) == TERN_NMEA_NOTHING);
    CHECK(push_all(&n, "\r\n", 2, &fix, &fixes) == TERN_NMEA_NOTHING);
    push_all(&n, s, len, &fix, &fixes);
    CHECK(push_all(&n, "\r", 1, &fix, &fixes) == TERN_NMEA_FIX);
    CHECK_EQ_U64(fixes, 3);

    /* A receiver that has lost its fix says so. */
    len = sentence("GPGGA,,,,,,0,00,99.99,,,,,,", s);
    push_all(&n, s, len, &fix, &fixes);
    CHECK(push_all(&n, "\r\n", 2, &fix, &fixes) == TERN_NMEA_NO_FIX);
}

int main(void) {
    RUN(a_fix_is_read_in_tenths_of_a_microdegree);
    RUN(south_and_west_are_negative);
    RUN(minutes_are_rounded_to_the_nearest);
    RUN(any_talker_gives_a_fix);
    RUN(no_fix_is_said_and_estimates_are_not_fixes);
    RUN(a_place_that_is_not_one_is_no_fix);
    RUN(what_is_missing_is_left_out);
    RUN(a_checksum_must_be_right);
    RUN(other_sentences_say_nothing);
    RUN(sentences_are_found_in_a_stream);
    return CHECK_DONE();
}
