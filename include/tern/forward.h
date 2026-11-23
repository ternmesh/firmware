#ifndef TERN_FORWARD_H
#define TERN_FORWARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tern/airtime.h"
#include "tern/route.h"
#include "tern/time.h"

/* The frames that follow routes: specification draft 0, draft/forwarding.md in ternmesh/spec.
 *
 * A message goes from node to node along the routes tern/route.h keeps. Each frame says in clear
 * which neighbour it is for now and which node it is for in the end, and nothing of where it came
 * from. A node that sends one listens for its next hop sending it on, sends it again if it hears
 * nothing, and after that tries another route if it has one. A node's own message is sent again,
 * from the start, until its destination's acknowledgement comes back or it has been tried often
 * enough.
 *
 * This is the simulator's candidate 3 (ternmesh/sim, src/distvec.c) as far as carrying a message:
 * its rescue floods and broadcast are not here.
 *
 * First contact's four frames (tern/contact.h) go the same way. The node that begins a handshake
 * keeps each of its two as it keeps a message, until the handshake's next frame answers it
 * (tern_forward_done()) or it has been tried often enough (tern_forward_contact_failed()).
 *
 * Given an airtime budget (tern/airtime.h), it keeps to it: a frame of the node's own waits until
 * its part holds it, one passed on that its part does not hold is dropped, and an acknowledgement
 * goes whatever its part holds.
 *
 * Like the router it owns no clock, radio or memory, and what a frame holds past its head is the
 * caller's: for a message, the secured unicast frame's tag, ciphertext and check (tern/unicast.h).
 *
 *     tern_forward_heard(&f, now, frame, len, snr, &got);   for each such frame received
 *     while ((len = tern_forward_poll(&f, now, frame, &dbm, &kind, &h)) != 0) { queue it }
 *     ... tern_forward_sent(&f, now, h) once each has gone on the air ...
 *     sleep until tern_forward_due(&f), or a frame arrives
 */

#define TERN_HDR_MESSAGE 0x48   /* a secured unicast frame */
#define TERN_HDR_NODE 0x49      /* one whose plaintext is for the node, not its user */
#define TERN_HDR_ACK 0x50       /* its destination's acknowledgement */
#define TERN_HDR_CONTACT_1 0x51 /* first contact's four frames, from message_1's */
#define TERN_HDR_CONTACT_4 0x54
#define TERN_HDR_HELD 0x55         /* a keeper's held notice (tern/sleep.h) */
#define TERN_HDR_TRACE 0x56        /* a trace: the way to a node, gathered as it goes */
#define TERN_HDR_TRACE_ANSWER 0x57 /* its answer, gathering the way back */

#define TERN_FORWARD_HEAD 11 /* hdr, hops, power, next hop and destination */
#define TERN_FORWARD_TAG 4   /* what tells one frame from another, after the head */
#define TERN_FORWARD_MIN (TERN_FORWARD_HEAD + TERN_FORWARD_TAG)
#define TERN_FORWARD_FRAME_MAX 255
#define TERN_ACK_LEN (TERN_FORWARD_MIN + 4)
#define TERN_HELD_LEN TERN_FORWARD_MIN /* a held notice: the head and the tag, nothing after */
#define TERN_MESSAGE_MIN (TERN_FORWARD_MIN + 8) /* an empty message: its tag and its check */
/* A first-contact frame: the handshake's message, and for the first the routing id it came from. */
#define TERN_CONTACT_LEN(hdr)                                                                      \
    ((hdr) == 0x51 ? 56u : (hdr) == 0x52 ? 60u : (hdr) == 0x53 ? 116u : 61u)
#define TERN_FORWARD_SLOTS_MAX 255
#define TERN_FORWARD_SALVAGE_MAX 4
#define TERN_FORWARD_NOTICE 0xFF /* the handle of a frame no slot holds: a held notice */
#define TERN_FORWARD_NOTICES 2   /* held notices waiting to go */
#define TERN_FORWARD_WAKES 4     /* destinations a source knows sleep */

/* Traces (the specification's "Traces"). A trace carries, after its tag, the routing id it came
 * from and then its entries; an answer, after its tag, `flags` and `out` and then its entries. An
 * entry is a routing id and how well that node heard the frame, an i8 in quarters of a dB. */
#define TERN_TRACE_FROM TERN_FORWARD_MIN             /* where a trace keeps the routing id */
#define TERN_TRACE_MIN (TERN_FORWARD_MIN + 4)        /* a trace with no entries: 19 */
#define TERN_TRACE_ANSWER_MIN (TERN_FORWARD_MIN + 2) /* an answer with none: 17 */
#define TERN_TRACE_ENTRY 5
#define TERN_TRACE_ENTRIES 16 /* each way */
#define TERN_TRACE_ANSWERS 4  /* traces a node answers a minute, as each of the three below */
/* An answer's flags: who answered. */
#define TERN_TRACE_BY_DESTINATION 0
#define TERN_TRACE_BY_KEEPER 1 /* the destination's keeper, the destination asleep */
#define TERN_TRACE_BY_RELAY 2  /* a relay with no route to the destination */
#define TERN_TRACE_BY_COUNT 3

/* Whether a frame is one of this layer's, by its first byte. */
static inline bool tern_forward_contact(uint8_t hdr) {
    return hdr >= TERN_HDR_CONTACT_1 && hdr <= TERN_HDR_CONTACT_4;
}

/* A message, to this layer: for the user or for the node, it goes and is answered the same way. */
static inline bool tern_forward_message(uint8_t hdr) {
    return hdr == TERN_HDR_MESSAGE || hdr == TERN_HDR_NODE;
}

/* A trace or a trace answer: each node it reaches adds its entry. */
static inline bool tern_forward_traced(uint8_t hdr) {
    return hdr == TERN_HDR_TRACE || hdr == TERN_HDR_TRACE_ANSWER;
}

static inline bool tern_forward_frame(const uint8_t *frame, size_t len) {
    return len > 0 && (tern_forward_message(frame[0]) || frame[0] == TERN_HDR_ACK ||
                       tern_forward_contact(frame[0]) || frame[0] == TERN_HDR_HELD ||
                       tern_forward_traced(frame[0]));
}

struct tern_forward_head {
    uint8_t hdr;
    uint8_t hops;  /* nodes it may still be passed on by */
    int8_t power;  /* what it was sent at, dBm */
    uint32_t next; /* the neighbour it is for */
    uint32_t destination;
};

/* Writes a head to the first TERN_FORWARD_HEAD bytes of a frame. */
void tern_forward_head_write(const struct tern_forward_head *h, uint8_t *frame);

/* Reads one. False for a frame the specification says to discard. */
bool tern_forward_head_read(struct tern_forward_head *h, const uint8_t *frame, size_t len);

/* A held notice (the specification's "Leaves that sleep"): `keeper`, the routing id of the keeper
 * that holds the frame, where a head has `next`, and `destination` and `tag` as the frame held has
 * them. Writes TERN_HELD_LEN bytes. tern_forward_head_read() reads its head, `keeper` as `next`. */
size_t tern_forward_held_write(int8_t power, uint32_t keeper, uint32_t destination,
                               const uint8_t tag[TERN_FORWARD_TAG], uint8_t frame[TERN_HELD_LEN]);

/* --- Rules, each as the specification states it, checked against its vectors. --- */

/* The entries a trace or a trace answer carries, and where the first is. A frame
 * tern_forward_head_read() takes. */
size_t tern_forward_entries(const uint8_t *frame, size_t len, size_t *at);

/* An entry, as it goes in a frame: the routing id, and the signal-to-noise ratio in quarters of a
 * dB kept to an i8. */
void tern_forward_entry_write(uint32_t id, int16_t snr_q, uint8_t entry[TERN_TRACE_ENTRY]);

/* A trace or trace answer a relay passes on, as `out`: `hops` one less, and its entry, `id` and
 * how well it heard the frame, added at the end. Returns the length, or 0 where it is dropped: it
 * has come as far as it may, or holds as many entries as it may. A frame
 * tern_forward_head_read() takes. */
size_t tern_forward_entry_pass(const uint8_t *frame, size_t len, uint32_t id, int16_t snr_q,
                               uint8_t out[TERN_FORWARD_FRAME_MAX]);

/* The answer the node `id` gives a trace it heard at `snr_q`, with `flags` saying why, as `out`:
 * to the trace's `from`, `hops` as given, its `next` and `power` 0, for tern_forward_send() to
 * fill. Returns the length, or 0 where the trace holds as many entries as it may. */
size_t tern_forward_answer_write(const uint8_t *trace, size_t len, uint32_t id, int16_t snr_q,
                                 uint8_t flags, uint8_t hops, uint8_t out[TERN_FORWARD_FRAME_MAX]);

/* Whether two frames are the same one, whatever hop each is on: of one length, with one hdr, and
 * equal in every byte from the destination on. A tag alone does not tell: two messages share one
 * now and then. */
bool tern_forward_same(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len);

/* Whether receiving the frame `heard` ends the hop of the frame `sent`, which this node sent: the
 * same frame one hop on, or for a message any acknowledgement with its tag. Both are frames
 * tern_forward_head_read() takes. */
bool tern_forward_ends(const uint8_t *sent, size_t sent_len, const uint8_t *heard,
                       size_t heard_len);

/* Whether a node that has sent this frame listens for its hop to succeed, and sends it again if it
 * does not. The last hop of an acknowledgement or of a first-contact frame has nothing to hear,
 * and is sent once. A frame tern_forward_head_read() takes. */
bool tern_forward_listens(const uint8_t *frame, size_t len);

/* What a frame goes at, in dBm: `neighbour` is what the router gives for its next hop, `back` what
 * the node it came from needs or INT8_MIN for none, `tries` how often it has gone before, each
 * `step` louder, and `full` the most the node sends at. */
int8_t tern_forward_power(int8_t neighbour, int8_t back, uint8_t tries, uint8_t step, int8_t full);

struct tern_forward_config {
    uint8_t hop_max;      /* nodes a frame may be passed on by */
    uint8_t hop_retries;  /* times a hop is sent again */
    uint8_t salvage;      /* other neighbours a frame given up on is tried at */
    uint8_t retries;      /* times a node's own message is sent again from the start */
    uint8_t step_db;      /* louder, each time a hop is sent again */
    uint8_t jitter;       /* airtimes a frame sent in answer to one received waits, at most */
    uint8_t retry_jitter; /* airtimes a frame sent again waits, at most */
    uint8_t ack_factor;   /* route metrics more the acknowledgement is waited for */
    tern_time hop_wait;
    tern_time ack_wait;
};

/* The specification's parameters. */
struct tern_forward_config tern_forward_defaults(void);

enum tern_forward_state {
    TERN_FORWARD_FREE,
    TERN_FORWARD_WAITING,   /* to be sent at `at` */
    TERN_FORWARD_OUT,       /* with the caller, not yet on the air */
    TERN_FORWARD_LISTENING, /* sent; its next hop is listened for until `at` */
    TERN_FORWARD_HELD,   /* a message of this node's, waiting on its acknowledgement until `at` */
    TERN_FORWARD_FAILED, /* given up on: for the caller to collect */
};

/* One frame in hand. The caller allocates these; their fields are the forwarder's. */
struct tern_forward_slot {
    uint8_t state;
    bool own;        /* this node's, not one passed on */
    bool tracked;    /* its acknowledgement is waited for, and it is sent again without */
    bool done;       /* while out: no longer wanted once it has gone */
    bool again;      /* while out: sent before, so already counted against the wait */
    uint8_t tries;   /* times this hop has been sent again */
    uint8_t unheard; /* times it has gone to this next hop and not been heard passed on */
    uint8_t salvages;
    uint8_t attempts; /* times the message has been sent again from the start */
    uint8_t hops;     /* as it goes */
    int8_t power;
    int8_t back; /* the least it may go at, for the node it came from; INT8_MIN for none */
    uint8_t len;
    /* A keeper said it holds this frame of the node's own for a sleeping leaf: with no wake told
     * for the destination, each wait for its answer is as long as a leaf may sleep. */
    bool held_long;
    uint32_t next;
    uint32_t gone[TERN_FORWARD_SALVAGE_MAX];
    tern_time at;
    tern_time deadline; /* when its acknowledgement is given up on, or -1 */
    /* A frame a keeper held for a sleeping leaf and is handing over: the leaf, and the second it
     * was first held (tern_time / 1 s, kept to 32 bits). 0 for any other frame. */
    uint32_t kept_for;
    uint32_t held_s;
    uint8_t frame[TERN_FORWARD_FRAME_MAX];
};

/* A frame a keeper holds for a sleeping leaf. The caller allocates these
 * (tern_forward_keeper()); their fields are the forwarder's. */
struct tern_forward_kept {
    uint32_t leaf;  /* 0 for a free one */
    uint32_t order; /* when it came, against the others */
    tern_time at;   /* when it was first held */
    uint8_t hops;   /* as it goes to the leaf */
    uint8_t len;
    uint8_t frame[TERN_FORWARD_FRAME_MAX];
};

struct tern_forward_counts {
    uint32_t passed_on;  /* frames taken to pass on */
    uint32_t no_route;   /* frames for another node with nowhere to go */
    uint32_t hop_limit;  /* and those that had come as far as they may */
    uint32_t no_room;    /* and those there was no slot for */
    uint32_t sent_again; /* hops sent again */
    uint32_t given_up;   /* hops given up on */
    uint32_t salvaged;   /* of them, tried another way */
    uint32_t unpaid;     /* frames passed on that the budget could not pay for, and dropped */
    uint32_t held;       /* frames held for a sleeping leaf */
    uint32_t held_full;  /* and those there was no room to hold */
    uint32_t held_late;  /* held frames dropped, held longer than HOLD_FOR */
    uint32_t handed;     /* held frames handed over */
};

struct tern_forward {
    struct tern_forward_config config;
    struct tern_route *route;
    struct tern_forward_slot *slot;
    size_t cap;
    uint64_t rng;
    struct tern_airtime *budget; /* or NULL, for none */
    struct tern_forward_counts counts;
    /* A keeper's: what it holds for its sleeping leaves (tern_forward_keeper()), and the held
     * notices waiting to go. */
    struct tern_forward_kept *kept;
    size_t kept_cap;
    uint32_t kept_order;
    /* The last time it was polled at: what it holds is due by it, or by the router's time if that
     * is later, so that a leaf's listen that ended since the router last ran is seen as ended. */
    tern_time polled_at;
    struct tern_forward_notice {
        int8_t power;
        uint8_t frame[TERN_HELD_LEN];
    } notices[TERN_FORWARD_NOTICES];
    size_t notice_count;
    /* A source's: the destinations it has been told sleep, and their wakes (tern_forward_wake()).
     */
    struct tern_forward_wake {
        uint32_t destination;
        uint16_t wake;
    } wakes[TERN_FORWARD_WAKES];
    /* Whether the last frame for this node that tern_forward_heard() took was passed on by a
     * relay, with fewer hops than hop_max: an acknowledgement sent in answer to it goes the way
     * tern_route_back() says. If not, it came from its source, and what it was sent at and heard
     * at are a worse sample of the source (tern_route_worse()), taken when it is acknowledged. */
    bool relayed;
    int8_t heard_power;
    int16_t heard_snr_q;
    /* When the last traces of each kind were answered: a second, kept to 16 bits, plus one; 0 for
     * none. */
    uint16_t answered_at[TERN_TRACE_BY_COUNT][TERN_TRACE_ANSWERS];
};

/* Starts a forwarder over a router. `seed` is for jitter and need not be secret. The slots need
 * not be cleared; no more than TERN_FORWARD_SLOTS_MAX of them are used, a handle being a byte. */
void tern_forward_init(struct tern_forward *f, const struct tern_forward_config *config,
                       struct tern_route *route, struct tern_forward_slot *slots, size_t cap,
                       uint64_t seed);

/* Holds the forwarder to an airtime budget, which the caller keeps, or with NULL to none, as it
 * starts. A frame is then offered only once the budget lets it go (tern_airtime_pays()): one of the
 * node's own waits for it, holding up those of its own behind it and nothing else, and one passed
 * on that the budget will not pay for is dropped, as a frame not passed on is. Each is charged to
 * the budget as tern_forward_poll() hands it over, and given back by tern_forward_withdrawn(). */
void tern_forward_budget(struct tern_forward *f, struct tern_airtime *budget);

/* Has a relay keep sleeping leaves (the specification's "Leaves that sleep"): frames for a leaf
 * that names it its keeper are held in these while it sleeps, at most `cap` of them for every
 * leaf together, and handed over once it polls; the node before is answered with a held notice.
 * The specification asks for room for at least TERN_SLEEP_HOLD_LEAF frames for each leaf kept.
 * Without it, as it starts, a relay holds nothing: frames for a sleeping leaf go to it as to any,
 * and are lost while it sleeps. */
void tern_forward_keeper(struct tern_forward *f, struct tern_forward_kept *store, size_t cap);

/* A source is told, by the destination's sleep content (tern/sleep.h), how long it sleeps: its
 * messages to it are waited for that much longer. 0 for a destination that no longer sleeps. */
void tern_forward_wake(struct tern_forward *f, uint32_t destination, uint16_t wake);

/* For a leaf that sleeps (tern_route_sleeping()): whether its radio may be off now, nothing being
 * listened for, and if so the time it must next be on, written to wake_at. False for a node that
 * does not sleep, and while it listens: after a poll, for a hop of its own, or while it has a
 * frame handed over to the radio not yet sent. */
bool tern_forward_may_sleep(const struct tern_forward *f, tern_time now, tern_time *wake_at);

/* Sends a frame of this node's to `destination`. `frame` is whole but for its head, of which only
 * hdr is read: the rest is filled in here. `tracked` is for a message, or a first-contact frame
 * that is to be answered: it is kept, and sent again until tern_forward_acked() or
 * tern_forward_done(), or it is given up on, which tern_forward_failed() or
 * tern_forward_contact_failed() tells. `back` is,
 * for a frame that answers one received - an acknowledgement, or any of a handshake's frames but
 * the first - what tern_forward_heard() gave, so that the node it came from hears the answer;
 * INT8_MIN otherwise. An acknowledgement sent with `back`, in answer to the message
 * tern_forward_heard() last took for this node, goes by tern_route_back(): if that message was
 * passed on by a relay, not direct to the node its source routed round, and with no route, by a
 * relay's route all the same. If it came direct from its source, it is a worse sample of the
 * source (tern_route_worse()): the caller acknowledges only a message it accepts, or a copy. An
 * acknowledgement or first-contact answer to a frame that came direct, with no route, goes straight
 * back to the node it answers, and a route is asked for.
 *
 * Returns false if there is no slot for it, or it is not tracked and there is no route. A tracked
 * frame with no route is kept while the router asks for one. */
bool tern_forward_send(struct tern_forward *f, tern_time now, uint32_t destination,
                       const uint8_t *frame, size_t len, bool tracked, int8_t back);

enum tern_forward_got {
    TERN_FORWARD_NOTHING, /* not for this node, or passed on, or discarded */
    TERN_FORWARD_MESSAGE, /* a message for this node: to open, and to acknowledge */
    TERN_FORWARD_ACK,     /* an acknowledgement for this node: to check */
    TERN_FORWARD_CONTACT, /* a first-contact frame for this node: for tern/contact.h */
    TERN_FORWARD_TRACE,   /* a trace for this node: tern_forward_trace_answer(), or drop it */
    TERN_FORWARD_TRACED,  /* the answer to a trace of this node's, which it ends */
};

struct tern_forward_heard {
    enum tern_forward_got got;
    int8_t back; /* for the answer's tern_forward_send() */
};

/* A frame of this layer's was received, at this signal-to-noise ratio in quarters of a decibel.
 * A relay answers, itself, a trace for a sleeping leaf it keeps, and one it has no route on for. */
void tern_forward_heard(struct tern_forward *f, tern_time now, const uint8_t *frame, size_t len,
                        int16_t snr_q, struct tern_forward_heard *out);

/* The acknowledgement for the message with this tag came, and was checked. False if no such
 * message is waiting. */
bool tern_forward_acked(struct tern_forward *f, const uint8_t tag[TERN_FORWARD_TAG]);

/* A message given up on, if there is one: its tag is written, and its slot freed. */
bool tern_forward_failed(struct tern_forward *f, uint8_t tag[TERN_FORWARD_TAG]);

/* A frame of this node's with this hdr and tag is no longer to be kept: a first-contact frame
 * whose answer came, or whose handshake is over. False if no such frame is kept. */
bool tern_forward_done(struct tern_forward *f, uint8_t hdr, const uint8_t tag[TERN_FORWARD_TAG]);

/* A first-contact frame given up on, if there is one: its hdr and tag are written, and its slot
 * freed. */
bool tern_forward_contact_failed(struct tern_forward *f, uint8_t *hdr,
                                 uint8_t tag[TERN_FORWARD_TAG]);

/* Begins a trace to `destination`, with `tag`: kept, as a message is, until its answer comes
 * (TERN_FORWARD_TRACED) or it is given up (tern_forward_trace_failed()). It is not begun again.
 * False with no slot, or a trace to that destination already held (tern_forward_tracing()). */
bool tern_forward_trace(struct tern_forward *f, tern_time now, uint32_t destination,
                        const uint8_t tag[TERN_FORWARD_TAG]);
bool tern_forward_tracing(const struct tern_forward *f, uint32_t destination);

/* A trace given up, if there is one: its tag and destination are written, `began` is whether it
 * went on the air (with no route found, it did not), and its slot is freed. */
bool tern_forward_trace_failed(struct tern_forward *f, uint8_t tag[TERN_FORWARD_TAG],
                               uint32_t *destination, bool *began);

/* Answers a trace received at `snr_q`, as `flags` says who this node is to it, `back` as
 * tern_forward_heard() gave. False where it is not: TERN_TRACE_ANSWERS of that kind went in the
 * last minute, the trace holds as many entries as it may, or there is no route to its source. */
bool tern_forward_trace_answer(struct tern_forward *f, tern_time now, const uint8_t *trace,
                               size_t len, int16_t snr_q, uint8_t flags, int8_t back);

/* When tern_forward_poll() next has something to do, or INT64_MAX. */
tern_time tern_forward_due(const struct tern_forward *f);

/* The node's own frames waiting to go, in the order they will: the airtime of each and its hdr,
 * for tern_airtime_waits(), and the slot it is in (a handle, as tern_forward_poll() gives). At most
 * `n` are written; the number written is returned. Acknowledgements are not among them, since
 * nothing holds them. */
size_t tern_forward_own_waiting(const struct tern_forward *f, struct tern_airtime_waiting *waiting,
                                uint8_t *handles, size_t n);

enum tern_forward_kind {
    TERN_FORWARD_OWN,   /* this node's message, or first-contact frame */
    TERN_FORWARD_RELAY, /* another's, passed on */
    TERN_FORWARD_REPLY, /* an acknowledgement, this node's or another's */
};

/* Does what is due at `now`. Returns the length of a frame to send, written to `frame`, the power
 * to send it at, what it is and a handle to name it by, or 0 when there is nothing to send for
 * now. Acknowledgements come first, then frames passed on, then this node's own; a caller that
 * queues frames should keep that order. Every frame returned is the caller's until it says what
 * became of it, with tern_forward_sent() or tern_forward_withdrawn(). */
size_t tern_forward_poll(struct tern_forward *f, tern_time now,
                         uint8_t frame[TERN_FORWARD_FRAME_MAX], int8_t *power_dbm,
                         enum tern_forward_kind *kind, uint8_t *handle);

/* Whether a frame the caller still holds is worth sending: its next hop may have been heard
 * passing on an earlier copy since. A caller that can take a frame back before it goes may, and
 * then says so with tern_forward_withdrawn() in place of tern_forward_sent(). */
bool tern_forward_wanted(const struct tern_forward *f, uint8_t handle);
void tern_forward_withdrawn(struct tern_forward *f, tern_time now, uint8_t handle);

/* A frame tern_forward_poll() returned has gone on the air. */
void tern_forward_sent(struct tern_forward *f, tern_time now, uint8_t handle);

#endif
