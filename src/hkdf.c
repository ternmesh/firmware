#include "tern/crypto.h"

/* HMAC (RFC 2104) with SHA-256, and HKDF-Expand (RFC 5869, section 2.3). */

void tern_hmac_sha256_init(struct tern_hmac_sha256 *c, const uint8_t *key, size_t key_len) {
    uint8_t k[TERN_SHA256_BLOCK] = {0};
    uint8_t pad[TERN_SHA256_BLOCK];

    if (key_len > TERN_SHA256_BLOCK) {
        struct tern_sha256 h;
        tern_sha256_init(&h);
        tern_sha256_update(&h, key, key_len);
        tern_sha256_final(&h, k);
    } else {
        for (size_t i = 0; i < key_len; i++) {
            k[i] = key[i];
        }
    }

    for (size_t i = 0; i < TERN_SHA256_BLOCK; i++) {
        pad[i] = k[i] ^ 0x36;
    }
    tern_sha256_init(&c->inner);
    tern_sha256_update(&c->inner, pad, sizeof pad);

    for (size_t i = 0; i < TERN_SHA256_BLOCK; i++) {
        pad[i] = k[i] ^ 0x5c;
    }
    tern_sha256_init(&c->outer);
    tern_sha256_update(&c->outer, pad, sizeof pad);

    tern_wipe(k, sizeof k);
    tern_wipe(pad, sizeof pad);
}

void tern_hmac_sha256_update(struct tern_hmac_sha256 *c, const uint8_t *data, size_t len) {
    tern_sha256_update(&c->inner, data, len);
}

void tern_hmac_sha256_final(struct tern_hmac_sha256 *c, uint8_t out[TERN_SHA256_LEN]) {
    uint8_t inner[TERN_SHA256_LEN];
    tern_sha256_final(&c->inner, inner);
    tern_sha256_update(&c->outer, inner, sizeof inner);
    tern_sha256_final(&c->outer, out);
    tern_wipe(inner, sizeof inner);
    tern_wipe(c, sizeof *c);
}

bool tern_hkdf_expand(uint8_t *out, size_t out_len, const uint8_t *prk, size_t prk_len,
                      const uint8_t *info, size_t info_len) {
    if (out_len > 255 * TERN_SHA256_LEN) {
        return false;
    }

    /* T(i) = HMAC(prk, T(i-1) || info || i), with T(0) empty; the output is T(1) || T(2) || ... */
    uint8_t t[TERN_SHA256_LEN];
    size_t done = 0;
    for (uint8_t i = 1; done < out_len; i++) {
        struct tern_hmac_sha256 h;
        tern_hmac_sha256_init(&h, prk, prk_len);
        if (i > 1) {
            tern_hmac_sha256_update(&h, t, sizeof t);
        }
        tern_hmac_sha256_update(&h, info, info_len);
        tern_hmac_sha256_update(&h, &i, 1);
        tern_hmac_sha256_final(&h, t);

        for (size_t j = 0; j < TERN_SHA256_LEN && done < out_len; j++) {
            out[done++] = t[j];
        }
    }
    tern_wipe(t, sizeof t);
    return true;
}

void tern_wipe(void *p, size_t len) {
    volatile uint8_t *v = p;
    for (size_t i = 0; i < len; i++) {
        v[i] = 0;
    }
}

bool tern_equal_ct(const uint8_t *a, const uint8_t *b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}
