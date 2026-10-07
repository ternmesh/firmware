#include "tern/crypto.h"

/* SHA-512 as FIPS 180-4 defines it, section 6.4. The constants are the first 64 bits of the
 * fractional parts of the cube roots of the first 80 primes (section 4.2.3) and of the square
 * roots of the first 8 (section 5.3.5). */

static const uint64_t k[80] = {
    0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc,
    0x3956c25bf348b538, 0x59f111f1b605d019, 0x923f82a4af194f9b, 0xab1c5ed5da6d8118,
    0xd807aa98a3030242, 0x12835b0145706fbe, 0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2,
    0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235, 0xc19bf174cf692694,
    0xe49b69c19ef14ad2, 0xefbe4786384f25e3, 0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65,
    0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5,
    0x983e5152ee66dfab, 0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4,
    0xc6e00bf33da88fc2, 0xd5a79147930aa725, 0x06ca6351e003826f, 0x142929670a0e6e70,
    0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed, 0x53380d139d95b3df,
    0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b,
    0xa2bfe8a14cf10364, 0xa81a664bbc423001, 0xc24b8b70d0f89791, 0xc76c51a30654be30,
    0xd192e819d6ef5218, 0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8,
    0x19a4c116b8d2d0c8, 0x1e376c085141ab53, 0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8,
    0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb, 0x5b9cca4f7763e373, 0x682e6ff3d6b2b8a3,
    0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
    0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b,
    0xca273eceea26619c, 0xd186b8c721c0c207, 0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178,
    0x06f067aa72176fba, 0x0a637dc5a2c898a6, 0x113f9804bef90dae, 0x1b710b35131c471b,
    0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc, 0x431d67c49c100d4c,
    0x4cc5d4becb3e42b6, 0x597f299cfc657e2a, 0x5fcb6fab3ad6faec, 0x6c44198c4a475817,
};

static uint64_t rotr(uint64_t x, unsigned n) { return (x >> n) | (x << (64 - n)); }

static void compress(uint64_t h[8], const uint8_t block[TERN_SHA512_BLOCK]) {
    uint64_t w[80];
    for (int t = 0; t < 16; t++) {
        w[t] = 0;
        for (int i = 0; i < 8; i++) {
            w[t] = w[t] << 8 | block[8 * t + i];
        }
    }
    for (int t = 16; t < 80; t++) {
        uint64_t s0 = rotr(w[t - 15], 1) ^ rotr(w[t - 15], 8) ^ (w[t - 15] >> 7);
        uint64_t s1 = rotr(w[t - 2], 19) ^ rotr(w[t - 2], 61) ^ (w[t - 2] >> 6);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }

    uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int t = 0; t < 80; t++) {
        uint64_t t1 =
            hh + (rotr(e, 14) ^ rotr(e, 18) ^ rotr(e, 41)) + ((e & f) ^ (~e & g)) + k[t] + w[t];
        uint64_t t2 = (rotr(a, 28) ^ rotr(a, 34) ^ rotr(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
    tern_wipe(w, sizeof w);
}

void tern_sha512_init(struct tern_sha512 *c) {
    static const uint64_t iv[8] = {0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b,
                                   0xa54ff53a5f1d36f1, 0x510e527fade682d1, 0x9b05688c2b3e6c1f,
                                   0x1f83d9abfb41bd6b, 0x5be0cd19137e2179};
    for (int i = 0; i < 8; i++) {
        c->h[i] = iv[i];
    }
    c->len = 0;
}

void tern_sha512_update(struct tern_sha512 *c, const uint8_t *data, size_t len) {
    size_t used = (size_t)(c->len % TERN_SHA512_BLOCK);
    c->len += len;
    for (size_t i = 0; i < len; i++) {
        c->buf[used++] = data[i];
        if (used == TERN_SHA512_BLOCK) {
            compress(c->h, c->buf);
            used = 0;
        }
    }
}

void tern_sha512_final(struct tern_sha512 *c, uint8_t out[TERN_SHA512_LEN]) {
    uint64_t bits = c->len * 8;
    size_t used = (size_t)(c->len % TERN_SHA512_BLOCK);

    /* A one bit, zeros up to 112 bytes into a block, then the length in bits as a 128-bit
     * big-endian number. Nothing here hashes 2^61 bytes, so its top 64 bits are zero. */
    c->buf[used++] = 0x80;
    if (used > TERN_SHA512_BLOCK - 16) {
        while (used < TERN_SHA512_BLOCK) {
            c->buf[used++] = 0;
        }
        compress(c->h, c->buf);
        used = 0;
    }
    while (used < TERN_SHA512_BLOCK - 8) {
        c->buf[used++] = 0;
    }
    for (int i = 0; i < 8; i++) {
        c->buf[TERN_SHA512_BLOCK - 1 - i] = (uint8_t)(bits >> (8 * i));
    }
    compress(c->h, c->buf);

    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            out[8 * i + j] = (uint8_t)(c->h[i] >> (56 - 8 * j));
        }
    }
    tern_wipe(c, sizeof *c);
}
