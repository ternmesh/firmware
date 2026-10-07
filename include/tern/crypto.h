#ifndef TERN_CRYPTO_H
#define TERN_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The cryptographic primitives the specification names, and nothing else: SHA-256 and HMAC for
 * HKDF (RFC 5869), AES-128 for AES-CCM (RFC 3610), and for first contact, SHA-512 and X25519
 * (RFC 7748). Each is written from its standard
 * and tested against that standard's published vectors (tests/crypto.c) before anything is built
 * on it.
 *
 * These are portable software implementations, written to run in constant time: no branch and no
 * memory index depends on a key or on a secret. AES in particular never indexes a table by a
 * secret byte (the S-box is read by scanning all of it), which makes it slow. Boards whose chips
 * have AES in hardware, as the nRF52840 does, are expected to use it; this is the reference every
 * other implementation is checked against. */

/* --- SHA-256 (FIPS 180-4) --------------------------------------------------------------------- */

#define TERN_SHA256_LEN 32
#define TERN_SHA256_BLOCK 64

struct tern_sha256 {
    uint32_t h[8];
    uint64_t len; /* bytes hashed so far */
    uint8_t buf[TERN_SHA256_BLOCK];
};

void tern_sha256_init(struct tern_sha256 *c);
void tern_sha256_update(struct tern_sha256 *c, const uint8_t *data, size_t len);
/* Writes the digest and erases the context. */
void tern_sha256_final(struct tern_sha256 *c, uint8_t out[TERN_SHA256_LEN]);

/* --- SHA-512 (FIPS 180-4) --------------------------------------------------------------------- */

/* Only for turning an identity seed into keys, as Ed25519 does (RFC 8032, section 5.1.5). */

#define TERN_SHA512_LEN 64
#define TERN_SHA512_BLOCK 128

struct tern_sha512 {
    uint64_t h[8];
    uint64_t len; /* bytes hashed so far */
    uint8_t buf[TERN_SHA512_BLOCK];
};

void tern_sha512_init(struct tern_sha512 *c);
void tern_sha512_update(struct tern_sha512 *c, const uint8_t *data, size_t len);
/* Writes the digest and erases the context. */
void tern_sha512_final(struct tern_sha512 *c, uint8_t out[TERN_SHA512_LEN]);

/* --- HMAC-SHA-256 (RFC 2104) and HKDF-Expand (RFC 5869) --------------------------------------- */

struct tern_hmac_sha256 {
    struct tern_sha256 inner;
    struct tern_sha256 outer; /* already fed the outer padded key */
};

void tern_hmac_sha256_init(struct tern_hmac_sha256 *c, const uint8_t *key, size_t key_len);
void tern_hmac_sha256_update(struct tern_hmac_sha256 *c, const uint8_t *data, size_t len);
/* Writes the MAC and erases the context. */
void tern_hmac_sha256_final(struct tern_hmac_sha256 *c, uint8_t out[TERN_SHA256_LEN]);

/* HKDF-Expand with SHA-256: out_len bytes (at most 255 * 32) from the pseudorandom key prk and
 * info. Returns false, writing nothing, if out_len is too long. */
bool tern_hkdf_expand(uint8_t *out, size_t out_len, const uint8_t *prk, size_t prk_len,
                      const uint8_t *info, size_t info_len);

/* --- AES-128 (FIPS 197), encryption only ------------------------------------------------------ */

/* CCM, and Tern's destination tags, only ever run AES forwards, so there is no decryption. */

#define TERN_AES_BLOCK 16
#define TERN_AES128_KEY 16

struct tern_aes128 {
    uint8_t rk[176]; /* the expanded key: 11 round keys */
};

void tern_aes128_init(struct tern_aes128 *a, const uint8_t key[TERN_AES128_KEY]);
/* out may be in. */
void tern_aes128_encrypt(const struct tern_aes128 *a, const uint8_t in[TERN_AES_BLOCK],
                         uint8_t out[TERN_AES_BLOCK]);

/* --- AES-128-CCM (RFC 3610) with M = 8 and L = 2 ---------------------------------------------- */

/* The one CCM the specification uses: an 8-byte tag and a 13-byte nonce, so messages of up to
 * 65535 bytes. This is COSE's AES-CCM-16-64-128. Associated data must be shorter than 65280
 * bytes, which keeps its length to the two-byte encoding. */

#define TERN_CCM_NONCE 13
#define TERN_CCM_TAG 8
#define TERN_CCM_MAX_AAD 65279u
#define TERN_CCM_MAX_MSG 65535u

/* Encrypts len bytes of in to out (which may be in) and writes the tag to tag. Returns false,
 * writing nothing, if aad_len or len is too long. */
bool tern_ccm_seal(const uint8_t key[TERN_AES128_KEY], const uint8_t nonce[TERN_CCM_NONCE],
                   const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t len, uint8_t *out,
                   uint8_t tag[TERN_CCM_TAG]);

/* Checks tag and decrypts len bytes of in to out (which may be in). Returns true only if the tag
 * is right; otherwise out is left all zeros, so nothing unauthenticated escapes. */
bool tern_ccm_open(const uint8_t key[TERN_AES128_KEY], const uint8_t nonce[TERN_CCM_NONCE],
                   const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t len,
                   const uint8_t tag[TERN_CCM_TAG], uint8_t *out);

/* --- X25519 (RFC 7748) ------------------------------------------------------------------------ */

#define TERN_X25519_LEN 32

/* out = X25519(scalar, u), as RFC 7748, section 5 defines it: the scalar is clamped and the top
 * bit of u ignored. Returns false if the result is all zeros, which it is exactly when u is a
 * point of small order; RFC 7748, section 6.1 and every protocol here require refusing it. out is
 * written either way. Constant time in the scalar. */
bool tern_x25519(uint8_t out[TERN_X25519_LEN], const uint8_t scalar[TERN_X25519_LEN],
                 const uint8_t u[TERN_X25519_LEN]);

/* out = X25519(scalar, 9): the public key of a private key. */
void tern_x25519_base(uint8_t out[TERN_X25519_LEN], const uint8_t scalar[TERN_X25519_LEN]);

/* --- Helpers ---------------------------------------------------------------------------------- */

/* Overwrites len bytes with zeros in a way the compiler may not remove, for erasing keys. */
void tern_wipe(void *p, size_t len);

/* Whether a and b hold the same len bytes, taking the same time whatever they hold. */
bool tern_equal_ct(const uint8_t *a, const uint8_t *b, size_t len);

#endif
