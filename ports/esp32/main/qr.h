#ifndef HELTEC_V3_QR_H
#define HELTEC_V3_QR_H

#include <stdbool.h>
#include <stdint.h>

/* A QR code of one size: version 3, 29 modules square, at error correction level L, holding text
 * in alphanumeric mode (the digits, upper-case letters, space and $%*+-./:). That is room for 77
 * characters, which a node's address in hex, sixty-four, fits with some to spare.
 *
 * Written from ISO/IEC 18004: the bits of the text, Reed-Solomon over GF(256), the fixed patterns,
 * the zigzag the codewords are laid in, and the eight masks scored as it says. One size and one
 * mode keep it small enough to read: nothing else is needed on the screen.
 *
 * Nothing here touches the hardware, so tests/qr.c runs it on a host. */

#define QR_SIZE 29
#define QR_TEXT_MAX 77
#define QR_MASK_BEST (-1)

struct qr {
    uint32_t rows[QR_SIZE]; /* a bit for each module, column 0 the lowest; set is dark */
};

/* Encodes `text` with mask `mask` (0 to 7), or with the mask the standard's penalty rules choose
 * if it is QR_MASK_BEST. False, and nothing encoded, if the text is longer than QR_TEXT_MAX or has
 * a character alphanumeric mode does not. */
bool qr_encode(struct qr *q, const char *text, int mask);

/* Whether the module in column x of row y is dark. */
static inline bool qr_dark(const struct qr *q, int x, int y) { return (q->rows[y] >> x & 1u) != 0; }

#endif
