#ifndef NODE_RELEASE_KEYS_H
#define NODE_RELEASE_KEYS_H

#include <stdint.h>

/* The release keys an image over the companion link must be signed by (draft/companion.md,
 * "Signed images"; tern/release.h): the Ed25519 public keys of whoever publishes this firmware.
 * A node holds a short list so that a key can be changed: a release adds the new key, releases
 * after it are signed with the new, and a later one drops the old. docs/signing.md says how a key
 * is made and added.
 *
 * Each key is a comment saying when it was added, then 32 bytes in braces, as
 * `python3 tools/release_key.py public KEYFILE` prints them, above the list's end. release.yml
 * checks that the key it signs a release with is one of these, and fails the release otherwise.
 *
 * With none, a node takes no update over the link at all: every UPDATE_BEGIN is refused with
 * ERROR 11. The specification's test key (tools/release_key.py --test) never goes here. */
static const uint8_t node_release_keys[][32] = {
    /* No release key yet: see docs/signing.md. */
    {0}, /* ends the list, and verifies nothing */
};

#endif
