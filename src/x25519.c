#include "fe25519.h"
#include "tern/crypto.h"

/* X25519 as RFC 7748, section 5 defines it: the Montgomery ladder over the u-coordinate, with a
 * conditional swap in place of every branch on a bit of the scalar. */

bool tern_x25519(uint8_t out[TERN_X25519_LEN], const uint8_t scalar[TERN_X25519_LEN],
                 const uint8_t u[TERN_X25519_LEN]) {
    uint8_t k[32];
    for (int i = 0; i < 32; i++) {
        k[i] = scalar[i];
    }
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;

    fe x1, x2, z2, x3, z3, a, aa, b, bb, e, c, d, da, cb, t;
    fe_frombytes(&x1, u);
    fe_1(&x2);
    fe_0(&z2);
    x3 = x1;
    fe_1(&z3);

    uint64_t swap = 0;
    for (int i = 254; i >= 0; i--) {
        uint64_t bit = (k[i >> 3] >> (i & 7)) & 1;
        swap ^= bit;
        fe_cswap(&x2, &x3, swap);
        fe_cswap(&z2, &z3, swap);
        swap = bit;

        fe_add(&a, &x2, &z2);
        fe_sq(&aa, &a);
        fe_sub(&b, &x2, &z2);
        fe_sq(&bb, &b);
        fe_sub(&e, &aa, &bb);
        fe_add(&c, &x3, &z3);
        fe_sub(&d, &x3, &z3);
        fe_mul(&da, &d, &a);
        fe_mul(&cb, &c, &b);
        fe_add(&t, &da, &cb);
        fe_sq(&x3, &t);
        fe_sub(&t, &da, &cb);
        fe_sq(&t, &t);
        fe_mul(&z3, &x1, &t);
        fe_mul(&x2, &aa, &bb);
        fe_mul_small(&t, &e, 121665); /* a24 = (486662 - 2) / 4 */
        fe_add(&t, &aa, &t);
        fe_mul(&z2, &e, &t);
    }
    fe_cswap(&x2, &x3, swap);
    fe_cswap(&z2, &z3, swap);

    fe_inv(&z2, &z2);
    fe_mul(&x2, &x2, &z2);
    fe_tobytes(out, &x2);

    /* All zeros, checked by ORing every byte, as RFC 7748, section 6.1 suggests. */
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) {
        acc |= out[i];
    }

    tern_wipe(k, sizeof k);
    fe *temps[] = {&x1, &x2, &z2, &x3, &z3, &a, &aa, &b, &bb, &e, &c, &d, &da, &cb, &t};
    for (size_t i = 0; i < sizeof temps / sizeof temps[0]; i++) {
        tern_wipe(temps[i], sizeof *temps[i]);
    }
    return acc != 0;
}

void tern_x25519_base(uint8_t out[TERN_X25519_LEN], const uint8_t scalar[TERN_X25519_LEN]) {
    static const uint8_t nine[32] = {9};
    (void)tern_x25519(out, scalar, nine);
}
