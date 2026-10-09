#ifndef TERN_ADDRESS_H
#define TERN_ADDRESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Addresses: specification draft 0, draft/first-contact.md in ternmesh/spec, section "Addresses".
 *
 * A node's identity is a 32-byte seed, from which Ed25519 (RFC 8032) makes a key pair. Its public
 * key is the node's address. The same seed gives the node an X25519 key (RFC 7748) for key
 * agreement, and anyone can compute the X25519 public key from the address. */

#define TERN_ADDRESS_LEN 32
#define TERN_SEED_LEN 32
#define TERN_SIGNATURE_LEN 64

struct tern_identity {
    uint8_t seed[TERN_SEED_LEN];
    uint8_t address[TERN_ADDRESS_LEN];
};

/* Makes an identity from a seed, which must be 32 secret random bytes, and computes its address.
 * The seed is copied; the caller should erase its own copy. */
void tern_identity_init(struct tern_identity *id, const uint8_t seed[TERN_SEED_LEN]);

/* Erases an identity. */
void tern_identity_wipe(struct tern_identity *id);

/* The X25519 private key of a seed: the first 32 bytes of SHA-512(seed), the same bytes Ed25519
 * makes its secret scalar from. */
void tern_identity_x25519(const struct tern_identity *id, uint8_t k[32]);

/* Whether an address is valid: it decodes to a point (RFC 8032, section 5.1.3), the point is not
 * the neutral element, and it is in the prime-order subgroup ([L]A is the neutral element). Not
 * constant time: addresses are public. */
bool tern_address_valid(const uint8_t address[TERN_ADDRESS_LEN]);

/* The X25519 public key of an address, u = (1 + y) / (1 - y). Returns false, writing nothing, if
 * the address is not valid. */
bool tern_address_x25519(uint8_t u[32], const uint8_t address[TERN_ADDRESS_LEN]);

/* Signs len bytes of m with the identity's key: Ed25519, RFC 8032, section 5.1.6. What the
 * signature is over says what it is for, so a node signs nothing but what a section of the
 * specification asks it to: today, a presence card (tern/card.h). */
void tern_identity_sign(const struct tern_identity *id, const uint8_t *m, size_t len,
                        uint8_t sig[TERN_SIGNATURE_LEN]);

/* Whether sig is the signature of the address's key over len bytes of m: RFC 8032, section
 * 5.1.7, with [S]B = R + [k]A checked as it stands, not multiplied by 8, and S required below L.
 * It does not check that the address is valid: an address of small order passes for messages it
 * never signed, so a caller checks tern_address_valid() first. Not constant time: a signature is
 * public. */
bool tern_address_verify(const uint8_t address[TERN_ADDRESS_LEN], const uint8_t *m, size_t len,
                         const uint8_t sig[TERN_SIGNATURE_LEN]);

#endif
