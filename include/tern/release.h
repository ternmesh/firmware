#ifndef TERN_RELEASE_H
#define TERN_RELEASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Signed images (draft/companion.md, "Signed images", version 13, in ternmesh/spec).
 *
 * Whoever publishes a node's firmware signs each image with a release key: Ed25519 (RFC 8032), over
 * the 16 ASCII bytes "TERN-FIRMWARE-V1" and the SHA-256 of the image exactly as published. A node
 * holds its publisher's release keys, a short list so that a key can be changed, and takes an image
 * over the companion link only if its signature verifies with one of them. */

#define TERN_RELEASE_KEY_LEN 32       /* a release key: an Ed25519 public key */
#define TERN_RELEASE_SIGNATURE_LEN 64 /* an Ed25519 signature */
#define TERN_RELEASE_MESSAGE_LEN 48   /* what a release key signs: the prefix and a SHA-256 */

/* What a release key signs for the image whose SHA-256 is digest. */
void tern_release_message(const uint8_t digest[32], uint8_t message[TERN_RELEASE_MESSAGE_LEN]);

/* Whether sig, len bytes, is the signature of one of n release keys over the image whose SHA-256
 * is digest: it must be TERN_RELEASE_SIGNATURE_LEN bytes, and verify as RFC 8032 has it, S below
 * the group order. A key that is not a valid point, or is of small order, verifies nothing. With
 * no keys, nothing verifies. */
bool tern_release_verify(const uint8_t (*keys)[TERN_RELEASE_KEY_LEN], size_t n,
                         const uint8_t digest[32], const uint8_t *sig, size_t len);

#endif
