#ifndef HELTEC_V3_QR_H
#define HELTEC_V3_QR_H

#include <stdbool.h>
#include <stdint.h>

/* A QR code of one size: version 3, 29 modules square, at error correction level L, the largest
 * the screen draws at two pixels a module. Text goes in runs: each run of the alphanumeric set (the
 * digits, upper-case letters, space and $%*+-./:) in alphanumeric mode, which is room for 77 of
 * them, and each run of anything else in byte mode. An address's link is one run of 75; a join
 * code's is two with its `#` between them, and fits whenever its group's name is 12 bytes or
 * fewer (draft/groups.md).
 *
 * Written from ISO/IEC 18004: the bits of the text, Reed-Solomon over GF(256), the fixed patterns,
 * the zigzag the codewords are laid in, and the eight masks scored as it says. One size and two
 * modes keep it small enough to read: nothing else is needed on the screen.
 *
 * Nothing here touches the hardware, so tests/qr.c runs it on a host. */

#define QR_SIZE 29
#define QR_TEXT_MAX 77 /* characters, all of the alphanumeric set */
#define QR_MASK_BEST (-1)

struct qr {
    uint32_t rows[QR_SIZE]; /* a bit for each module, column 0 the lowest; set is dark */
};

/* Encodes `text` with mask `mask` (0 to 7), or with the mask the standard's penalty rules choose
 * if it is QR_MASK_BEST. False, and nothing encoded, if its segments do not fit: QR_TEXT_MAX
 * characters of the alphanumeric set alone, and fewer the more of anything else. */
bool qr_encode(struct qr *q, const char *text, int mask);

/* Whether the module in column x of row y is dark. */
static inline bool qr_dark(const struct qr *q, int x, int y) { return (q->rows[y] >> x & 1u) != 0; }

#endif
