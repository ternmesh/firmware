#include "tern/address.h"

#include <stddef.h>

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

/* r = p - q: the addition with q's negation, (-X, Y, Z, -T), which swaps the sums and differences
 * of q's coordinates in A and B and the sign of C. r may be p or q. */
static void ge_sub(ge *r, const ge *p, const ge *q) {
    fe a, b, c, d, t;
    fe_sub(&a, &p->y, &p->x);
    fe_add(&t, &q->y, &q->x);
    fe_mul(&a, &a, &t); /* A */
    fe_add(&b, &p->y, &p->x);
    fe_sub(&t, &q->y, &q->x);
    fe_mul(&b, &b, &t); /* B */
    fe_mul(&c, &p->t, &q->t);
    fe_mul(&c, &c, &fe_d2); /* -C */
    fe_mul(&d, &p->z, &q->z);
    fe_add(&d, &d, &d); /* D */
    fe_sub(&t, &b, &a); /* E */
    fe_add(&a, &b, &a); /* H */
    fe_add(&b, &d, &c); /* F */
    fe_sub(&d, &d, &c); /* G */
    fe_mul(&r->x, &t, &b);
    fe_mul(&r->y, &d, &a);
    fe_mul(&r->t, &t, &a);
    fe_mul(&r->z, &b, &d);
}

/* r = 2p, by RFC 8032's doubling formula (section 5.1.4): four squarings and four products, where
 * the addition takes nine products. p's T is not read. r may be p: nothing is written to r until
 * p has been read. */
static void ge_dbl(ge *r, const ge *p) {
    fe a, b, c, h, t;
    fe_sq(&a, &p->x); /* A */
    fe_sq(&b, &p->y); /* B */
    fe_sq(&c, &p->z);
    fe_add(&c, &c, &c); /* C */
    fe_add(&h, &a, &b); /* H */
    fe_add(&t, &p->x, &p->y);
    fe_sq(&t, &t);
    fe_sub(&t, &h, &t); /* E */
    fe_sub(&a, &a, &b); /* G */
    fe_add(&b, &c, &a); /* F */
    fe_mul(&r->x, &t, &b);
    fe_mul(&r->y, &a, &h);
    fe_mul(&r->t, &t, &h);
    fe_mul(&r->z, &b, &a);
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

/* --- Public points, in variable time --- */

/* Everything a node checks, an address and a signature, is public, so it is checked by
 * multiplications whose time depends on the scalars: each scalar in width-3 non-adjacent form,
 * digits 0, ±1 and ±3 with at least two zeros after any that is not, so that about a quarter of
 * them cost an addition, from a table of the point and three times it. Signing and the keys keep
 * the ladder above. */

/* k, below 2^253, as naf[0] + 2 naf[1] + 4 naf[2] + ...: returns how many digits up to the last
 * that is not zero. Each step takes the lowest digit, odd k's residue mod 8 brought to -3..3,
 * subtracts it, which leaves k a multiple of 8, and halves k. */
static int wnaf(int8_t naf[256], const uint8_t k[32]) {
    uint32_t x[9] = {0};
    int n = 0;
    for (int i = 0; i < 32; i++) {
        x[i / 4] |= (uint32_t)k[i] << (8 * (i % 4));
    }
    for (int i = 0; i < 256; i++) {
        int d = 0;
        if (x[0] & 1) {
            d = (int)(x[0] & 7);
            if (d > 4) {
                d -= 8;
            }
            if (d > 0) {
                x[0] -= (uint32_t)d; /* its low bits are d: nothing borrowed */
            } else {
                uint32_t carry = (uint32_t)-d;
                for (int j = 0; j < 9 && carry != 0; j++) {
                    x[j] += carry;
                    carry = x[j] < carry;
                }
            }
            n = i + 1;
        }
        naf[i] = (int8_t)d;
        for (int j = 0; j < 8; j++) {
            x[j] = x[j] >> 1 | x[j + 1] << 31;
        }
        x[8] >>= 1;
    }
    return n;
}

/* B's x, little-endian (RFC 8032, section 5.1), so that B is had without the square root
 * ge_frombytes() takes to find it. */
static const uint8_t base_x[32] = {
    0x1a, 0xd5, 0x25, 0x8f, 0x60, 0x2d, 0x56, 0xc9, 0xb2, 0xa7, 0x25, 0x95, 0x60, 0xc7, 0x2c, 0x69,
    0x5c, 0xdc, 0xd6, 0xfd, 0x31, 0xe2, 0xa4, 0xc0, 0xfe, 0x53, 0x6e, 0xcd, 0xd3, 0x36, 0x69, 0x21,
};

static void ge_base(ge *b) {
    fe_frombytes(&b->x, base_x);
    fe_frombytes(&b->y, base_bytes);
    fe_1(&b->z);
    fe_mul(&b->t, &b->x, &b->y);
}

/* A point's table: p and 3p. */
static void ge_table(ge t[2], const ge *p) {
    t[0] = *p;
    ge_dbl(&t[1], p);
    ge_add(&t[1], &t[1], p);
}

/* r += [d]p, for a digit d, from p's table. */
static void ge_digit(ge *r, int d, const ge t[2]) {
    if (d > 0) {
        ge_add(r, r, &t[d / 2]);
    } else if (d < 0) {
        ge_sub(r, r, &t[-d / 2]);
    }
}

/* r = [a]P + [b]B, P given by its table and a and b below 2^253, or only [a]P if b is NULL: both
 * scalars' digits at once (Straus), one doubling each, about 253 doublings and 130 additions with
 * b, where the ladder takes 512 additions for each. Vartime. */
static void ge_mul_vartime(ge *r, const uint8_t a[32], const ge p[2], const uint8_t *b) {
    int8_t na[256], nb[256] = {0};
    ge base[2];
    int n = wnaf(na, a);
    if (b != NULL) {
        int nb_len = wnaf(nb, b);
        n = nb_len > n ? nb_len : n;
        ge_base(&base[0]);
        ge_table(base, &base[0]);
    }
    ge_neutral(r);
    for (int i = n - 1; i >= 0; i--) {
        ge_dbl(r, r);
        ge_digit(r, na[i], p);
        if (b != NULL) {
            ge_digit(r, nb[i], base);
        }
    }
}

/* The table of an address's point, negated if `negate`: false if the address does not decode, or,
 * if `check`, if its point is the neutral element or is not of order L. */
static bool address_table(ge t[2], const uint8_t address[TERN_ADDRESS_LEN], bool negate,
                          bool check) {
    ge a;
    if (!ge_frombytes(&a, address)) {
        return false;
    }
    if (negate) {
        fe zero;
        fe_0(&zero);
        fe_sub(&a.x, &zero, &a.x);
        fe_sub(&a.t, &zero, &a.t);
    }
    ge_table(t, &a);
    if (check) {
        /* [L]A is the neutral element exactly when A is in the prime-order subgroup, and so is
         * [L](-A). */
        if (ge_is_neutral(&a)) {
            return false;
        }
        ge_mul_vartime(&a, order, t, NULL);
        if (!ge_is_neutral(&a)) {
            return false;
        }
    }
    return true;
}

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
    ge t[2];
    return address_table(t, address, false, true);
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

/* --- Signatures (RFC 8032, sections 5.1.6 and 5.1.7) --- */

/* Arithmetic shift right of a value that may be negative, without relying on how the compiler
 * shifts one: the bias makes it positive, and is a multiple of 2^n, so it comes off whole. Every
 * value shifted here is far below it. */
static int64_t asr(int64_t x, unsigned n) {
    const int64_t bias = (int64_t)1 << 62;
    return ((x + bias) >> n) - (bias >> n);
}

/* r = x mod L, for x of 64 limbs of a byte's weight each, any of them up to about 2^40 or down
 * to -2^40: each limb from the top down is folded into the ones 32 below it, as 2^256 is
 * -16 (L - 2^252) mod L, and what is left is brought below L. The same work for every x. */
static void sc_reduce(uint8_t r[32], int64_t x[64]) {
    int64_t carry;
    for (int i = 63; i >= 32; i--) {
        int j;
        carry = 0;
        for (j = i - 32; j < i - 12; j++) {
            x[j] += carry - 16 * x[i] * order[j - (i - 32)];
            carry = asr(x[j] + 128, 8);
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (int j = 0; j < 32; j++) {
        x[j] += carry - asr(x[31], 4) * order[j];
        carry = asr(x[j], 8);
        x[j] -= carry * 256;
    }
    for (int j = 0; j < 32; j++) {
        x[j] -= carry * order[j];
    }
    for (int i = 0; i < 32; i++) {
        x[i + 1] += asr(x[i], 8);
        r[i] = (uint8_t)(x[i] & 255);
    }
}

/* A SHA-512 digest, read as a little-endian number, mod L. */
static void sc_from_digest(uint8_t r[32], const uint8_t h[TERN_SHA512_LEN]) {
    int64_t x[64];
    for (int i = 0; i < 64; i++) {
        x[i] = h[i];
    }
    sc_reduce(r, x);
    tern_wipe(x, sizeof x);
}

/* s = (a + b c) mod L. */
static void sc_muladd(uint8_t s[32], const uint8_t a[32], const uint8_t b[32],
                      const uint8_t c[32]) {
    int64_t x[64] = {0};
    for (int i = 0; i < 32; i++) {
        x[i] = a[i];
    }
    for (int i = 0; i < 32; i++) {
        for (int j = 0; j < 32; j++) {
            x[i + j] += (int64_t)b[i] * c[j];
        }
    }
    sc_reduce(s, x);
    tern_wipe(x, sizeof x);
}

/* Whether a little-endian s is below L. Vartime: s is public. */
static bool sc_canonical(const uint8_t s[32]) {
    for (int i = 31; i >= 0; i--) {
        if (s[i] != order[i]) {
            return s[i] < order[i];
        }
    }
    return false;
}

/* k = SHA-512(R || A || M) mod L. */
static void challenge(uint8_t k[32], const uint8_t r[32], const uint8_t a[TERN_ADDRESS_LEN],
                      const uint8_t *m, size_t len) {
    struct tern_sha512 c;
    uint8_t h[TERN_SHA512_LEN];
    tern_sha512_init(&c);
    tern_sha512_update(&c, r, 32);
    tern_sha512_update(&c, a, TERN_ADDRESS_LEN);
    tern_sha512_update(&c, m, len);
    tern_sha512_final(&c, h);
    sc_from_digest(k, h);
}

void tern_identity_sign(const struct tern_identity *id, const uint8_t *m, size_t len,
                        uint8_t sig[TERN_SIGNATURE_LEN]) {
    uint8_t h[TERN_SHA512_LEN], r[TERN_SHA512_LEN], rs[32], k[32];
    struct tern_sha512 c;
    ge p;

    secret_scalar(id->seed, h);
    h[0] &= 248;
    h[31] &= 127;
    h[31] |= 64;

    /* r = SHA-512(prefix || M) mod L, the prefix being the digest's second half. */
    tern_sha512_init(&c);
    tern_sha512_update(&c, h + 32, 32);
    tern_sha512_update(&c, m, len);
    tern_sha512_final(&c, r);
    sc_from_digest(rs, r);

    (void)ge_frombytes(&p, base_bytes);
    ge_scalarmult(&p, rs, &p);
    ge_tobytes(sig, &p);

    challenge(k, sig, id->address, m, len);
    sc_muladd(sig + 32, rs, k, h); /* S = r + k s */

    tern_wipe(h, sizeof h);
    tern_wipe(r, sizeof r);
    tern_wipe(rs, sizeof rs);
    tern_wipe(&p, sizeof p);
}

/* [S]B - [k]A, which is R for a signature made with A's key: one multiplication of both, from the
 * table of -A. Whether the address was refused (only if `check`), or the signature, or neither. */
enum checked { ADDRESS_REFUSED, SIGNATURE_REFUSED, SIGNATURE_GOOD };

static enum checked verify(const uint8_t address[TERN_ADDRESS_LEN], const uint8_t *m, size_t len,
                           const uint8_t sig[TERN_SIGNATURE_LEN], bool check) {
    uint8_t k[32], check_r[32];
    ge t[2], r;

    if (!address_table(t, address, true, check)) {
        return ADDRESS_REFUSED;
    }
    if (!sc_canonical(sig + 32)) {
        return SIGNATURE_REFUSED;
    }
    challenge(k, sig, address, m, len);
    ge_mul_vartime(&r, k, t, sig + 32);
    ge_tobytes(check_r, &r);

    uint8_t differ = 0;
    for (int i = 0; i < 32; i++) {
        differ |= (uint8_t)(check_r[i] ^ sig[i]);
    }
    return differ == 0 ? SIGNATURE_GOOD : SIGNATURE_REFUSED;
}

bool tern_address_verify(const uint8_t address[TERN_ADDRESS_LEN], const uint8_t *m, size_t len,
                         const uint8_t sig[TERN_SIGNATURE_LEN]) {
    return verify(address, m, len, sig, true) == SIGNATURE_GOOD;
}

bool tern_address_verify_valid(const uint8_t address[TERN_ADDRESS_LEN], const uint8_t *m,
                               size_t len, const uint8_t sig[TERN_SIGNATURE_LEN]) {
    return verify(address, m, len, sig, false) == SIGNATURE_GOOD;
}

bool tern_address_verify_cached(struct tern_address_cache *c,
                                const uint8_t address[TERN_ADDRESS_LEN], const uint8_t *m,
                                size_t len, const uint8_t sig[TERN_SIGNATURE_LEN]) {
    for (size_t i = 0; i < c->used && i < TERN_ADDRESS_CACHED; i++) {
        bool held = true;
        for (size_t j = 0; j < TERN_ADDRESS_LEN && held; j++) {
            held = c->address[i][j] == address[j]; /* vartime: addresses are public */
        }
        if (held) {
            return verify(address, m, len, sig, false) == SIGNATURE_GOOD;
        }
    }
    enum checked v = verify(address, m, len, sig, true);
    if (v == ADDRESS_REFUSED) {
        return false;
    }
    /* Valid, whatever became of the signature: kept. */
    size_t at;
    if (c->used < TERN_ADDRESS_CACHED) {
        at = c->used++;
    } else {
        at = c->next % TERN_ADDRESS_CACHED;
        c->next = (uint8_t)((at + 1) % TERN_ADDRESS_CACHED);
    }
    for (size_t j = 0; j < TERN_ADDRESS_LEN; j++) {
        c->address[at][j] = address[j];
    }
    return v == SIGNATURE_GOOD;
}
