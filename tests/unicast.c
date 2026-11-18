#include "tern/unicast.h"

#include <stdbool.h>
#include <string.h>

#include "check.h"
#include "tern/err.h"
#include "tern/forward.h"

/* Secured unicast frames against the specification's test vectors (ternmesh/spec,
 * vectors/unicast-security.json, copied to tests/vectors/), through the public functions only:
 * what a sender is given and what a receiver is shown are exactly what the specification's
 * Conformance section lists. The structs below are the shape the generated header fills in. */

struct accepted_case {
    const char *name;
    uint8_t secret[32];
    uint8_t dir;
    uint32_t counter;
    uint8_t hdr;
    uint8_t hops;
    int8_t power;
    uint32_t next, destination;
    const uint8_t *pt;
    size_t pt_len;
    const uint8_t *frame;
    size_t frame_len;
    uint8_t epoch_key[32];
    uint8_t proof[4];
};

struct rejected_case {
    const char *name;
    uint8_t secret[32];
    uint8_t dir;
    uint32_t counter;
    const uint8_t *frame;
    size_t frame_len;
};

struct delivery {
    const uint8_t *frame;
    size_t frame_len;
    bool accept;
    uint32_t counter;
    const uint8_t *pt;
    size_t pt_len;
    bool acknowledge;
    uint8_t proof[4];
    bool restart; /* the receiver restarts first, keeping what it stored */
    uint32_t at;  /* seconds */
};

struct sequence_case {
    const char *name;
    uint8_t secret[32];
    uint8_t dir;
    const struct delivery *deliveries;
    size_t count;
};

struct collision_session {
    uint8_t secret[32];
    uint8_t dir;
};

struct collision_ack {
    size_t session;
    uint32_t counter;
    uint8_t proof[4];
};

struct collision_delivery {
    const uint8_t *frame;
    size_t frame_len;
    bool accept;
    size_t session;
    uint32_t counter;
    const uint8_t *pt;
    size_t pt_len;
    const struct collision_ack *acks;
    size_t ack_count;
    uint32_t at;
};

struct collision_case {
    const char *name;
    const struct collision_session *sessions;
    size_t session_count;
    const struct collision_delivery *deliveries;
    size_t count;
};

enum then { THEN_NOTHING, THEN_ACKNOWLEDGED, THEN_GIVEN_UP };

struct send {
    bool restart; /* both ends restart first */
    uint8_t dir;
    const uint8_t *pt;
    size_t pt_len;
    uint32_t counter;
    const uint8_t *frame; /* NULL: the sender must start again instead */
    size_t frame_len;
    enum then then;
};

struct sender_case {
    const char *name;
    uint8_t secret[32];
    const struct send *sends;
    size_t count;
};

struct lifetime_case {
    uint32_t made, at;
    bool sends, holds;
};

struct ack_case {
    const char *name;
    uint8_t secret[32];
    uint8_t dir;
    uint32_t counter;
    const uint8_t *frame;
    size_t frame_len;
    bool valid;
};

#include "unicast_security.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* The vectors give the direction a frame travels in. Its sender is the node of that role, and its
 * receiver the other. */
static enum tern_role sender_of(uint8_t dir) { return (enum tern_role)dir; }
static enum tern_role receiver_of(uint8_t dir) {
    return dir == TERN_INITIATOR ? TERN_RESPONDER : TERN_INITIATOR;
}

static void start(struct tern_session *s, const uint8_t secret[32], enum tern_role role) {
    uint8_t copy[32];
    memcpy(copy, secret, sizeof copy);
    tern_session_init(s, copy, role);
}

static bool bytes_eq(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len) {
    return a_len == b_len && (a_len == 0 || memcmp(a, b, a_len) == 0);
}

/* The forwarder's ten bytes, as the vectors' description gives them where a case does not. */
static void route(uint8_t *frame, uint8_t hops, int8_t power, uint32_t next, uint32_t destination) {
    struct tern_forward_head h = {frame[0], hops, power, next, destination};
    tern_forward_head_write(&h, frame);
}

static void route_default(uint8_t *frame) { route(frame, 32, 14, 0x0A0B0C0Du, 0x01020304u); }

static struct tern_unicast_received receive_at(struct tern_unicast_rx *rx, const uint8_t *frame,
                                               size_t len, uint8_t *pt, tern_time now) {
    struct tern_unicast_received r;
    struct tern_unicast_rx *one[] = {rx};
    CHECK(tern_unicast_open(one, 1, frame, len, pt, TERN_UNICAST_MAX_PLAINTEXT, now, &r) ==
          TERN_OK);
    return r;
}

static struct tern_unicast_received receive(struct tern_unicast_rx *rx, const uint8_t *frame,
                                            size_t len, uint8_t *pt) {
    return receive_at(rx, frame, len, pt, 0);
}

/* A restart: what is kept is what was packed. */
static void restart(struct tern_session *s) {
    uint8_t packed[TERN_SESSION_PACKED];
    tern_session_pack(s, packed);
    CHECK(tern_session_unpack(s, packed));
}

/* A frame just accepted is acknowledged, and its sender takes the acknowledgement, as in a session
 * in use: a sender never acknowledged starts again after 32 messages. */
static void acknowledge(struct tern_session *to, struct tern_session *from, uint32_t counter) {
    uint8_t ack[TERN_UNICAST_ACK_LEN];
    CHECK(tern_unicast_ack(&to->rx, counter, ack));
    CHECK(tern_unicast_acked(&from->tx, counter, ack, sizeof ack));
}

/* Sends an empty message from one end to the other, and has it taken and acknowledged. */
static void pass(struct tern_session *from, struct tern_session *to) {
    uint8_t frame[TERN_UNICAST_MAX_FRAME], pt[TERN_UNICAST_MAX_PLAINTEXT];
    CHECK(tern_unicast_seal(&from->tx, NULL, 0, frame, sizeof frame) == TERN_OK);
    struct tern_unicast_received r = receive(&to->rx, frame, TERN_UNICAST_OVERHEAD, pt);
    CHECK(r.verdict == TERN_UNICAST_ACCEPTED);
    acknowledge(to, from, r.counter);
}

/* accepted: the sender, reaching the case's counter by sending that many messages first, makes
 * exactly the frame; the receiver, having been shown those messages, accepts it. */
static void accepted_cases(void) {
    for (size_t i = 0; i < COUNT(accepted); i++) {
        const struct accepted_case *c = &accepted[i];
        struct tern_session tx, rx;
        uint8_t frame[TERN_UNICAST_MAX_FRAME], pt[TERN_UNICAST_MAX_PLAINTEXT];
        start(&tx, c->secret, sender_of(c->dir));
        start(&rx, c->secret, receiver_of(c->dir));

        for (uint32_t n = 0; n < c->counter; n++) {
            pass(&tx, &rx);
        }
        /* The sender holds the key of the epoch it is in, and no other. */
        CHECK(memcmp(tx.tx.epoch_key, c->epoch_key, 32) == 0);

        CHECK((c->hdr == TERN_UNICAST_HDR_NODE ? tern_unicast_seal_node : tern_unicast_seal)(
                  &tx.tx, c->pt, c->pt_len, frame, sizeof frame) == TERN_OK);
        CHECK(tern_unicast_for_node(frame) == (c->hdr == TERN_UNICAST_HDR_NODE));
        /* Sealed, the forwarder's bytes are zero. */
        static const uint8_t zeros[TERN_UNICAST_HEAD - 1] = {0};
        CHECK(memcmp(&frame[1], zeros, sizeof zeros) == 0);
        route(frame, c->hops, c->power, c->next, c->destination);
        if (!bytes_eq(frame, c->pt_len + TERN_UNICAST_OVERHEAD, c->frame, c->frame_len)) {
            fprintf(stderr, "accepted %s: frame differs\n", c->name);
            check_failures++;
        }

        struct tern_unicast_received r = receive(&rx.rx, c->frame, c->frame_len, pt);
        if (r.verdict != TERN_UNICAST_ACCEPTED || r.counter != c->counter ||
            !bytes_eq(pt, r.len, c->pt, c->pt_len)) {
            fprintf(stderr, "accepted %s: not received as sent\n", c->name);
            check_failures++;
        }

        /* Its acknowledgement carries the frame's tag and the case's proof, and its sender takes
         * it. */
        uint8_t ack[TERN_UNICAST_ACK_LEN];
        CHECK(tern_unicast_ack(&rx.rx, c->counter, ack));
        if (ack[0] != TERN_UNICAST_ACK_HDR || memcmp(&ack[1], zeros, sizeof zeros) != 0 ||
            memcmp(&ack[11], &c->frame[11], 4) != 0 || memcmp(&ack[15], c->proof, 4) != 0) {
            fprintf(stderr, "accepted %s: acknowledgement differs\n", c->name);
            check_failures++;
        }
        CHECK(tern_unicast_acked(&tx.tx, c->counter, ack, sizeof ack));
    }
}

static void rejected_cases(void) {
    for (size_t i = 0; i < COUNT(rejected); i++) {
        const struct rejected_case *c = &rejected[i];
        struct tern_session tx, rx;
        uint8_t pt[TERN_UNICAST_MAX_PLAINTEXT];
        start(&tx, c->secret, sender_of(c->dir));
        start(&rx, c->secret, receiver_of(c->dir));
        /* Expecting the case's counter: shown every one before it. */
        for (uint32_t n = 0; n < c->counter; n++) {
            pass(&tx, &rx);
        }
        bool started = rx.rx.started;
        uint32_t high = rx.rx.high, seen = rx.rx.seen;
        struct tern_unicast_received r = receive(&rx.rx, c->frame, c->frame_len, pt);
        if (r.verdict == TERN_UNICAST_ACCEPTED) {
            fprintf(stderr, "rejected %s: accepted\n", c->name);
            check_failures++;
        }
        /* Nothing it was shown moves the window: the frame the case was made from still goes. */
        CHECK(rx.rx.started == started && rx.rx.high == high && rx.rx.seen == seen);
    }
}

static void sequence_cases(void) {
    for (size_t i = 0; i < COUNT(sequences); i++) {
        const struct sequence_case *c = &sequences[i];
        struct tern_session rx;
        uint8_t pt[TERN_UNICAST_MAX_PLAINTEXT];
        start(&rx, c->secret, receiver_of(c->dir));
        for (size_t j = 0; j < c->count; j++) {
            const struct delivery *d = &c->deliveries[j];
            if (d->restart) {
                restart(&rx);
            }
            struct tern_unicast_received r =
                receive_at(&rx.rx, d->frame, d->frame_len, pt, TERN_S(d->at));
            bool ok = d->accept ? r.verdict == TERN_UNICAST_ACCEPTED && r.counter == d->counter &&
                                      bytes_eq(pt, r.len, d->pt, d->pt_len)
                                : r.verdict != TERN_UNICAST_ACCEPTED;
            if (!ok) {
                fprintf(stderr, "sequence %s: delivery %zu\n", c->name, j);
                check_failures++;
            }
            /* Acknowledged: accepted, or a copy of one that was and is not too old. */
            uint8_t ack[TERN_UNICAST_ACK_LEN];
            bool owed = (r.verdict == TERN_UNICAST_ACCEPTED || r.verdict == TERN_UNICAST_COPY) &&
                        tern_unicast_ack(&rx.rx, r.counter, ack);
            if (owed != d->acknowledge || (owed && (memcmp(&ack[11], &d->frame[11], 4) != 0 ||
                                                    memcmp(&ack[15], d->proof, 4) != 0))) {
                fprintf(stderr, "sequence %s: delivery %zu acknowledged wrongly\n", c->name, j);
                check_failures++;
            }
            CHECK((r.verdict == TERN_UNICAST_COPY) == (d->acknowledge && !d->accept));
        }
    }
}

static void collision_cases(void) {
    for (size_t i = 0; i < COUNT(collisions); i++) {
        const struct collision_case *c = &collisions[i];
        struct tern_session s[2];
        struct tern_unicast_rx *rx[2];
        uint8_t pt[TERN_UNICAST_MAX_PLAINTEXT];
        CHECK(c->session_count == 2);
        for (size_t k = 0; k < 2; k++) {
            start(&s[k], c->sessions[k].secret, receiver_of(c->sessions[k].dir));
            rx[k] = &s[k].rx;
        }
        for (size_t j = 0; j < c->count; j++) {
            const struct collision_delivery *d = &c->deliveries[j];
            struct tern_unicast_received r;
            tern_time now = TERN_S(d->at);
            CHECK(tern_unicast_open(rx, 2, d->frame, d->frame_len, pt, sizeof pt, now, &r) ==
                  TERN_OK);
            bool ok = d->accept
                          ? r.verdict == TERN_UNICAST_ACCEPTED && r.session == d->session &&
                                r.counter == d->counter && bytes_eq(pt, r.len, d->pt, d->pt_len)
                          : r.verdict == TERN_UNICAST_COPY;
            if (!ok) {
                fprintf(stderr, "collision %s: delivery %zu\n", c->name, j);
                check_failures++;
                continue;
            }
            /* The acknowledgements it earns: its own if accepted, and if a copy, one for every
             * message it is a copy of. The vectors list them in the order they are found. */
            size_t n = 0;
            do {
                uint8_t ack[TERN_UNICAST_ACK_LEN];
                const struct collision_ack *a = &d->acks[n < d->ack_count ? n : 0];
                if (n >= d->ack_count || r.session != a->session || r.counter != a->counter ||
                    !tern_unicast_ack(rx[r.session], r.counter, ack) ||
                    memcmp(&ack[11], &d->frame[11], 4) != 0 || memcmp(&ack[15], a->proof, 4) != 0) {
                    fprintf(stderr, "collision %s: delivery %zu, acknowledgement %zu\n", c->name, j,
                            n);
                    check_failures++;
                }
                n++;
            } while (tern_unicast_copy_next(rx, 2, d->frame, d->frame_len, now, &r));
            CHECK_EQ_U64(n, d->ack_count);
        }
    }
}

static void sender_cases(void) {
    for (size_t i = 0; i < COUNT(senders); i++) {
        const struct sender_case *c = &senders[i];
        struct tern_session node[3]; /* indexed by role */
        uint8_t frame[TERN_UNICAST_MAX_FRAME];
        uint8_t pt[TERN_UNICAST_MAX_PLAINTEXT];
        start(&node[TERN_INITIATOR], c->secret, TERN_INITIATOR);
        start(&node[TERN_RESPONDER], c->secret, TERN_RESPONDER);
        for (size_t j = 0; j < c->count; j++) {
            const struct send *d = &c->sends[j];
            if (d->restart) {
                restart(&node[TERN_INITIATOR]);
                restart(&node[TERN_RESPONDER]);
            }
            struct tern_session *from = &node[sender_of(d->dir)], *to = &node[receiver_of(d->dir)];
            if (d->frame == NULL) {
                /* It starts again, and uses no counter. */
                uint32_t next = from->tx.next;
                CHECK(tern_unicast_restart(&from->tx));
                CHECK(tern_unicast_seal(&from->tx, d->pt, d->pt_len, frame, sizeof frame) ==
                      TERN_ESPENT);
                CHECK_EQ_U64(from->tx.next, next);
                continue;
            }
            CHECK(!tern_unicast_restart(&from->tx));
            CHECK_EQ_U64(from->tx.next, d->counter);
            CHECK(tern_unicast_seal(&from->tx, d->pt, d->pt_len, frame, sizeof frame) == TERN_OK);
            route_default(frame);
            if (!bytes_eq(frame, d->pt_len + TERN_UNICAST_OVERHEAD, d->frame, d->frame_len)) {
                fprintf(stderr, "sender %s: send %zu differs\n", c->name, j);
                check_failures++;
            }
            if (d->then == THEN_ACKNOWLEDGED) {
                CHECK(receive(&to->rx, frame, d->frame_len, pt).verdict == TERN_UNICAST_ACCEPTED);
                acknowledge(to, from, d->counter);
            } else if (d->then == THEN_GIVEN_UP) {
                tern_unicast_given_up(&from->tx);
            }
        }
    }
}

/* acknowledgements: the node that sent the case's message, having sent those before it, takes the
 * frame as showing that it arrived, or does not. */
static void ack_cases(void) {
    for (size_t i = 0; i < COUNT(acknowledgements); i++) {
        const struct ack_case *c = &acknowledgements[i];
        struct tern_session tx;
        uint8_t frame[TERN_UNICAST_MAX_FRAME];
        struct tern_session rx;
        start(&tx, c->secret, sender_of(c->dir));
        start(&rx, c->secret, receiver_of(c->dir));
        for (uint32_t n = 0; n < c->counter; n++) {
            pass(&tx, &rx);
        }
        CHECK(tern_unicast_seal(&tx.tx, NULL, 0, frame, sizeof frame) == TERN_OK);
        if (tern_unicast_acked(&tx.tx, c->counter, c->frame, c->frame_len) != c->valid) {
            fprintf(stderr, "acknowledgement %s: %s\n", c->name, c->valid ? "not taken" : "taken");
            check_failures++;
        }
    }
}

/* lifetimes: when a session may be sent in, and when it is erased. The clock is the caller's, so
 * what is checked here is the core's two numbers; ports/node/demo.c keeps to them, and
 * tests/demo.c checks that it does. */
static void lifetime_cases(void) {
    for (size_t i = 0; i < COUNT(lifetimes); i++) {
        const struct lifetime_case *c = &lifetimes[i];
        tern_time age = TERN_S(c->at - c->made);
        CHECK((age <= TERN_SESSION_LIFE) == c->sends);
        CHECK((age <= TERN_SESSION_LIFE + TERN_SESSION_GRACE) == c->holds);
    }
}

/* --- Beyond the vectors ----------------------------------------------------------------------- */

static const uint8_t secret[32] = {0x54, 0x65, 0x72, 0x6e}; /* "Tern", then zeros */

static void secret_is_erased(void) {
    uint8_t s[32];
    struct tern_session session;
    memcpy(s, secret, sizeof s);
    tern_session_init(&session, s, TERN_INITIATOR);
    static const uint8_t zero[32] = {0};
    CHECK(memcmp(s, zero, sizeof s) == 0);
}

static void round_trips_every_length(void) {
    struct tern_session a, b;
    uint8_t msg[TERN_UNICAST_MAX_PLAINTEXT], frame[TERN_UNICAST_MAX_FRAME];
    uint8_t pt[TERN_UNICAST_MAX_PLAINTEXT];
    start(&a, secret, TERN_INITIATOR);
    start(&b, secret, TERN_RESPONDER);
    for (size_t len = 0; len <= TERN_UNICAST_MAX_PLAINTEXT; len++) {
        for (size_t k = 0; k < len; k++) {
            msg[k] = (uint8_t)(len * 7 + k);
        }
        /* Alternate directions, so each counts on its own. */
        struct tern_session *from = len % 2 ? &a : &b, *to = len % 2 ? &b : &a;
        CHECK(tern_unicast_seal(&from->tx, msg, len, frame, sizeof frame) == TERN_OK);
        struct tern_unicast_received r = receive(&to->rx, frame, len + TERN_UNICAST_OVERHEAD, pt);
        CHECK(r.verdict == TERN_UNICAST_ACCEPTED);
        acknowledge(to, from, r.counter);
        CHECK(bytes_eq(pt, r.len, msg, len));
        CHECK_EQ_U64(r.counter, len / 2);
    }
}

static void seal_checks_its_arguments(void) {
    struct tern_session a;
    uint8_t msg[TERN_UNICAST_MAX_PLAINTEXT + 1] = {0}, frame[TERN_UNICAST_MAX_FRAME + 1];
    start(&a, secret, TERN_INITIATOR);
    CHECK(tern_unicast_seal(&a.tx, msg, sizeof msg, frame, sizeof frame) == TERN_EINVAL);
    CHECK(tern_unicast_seal(&a.tx, msg, 10, frame, 32) == TERN_EINVAL);
    CHECK(tern_unicast_seal(&a.tx, msg, 10, NULL, 33) == TERN_EINVAL);
    CHECK(tern_unicast_seal(&a.tx, NULL, 10, frame, 33) == TERN_EINVAL);
    CHECK_EQ_U64(a.tx.next, 0); /* a refused message uses no counter */
    CHECK(tern_unicast_seal(&a.tx, msg, 10, frame, 33) == TERN_OK);
    CHECK_EQ_U64(a.tx.next, 1);
}

static void last_counter_spends_the_session(void) {
    struct tern_session a;
    uint8_t frame[TERN_UNICAST_MAX_FRAME];
    start(&a, secret, TERN_INITIATOR);
    /* Reaching 2^32 - 1 honestly would take 2^32 messages, so the test moves the counter itself.
     * The frame it sends is wrong (the epoch key is epoch 0's), but this is about what follows. */
    a.tx.next = UINT32_MAX;
    a.tx.acked_any = true; /* and its peer has acknowledged every message before */
    a.tx.acked = UINT32_MAX - 1;
    CHECK(tern_unicast_seal(&a.tx, NULL, 0, frame, sizeof frame) == TERN_OK);
    CHECK(a.tx.spent);
    CHECK(tern_unicast_seal(&a.tx, NULL, 0, frame, sizeof frame) == TERN_ESPENT);
    static const uint8_t zero[32] = {0};
    CHECK(memcmp(a.tx.epoch_key, zero, 32) == 0);
}

static void open_sorts_what_it_is_shown(void) {
    struct tern_session a, b;
    struct tern_unicast_rx *rx[] = {&b.rx};
    uint8_t frame[TERN_UNICAST_MAX_FRAME], pt[TERN_UNICAST_MAX_PLAINTEXT];
    struct tern_unicast_received r;
    start(&a, secret, TERN_INITIATOR);
    start(&b, secret, TERN_RESPONDER);
    CHECK(tern_unicast_seal(&a.tx, (const uint8_t *)"hi", 2, frame, sizeof frame) == TERN_OK);

    /* Too short, and an unknown header, are malformed. */
    CHECK(tern_unicast_open(rx, 1, frame, 22, pt, sizeof pt, 0, &r) == TERN_OK);
    CHECK(r.verdict == TERN_UNICAST_MALFORMED);
    frame[0] ^= 0x02;
    CHECK(tern_unicast_open(rx, 1, frame, 25, pt, sizeof pt, 0, &r) == TERN_OK);
    CHECK(r.verdict == TERN_UNICAST_MALFORMED);
    frame[0] ^= 0x02;
    /* The node flag is a header this code knows, and authenticated: set on a frame sealed
     * without it, the frame's tag matches and its check does not. */
    frame[0] ^= 0x01;
    CHECK(tern_unicast_open(rx, 1, frame, 25, pt, sizeof pt, 0, &r) == TERN_OK);
    CHECK(r.verdict == TERN_UNICAST_FORGED);
    frame[0] ^= 0x01;

    /* A tag in no window is someone else's. */
    frame[11] ^= 0x80;
    CHECK(tern_unicast_open(rx, 1, frame, 25, pt, sizeof pt, 0, &r) == TERN_OK);
    CHECK(r.verdict == TERN_UNICAST_NOT_OURS);
    frame[11] ^= 0x80;

    /* A known tag on a frame that does not authenticate is a forgery, and leaves no plaintext. */
    frame[24] ^= 0x01;
    memset(pt, 0xAA, sizeof pt);
    CHECK(tern_unicast_open(rx, 1, frame, 25, pt, sizeof pt, 0, &r) == TERN_OK);
    CHECK(r.verdict == TERN_UNICAST_FORGED);
    CHECK(pt[0] == 0 && pt[1] == 0);
    /* And earns no acknowledgement: nothing has been accepted. */
    uint8_t ack[TERN_UNICAST_ACK_LEN];
    CHECK(!tern_unicast_ack(&b.rx, 0, ack));
    frame[24] ^= 0x01;

    /* Too little room for the plaintext is the caller's mistake, not the frame's. */
    CHECK(tern_unicast_open(rx, 1, frame, 25, pt, 1, 0, &r) == TERN_EINVAL);

    /* The ten bytes after hdr are the forwarder's: changing them changes nothing. */
    for (int i = 1; i < TERN_UNICAST_HEAD; i++) {
        frame[i] = (uint8_t)(0xA0 + i);
    }
    CHECK(tern_unicast_open(rx, 1, frame, 25, pt, sizeof pt, 0, &r) == TERN_OK);
    CHECK(r.verdict == TERN_UNICAST_ACCEPTED);
    CHECK(pt[0] == 'h' && pt[1] == 'i');

    /* Shown again, it is a copy: not opened, and owed the same acknowledgement, whatever follows
     * the first four bytes of its AEAD tag. */
    uint8_t first[TERN_UNICAST_ACK_LEN], again[TERN_UNICAST_ACK_LEN];
    CHECK(tern_unicast_ack(&b.rx, 0, first));
    memset(pt, 0xAA, sizeof pt);
    frame[24] ^= 0x01;
    CHECK(tern_unicast_open(rx, 1, frame, 25, pt, sizeof pt, 0, &r) == TERN_OK);
    CHECK(r.verdict == TERN_UNICAST_COPY && r.session == 0 && r.counter == 0 && r.len == 0);
    CHECK(pt[0] == 0xAA); /* nothing of it was opened */
    CHECK(tern_unicast_ack(&b.rx, 0, again) && memcmp(first, again, sizeof first) == 0);
    CHECK(tern_unicast_acked(&a.tx, 0, first, sizeof first));
    /* The sender has sent one message: an acknowledgement of a second is not one. */
    CHECK(!tern_unicast_acked(&a.tx, 1, first, sizeof first));
    CHECK(!tern_unicast_ack(&b.rx, 1, again));
}

/* The receiver keeps the epoch keys its window needs and erases the rest (Receiving, step 6). */
static void receiver_keeps_only_needed_epochs(void) {
    struct tern_session a, b;
    uint8_t frame[TERN_UNICAST_MAX_FRAME], pt[TERN_UNICAST_MAX_PLAINTEXT];
    start(&a, secret, TERN_INITIATOR);
    start(&b, secret, TERN_RESPONDER);

    CHECK_EQ_U64(b.rx.first_epoch, 0);
    CHECK_EQ_U64(b.rx.epochs, 1);

    for (uint32_t n = 0; n < 200; n++) {
        pass(&a, &b);

        /* Every counter up to n is in, so the window's lowest is n + 1, and it reaches n + 32. */
        uint32_t first = (n + 1) / 32, last = (n + 32) / 32;
        CHECK_EQ_U64(b.rx.first_epoch, first);
        CHECK_EQ_U64(b.rx.epochs, last - first + 1);
        /* And what it holds is what tern_session_unpack() takes as a session's. */
        uint8_t packed[TERN_SESSION_PACKED];
        static struct tern_session again;
        tern_session_pack(&b, packed);
        CHECK(tern_session_unpack(&again, packed));
    }

    /* Receiving 31, then nothing below it: epoch 0 must stay, for counters 0 to 30. */
    struct tern_session c, d;
    start(&c, secret, TERN_INITIATOR);
    start(&d, secret, TERN_RESPONDER);
    for (uint32_t n = 0; n <= 31; n++) {
        CHECK(tern_unicast_seal(&c.tx, NULL, 0, frame, sizeof frame) == TERN_OK);
    }
    CHECK(receive(&d.rx, frame, TERN_UNICAST_OVERHEAD, pt).verdict == TERN_UNICAST_ACCEPTED);
    CHECK_EQ_U64(d.rx.first_epoch, 0);
    CHECK_EQ_U64(d.rx.epochs, 2); /* the window is 0 to 63 */
}

/* A session packed and unpacked goes on as the one packed would have, in both directions, and
 * packs again to the same bytes: at the start, and with a window that has gaps, copies and three
 * epochs in it. */
#define END (1 + 57 + 124) /* where the packed epoch keys end; the age follows */
static void packed_session_goes_on(void) {
    static struct tern_session a, b, a2, b2;
    static uint8_t frames[120][TERN_UNICAST_OVERHEAD + 1];
    uint8_t pt[TERN_UNICAST_MAX_PLAINTEXT], packed[TERN_SESSION_PACKED], again[TERN_SESSION_PACKED];
    start(&a, secret, TERN_INITIATOR);
    start(&b, secret, TERN_RESPONDER);

    for (int round = 0; round < 2; round++) {
        if (round == 1) {
            /* Counters 0 to 119 sent; every third of 60 to 119 not yet received, so the window
             * reaches back past gaps and across epochs. */
            for (uint32_t n = 0; n < 120; n++) {
                uint8_t m = (uint8_t)n;
                CHECK(tern_unicast_seal(&a.tx, &m, 1, frames[n], sizeof frames[n]) == TERN_OK);
                if (n < 60 || n % 3 != 0) {
                    struct tern_unicast_received r =
                        receive(&b.rx, frames[n], sizeof frames[n], pt);
                    CHECK(r.verdict == TERN_UNICAST_ACCEPTED);
                    acknowledge(&b, &a, r.counter);
                }
            }
            CHECK(b.rx.epochs > 1);
        }
        tern_session_pack(&a, packed);
        CHECK(tern_session_unpack(&a2, packed));
        tern_session_pack(&a2, again);
        CHECK(memcmp(packed, again, sizeof packed) == 0);
        tern_session_pack(&b, packed);
        CHECK(tern_session_unpack(&b2, packed));
        tern_session_pack(&b2, again);
        CHECK(memcmp(packed, again, sizeof packed) == 0);

        /* The sender seals the same next frame. */
        uint8_t f1[TERN_UNICAST_OVERHEAD + 2], f2[TERN_UNICAST_OVERHEAD + 2];
        CHECK(tern_unicast_seal(&a.tx, (const uint8_t *)"go", 2, f1, sizeof f1) == TERN_OK);
        CHECK(tern_unicast_seal(&a2.tx, (const uint8_t *)"go", 2, f2, sizeof f2) == TERN_OK);
        CHECK(memcmp(f1, f2, sizeof f1) == 0);

        /* The receiver takes it, takes the gaps late, knows a copy, and acknowledges the same. */
        struct tern_unicast_received r1 = receive(&b.rx, f1, sizeof f1, pt);
        struct tern_unicast_received r2 = receive(&b2.rx, f1, sizeof f1, pt);
        CHECK(r1.verdict == TERN_UNICAST_ACCEPTED && r2.verdict == TERN_UNICAST_ACCEPTED);
        CHECK_EQ_U64(r2.counter, r1.counter);
        /* Counters below 89 have left the window (H is 120): those are someone else's. What
         * answers copies is not packed, so the session unpacked answers none of those it took
         * before, and takes the same frames late. */
        unsigned late = 0, copies = 0, unanswered = 0;
        for (uint32_t n = 60; round == 1 && n < 120; n++) {
            struct tern_unicast_received g1 = receive(&b.rx, frames[n], sizeof frames[n], pt);
            struct tern_unicast_received g2 = receive(&b2.rx, frames[n], sizeof frames[n], pt);
            CHECK((g1.verdict == TERN_UNICAST_ACCEPTED) == (g2.verdict == TERN_UNICAST_ACCEPTED));
            CHECK(g2.verdict != TERN_UNICAST_COPY);
            late += g2.verdict == TERN_UNICAST_ACCEPTED;
            copies += g1.verdict == TERN_UNICAST_COPY;
            unanswered += g1.verdict == TERN_UNICAST_COPY && g2.verdict == TERN_UNICAST_NOT_OURS;
        }
        CHECK(round == 0 || (late == 10 && copies == 21 && unanswered == 21));
        uint8_t k1[TERN_UNICAST_ACK_LEN], k2[TERN_UNICAST_ACK_LEN];
        CHECK(tern_unicast_ack(&b.rx, r1.counter, k1) && tern_unicast_ack(&b2.rx, r1.counter, k2));
        CHECK(memcmp(k1, k2, sizeof k1) == 0);
        CHECK(tern_unicast_acked(&a2.tx, r1.counter, k2, sizeof k2));

        /* And the other way. */
        CHECK(tern_unicast_seal(&b.tx, (const uint8_t *)"ok", 2, f1, sizeof f1) == TERN_OK);
        CHECK(tern_unicast_seal(&b2.tx, (const uint8_t *)"ok", 2, f2, sizeof f2) == TERN_OK);
        CHECK(memcmp(f1, f2, sizeof f1) == 0);
        CHECK(receive(&a2.rx, f2, sizeof f2, pt).verdict == TERN_UNICAST_ACCEPTED);
        CHECK(receive(&a.rx, f2, sizeof f2, pt).verdict == TERN_UNICAST_ACCEPTED);
    }

    /* What is not a packed session is refused, and leaves nothing behind: a form this code does
     * not know, or the first, from before each epoch had its own tag key. */
    static const uint8_t zero[sizeof(struct tern_session)] = {0};
    tern_session_pack(&a, packed);
    packed[0] = 0x03;
    CHECK(!tern_session_unpack(&a2, packed) && memcmp(&a2, zero, sizeof a2) == 0);
    packed[0] = 0x01;
    CHECK(!tern_session_unpack(&a2, packed) && memcmp(&a2, zero, sizeof a2) == 0);
    tern_session_pack(&a, packed);
    packed[1 + 13 + 32 + 4] = TERN_INITIATOR + TERN_RESPONDER; /* the sender's direction */
    CHECK(!tern_session_unpack(&a2, packed) && memcmp(&a2, zero, sizeof a2) == 0);
    tern_session_pack(&a, packed);
    packed[1 + 13 + 32 + 4 + 2] = 2; /* acked_any, which is 0 or 1 */
    CHECK(!tern_session_unpack(&a2, packed) && memcmp(&a2, zero, sizeof a2) == 0);
    tern_session_pack(&a, packed);
    packed[END - 96 - 1] = 4; /* more epochs than a window holds */
    CHECK(!tern_session_unpack(&a2, packed) && memcmp(&a2, zero, sizeof a2) == 0);

    /* Nor one whose epoch keys do not cover every counter the window still waits for: the first
     * epoch past counter 0, still to come, or no keys at all. */
    static struct tern_session fresh;
    start(&fresh, secret, TERN_RESPONDER);
    tern_session_pack(&fresh, packed);
    CHECK(tern_session_unpack(&a2, packed));
    packed[END - 96 - 1 - 4] = 1; /* first_epoch, little-endian */
    CHECK(!tern_session_unpack(&a2, packed) && memcmp(&a2, zero, sizeof a2) == 0);
    tern_session_pack(&fresh, packed);
    packed[END - 96 - 1] = 0;
    CHECK(!tern_session_unpack(&a2, packed) && memcmp(&a2, zero, sizeof a2) == 0);
    tern_session_pack(&fresh, packed);
    packed[END - 96 - 1] = 2; /* a key for epoch 1, which it does not yet need */
    CHECK(!tern_session_unpack(&a2, packed) && memcmp(&a2, zero, sizeof a2) == 0);

    /* Nor one that has not accepted its highest counter, which would take that frame again, nor
     * one that has accepted nothing and says what its highest is. */
    tern_session_pack(&b, packed);
    CHECK(tern_session_unpack(&a2, packed));
    packed[END - 96 - 1 - 4 - 4] &= 0xFE; /* seen's bit 0, little-endian */
    CHECK(!tern_session_unpack(&a2, packed) && memcmp(&a2, zero, sizeof a2) == 0);
    tern_session_pack(&fresh, packed);
    packed[END - 96 - 1 - 4 - 4 - 4] = 5; /* high */
    CHECK(!tern_session_unpack(&a2, packed) && memcmp(&a2, zero, sizeof a2) == 0);
}

int main(void) {
    RUN(accepted_cases);
    RUN(rejected_cases);
    RUN(sequence_cases);
    RUN(collision_cases);
    RUN(sender_cases);
    RUN(ack_cases);
    RUN(lifetime_cases);
    RUN(secret_is_erased);
    RUN(round_trips_every_length);
    RUN(seal_checks_its_arguments);
    RUN(last_counter_spends_the_session);
    RUN(open_sorts_what_it_is_shown);
    RUN(receiver_keeps_only_needed_epochs);
    RUN(packed_session_goes_on);
    return CHECK_DONE();
}
