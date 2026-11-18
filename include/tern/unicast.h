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
 * erased as soon as it has been used, an epoch key and its tag key once the chain has moved past
 * them, and each message key straight after its one use. A tag key is kept a little longer than
 * its epoch key, as the specification allows, only while it is still needed: the sender's, while
 * an acknowledgement of a message of its epoch may yet come; the receiver's, while a copy of one
 * is still answered. tern_unicast_expire() lets them go when the time for that has passed.
 *
 * So is when a sender must start again (the specification's "Starting again"): seal refuses with
 * TERN_ESPENT once the next counter could be beyond the receiver's window, or RESTART_AFTER
 * messages have been given up in a row, and the caller makes first contact again. A session's
 * life is the caller's, which has the clock: tern_session keeps its age for it. */

#include "tern/time.h"

#define TERN_UNICAST_COPY_ACKS 4            /* forwarding.md: acknowledgements of one message */
#define TERN_UNICAST_COPY_TIME TERN_S(600)  /* and for how long after it was accepted */
#define TERN_UNICAST_RESTART_AFTER 2        /* messages given up in a row */
#define TERN_SESSION_LIFE TERN_S(7 * 86400) /* no frame is sent in a session older */
#define TERN_SESSION_GRACE TERN_S(86400)    /* and it is erased this much later */

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
    struct tern_aes128 tag_key; /* TK_d(e) for the epoch of next, expanded */
    uint8_t iv[TERN_CCM_NONCE]; /* IV_d */
    uint8_t epoch_key[32];      /* EK_d(e) for the epoch of next */
    uint32_t next;              /* the counter the next message will use */
    uint8_t dir;
    bool spent; /* every counter has been used; the session needs replacing */
    /* Starting again: K, the highest counter validly acknowledged (if acked_any), and G, the
     * messages given up in a row since. */
    bool acked_any;
    uint8_t given_up;
    uint32_t acked;
    /* The tag key of the epoch before next's, while an acknowledgement of a message of it may
     * still come: prev_at is when that was first seen to be so, in seconds of the caller's clock
     * (tern_unicast_expire()). Not packed, and nothing sealed before sent_from, which a restart
     * sets, is checked: its acknowledgement is not taken. */
    bool prev_held, prev_dated;
    uint8_t prev_tag_key[TERN_AES128_KEY];
    uint32_t prev_at;
    uint32_t sent_from;
    /* Seconds longer than COPY_TIME that prev's tag key is kept: the other end sleeps, and its
     * acknowledgements come that much later (tern_session_sleep()). Not packed. */
    uint32_t wait_extra;
};

/* One direction, as its receiver holds it. The window is every counter from max(0, high - 31) to
 * min(high + 32, 2^32 - 1), or 0 to 31 until something is accepted, less those accepted. */
struct tern_unicast_rx {
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
    /* What is kept of each accepted counter no more than 31 below high, to answer copies of it:
     * counter n's at n % 32, if bit n % 32 of `kept` is set. The first four bytes of its AEAD tag,
     * its length less 23, whether it had the node flag, when it was accepted (seconds of the
     * caller's clock, kept to 16 bits: COPY_TIME is far less) and how many acknowledgements of it
     * have gone (two bits each). And the tag keys of the two epochs below first_epoch, which
     * give the proofs of those counters' acknowledgements, while any of them is kept. Not
     * packed: after a restart no copy of a message accepted before is answered. */
    uint32_t kept, node;
    uint64_t acks;
    uint8_t check[32][4];
    uint8_t len[32];
    uint16_t at[32];
    uint8_t old_held; /* bit i: old_tag_keys[i], for epoch first_epoch - 2 + i, is held */
    uint8_t old_tag_keys[2][TERN_AES128_KEY];
    /* Seconds longer than COPY_TIME that copies are answered: this node sleeps, and its sources'
     * later tries come that much later (tern_session_sleep()). Not packed. */
    uint16_t copy_extra;
};

struct tern_session {
    struct tern_unicast_tx tx;
    struct tern_unicast_rx rx;
    /* Seconds the session had been held when last packed: the caller's to keep up to date, by
     * whatever clock it has, for the session's life. */
    uint32_t age;
};

/* Sets up both directions of a session from its 32-byte secret, as this node's role, and then
 * erases the secret: the caller's copy is all zeros afterwards, as the specification requires. */
void tern_session_init(struct tern_session *s, uint8_t secret[TERN_UNICAST_SECRET],
                       enum tern_role role);

/* Leaves that sleep (tern/sleep.h): `own_wake` is this node's wake if it sleeps, and `peer_wake`
 * the other end's, as its sleep content said, each in a promise's encoding, 0 for not sleeping. A
 * sleeping node answers copies for COPY_TIME plus RETRIES + 1 of its wakes, and a source to a
 * sleeping node keeps what checks its acknowledgements as much longer. Neither is packed: the
 * caller says them again after a restart. */
void tern_session_sleep(struct tern_session *s, uint16_t own_wake, uint16_t peer_wake);

/* Erases every key a session holds. */
void tern_session_wipe(struct tern_session *s);

/* A session in TERN_SESSION_PACKED bytes, for a node to keep in flash: what the specification
 * requires to outlive a restart (each direction's counter, window and keys, and the sender's
 * K and G) and the session's age. The struct also holds what can be worked out again from that
 * (the tag keys and the destination tag of every counter in the window), and what need not
 * outlive a restart (what answers copies and checks acknowledgements). Not part of the
 * specification: the layout is this code's, little-endian, and begins with a byte that names it.
 * The first form, from before each epoch had its own tag key, is not taken: its sessions are made
 * again by first contact.
 *
 * Unpacking gives back a session that seals and opens exactly as the packed one would have; it
 * is false, with s erased, for bytes that are not a packed session. Both leave keys in their
 * output, which the caller erases when it is done with it. */
#define TERN_SESSION_PACKED 208
void tern_session_pack(const struct tern_session *s, uint8_t out[TERN_SESSION_PACKED]);
bool tern_session_unpack(struct tern_session *s, const uint8_t in[TERN_SESSION_PACKED]);

/* Seals len bytes of plaintext (at most TERN_UNICAST_MAX_PLAINTEXT) into a frame of len + 23
 * bytes at frame, as the next message in this session: the one whose counter is tx->next before
 * the call. plaintext and frame must not overlap. Returns TERN_OK, TERN_EINVAL if len is too long
 * or frame_cap too small, or TERN_ESPENT if every counter has been used or the session must be
 * started again (tern_unicast_restart()). */
int tern_unicast_seal(struct tern_unicast_tx *tx, const uint8_t *plaintext, size_t len,
                      uint8_t *frame, size_t frame_cap);

/* The same, with the node flag set: a plaintext for the other node itself, such as a group's
 * invite (tern/group.h), whose first byte says what it is. It takes the session's next counter
 * like any other message. */
int tern_unicast_seal_node(struct tern_unicast_tx *tx, const uint8_t *plaintext, size_t len,
                           uint8_t *frame, size_t frame_cap);

/* Whether this sender must start the session again before it sends: its next counter could be
 * beyond the receiver's window (more than 32 above the highest acknowledged, or than 31 with
 * nothing acknowledged), or RESTART_AFTER messages have been given up in a row. */
bool tern_unicast_restart(const struct tern_unicast_tx *tx);

/* A message this sender sent was given up: its source tried it RETRIES times beyond the first
 * and heard no valid acknowledgement. */
void tern_unicast_given_up(struct tern_unicast_tx *tx);

/* Lets go of what a session keeps only for a while: what answers a copy of a message accepted
 * more than TERN_UNICAST_COPY_TIME ago, and the tag keys of epochs nothing still needs. now is
 * the caller's clock. Call it now and then; tern_unicast_open() does it for the receivers it is
 * given. */
void tern_unicast_expire(struct tern_session *s, tern_time now);

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
    /* Not accepted, and a copy of a message accepted not long before: of its length and header,
     * with its tag and the first four bytes of its AEAD tag. Its sender did not hear the
     * acknowledgement, which is owed again, if fewer than TERN_UNICAST_COPY_ACKS have gone and
     * no more than TERN_UNICAST_COPY_TIME has passed. Nothing else in it was checked. session
     * and counter are the first such message's; tern_unicast_copy_next() gives any others. */
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
 * frame: a failed attempt erases it, and the next attempt still needs the frame. now is the
 * caller's clock, for how long ago a message was accepted.
 *
 * Returns TERN_OK with out filled in, or TERN_EINVAL if plaintext_cap cannot hold the plaintext
 * of a frame of this length. */
int tern_unicast_open(struct tern_unicast_rx *const *rx, size_t count, const uint8_t *frame,
                      size_t len, uint8_t *plaintext, size_t plaintext_cap, tern_time now,
                      struct tern_unicast_received *out);

/* Whether tern_unicast_open() would accept the frame, and in which session, changing nothing: for a
 * caller that keeps a copy of that one session's receiver, to put back if it cannot store what
 * accepting the frame does to it. It opens the frame to know, into plaintext, which it erases. */
bool tern_unicast_peek(struct tern_unicast_rx *const *rx, size_t count, const uint8_t *frame,
                       size_t len, uint8_t *plaintext, size_t plaintext_cap, size_t *session);

/* A frame that is a COPY may be one of more than one message: four bytes of tag and four of AEAD
 * tag can be those of two, in two sessions or in one, and nothing else in the frame says which.
 * Each still answered is owed its acknowledgement. Given what tern_unicast_open() or this last
 * said of the same frame, moves `got` on to the next such message; false when there are no
 * more. */
bool tern_unicast_copy_next(struct tern_unicast_rx *const *rx, size_t count, const uint8_t *frame,
                            size_t len, tern_time now, struct tern_unicast_received *got);

/* Writes the acknowledgement of message `counter` to frame: its header, its tag and its proof,
 * with the forwarder's ten bytes zero, and counts it against TERN_UNICAST_COPY_ACKS. False, and
 * nothing written, unless this receiver has accepted that counter, it is no more than 31 below
 * the highest, and what answers copies of it is still held: tern_unicast_open() said ACCEPTED or
 * COPY of it. */
bool tern_unicast_ack(struct tern_unicast_rx *rx, uint32_t counter,
                      uint8_t frame[TERN_UNICAST_ACK_LEN]);

/* Whether a frame shows that message `counter`, which this sender sent, arrived: an
 * acknowledgement with that message's tag and its proof. If it does, the sender takes note: it
 * is what tells it when to start again. */
bool tern_unicast_acked(struct tern_unicast_tx *tx, uint32_t counter, const uint8_t *frame,
                        size_t len);

#endif
