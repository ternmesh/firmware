#include "tern/release.h"

#include <string.h>

#include "tern/address.h"

#define PREFIX "TERN-FIRMWARE-V1" /* sixteen bytes, without the NUL */
#define PREFIX_LEN 16

void tern_release_message(const uint8_t digest[32], uint8_t message[TERN_RELEASE_MESSAGE_LEN]) {
    _Static_assert(sizeof PREFIX - 1 == PREFIX_LEN, "the prefix is sixteen bytes");
    _Static_assert(PREFIX_LEN + 32 == TERN_RELEASE_MESSAGE_LEN, "the prefix and a SHA-256");
    memcpy(message, PREFIX, PREFIX_LEN);
    memcpy(message + PREFIX_LEN, digest, 32);
}

bool tern_release_verify(const uint8_t (*keys)[TERN_RELEASE_KEY_LEN], size_t n,
                         const uint8_t digest[32], const uint8_t *sig, size_t len) {
    if (len != TERN_RELEASE_SIGNATURE_LEN || keys == NULL) {
        return false;
    }
    uint8_t message[TERN_RELEASE_MESSAGE_LEN];
    tern_release_message(digest, message);
    for (size_t i = 0; i < n; i++) {
        /* tern_address_verify() takes any point as the key, and one of small order passes for
         * messages it never signed: a release key is checked as an address is. */
        if (tern_address_valid(keys[i]) &&
            tern_address_verify(keys[i], message, sizeof message, sig)) {
            return true;
        }
    }
    return false;
}
