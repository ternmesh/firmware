#ifndef TERN_FE25519_H
#define TERN_FE25519_H

/* Arithmetic modulo p = 2^255 - 19, for X25519 and Ed25519. Internal to the core.
 *
 * An element is sixteen 16-bit limbs, least significant first, held in 64-bit integers so that a
 * product of two never overflows. Every operation leaves its result carried: limbs 1 to 15 below
 * 2^16, and limb 0 below 2^16 + 38. The value may exceed p until it is encoded, which reduces it
 * fully. Limbs are never negative (subtraction adds 4p first), so every shift is well defined.
 *
 * Nothing here branches on, or indexes memory by, the value of an element, except fe_tobytes'
 * caller-visible result and the functions marked vartime, which are only for public values. */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int64_t v[16];
} fe;

extern const fe fe_d;      /* the Edwards curve constant d = -121665 / 121666 */
extern const fe fe_d2;     /* 2d */
extern const fe fe_sqrtm1; /* a square root of -1: 2^((p - 1) / 4) */

void fe_0(fe *o);
void fe_1(fe *o);
void fe_add(fe *o, const fe *a, const fe *b);
void fe_sub(fe *o, const fe *a, const fe *b);
void fe_mul(fe *o, const fe *a, const fe *b);
void fe_sq(fe *o, const fe *a);
void fe_mul_small(fe *o, const fe *a, int64_t k); /* k below 2^20 */
void fe_inv(fe *o, const fe *a);                  /* a^(p - 2); 0 for 0 */
void fe_pow2523(fe *o, const fe *a);              /* a^((p - 5) / 8), for square roots */

/* 32 bytes, little-endian, top bit ignored. */
void fe_frombytes(fe *o, const uint8_t b[32]);
/* The canonical encoding: the value reduced below p. */
void fe_tobytes(uint8_t b[32], const fe *a);

/* Swaps a and b if bit is 1, leaves them if 0, in the same time either way. */
void fe_cswap(fe *a, fe *b, uint64_t bit);

bool fe_iszero(const fe *a); /* constant time */
bool fe_isodd(const fe *a);  /* the low bit of the canonical encoding */
bool fe_eq(const fe *a, const fe *b);

#endif
