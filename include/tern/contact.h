#ifndef TERN_CONTACT_H
#define TERN_CONTACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tern/address.h"
#include "tern/unicast.h"

/* First contact: specification draft 0, draft/first-contact.md in ternmesh/spec.
 *
 * Two nodes, one of which knows the other's address, run an EDHOC handshake (RFC 9528, method 3,
 * cipher suite 0) of four frames and come out sharing a session secret, which starts a secured
 * unicast session (tern/unicast.h). Each side holds a struct tern_contact for the handshake.
 *
 * The initiator calls tern_contact_start() for message_1. A node that hears a frame with header
 * 0x51 calls tern_contact_respond() with a fresh struct, which answers with message_2 if the frame
 * is a message_1 for this node. After that, each side gives every frame it hears to
 * tern_contact_receive() for each handshake it is in, until one is complete; then
 * tern_contact_finish() starts the session.
 *
 * The core has no source of randomness, so the caller supplies each handshake's ephemeral private
 * key: 32 bytes it MUST draw fresh from a cryptographically secure generator, and never reuse.
 * The vectors set them, as a test hook.
 *
 * Frames are written with hop and label 0. They belong to the routing layer, are not protected,
 * and may be set in bytes 1 to 3 before sending.
 *
 * Stack: the deepest call, tern_contact_receive() checking the initiator's address in message_3,
 * uses about 3.6 KB on a Cortex-M4 (Clang, -Os), most of it elliptic-curve arithmetic. A task that
 * runs a handshake needs that on top of its own use; ESP-IDF's default main task (3.5 KB) does
 * not have it. */

#define TERN_CONTACT_EPHEMERAL 32
#define TERN_CONTACT_SECRET 32
#define TERN_CONTACT_MAX_FRAME 73 /* message_3's frame; the others are 45, 53 and 17 bytes */

struct tern_contact {
    uint8_t role;   /* TERN_INITIATOR or TERN_RESPONDER; 0 if unused or aborted */
    uint8_t expect; /* the message number this side waits for next, or 0 */
    bool complete;
    uint8_t peer[TERN_ADDRESS_LEN];      /* the initiator's target, or what the responder learns */
    uint8_t ephemeral[32];               /* x or y, until it is no longer needed */
    uint8_t ctag[4][4];                  /* ctag_1 to ctag_4 */
    uint8_t h_message_1[32];             /* the initiator's, until message_2 */
    uint8_t g_rx[32];                    /* the initiator's, until message_2 */
    uint8_t prk[32];                     /* PRK_3e2m (responder) or PRK_4e3m (initiator) */
    uint8_t th[32];                      /* TH_3 (responder) or TH_4 (initiator) */
    uint8_t secret[TERN_CONTACT_SECRET]; /* S, once complete */
};

/* What became of a frame. */
enum tern_contact_verdict {
    /* It was for this handshake and was processed. *out_len is the length of the frame to send
     * in reply, or 0 if there is none; tern_contact_complete() tells whether S is ready. */
    TERN_CONTACT_PROCESSED = 1,
    /* Not a frame for this handshake: someone else's, a different message, or garbage. Nothing
     * changed. */
    TERN_CONTACT_NOT_OURS,
    /* It was for this handshake but failed a check. The handshake is aborted and its state
     * erased, as the specification requires. */
    TERN_CONTACT_FAILED,
};

/* Starts a handshake with the node whose address is target: writes message_1's frame (45 bytes)
 * to frame and its length to *len. c_i is the connection identifier, a byte from 0x00 to 0x17 or
 * 0x20 to 0x37. Returns TERN_OK, or TERN_EINVAL if target is not a valid address, c_i is out of
 * range or frame_cap is too small. */
int tern_contact_start(struct tern_contact *c, const uint8_t target[TERN_ADDRESS_LEN],
                       const uint8_t ephemeral[TERN_CONTACT_EPHEMERAL], uint8_t c_i, uint8_t *frame,
                       size_t frame_cap, size_t *len);

/* Answers a frame that may be a message_1 for this node. If it is, sets c up as the responder,
 * writes message_2's frame (53 bytes) to out and returns TERN_CONTACT_PROCESSED. If it is not,
 * returns TERN_CONTACT_NOT_OURS, sends nothing and leaves c unused. c_r is as c_i above. Returns
 * TERN_EINVAL if c_r is out of range or out_cap too small. */
int tern_contact_respond(struct tern_contact *c, const struct tern_identity *me,
                         const uint8_t ephemeral[TERN_CONTACT_EPHEMERAL], uint8_t c_r,
                         const uint8_t *frame, size_t len, uint8_t *out, size_t out_cap,
                         size_t *out_len);

/* Gives a frame to a handshake under way. The initiator answers message_2 with message_3, and
 * completes on message_4; the responder answers message_3 with message_4 and completes. Returns a
 * verdict, or TERN_EINVAL if out_cap is smaller than TERN_CONTACT_MAX_FRAME. */
int tern_contact_receive(struct tern_contact *c, const struct tern_identity *me,
                         const uint8_t *frame, size_t len, uint8_t *out, size_t out_cap,
                         size_t *out_len);

/* Whether the handshake has produced S. */
bool tern_contact_complete(const struct tern_contact *c);

/* Starts the unicast session from a complete handshake, as this node's role, copies the peer's
 * address to peer, and erases the handshake. Returns TERN_OK, or TERN_EINVAL if the handshake is
 * not complete. */
int tern_contact_finish(struct tern_contact *c, struct tern_session *s,
                        uint8_t peer[TERN_ADDRESS_LEN]);

/* Erases a handshake: an abandoned one, or one that timed out. */
void tern_contact_wipe(struct tern_contact *c);

#endif
