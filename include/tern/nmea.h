#ifndef TERN_NMEA_H
#define TERN_NMEA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A satellite receiver's fix, from the NMEA 0183 sentences it sends over its serial port.
 *
 * A board with a receiver (a GPS, or one of the receivers that hears several constellations)
 * passes on every byte the receiver sends; this finds the sentences in them, checks each one's
 * checksum, and reads the fix from GGA, which every receiver sends once a fix, from whichever
 * constellation (GPGGA, GNGGA, ...). Nothing else is read: GGA has the position, the height, how
 * many satellites made the fix and how good their geometry was, and whether there is a fix at
 * all.
 *
 * What comes out is what draft/companion.md's FIX and draft/positions.md want of a node's own fix:
 * latitude and longitude in units of 10^-7 degree, the height above the WGS 84 ellipsoid in whole
 * metres (GGA's height above sea level and the geoid's separation, added), and an accuracy in
 * metres. A receiver gives no accuracy in GGA, only the horizontal dilution of precision, a
 * multiplier on its error; the accuracy is that times TERN_NMEA_UERE, a usual figure for one
 * receiver on its own, and an estimate. No floating point. */

#define TERN_NMEA_MAX 82 /* a sentence, '$' to the line's end, at most, as NMEA 0183 has it */
#define TERN_NMEA_NO_ALTITUDE (-32768)
#define TERN_NMEA_UERE 5 /* metres of error for each unit of the dilution of precision */

struct tern_nmea_fix {
    int32_t lat, lon;   /* 10^-7 degree, north and east positive */
    int16_t altitude;   /* metres above the ellipsoid, or TERN_NMEA_NO_ALTITUDE */
    uint16_t accuracy;  /* metres, 1 at least, or 0 if the receiver gave no dilution */
    uint8_t satellites; /* used in the fix, as the receiver says */
};

enum tern_nmea_read {
    TERN_NMEA_NOTHING, /* no sentence ended, or one that says nothing of a fix */
    TERN_NMEA_FIX,     /* a GGA with a fix: *fix is it */
    TERN_NMEA_NO_FIX,  /* a GGA that says the receiver has none */
};

/* The sentence being read from a stream of bytes. */
struct tern_nmea {
    char line[TERN_NMEA_MAX];
    size_t len;
    bool in; /* within a sentence: after its '$', and it has not run past TERN_NMEA_MAX */
};

void tern_nmea_init(struct tern_nmea *n);

/* One byte from the receiver. When it ends a sentence, what the sentence said: *fix is written
 * only for TERN_NMEA_FIX. A sentence whose checksum is wrong or missing, longer than NMEA allows,
 * or cut short by the next '$' says nothing. */
enum tern_nmea_read tern_nmea_push(struct tern_nmea *n, uint8_t byte, struct tern_nmea_fix *fix);

/* One whole sentence, from its '$' to before its line's end. */
enum tern_nmea_read tern_nmea_read(const char *s, size_t len, struct tern_nmea_fix *fix);

#endif
