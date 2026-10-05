#include "tern/unicast.h"

#include <stdbool.h>
#include <string.h>

#include "check.h"
#include "tern/err.h"

/* Secured unicast frames against the specification's test vectors (ternmesh/spec,
 * vectors/unicast-security.json, copied to tests/vectors/), through the public functions only:
 * what a sender is given and what a receiver is shown are exactly what the specification's
 * Conformance section lists. The structs below are the shape the generated header fills in. */

struct accepted_case {
    const char *name;
    uint8_t secret[32];
    uint8_t dir;
    uint32_t counter;
    uint8_t hop;
    uint16_t label;
    const uint8_t *pt;
    size_t pt_len;
    const uint8_t *frame;
    size_t frame_len;
    uint8_t epoch_key[32];
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

struct collision_delivery {
    const uint8_t *frame;
    size_t frame_len;
    size_t session;
    uint32_t counter;
    const uint8_t *pt;
    size_t pt_len;
};

struct collision_case {
    const char *name;
    const struct collision_session *sessions;
    size_t session_count;
    const struct collision_delivery *deliveries;
    size_t count;
};

struct send {
    uint8_t dir;
    const uint8_t *pt;
    size_t pt_len;
    uint32_t counter;
    const uint8_t *frame;
    size_t frame_len;
};

struct sender_case {
    const char *name;
    uint8_t secret[32];
    const struct send *sends;
    size_t count;
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

static struct tern_unicast_received receive(struct tern_unicast_rx *rx, const uint8_t *frame,
                                            size_t len, uint8_t *pt) {
    struct tern_unicast_received r;
    struct tern_unicast_rx *one[] = {rx};
    CHECK(tern_unicast_open(one, 1, frame, len, pt, TERN_UNICAST_MAX_PLAINTEXT, &r) == TERN_OK);
    return r;
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
            CHECK(tern_unicast_seal(&tx.tx, 0, 0, NULL, 0, frame, sizeof frame) == TERN_OK);
            CHECK(receive(&rx.rx, frame, TERN_UNICAST_OVERHEAD, pt).verdict ==
                  TERN_UNICAST_ACCEPTED);
        }
        /* The sender holds the key of the epoch it is in, and no other. */
        CHECK(memcmp(tx.tx.epoch_key, c->epoch_key, 32) == 0);

        CHECK(tern_unicast_seal(&tx.tx, c->hop, c->label, c->pt, c->pt_len, frame, sizeof frame) ==
              TERN_OK);
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
    }
}

static void rejected_cases(void) {
    for (size_t i = 0; i < COUNT(rejected); i++) {
        const struct rejected_case *c = &rejected[i];
        struct tern_session rx;
        uint8_t pt[TERN_UNICAST_MAX_PLAINTEXT];
        CHECK(c->counter == 0); /* a new receiver expects 0; a later counter would need setup */
        start(&rx, c->secret, receiver_of(c->dir));
        struct tern_unicast_received r = receive(&rx.rx, c->frame, c->frame_len, pt);
        if (r.verdict == TERN_UNICAST_ACCEPTED) {
            fprintf(stderr, "rejected %s: accepted\n", c->name);
            check_failures++;
        }
        /* Nothing it was shown moves the window: the frame the case was made from still goes. */
        CHECK(!rx.rx.started);
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
            struct tern_unicast_received r = receive(&rx.rx, d->frame, d->frame_len, pt);
            bool ok = d->accept ? r.verdict == TERN_UNICAST_ACCEPTED && r.counter == d->counter &&
                                      bytes_eq(pt, r.len, d->pt, d->pt_len)
                                : r.verdict != TERN_UNICAST_ACCEPTED;
            if (!ok) {
                fprintf(stderr, "sequence %s: delivery %zu\n", c->name, j);
                check_failures++;
            }
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
            CHECK(tern_unicast_open(rx, 2, d->frame, d->frame_len, pt, sizeof pt, &r) == TERN_OK);
            if (r.verdict != TERN_UNICAST_ACCEPTED || r.session != d->session ||
                r.counter != d->counter || !bytes_eq(pt, r.len, d->pt, d->pt_len)) {
                fprintf(stderr, "collision %s: delivery %zu\n", c->name, j);
                check_failures++;
            }
        }
    }
}

static void sender_cases(void) {
    for (size_t i = 0; i < COUNT(senders); i++) {
        const struct sender_case *c = &senders[i];
        struct tern_session node[3]; /* indexed by role */
        uint8_t frame[TERN_UNICAST_MAX_FRAME];
        start(&node[TERN_INITIATOR], c->secret, TERN_INITIATOR);
        start(&node[TERN_RESPONDER], c->secret, TERN_RESPONDER);
        for (size_t j = 0; j < c->count; j++) {
            const struct send *d = &c->sends[j];
            struct tern_unicast_tx *tx = &node[sender_of(d->dir)].tx;
            CHECK_EQ_U64(tx->next, d->counter);
            CHECK(tern_unicast_seal(tx, 0, 0, d->pt, d->pt_len, frame, sizeof frame) == TERN_OK);
            if (!bytes_eq(frame, d->pt_len + TERN_UNICAST_OVERHEAD, d->frame, d->frame_len)) {
                fprintf(stderr, "sender %s: send %zu differs\n", c->name, j);
                check_failures++;
            }
        }
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
        CHECK(tern_unicast_seal(&from->tx, 1, 2, msg, len, frame, sizeof frame) == TERN_OK);
        struct tern_unicast_received r = receive(&to->rx, frame, len + TERN_UNICAST_OVERHEAD, pt);
        CHECK(r.verdict == TERN_UNICAST_ACCEPTED);
        CHECK(bytes_eq(pt, r.len, msg, len));
        CHECK_EQ_U64(r.counter, len / 2);
    }
}

static void seal_checks_its_arguments(void) {
    struct tern_session a;
    uint8_t msg[TERN_UNICAST_MAX_PLAINTEXT + 1] = {0}, frame[TERN_UNICAST_MAX_FRAME + 1];
    start(&a, secret, TERN_INITIATOR);
    CHECK(tern_unicast_seal(&a.tx, 0, 0, msg, sizeof msg, frame, sizeof frame) == TERN_EINVAL);
    CHECK(tern_unicast_seal(&a.tx, 0, 0, msg, 10, frame, 25) == TERN_EINVAL);
    CHECK(tern_unicast_seal(&a.tx, 0, 0, msg, 10, NULL, 26) == TERN_EINVAL);
    CHECK(tern_unicast_seal(&a.tx, 0, 0, NULL, 10, frame, 26) == TERN_EINVAL);
    CHECK_EQ_U64(a.tx.next, 0); /* a refused message uses no counter */
    CHECK(tern_unicast_seal(&a.tx, 0, 0, msg, 10, frame, 26) == TERN_OK);
    CHECK_EQ_U64(a.tx.next, 1);
}

static void last_counter_spends_the_session(void) {
    struct tern_session a;
    uint8_t frame[TERN_UNICAST_MAX_FRAME];
    start(&a, secret, TERN_INITIATOR);
    /* Reaching 2^32 - 1 honestly would take 2^32 messages, so the test moves the counter itself.
     * The frame it sends is wrong (the epoch key is epoch 0's), but this is about what follows. */
    a.tx.next = UINT32_MAX;
    CHECK(tern_unicast_seal(&a.tx, 0, 0, NULL, 0, frame, sizeof frame) == TERN_OK);
    CHECK(a.tx.spent);
    CHECK(tern_unicast_seal(&a.tx, 0, 0, NULL, 0, frame, sizeof frame) == TERN_ESPENT);
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
    CHECK(tern_unicast_seal(&a.tx, 0, 0, (const uint8_t *)"hi", 2, frame, sizeof frame) == TERN_OK);

    /* Too short, and an unknown header, are malformed. */
    CHECK(tern_unicast_open(rx, 1, frame, 15, pt, sizeof pt, &r) == TERN_OK);
    CHECK(r.verdict == TERN_UNICAST_MALFORMED);
    frame[0] ^= 0x01;
    CHECK(tern_unicast_open(rx, 1, frame, 18, pt, sizeof pt, &r) == TERN_OK);
    CHECK(r.verdict == TERN_UNICAST_MALFORMED);
    frame[0] ^= 0x01;

    /* A tag in no window is someone else's. */
    frame[4] ^= 0x80;
    CHECK(tern_unicast_open(rx, 1, frame, 18, pt, sizeof pt, &r) == TERN_OK);
    CHECK(r.verdict == TERN_UNICAST_NOT_OURS);
    frame[4] ^= 0x80;

    /* A known tag on a frame that does not authenticate is a forgery, and leaves no plaintext. */
    frame[17] ^= 0x01;
    memset(pt, 0xAA, sizeof pt);
    CHECK(tern_unicast_open(rx, 1, frame, 18, pt, sizeof pt, &r) == TERN_OK);
    CHECK(r.verdict == TERN_UNICAST_FORGED);
    CHECK(pt[0] == 0 && pt[1] == 0);
    frame[17] ^= 0x01;

    /* Too little room for the plaintext is the caller's mistake, not the frame's. */
    CHECK(tern_unicast_open(rx, 1, frame, 18, pt, 1, &r) == TERN_EINVAL);

    /* hop and label are the routing layer's: changing them changes nothing. */
    frame[1] = 9;
    frame[2] = 0xAB;
    CHECK(tern_unicast_open(rx, 1, frame, 18, pt, sizeof pt, &r) == TERN_OK);
    CHECK(r.verdict == TERN_UNICAST_ACCEPTED);
    CHECK(pt[0] == 'h' && pt[1] == 'i');
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
        CHECK(tern_unicast_seal(&a.tx, 0, 0, NULL, 0, frame, sizeof frame) == TERN_OK);
        CHECK(receive(&b.rx, frame, TERN_UNICAST_OVERHEAD, pt).verdict == TERN_UNICAST_ACCEPTED);

        /* Every counter up to n is in, so the window's lowest is n + 1, and it reaches n + 32. */
        uint32_t first = (n + 1) / 32, last = (n + 32) / 32;
        CHECK_EQ_U64(b.rx.first_epoch, first);
        CHECK_EQ_U64(b.rx.epochs, last - first + 1);
    }

    /* Receiving 31, then nothing below it: epoch 0 must stay, for counters 0 to 30. */
    struct tern_session c, d;
    start(&c, secret, TERN_INITIATOR);
    start(&d, secret, TERN_RESPONDER);
    for (uint32_t n = 0; n <= 31; n++) {
        CHECK(tern_unicast_seal(&c.tx, 0, 0, NULL, 0, frame, sizeof frame) == TERN_OK);
    }
    CHECK(receive(&d.rx, frame, TERN_UNICAST_OVERHEAD, pt).verdict == TERN_UNICAST_ACCEPTED);
    CHECK_EQ_U64(d.rx.first_epoch, 0);
    CHECK_EQ_U64(d.rx.epochs, 2); /* the window is 0 to 63 */
}

int main(void) {
    RUN(accepted_cases);
    RUN(rejected_cases);
    RUN(sequence_cases);
    RUN(collision_cases);
    RUN(sender_cases);
    RUN(secret_is_erased);
    RUN(round_trips_every_length);
    RUN(seal_checks_its_arguments);
    RUN(last_counter_spends_the_session);
    RUN(open_sorts_what_it_is_shown);
    RUN(receiver_keeps_only_needed_epochs);
    return CHECK_DONE();
}
