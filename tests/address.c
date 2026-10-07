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

int main(void) {
    RUN(addresses_match_rfc_8032);
    RUN(conversion_agrees_with_x25519);
    return CHECK_DONE();
}
