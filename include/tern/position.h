#ifndef TERN_POSITION_H
#define TERN_POSITION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tern/time.h"

/* Positions: specification draft 0, draft/positions.md in ternmesh/spec.
 *
 * A position is where a node is, as a cell of a grid the whole world shares, as coarse as its
 * user chose: precision b splits longitude into 2^b columns and latitude into 2^(b-1) rows, so a
 * cell is 360 / 2^b degrees each way. The same point always gives the same cell, so a position
 * sent a hundred times tells no more than one. It travels as the plaintext of a message for the
 * node: a unicast message, or a group frame, with the flag `node` set.
 *
 * This is the arithmetic and the rules a node keeps: the cell, the bytes, what a receiver holds,
 * and when one is due. Whom a node shares with, and the frames that carry it, are the port's.
 *
 * Latitude and longitude are WGS 84 in units of 10^-7 degree, north and east positive. Every
 * value fits a signed 64-bit integer, and nothing here needs floating point. */

#define TERN_POSITION_KIND 0x02 /* the first byte of a message for the node that is a position */
#define TERN_POSITION_PRECISION_MAX 24
#define TERN_POSITION_ALTITUDE 0x01 /* form's low bits: which optional fields follow the cell */
#define TERN_POSITION_ACCURACY 0x02
#define TERN_POSITION_AGE 0x04
#define TERN_POSITION_FIELDS 0x07
#define TERN_POSITION_NO_ALTITUDE (-32768)
#define TERN_POSITION_MAX 12 /* kind, form, six bytes of cell, altitude, accuracy, age */

#define TERN_POSITION_LAT_MAX 900000000
#define TERN_POSITION_LON_MAX 1800000000

/* The draft's parameters. */
#define TERN_POSITION_MIN TERN_S(60)        /* the least interval for a contact */
#define TERN_POSITION_GROUP_MIN TERN_S(300) /* the least interval for a group */
#define TERN_POSITION_REFRESH TERN_S(3600)  /* an unchanged position is sent again after this */
#define TERN_POSITION_STALE TERN_S(3600)    /* a fix older than this is not sent */
#define TERN_POSITION_KEEP TERN_S(86400)    /* a received position is forgotten after this */

/* A position, as it goes on the air. precision 0 is a stopped position, with nothing else. */
struct tern_position {
    uint8_t precision; /* 0 to 24 */
    uint8_t fields;    /* TERN_POSITION_ALTITUDE, _ACCURACY and _AGE: which of the three follow */
    uint32_t row, column;
    int16_t altitude; /* metres above the ellipsoid; never TERN_POSITION_NO_ALTITUDE */
    uint8_t accuracy; /* metres, 1 to 255 */
    uint32_t age;     /* seconds: as read, the least its byte stands for */
};

/* The cell a point is in at a precision from 1 to 24: sets precision, row and column, and
 * nothing else. The point must be within the latitude and longitude limits. */
void tern_position_locate(struct tern_position *p, int32_t lat, int32_t lon, uint8_t precision);

/* A cell's centre, rounded down, which is where a receiver shows it, and its south-west corner,
 * exactly. Both for a precision from 1 to 24. */
void tern_position_centre(const struct tern_position *p, int32_t *lat, int32_t *lon);
void tern_position_corner(const struct tern_position *p, int32_t *south, int32_t *west);

/* Whether a fix is still in the cell a node last sent, taking a fix outside it by no more than a
 * quarter of a cell's height or width as in it: so a user on a cell's edge does not send each
 * side in turn, and say where the edge is. */
bool tern_position_near(const struct tern_position *last, int32_t lat, int32_t lon);

/* The bytes `where` takes at a precision: 2b - 1 bits, rounded up. 0 for a stopped position. */
size_t tern_position_where_len(uint8_t precision);

/* A fix's age as the byte the draft gives it, and the least age a byte stands for. */
uint8_t tern_position_age_byte(uint32_t seconds);
uint32_t tern_position_age_seconds(uint8_t byte);

/* Accuracy in metres as the byte: rounded up by the caller, then 1 to 255. */
uint8_t tern_position_accuracy_byte(uint32_t metres);

/* Writes a position into out (TERN_POSITION_MAX bytes), with age as tern_position_age_byte
 * makes it. Returns its length, or 0 if precision is above 24, a stopped position has fields,
 * a field is not defined, altitude is TERN_POSITION_NO_ALTITUDE or accuracy 0. */
size_t tern_position_write(const struct tern_position *p, uint8_t *out);

/* Reads a position from the plaintext of a message for the node. Returns false for one a node
 * must ignore: another kind, cut short, a precision above 24, a stopped position with fields, a
 * padding bit set, altitude -32768 or accuracy 0. Bytes past the fields are read past. */
bool tern_position_read(struct tern_position *p, const uint8_t *plaintext, size_t len);

/* Whether a position is due to a destination now. sent says whether one has gone there since
 * sharing was turned on or last changed, and last when it first went on the air; changed,
 * whether the cell is not that one's (by tern_position_near); age, how old the fix is. */
bool tern_position_due(tern_time interval, bool sent, tern_time last, bool changed, tern_time age,
                       tern_time now);

/* What a node keeps from the other end of one session: the position it holds, when it came, and
 * the counter of the last message it took a position from, kept after the position is forgotten
 * so that an older one cannot overtake a newer, or bring back one that was stopped. */
struct tern_position_held {
    struct tern_position position;
    tern_time at;     /* when it was received */
    uint32_t counter; /* of the last message a position was taken from */
    bool holds;       /* a position is held */
    bool counted;     /* counter is set */
};

/* For a new session: nothing held, and no counter. */
void tern_position_held_init(struct tern_position_held *h);

/* Takes a message for the node, received at `now` over the session at `counter`, from an
 * address that is a contact or not. Returns true if what is held changed: a position taken, or
 * the one held forgotten for a stopped one. A position from an address that is not a contact,
 * one the reader ignores, and one from a message older than the last taken change nothing. */
bool tern_position_receive(struct tern_position_held *h, bool contact, uint32_t counter,
                           const uint8_t *plaintext, size_t len, tern_time now);

/* Forgets a position held TERN_POSITION_KEEP or longer. Returns true if it did. */
bool tern_position_expire(struct tern_position_held *h, tern_time now);

#endif
