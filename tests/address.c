#include "tern/address.h"

#include <string.h>

#include "check.h"
#include "tern/crypto.h"

/* Addresses against RFC 8032's published keys, and the specification's address vectors
 * (tests/contact.c checks those, with the rest of first contact). */

static void unhex(const char *hex, uint8_t *out) {
    for (size_t i = 0; hex[2 * i] && hex[2 * i + 1]; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

/* RFC 8032, section 7.1: TEST 1, 2, 3 and 1024, secret key to public key. */
static void addresses_match_rfc_8032(void) {
    static const char *const keys[][2] = {
        {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
         "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"},
        {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
         "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c"},
        {"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
         "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025"},
        {"f5e5767cf153319517630f226876b86c8160cc583bc013744c6bf255f5cc0ee5",
         "278117fc144c72340f67d0f2316e8386ceffbf2b2428c9c51fef7c597f1d426e"},
    };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        uint8_t seed[32], want[32];
        struct tern_identity id;
        unhex(keys[i][0], seed);
        unhex(keys[i][1], want);
        tern_identity_init(&id, seed);
        CHECK(memcmp(id.address, want, 32) == 0);
        CHECK(tern_address_valid(id.address));
        tern_identity_wipe(&id);
    }
}

/* An address's X25519 key, converted from it, is the X25519 public key of its X25519 private
 * key: two routes to the same 32 bytes. */
static void conversion_agrees_with_x25519(void) {
    for (uint8_t s = 1; s <= 8; s++) {
        uint8_t seed[32], k[32], via_address[32], via_x25519[32];
        struct tern_identity id;
        memset(seed, s, sizeof seed);
        tern_identity_init(&id, seed);
        tern_identity_x25519(&id, k);
        CHECK(tern_address_x25519(via_address, id.address));
        tern_x25519_base(via_x25519, k);
        CHECK(memcmp(via_address, via_x25519, 32) == 0);
        tern_identity_wipe(&id);
    }
}

/* RFC 8032, section 7.1: TEST 1, 2 and 3, signed and checked. */
static void signatures_match_rfc_8032(void) {
    static const char *const cases[][3] = {
        {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e3970"
         "1cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613"
         "d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
        {"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7", "af82",
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760"
         "984dc6594a7c15e9716ed28dc027beceea1ec40a"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint8_t seed[32], msg[2], want[64], sig[64];
        size_t len = strlen(cases[i][1]) / 2;
        struct tern_identity id;
        unhex(cases[i][0], seed);
        unhex(cases[i][1], msg);
        unhex(cases[i][2], want);
        tern_identity_init(&id, seed);
        tern_identity_sign(&id, msg, len, sig);
        CHECK(memcmp(sig, want, 64) == 0);
        CHECK(tern_address_verify(id.address, msg, len, want));

        /* Any bit of R or S changed, or of the message, and it is no longer good. */
        for (size_t bit = 0; bit < 512; bit += 37) {
            sig[bit / 8] ^= (uint8_t)(1u << (bit % 8));
            CHECK(!tern_address_verify(id.address, msg, len, sig));
            sig[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        }
        if (len) {
            msg[0] ^= 1;
            CHECK(!tern_address_verify(id.address, msg, len, sig));
        }
        tern_identity_wipe(&id);
    }
}

/* S + L is the same scalar, and RFC 8032 refuses it: a signature has one S, so no one can make a
 * second from it. */
static void signature_s_must_be_below_l(void) {
    static const uint8_t order[32] = {
        0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
        0xa2, 0xde, 0xf9, 0xde, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
    };
    uint8_t seed[32], sig[64];
    struct tern_identity id;
    unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed);
    tern_identity_init(&id, seed);
    tern_identity_sign(&id, (const uint8_t *)"", 0, sig);
    CHECK(tern_address_verify(id.address, (const uint8_t *)"", 0, sig));
    unsigned carry = 0;
    for (int i = 0; i < 32; i++) {
        unsigned v = sig[32 + i] + order[i] + carry;
        sig[32 + i] = (uint8_t)v;
        carry = v >> 8;
    }
    CHECK(carry == 0 && !tern_address_verify(id.address, (const uint8_t *)"", 0, sig));
    tern_identity_wipe(&id);
}

int main(void) {
    RUN(addresses_match_rfc_8032);
    RUN(conversion_agrees_with_x25519);
    RUN(signatures_match_rfc_8032);
    RUN(signature_s_must_be_below_l);
    return CHECK_DONE();
}
