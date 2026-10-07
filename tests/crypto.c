#include "tern/crypto.h"

#include <string.h>

#include "check.h"

/* Each primitive against the published vectors of the standard that defines it. The unicast
 * vectors (tests/unicast.c) depend on all of these, so a failure here explains a failure there. */

static size_t unhex(const char *hex, uint8_t *out) {
    size_t n = 0;
    for (; hex[0] && hex[1]; hex += 2) {
        unsigned v;
        sscanf(hex, "%2x", &v);
        out[n++] = (uint8_t)v;
    }
    return n;
}

static void check_hex(const uint8_t *got, size_t len, const char *want, const char *what) {
    uint8_t w[512];
    size_t n = unhex(want, w);
    if (n != len || memcmp(got, w, len) != 0) {
        fprintf(stderr, "%s: wrong output\n", what);
        check_failures++;
    }
}

static void sha256_of(const char *msg, size_t len, uint8_t out[32]) {
    struct tern_sha256 c;
    tern_sha256_init(&c);
    tern_sha256_update(&c, (const uint8_t *)msg, len);
    tern_sha256_final(&c, out);
}

/* FIPS 180-4's examples (NIST's "SHA256.pdf"): one block, two blocks, and the empty message. */
static void sha256_matches_fips_180_4(void) {
    uint8_t d[32];
    sha256_of("abc", 3, d);
    check_hex(d, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc");
    sha256_of("", 0, d);
    check_hex(d, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty");
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_of(two, strlen(two), d);
    check_hex(d, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
              "448 bits");
}

/* A million "a"s, fed in pieces of awkward sizes, so that every way update() can meet a block
 * boundary gets used. */
static void sha256_million_a_in_pieces(void) {
    struct tern_sha256 c;
    uint8_t a[997], d[32];
    memset(a, 'a', sizeof a);
    tern_sha256_init(&c);
    size_t left = 1000000;
    for (size_t step = 1; left > 0; step = (step + 13) % sizeof a + 1) {
        size_t n = step < left ? step : left;
        tern_sha256_update(&c, a, n);
        left -= n;
    }
    tern_sha256_final(&c, d);
    check_hex(d, 32, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "million");
}

static void hmac_of(const uint8_t *key, size_t key_len, const char *msg, uint8_t out[32]) {
    struct tern_hmac_sha256 c;
    tern_hmac_sha256_init(&c, key, key_len);
    tern_hmac_sha256_update(&c, (const uint8_t *)msg, strlen(msg));
    tern_hmac_sha256_final(&c, out);
}

/* RFC 4231, test cases 1, 2 and 6 (the last with a key longer than a block). */
static void hmac_matches_rfc_4231(void) {
    uint8_t key[131], d[32];
    memset(key, 0x0b, 20);
    hmac_of(key, 20, "Hi There", d);
    check_hex(d, 32, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", "case 1");
    hmac_of((const uint8_t *)"Jefe", 4, "what do ya want for nothing?", d);
    check_hex(d, 32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", "case 2");
    memset(key, 0xaa, sizeof key);
    hmac_of(key, sizeof key, "Test Using Larger Than Block-Size Key - Hash Key First", d);
    check_hex(d, 32, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", "case 6");
}

/* RFC 5869, test cases 1 and 3: Expand from the PRK each gives. */
static void hkdf_expand_matches_rfc_5869(void) {
    uint8_t prk[32], info[10], okm[42];
    unhex("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", prk);
    unhex("f0f1f2f3f4f5f6f7f8f9", info);
    CHECK(tern_hkdf_expand(okm, sizeof okm, prk, sizeof prk, info, sizeof info));
    check_hex(
        okm, sizeof okm,
        "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865",
        "case 1");

    unhex("19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04", prk);
    CHECK(tern_hkdf_expand(okm, sizeof okm, prk, sizeof prk, NULL, 0));
    check_hex(
        okm, sizeof okm,
        "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8",
        "case 3");

    CHECK(!tern_hkdf_expand(okm, 255 * 32 + 1, prk, sizeof prk, NULL, 0));
}

/* FIPS 197, appendix B (the worked example) and appendix C.1. */
static void aes128_matches_fips_197(void) {
    struct tern_aes128 a;
    uint8_t key[16], block[16];

    unhex("2b7e151628aed2a6abf7158809cf4f3c", key);
    unhex("3243f6a8885a308d313198a2e0370734", block);
    tern_aes128_init(&a, key);
    tern_aes128_encrypt(&a, block, block);
    check_hex(block, 16, "3925841d02dc09fbdc118597196a0b32", "appendix B");

    unhex("000102030405060708090a0b0c0d0e0f", key);
    unhex("00112233445566778899aabbccddeeff", block);
    tern_aes128_init(&a, key);
    tern_aes128_encrypt(&a, block, block);
    check_hex(block, 16, "69c4e0d86a7b0430d8cdb78070b4c55a", "appendix C.1");
}

/* RFC 3610, packet vectors 1 to 3: the parameters Tern uses (M = 8, L = 2), with messages of 23,
 * 24 and 25 bytes, so the last block is short, exact and one byte over. */
static void ccm_matches_rfc_3610(void) {
    static const struct {
        const char *nonce, *ciphertext;
        size_t len;
    } packets[] = {
        {"00000003020100a0a1a2a3a4a5",
         "588c979a61c663d2f066d0c2c0f989806d5f6b61dac38417e8d12cfdf926e0", 23},
        {"00000004030201a0a1a2a3a4a5",
         "72c91a36e135f8cf291ca894085c87e3cc15c439c9e43a3ba091d56e10400916", 24},
        {"00000005040302a0a1a2a3a4a5",
         "51b1e5f44a197d1da46b0f8e2d282ae871e838bb64da8596574adaa76fbd9fb0c5", 25},
    };
    uint8_t key[16], aad[8], msg[25];
    unhex("c0c1c2c3c4c5c6c7c8c9cacbcccdcecf", key);
    for (uint8_t i = 0; i < 8; i++) {
        aad[i] = i;
    }
    for (uint8_t i = 0; i < 25; i++) {
        msg[i] = (uint8_t)(8 + i);
    }

    for (size_t p = 0; p < 3; p++) {
        uint8_t nonce[13], out[25 + 8], back[25];
        size_t len = packets[p].len;
        unhex(packets[p].nonce, nonce);
        CHECK(tern_ccm_seal(key, nonce, aad, sizeof aad, msg, len, out, out + len));
        check_hex(out, len + 8, packets[p].ciphertext, packets[p].nonce);

        CHECK(tern_ccm_open(key, nonce, aad, sizeof aad, out, len, out + len, back));
        CHECK(memcmp(back, msg, len) == 0);

        /* Any change, to the ciphertext, the tag or the associated data, fails, and leaves
         * nothing behind. */
        out[0] ^= 1;
        CHECK(!tern_ccm_open(key, nonce, aad, sizeof aad, out, len, out + len, back));
        out[0] ^= 1;
        out[len + 7] ^= 0x80;
        CHECK(!tern_ccm_open(key, nonce, aad, sizeof aad, out, len, out + len, back));
        out[len + 7] ^= 0x80;
        aad[3] ^= 4;
        memset(back, 0x55, sizeof back);
        CHECK(!tern_ccm_open(key, nonce, aad, sizeof aad, out, len, out + len, back));
        aad[3] ^= 4;
        for (size_t i = 0; i < len; i++) {
            CHECK(back[i] == 0);
        }

        /* In place, as the header allows. */
        memcpy(back, msg, len);
        uint8_t tag[8];
        CHECK(tern_ccm_seal(key, nonce, aad, sizeof aad, back, len, back, tag));
        CHECK(memcmp(back, out, len) == 0 && memcmp(tag, out + len, 8) == 0);
        CHECK(tern_ccm_open(key, nonce, aad, sizeof aad, back, len, tag, back));
        CHECK(memcmp(back, msg, len) == 0);
    }
}

/* Cases RFC 3610 has none of: no message, and associated data spanning several blocks. These
 * values were checked against PyCryptodome's AES-CCM, not taken from a standard. */
static void ccm_edges(void) {
    uint8_t key[16], nonce[13] = {0}, aad[40], tag[8];
    unhex("c0c1c2c3c4c5c6c7c8c9cacbcccdcecf", key);
    for (uint8_t i = 0; i < sizeof aad; i++) {
        aad[i] = i;
    }
    CHECK(tern_ccm_seal(key, nonce, NULL, 0, NULL, 0, NULL, tag));
    check_hex(tag, 8, "d7f35de12312b86e", "empty");
    CHECK(tern_ccm_open(key, nonce, NULL, 0, NULL, 0, tag, NULL));
    CHECK(tern_ccm_seal(key, nonce, aad, sizeof aad, NULL, 0, NULL, tag));
    check_hex(tag, 8, "88a44c05afaa7589", "40 bytes of associated data");
}

static void sha512_of(const char *msg, size_t len, uint8_t out[64]) {
    struct tern_sha512 c;
    tern_sha512_init(&c);
    tern_sha512_update(&c, (const uint8_t *)msg, len);
    tern_sha512_final(&c, out);
}

/* FIPS 180-4's examples (NIST's "SHA512.pdf"): one block, two blocks, and the empty message. */
static void sha512_matches_fips_180_4(void) {
    uint8_t d[64];
    sha512_of("abc", 3, d);
    check_hex(d, 64,
              "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
              "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
              "abc");
    sha512_of("", 0, d);
    check_hex(d, 64,
              "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
              "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
              "empty");
    const char *two = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                      "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    sha512_of(two, strlen(two), d);
    check_hex(d, 64,
              "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
              "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909",
              "896 bits");
}

/* A million "a"s in pieces of awkward sizes, as for SHA-256. */
static void sha512_million_a_in_pieces(void) {
    struct tern_sha512 c;
    uint8_t a[997], d[64];
    memset(a, 'a', sizeof a);
    tern_sha512_init(&c);
    size_t left = 1000000;
    for (size_t step = 1; left > 0; step = (step + 13) % sizeof a + 1) {
        size_t n = step < left ? step : left;
        tern_sha512_update(&c, a, n);
        left -= n;
    }
    tern_sha512_final(&c, d);
    check_hex(d, 64,
              "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
              "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b",
              "a million a");
}

/* RFC 7748, section 5.2: the two single vectors, and the iterated test, in which the output becomes
 * the next scalar and the scalar the next u. A thousand iterations is quick on a host; the
 * million-iteration value is left to anyone patient. */
static void x25519_matches_rfc_7748(void) {
    uint8_t k[32], u[32], out[32];
    unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", k);
    unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u);
    CHECK(tern_x25519(out, k, u));
    check_hex(out, 32, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552",
              "vector 1");
    unhex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", k);
    unhex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", u);
    CHECK(tern_x25519(out, k, u));
    check_hex(out, 32, "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957",
              "vector 2 (u has its top bit set, which is ignored)");

    memset(k, 0, sizeof k);
    memset(u, 0, sizeof u);
    k[0] = u[0] = 9;
    for (int i = 1; i <= 1000; i++) {
        CHECK(tern_x25519(out, k, u));
        memcpy(u, k, 32);
        memcpy(k, out, 32);
        if (i == 1) {
            check_hex(k, 32, "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079",
                      "one iteration");
        }
    }
    check_hex(k, 32, "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",
              "1000 iterations");
}

/* RFC 7748, section 6.1: Alice and Bob agree on a key. */
static void x25519_diffie_hellman_matches_rfc_7748(void) {
    uint8_t a[32], b[32], ka[32], kb[32], k1[32], k2[32];
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a);
    unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b);
    tern_x25519_base(ka, a);
    tern_x25519_base(kb, b);
    check_hex(ka, 32, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", "K_A");
    check_hex(kb, 32, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", "K_B");
    CHECK(tern_x25519(k1, a, kb));
    CHECK(tern_x25519(k2, b, ka));
    check_hex(k1, 32, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", "K");
    CHECK(memcmp(k1, k2, 32) == 0);
}

/* Points of small order give all zeros, and are refused: u = 0, u = 1, and u = p - 1, the
 * encodings of points of order 4, 1 and 2 on the Montgomery curve. */
static void x25519_refuses_small_order(void) {
    uint8_t k[32], u[32], out[32];
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", k);
    memset(u, 0, sizeof u);
    CHECK(!tern_x25519(out, k, u));
    u[0] = 1;
    CHECK(!tern_x25519(out, k, u));
    unhex("ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f", u);
    CHECK(!tern_x25519(out, k, u));
}

static void equal_ct_compares(void) {
    const uint8_t a[4] = {1, 2, 3, 4}, b[4] = {1, 2, 3, 5};
    CHECK(tern_equal_ct(a, a, 4));
    CHECK(!tern_equal_ct(a, b, 4));
    CHECK(tern_equal_ct(a, b, 3));
    CHECK(tern_equal_ct(a, b, 0));
}

int main(void) {
    RUN(sha256_matches_fips_180_4);
    RUN(sha256_million_a_in_pieces);
    RUN(hmac_matches_rfc_4231);
    RUN(hkdf_expand_matches_rfc_5869);
    RUN(aes128_matches_fips_197);
    RUN(ccm_matches_rfc_3610);
    RUN(ccm_edges);
    RUN(sha512_matches_fips_180_4);
    RUN(sha512_million_a_in_pieces);
    RUN(x25519_matches_rfc_7748);
    RUN(x25519_diffie_hellman_matches_rfc_7748);
    RUN(x25519_refuses_small_order);
    RUN(equal_ct_compares);
    return CHECK_DONE();
}
