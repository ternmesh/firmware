#include "tern/crypto.h"

/* AES-CCM as RFC 3610 defines it, with the tag length M = 8 and the length field L = 2 fixed.
 *
 * CCM is two passes with one key. The tag is a CBC-MAC over a first block B0 (flags, nonce and
 * message length), the associated data and the message. The message and the tag are then
 * encrypted in counter mode: block A_i is flags, nonce and the counter i, and A_0's keystream
 * encrypts the tag while A_1, A_2, ... encrypt the message. */

#define M TERN_CCM_TAG
#define L 2

/* Runs the CBC-MAC over B0, the associated data and the message, leaving the full 16-byte MAC in
 * x. */
static void cbc_mac(const struct tern_aes128 *aes, const uint8_t nonce[TERN_CCM_NONCE],
                    const uint8_t *aad, size_t aad_len, const uint8_t *msg, size_t len,
                    uint8_t x[TERN_AES_BLOCK]) {
    /* B0: flags, nonce, message length. Flags (RFC 3610, section 2.2) are Adata in bit 6,
     * (M - 2) / 2 in bits 5-3 and L - 1 in bits 2-0. */
    x[0] = (uint8_t)((aad_len > 0 ? 0x40 : 0) | ((M - 2) / 2) << 3 | (L - 1));
    for (int i = 0; i < TERN_CCM_NONCE; i++) {
        x[1 + i] = nonce[i];
    }
    x[14] = (uint8_t)(len >> 8);
    x[15] = (uint8_t)len;
    tern_aes128_encrypt(aes, x, x);

    /* The associated data, after its length in two bytes, padded with zeros to whole blocks. */
    if (aad_len > 0) {
        size_t pos = 2;
        x[0] ^= (uint8_t)(aad_len >> 8);
        x[1] ^= (uint8_t)aad_len;
        for (size_t i = 0; i < aad_len; i++) {
            x[pos++] ^= aad[i];
            if (pos == TERN_AES_BLOCK) {
                tern_aes128_encrypt(aes, x, x);
                pos = 0;
            }
        }
        if (pos > 0) {
            tern_aes128_encrypt(aes, x, x);
        }
    }

    /* The message, padded the same way. */
    for (size_t i = 0; i < len; i += TERN_AES_BLOCK) {
        for (size_t j = 0; j < TERN_AES_BLOCK && i + j < len; j++) {
            x[j] ^= msg[i + j];
        }
        tern_aes128_encrypt(aes, x, x);
    }
}

/* The counter-mode keystream block for counter i. */
static void keystream(const struct tern_aes128 *aes, const uint8_t nonce[TERN_CCM_NONCE],
                      uint16_t i, uint8_t s[TERN_AES_BLOCK]) {
    s[0] = L - 1;
    for (int j = 0; j < TERN_CCM_NONCE; j++) {
        s[1 + j] = nonce[j];
    }
    s[14] = (uint8_t)(i >> 8);
    s[15] = (uint8_t)i;
    tern_aes128_encrypt(aes, s, s);
}

/* XORs the keystream into len bytes of in, writing out. */
static void ctr(const struct tern_aes128 *aes, const uint8_t nonce[TERN_CCM_NONCE],
                const uint8_t *in, size_t len, uint8_t *out) {
    uint8_t s[TERN_AES_BLOCK];
    for (size_t i = 0; i < len; i += TERN_AES_BLOCK) {
        keystream(aes, nonce, (uint16_t)(1 + i / TERN_AES_BLOCK), s);
        for (size_t j = 0; j < TERN_AES_BLOCK && i + j < len; j++) {
            out[i + j] = in[i + j] ^ s[j];
        }
    }
    tern_wipe(s, sizeof s);
}

bool tern_ccm_seal(const uint8_t key[TERN_AES128_KEY], const uint8_t nonce[TERN_CCM_NONCE],
                   const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t len, uint8_t *out,
                   uint8_t tag[TERN_CCM_TAG]) {
    if (aad_len > TERN_CCM_MAX_AAD || len > TERN_CCM_MAX_MSG) {
        return false;
    }

    struct tern_aes128 aes;
    uint8_t mac[TERN_AES_BLOCK], s0[TERN_AES_BLOCK];
    tern_aes128_init(&aes, key);

    /* The MAC is over the plaintext, so it has to be taken before out overwrites in. */
    cbc_mac(&aes, nonce, aad, aad_len, in, len, mac);
    ctr(&aes, nonce, in, len, out);
    keystream(&aes, nonce, 0, s0);
    for (int i = 0; i < M; i++) {
        tag[i] = mac[i] ^ s0[i];
    }

    tern_wipe(&aes, sizeof aes);
    tern_wipe(mac, sizeof mac);
    tern_wipe(s0, sizeof s0);
    return true;
}

bool tern_ccm_open(const uint8_t key[TERN_AES128_KEY], const uint8_t nonce[TERN_CCM_NONCE],
                   const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t len,
                   const uint8_t tag[TERN_CCM_TAG], uint8_t *out) {
    if (aad_len > TERN_CCM_MAX_AAD || len > TERN_CCM_MAX_MSG) {
        tern_wipe(out, len > TERN_CCM_MAX_MSG ? 0 : len);
        return false;
    }

    struct tern_aes128 aes;
    uint8_t mac[TERN_AES_BLOCK], s0[TERN_AES_BLOCK], want[M];
    tern_aes128_init(&aes, key);

    ctr(&aes, nonce, in, len, out);
    cbc_mac(&aes, nonce, aad, aad_len, out, len, mac);
    keystream(&aes, nonce, 0, s0);
    for (int i = 0; i < M; i++) {
        want[i] = mac[i] ^ s0[i];
    }
    bool ok = tern_equal_ct(want, tag, M);
    if (!ok) {
        tern_wipe(out, len);
    }

    tern_wipe(&aes, sizeof aes);
    tern_wipe(mac, sizeof mac);
    tern_wipe(s0, sizeof s0);
    tern_wipe(want, sizeof want);
    return ok;
}
