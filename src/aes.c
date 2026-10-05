#include "tern/crypto.h"

/* AES-128 encryption as FIPS 197 defines it, written so that no branch and no memory address
 * depends on the key or the data.
 *
 * The usual software AES looks the S-box up by the byte it is substituting. Which part of the
 * table that touches shows up in cache and bus timing, and has been used to recover keys. Here
 * every substitution reads the whole table, 64 words of 4 entries, and keeps the word it wants
 * with a mask, then shifts the right byte out of it. It is about 64 times the work of a lookup,
 * which a LoRa node, sending a frame every few seconds at most, can afford. */

/* The S-box (FIPS 197, figure 7), four entries to a word, lowest entry in the lowest byte. */
static const uint32_t sbox[64] = {
    0x7b777c63, 0xc56f6bf2, 0x2b670130, 0x76abd7fe, 0x7dc982ca, 0xf04759fa, 0xafa2d4ad, 0xc072a49c,
    0x2693fdb7, 0xccf73f36, 0xf1e5a534, 0x1531d871, 0xc323c704, 0x9a059618, 0xe2801207, 0x75b227eb,
    0x1a2c8309, 0xa05a6e1b, 0xb3d63b52, 0x842fe329, 0xed00d153, 0x5bb1fc20, 0x39becb6a, 0xcf584c4a,
    0xfbaaefd0, 0x85334d43, 0x7f02f945, 0xa89f3c50, 0x8f40a351, 0xf5389d92, 0x21dab6bc, 0xd2f3ff10,
    0xec130ccd, 0x1744975f, 0x3d7ea7c4, 0x73195d64, 0xdc4f8160, 0x88902a22, 0x14b8ee46, 0xdb0b5ede,
    0x0a3a32e0, 0x5c240649, 0x62acd3c2, 0x79e49591, 0x6d37c8e7, 0xa94ed58d, 0xeaf4566c, 0x08ae7a65,
    0x2e2578ba, 0xc6b4a61c, 0x1f74dde8, 0x8a8bbd4b, 0x66b53e70, 0x0ef60348, 0xb9573561, 0x9e1dc186,
    0x1198f8e1, 0x948ed969, 0xe9871e9b, 0xdf2855ce, 0x0d89a18c, 0x6842e6bf, 0x0f2d9941, 0x16bb54b0,
};

static uint8_t sub(uint8_t x) {
    uint32_t want = (uint32_t)x >> 2;
    uint32_t word = 0;
    for (uint32_t j = 0; j < 64; j++) {
        /* All ones when j == want, else zero, without comparing: j ^ want is 0..63, and only 0
         * wraps to a top bit set when one is taken away. */
        uint32_t mask = 0u - (((j ^ want) - 1u) >> 31);
        word |= sbox[j] & mask;
    }
    return (uint8_t)(word >> (8 * (x & 3u)));
}

/* Multiplication by x in GF(2^8), without branching on the top bit. */
static uint8_t xtime(uint8_t b) { return (uint8_t)((b << 1) ^ (0x1bu & (0u - (b >> 7)))); }

void tern_aes128_init(struct tern_aes128 *a, const uint8_t key[TERN_AES128_KEY]) {
    uint8_t *w = a->rk;
    for (int i = 0; i < 16; i++) {
        w[i] = key[i];
    }

    /* FIPS 197, section 5.2: each four-byte word is the one four back, XORed with the previous
     * word, which at the start of each round key is first rotated, substituted and given the
     * round constant. */
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t0 = w[i - 4], t1 = w[i - 3], t2 = w[i - 2], t3 = w[i - 1];
        if (i % 16 == 0) {
            uint8_t r = t0;
            t0 = (uint8_t)(sub(t1) ^ rcon);
            t1 = sub(t2);
            t2 = sub(t3);
            t3 = sub(r);
            rcon = xtime(rcon);
        }
        w[i] = w[i - 16] ^ t0;
        w[i + 1] = w[i - 15] ^ t1;
        w[i + 2] = w[i - 14] ^ t2;
        w[i + 3] = w[i - 13] ^ t3;
    }
}

void tern_aes128_encrypt(const struct tern_aes128 *a, const uint8_t in[TERN_AES_BLOCK],
                         uint8_t out[TERN_AES_BLOCK]) {
    /* The state, column by column: s[4c + r] is row r of column c, as in the input. */
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) {
        s[i] = in[i] ^ a->rk[i];
    }

    for (int round = 1; round <= 10; round++) {
        /* SubBytes and ShiftRows together: row r moves r columns to the left. */
        for (int c = 0; c < 4; c++) {
            for (int r = 0; r < 4; r++) {
                t[4 * c + r] = sub(s[4 * ((c + r) % 4) + r]);
            }
        }

        /* MixColumns, except in the last round. */
        if (round < 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t *col = &t[4 * c];
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                uint8_t all = a0 ^ a1 ^ a2 ^ a3;
                col[0] = a0 ^ all ^ xtime(a0 ^ a1);
                col[1] = a1 ^ all ^ xtime(a1 ^ a2);
                col[2] = a2 ^ all ^ xtime(a2 ^ a3);
                col[3] = a3 ^ all ^ xtime(a3 ^ a0);
            }
        }

        const uint8_t *k = &a->rk[16 * round];
        for (int i = 0; i < 16; i++) {
            s[i] = t[i] ^ k[i];
        }
    }

    for (int i = 0; i < 16; i++) {
        out[i] = s[i];
    }
    tern_wipe(s, sizeof s);
    tern_wipe(t, sizeof t);
}
