#include "tern/address.h"

#include "fe25519.h"
#include "tern/crypto.h"

/* The Ed25519 group, as RFC 8032, section 5.1 defines it: the twisted Edwards curve
 * -x^2 + y^2 = 1 + d x^2 y^2 over GF(p), in extended coordinates (X, Y, Z, T) with x = X/Z,
 * y = Y/Z and xy = T/Z. The addition formula (section 5.1.4) is complete for this curve, so it
 * also doubles, and one formula serves for everything. */

typedef struct {
    fe x, y, z, t;
} ge;

static void ge_neutral(ge *p) {
    fe_0(&p->x);
    fe_1(&p->y);
    fe_1(&p->z);
    fe_0(&p->t);
}

/* RFC 8032's A to H, in five temporaries to keep the stack small: after D, E goes in t, H in a, F
 * in b and G in d. r may be p or q: nothing is written to r until both have been read. */
static void ge_add(ge *r, const ge *p, const ge *q) {
    fe a, b, c, d, t;
    fe_sub(&a, &p->y, &p->x);
    fe_sub(&t, &q->y, &q->x);
    fe_mul(&a, &a, &t); /* A */
    fe_add(&b, &p->y, &p->x);
    fe_add(&t, &q->y, &q->x);
    fe_mul(&b, &b, &t); /* B */
    fe_mul(&c, &p->t, &q->t);
    fe_mul(&c, &c, &fe_d2); /* C */
    fe_mul(&d, &p->z, &q->z);
    fe_add(&d, &d, &d); /* D */
    fe_sub(&t, &b, &a); /* E */
    fe_add(&a, &b, &a); /* H */
    fe_sub(&b, &d, &c); /* F */
    fe_add(&d, &d, &c); /* G */
    fe_mul(&r->x, &t, &b);
    fe_mul(&r->y, &d, &a);
    fe_mul(&r->t, &t, &a);
    fe_mul(&r->z, &b, &d);
}

static void ge_cswap(ge *p, ge *q, uint64_t bit) {
    fe_cswap(&p->x, &q->x, bit);
    fe_cswap(&p->y, &q->y, bit);
    fe_cswap(&p->z, &q->z, bit);
    fe_cswap(&p->t, &q->t, bit);
}

/* r = [k]p for a 256-bit little-endian k, by a ladder that does the same work for every bit:
 * r0 and r1 always differ by p, and each bit only decides, by a constant-time swap, which of them
 * is doubled. So it is safe for a secret k. r may be p. */
static void ge_scalarmult(ge *r, const uint8_t k[32], const ge *p) {
    ge r0, r1 = *p;
    ge_neutral(&r0);
    for (int i = 255; i >= 0; i--) {
        uint64_t bit = (k[i >> 3] >> (i & 7)) & 1;
        ge_cswap(&r0, &r1, bit);
        ge_add(&r1, &r0, &r1);
        ge_add(&r0, &r0, &r0);
        ge_cswap(&r0, &r1, bit);
    }
    *r = r0;
    tern_wipe(&r0, sizeof r0);
    tern_wipe(&r1, sizeof r1);
}

static bool ge_is_neutral(const ge *p) {
    /* (0, 1): X = 0 and Y = Z. */
    return fe_iszero(&p->x) && fe_eq(&p->y, &p->z);
}

static void ge_tobytes(uint8_t b[32], const ge *p) {
    fe zi, x, y;
    fe_inv(&zi, &p->z);
    fe_mul(&x, &p->x, &zi);
    fe_mul(&y, &p->y, &zi);
    fe_tobytes(b, &y);
    b[31] |= (uint8_t)(fe_isodd(&x) << 7);
    tern_wipe(&zi, sizeof zi);
    tern_wipe(&x, sizeof x);
    tern_wipe(&y, sizeof y);
}

/* RFC 8032, section 5.1.3. Vartime: only for public points. */
static bool ge_frombytes(ge *p, const uint8_t b[32]) {
    uint8_t check[32];
    fe u, v, v3, x, vxx, t;

    /* y must be below p: its encoding, top bit aside, must already be canonical. */
    fe_frombytes(&p->y, b);
    fe_tobytes(check, &p->y);
    check[31] |= b[31] & 0x80;
    for (int i = 0; i < 32; i++) {
        if (check[i] != b[i]) {
            return false;
        }
    }

    /* x^2 = (y^2 - 1) / (d y^2 + 1) = u / v; the candidate root is u v^3 (u v^7)^((p - 5) / 8). */
    fe_1(&p->z);
    fe_sq(&u, &p->y);
    fe_mul(&v, &u, &fe_d);
    fe_sub(&u, &u, &p->z);
    fe_add(&v, &v, &p->z);
    fe_sq(&v3, &v);
    fe_mul(&v3, &v3, &v);
    fe_sq(&x, &v3);
    fe_mul(&x, &x, &v);
    fe_mul(&x, &x, &u);
    fe_pow2523(&x, &x);
    fe_mul(&x, &x, &v3);
    fe_mul(&x, &x, &u);

    fe_sq(&vxx, &x);
    fe_mul(&vxx, &vxx, &v);
    if (!fe_eq(&vxx, &u)) {
        fe_0(&t);
        fe_sub(&t, &t, &u);
        if (!fe_eq(&vxx, &t)) {
            return false; /* no square root: no point has this y */
        }
        fe_mul(&x, &x, &fe_sqrtm1);
    }

    bool sign = b[31] >> 7;
    if (fe_iszero(&x) && sign) {
        return false;
    }
    if (fe_isodd(&x) != sign) {
        fe_0(&t);
        fe_sub(&x, &t, &x);
    }
    p->x = x;
    fe_mul(&p->t, &p->x, &p->y);
    return true;
}

/* The base point B, whose y is 4/5 and whose x is even (RFC 8032, section 5.1). */
static const uint8_t base_bytes[32] = {
    0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
};

/* L, the prime order of B, little-endian: 2^252 + 27742317777372353535851937790883648493. */
static const uint8_t order[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
};

static void secret_scalar(const uint8_t seed[TERN_SEED_LEN], uint8_t h[TERN_SHA512_LEN]) {
    struct tern_sha512 c;
    tern_sha512_init(&c);
    tern_sha512_update(&c, seed, TERN_SEED_LEN);
    tern_sha512_final(&c, h);
}

void tern_identity_init(struct tern_identity *id, const uint8_t seed[TERN_SEED_LEN]) {
    uint8_t h[TERN_SHA512_LEN];
    ge a;

    for (int i = 0; i < TERN_SEED_LEN; i++) {
        id->seed[i] = seed[i];
    }
    /* RFC 8032, section 5.1.5: the first half of SHA-512(seed), clamped, times B. */
    secret_scalar(seed, h);
    h[0] &= 248;
    h[31] &= 127;
    h[31] |= 64;
    (void)ge_frombytes(&a, base_bytes);
    ge_scalarmult(&a, h, &a);
    ge_tobytes(id->address, &a);

    tern_wipe(h, sizeof h);
    tern_wipe(&a, sizeof a);
}

void tern_identity_wipe(struct tern_identity *id) { tern_wipe(id, sizeof *id); }

void tern_identity_x25519(const struct tern_identity *id, uint8_t k[32]) {
    uint8_t h[TERN_SHA512_LEN];
    secret_scalar(id->seed, h);
    for (int i = 0; i < 32; i++) {
        k[i] = h[i];
    }
    tern_wipe(h, sizeof h);
}

bool tern_address_valid(const uint8_t address[TERN_ADDRESS_LEN]) {
    ge a;
    if (!ge_frombytes(&a, address) || ge_is_neutral(&a)) {
        return false;
    }
    ge_scalarmult(&a, order, &a);
    return ge_is_neutral(&a);
}

bool tern_address_x25519(uint8_t u[32], const uint8_t address[TERN_ADDRESS_LEN]) {
    fe y, one, num, den;
    if (!tern_address_valid(address)) {
        return false;
    }
    fe_frombytes(&y, address); /* valid, so y is canonical; the top bit is x's sign */
    fe_1(&one);
    fe_add(&num, &one, &y);
    fe_sub(&den, &one, &y);
    fe_inv(&den, &den);
    fe_mul(&num, &num, &den);
    fe_tobytes(u, &num);
    return true;
}
