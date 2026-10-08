#ifndef TERN_UNICAST_H
#define TERN_UNICAST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tern/crypto.h"

/* Secured unicast frames: specification draft 0, draft/unicast-security.md in ternmesh/spec, and
 * the acknowledgements their destinations answer with (draft/forwarding.md, "Messages").
 *
 * Two nodes that share a session secret each hold a struct tern_session. It has a sender for the
 * direction this node sends in and a receiver for the other. The sender picks every counter
 * itself, so there is no way to send a counter twice or skip one. The receiver keeps the window
 * of counters it will accept and the destination tag of each, so it can tell from a frame's four
 * clear bytes whether the frame is for it, and from which session and counter.
 *
 * A frame starts with the eleven-byte head of every frame that follows a route. Only its first
 * byte is this layer's: the other ten are the forwarder's (tern/forward.h), written as zeros here
 * and never read.
 *
 * The specification's erasure rules are kept here, not left to the caller: the session secret is
 * erased as soon as it has been used, an epoch key once the chain has moved past it, and each
 * message key straight after its one use. */

#define TERN_UNICAST_HDR 0x48      /* format 01 (draft 0), type 001 (secured unicast), no flags */
#define TERN_UNICAST_HDR_NODE 0x49 /* the node flag: a plaintext for the node, not its user */
#define TERN_UNICAST_HEAD 11       /* hdr, and the forwarder's ten bytes */
#define TERN_UNICAST_OVERHEAD 23   /* the head, dtag and the 8-byte AEAD tag */
#define TERN_UNICAST_MAX_FRAME 255 /* the largest LoRa frame */
#define TERN_UNICAST_MAX_PLAINTEXT (TERN_UNICAST_MAX_FRAME - TERN_UNICAST_OVERHEAD)
#define TERN_UNICAST_SECRET 32
#define TERN_UNICAST_DTAG 4
#define TERN_UNICAST_ACK_HDR 0x50 /* an acknowledgement */
#define TERN_UNICAST_PROOF 4
#define TERN_UNICAST_ACK_LEN (TERN_UNICAST_HEAD + TERN_UNICAST_DTAG + TERN_UNICAST_PROOF)

/* Directions, numbered as in the specification. */
enum tern_role {
    TERN_INITIATOR = 0x01, /* sends in direction 0x01, receives in 0x02 */
    TERN_RESPONDER = 0x02, /* sends in direction 0x02, receives in 0x01 */
};

/* One direction, as its sender holds it. */
struct tern_unicast_tx {
    struct tern_aes128 tag_key; /* TK_d, expanded */
    uint8_t iv[TERN_CCM_NONCE]; /* IV_d */
    uint8_t epoch_key[32];      /* EK_d(e) for the epoch of next */
    uint32_t next;              /* the counter the next message will use */
    uint8_t dir;
    bool spent; /* every counter has been used; the session needs replacing */
};

/* One direction, as its receiver holds it. The window is every counter from max(0, high - 31) to
 * min(high + 32, 2^32 - 1), or 0 to 31 until something is accepted, less those accepted. */
struct tern_unicast_rx {
    struct tern_aes128 tag_key;
    uint8_t iv[TERN_CCM_NONCE];
    uint8_t dir;
    bool started;  /* something has been accepted, so high is set */
    uint32_t high; /* H, the highest counter accepted */
    uint32_t seen; /* bit i: counter high - i has been accepted (i = 0 to 31) */
    /* The epoch keys the window needs, one to three of them, oldest first. */
    uint32_t first_epoch;
    uint8_t epochs;
    uint8_t epoch_keys[3][32];
    /* The destination tag of every counter in the window: counter n's is at n % 64. */
    uint8_t dtags[64][TERN_UNICAST_DTAG];
};

struct tern_session {
    struct tern_unicast_tx tx;
    struct tern_unicast_rx rx;
};

/* Sets up both directions of a session from its 32-byte secret, as this node's role, and then
 * erases the secret: the caller's copy is all zeros afterwards, as the specification requires. */
void tern_session_init(struct tern_session *s, uint8_t secret[TERN_UNICAST_SECRET],
                       enum tern_role role);

/* Erases every key a session holds. */
void tern_session_wipe(struct tern_session *s);

/* Seals len bytes of plaintext (at most TERN_UNICAST_MAX_PLAINTEXT) into a frame of len + 23
 * bytes at frame, as the next message in this session: the one whose counter is tx->next before
 * the call. plaintext and frame must not overlap. Returns TERN_OK, TERN_EINVAL if len is too long
 * or frame_cap too small, or TERN_ESPENT if every counter has been used. */
int tern_unicast_seal(struct tern_unicast_tx *tx, const uint8_t *plaintext, size_t len,
                      uint8_t *frame, size_t frame_cap);

/* The same, with the node flag set: a plaintext for the other node itself, such as a group's
 * invite (tern/group.h), whose first byte says what it is. It takes the session's next counter
 * like any other message. */
int tern_unicast_seal_node(struct tern_unicast_tx *tx, const uint8_t *plaintext, size_t len,
                           uint8_t *frame, size_t frame_cap);

/* Whether a frame tern_unicast_open() accepted is for the node, not its user. */
static inline bool tern_unicast_for_node(const uint8_t *frame) {
    return frame[0] == TERN_UNICAST_HDR_NODE;
}

/* What became of a received frame. */
enum tern_unicast_verdict {
    TERN_UNICAST_ACCEPTED = 1,
    TERN_UNICAST_NOT_OURS,  /* its tag is in no window: someone else's frame, not an error */
    TERN_UNICAST_MALFORMED, /* too short, too long, or a header this code does not implement */
    TERN_UNICAST_FORGED,    /* its tag matched, but no matching key authenticated it */
    /* Not accepted, and its tag is that of a message accepted not long before: its sender did not
     * hear the acknowledgement, which is owed again. Nothing in it was checked. session and
     * counter are the first such message's; tern_unicast_copy_next() gives any others. */
    TERN_UNICAST_COPY,
};

struct tern_unicast_received {
    enum tern_unicast_verdict verdict;
    /* ACCEPTED and COPY only. */
    size_t session; /* index into the array of receivers given */
    uint32_t counter;
    size_t len; /* plaintext bytes written; 0 for a copy */
};

/* Receives a frame on behalf of every session the node holds. rx[0..count-1] are the receivers;
 * all their windows' tags are checked, and every match tried, so a tag shared by two sessions or
 * two counters never loses a frame. On acceptance the plaintext (len - 23 bytes) is written to
 * plaintext, and the window moves. Nothing else changes any state. plaintext must not overlap
 * frame: a failed attempt erases it, and the next attempt still needs the frame.
 *
 * Returns TERN_OK with out filled in, or TERN_EINVAL if plaintext_cap cannot hold the plaintext
 * of a frame of this length. */
int tern_unicast_open(struct tern_unicast_rx *const *rx, size_t count, const uint8_t *frame,
                      size_t len, uint8_t *plaintext, size_t plaintext_cap,
                      struct tern_unicast_received *out);

/* A frame that is a COPY may be one of more than one message: four bytes can be the tag of two,
 * in two sessions or in one, and nothing else in the frame says which. Each is owed its
 * acknowledgement. Given what tern_unicast_open() or this last said of the same frame, moves
 * `got` on to the next such message; false when there are no more. */
bool tern_unicast_copy_next(struct tern_unicast_rx *const *rx, size_t count, const uint8_t *frame,
                            size_t len, struct tern_unicast_received *got);

/* Writes the acknowledgement of message `counter` to frame: its header, its tag and its proof,
 * with the forwarder's ten bytes zero. False, and nothing written, unless this receiver has
 * accepted that counter and it is no more than 31 below the highest: tern_unicast_open() said
 * ACCEPTED or COPY of it. */
bool tern_unicast_ack(const struct tern_unicast_rx *rx, uint32_t counter,
                      uint8_t frame[TERN_UNICAST_ACK_LEN]);

/* Whether a frame shows that message `counter`, which this sender sent, arrived: an
 * acknowledgement with that message's tag and its proof. */
bool tern_unicast_acked(const struct tern_unicast_tx *tx, uint32_t counter, const uint8_t *frame,
                        size_t len);

#endif
