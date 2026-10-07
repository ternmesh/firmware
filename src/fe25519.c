#include "fe25519.h"

#include "tern/crypto.h"

const fe fe_d = {{0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070, 0xe898, 0x7779,
                  0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203}};
const fe fe_d2 = {{0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0, 0xd130, 0xeef3,
                   0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406}};
const fe fe_sqrtm1 = {{0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43, 0xd7a7,
                       0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83}};

/* 4p, limb by limb: added before subtracting, so no limb goes negative. Every limb of 4p is
 * above 2^17 - 5, and every limb of a carried element is below 2^16 + 38. */
static const int64_t four_p[16] = {0x3ffb4, 0x3fffc, 0x3fffc, 0x3fffc, 0x3fffc, 0x3fffc,
                                   0x3fffc, 0x3fffc, 0x3fffc, 0x3fffc, 0x3fffc, 0x3fffc,
                                   0x3fffc, 0x3fffc, 0x3fffc, 0x1fffc};

/* One pass: each limb keeps its low 16 bits and passes the rest up. What leaves the top limb is a
 * multiple of 2^256, which is 38 modulo p, so it comes back into limb 0 times 38. */
static void carry(fe *a) {
    for (int i = 0; i < 15; i++) {
        int64_t c = a->v[i] >> 16;
        a->v[i] &= 0xffff;
        a->v[i + 1] += c;
    }
    int64_t c = a->v[15] >> 16;
    a->v[15] &= 0xffff;
    a->v[0] += 38 * c;
}

void fe_0(fe *o) {
    for (int i = 0; i < 16; i++) {
        o->v[i] = 0;
    }
}

void fe_1(fe *o) {
    fe_0(o);
    o->v[0] = 1;
}

void fe_add(fe *o, const fe *a, const fe *b) {
    for (int i = 0; i < 16; i++) {
        o->v[i] = a->v[i] + b->v[i];
    }
    carry(o);
    carry(o);
}

void fe_sub(fe *o, const fe *a, const fe *b) {
    for (int i = 0; i < 16; i++) {
        o->v[i] = a->v[i] + four_p[i] - b->v[i];
    }
    carry(o);
    carry(o);
}

/* Each partial product is below 2^33 and a column sums sixteen of them; folding the top half back
 * in at 38 times keeps every column below 2^43. Two passes then carry it: the first leaves limb 0
 * below 2^33, and the second can pass at most 1 out of the top limb. */
void fe_mul(fe *o, const fe *a, const fe *b) {
    int64_t t[31] = {0};
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) {
            t[i + j] += a->v[i] * b->v[j];
        }
    }
    for (int i = 0; i < 15; i++) {
        t[i] += 38 * t[i + 16];
    }
    for (int i = 0; i < 16; i++) {
        o->v[i] = t[i];
    }
    carry(o);
    carry(o);
    tern_wipe(t, sizeof t);
}

void fe_sq(fe *o, const fe *a) { fe_mul(o, a, a); }

void fe_mul_small(fe *o, const fe *a, int64_t k) {
    for (int i = 0; i < 16; i++) {
        o->v[i] = a->v[i] * k;
    }
    carry(o);
    carry(o);
}

/* a^(p - 2), by squaring and multiplying through the bits of p - 2 = 2^255 - 21, from the top.
 * The exponent is public, so which steps multiply gives nothing away. Its bits are all ones
 * except bits 2 and 4. */
void fe_inv(fe *o, const fe *a) {
    fe c = *a;
    for (int i = 253; i >= 0; i--) {
        fe_sq(&c, &c);
        if (i != 2 && i != 4) {
            fe_mul(&c, &c, a);
        }
    }
    *o = c;
    tern_wipe(&c, sizeof c);
}

/* a^((p - 5) / 8) = a^(2^252 - 3), whose bits are all ones except bit 1. */
void fe_pow2523(fe *o, const fe *a) {
    fe c = *a;
    for (int i = 250; i >= 0; i--) {
        fe_sq(&c, &c);
        if (i != 1) {
            fe_mul(&c, &c, a);
        }
    }
    *o = c;
    tern_wipe(&c, sizeof c);
}

void fe_frombytes(fe *o, const uint8_t b[32]) {
    for (int i = 0; i < 16; i++) {
        o->v[i] = (int64_t)b[2 * i] | (int64_t)b[2 * i + 1] << 8;
    }
    o->v[15] &= 0x7fff;
}

/* Three passes leave every limb below 2^16, so the value is below 2^256 = 2p + 38. Subtracting p
 * where it does not borrow, twice, then gives the value below p. */
void fe_tobytes(uint8_t b[32], const fe *a) {
    static const uint64_t p[16] = {0xffed, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
                                   0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0x7fff};
    fe t = *a;
    carry(&t);
    carry(&t);
    carry(&t);
    for (int round = 0; round < 2; round++) {
        uint64_t m[16];
        uint64_t borrow = 0;
        for (int i = 0; i < 16; i++) {
            uint64_t d = (uint64_t)t.v[i] - p[i] - borrow;
            borrow = d >> 63;
            m[i] = d & 0xffff;
        }
        /* No borrow out of the top: t was at least p, so take t - p. */
        uint64_t take = 0 - (borrow ^ 1);
        for (int i = 0; i < 16; i++) {
            uint64_t x = take & ((uint64_t)t.v[i] ^ m[i]);
            t.v[i] = (int64_t)((uint64_t)t.v[i] ^ x);
        }
        tern_wipe(m, sizeof m);
    }
    for (int i = 0; i < 16; i++) {
        b[2 * i] = (uint8_t)t.v[i];
        b[2 * i + 1] = (uint8_t)(t.v[i] >> 8);
    }
    tern_wipe(&t, sizeof t);
}

void fe_cswap(fe *a, fe *b, uint64_t bit) {
    uint64_t mask = 0 - bit;
    for (int i = 0; i < 16; i++) {
        uint64_t x = mask & ((uint64_t)a->v[i] ^ (uint64_t)b->v[i]);
        a->v[i] = (int64_t)((uint64_t)a->v[i] ^ x);
        b->v[i] = (int64_t)((uint64_t)b->v[i] ^ x);
    }
}

bool fe_iszero(const fe *a) {
    uint8_t b[32];
    uint8_t acc = 0;
    fe_tobytes(b, a);
    for (int i = 0; i < 32; i++) {
        acc |= b[i];
    }
    tern_wipe(b, sizeof b);
    return acc == 0;
}

bool fe_isodd(const fe *a) {
    uint8_t b[32];
    fe_tobytes(b, a);
    bool odd = b[0] & 1;
    tern_wipe(b, sizeof b);
    return odd;
}

bool fe_eq(const fe *a, const fe *b) {
    fe d;
    fe_sub(&d, a, b);
    bool z = fe_iszero(&d);
    tern_wipe(&d, sizeof d);
    return z;
}
