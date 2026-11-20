#include <string.h>

#include "check.h"
#include "tern/address.h"
#include "tern/crypto.h"
#include "tern/forward.h"
#include "tern/manage.h"
#include "tern/region.h"
#include "tern/route.h"
#include "tern/sleep.h"

/* Leaves that sleep (draft/sleeping.md): the specification's vectors, and a few nodes on a channel
 * that takes no time, where a leaf whose forwarder says it may sleep hears nothing. */

struct poll_case {
    struct tern_poll fields;
    uint8_t frame[TERN_POLL_LEN];
};
struct answer_case {
    struct tern_poll_answer fields;
    uint8_t frame[TERN_POLL_ANSWER_LEN];
};
struct held_case {
    int8_t power;
    uint32_t keeper, destination, tag;
    uint8_t frame[TERN_HELD_LEN];
};
struct rejected_case {
    const char *why;
    uint8_t frame[32];
    size_t len;
};
struct announce_case {
    struct tern_announce fields;
    uint8_t seed[32];
    bool taken;
    size_t len;
    uint8_t frame[TERN_ROUTE_FRAME_MAX];
};
struct sleep_case {
    bool has_wake;
    uint16_t wake;
    uint8_t content[8];
    size_t len;
    bool valid;
};
struct holds_case {
    int64_t at_ms;
    uint8_t listen;
    bool holds;
};
struct poll_id_case {
    bool polled;
    uint16_t last, id;
    bool answers;
};
struct rest_case {
    bool given_up;
    int64_t since_s;
    bool answered, may_take;
};
struct hold_for_case {
    uint16_t wake;
    int64_t hold_for;
};
struct wait_case {
    uint16_t metric, wake;
    int64_t wait_ms;
};

#include "sleeping.h"

#define COUNT(a) (sizeof(a) / sizeof(a)[0])

static void parameters_are_the_specifications(void) {
    CHECK_EQ_I64(TERN_SLEEP_WAKE_MIN_S, V_WAKE_MIN);
    CHECK_EQ_I64(TERN_SLEEP_WAKE_MAX_S, V_WAKE_MAX);
    CHECK_EQ_I64(TERN_SLEEP_LISTEN_MIN_S, V_LISTEN_MIN);
    CHECK_EQ_I64(TERN_SLEEP_HOLD_EXTRA_S, V_HOLD_EXTRA);
    CHECK_EQ_I64(TERN_SLEEP_KEEPER_REST_S, V_KEEPER_REST);
}

static void polls_and_answers_are_written_and_read(void) {
    for (size_t i = 0; i < COUNT(polls); i++) {
        const struct poll_case *c = &polls[i];
        uint8_t frame[TERN_POLL_LEN];
        struct tern_poll q;
        CHECK_EQ_I64(tern_poll_write(&c->fields, frame), TERN_POLL_LEN);
        CHECK(memcmp(frame, c->frame, TERN_POLL_LEN) == 0);
        CHECK(tern_route_frame(c->frame, TERN_POLL_LEN));
        CHECK(tern_poll_read(&q, c->frame, TERN_POLL_LEN));
        CHECK(q.sender == c->fields.sender && q.keeper == c->fields.keeper &&
              q.id == c->fields.id && q.wake == c->fields.wake && q.listen == c->fields.listen &&
              q.power == c->fields.power);
    }
    for (size_t i = 0; i < COUNT(answers); i++) {
        const struct answer_case *c = &answers[i];
        uint8_t frame[TERN_POLL_ANSWER_LEN];
        struct tern_poll_answer a;
        CHECK_EQ_I64(tern_poll_answer_write(&c->fields, frame), TERN_POLL_ANSWER_LEN);
        CHECK(memcmp(frame, c->frame, TERN_POLL_ANSWER_LEN) == 0);
        CHECK(tern_route_frame(c->frame, TERN_POLL_ANSWER_LEN));
        CHECK(tern_poll_answer_read(&a, c->frame, TERN_POLL_ANSWER_LEN));
        CHECK(a.sender == c->fields.sender && a.leaf == c->fields.leaf && a.id == c->fields.id &&
              a.margin == c->fields.margin && a.held == c->fields.held &&
              a.power == c->fields.power);
    }
}

static void held_notices_are_written_and_read(void) {
    for (size_t i = 0; i < COUNT(helds); i++) {
        const struct held_case *c = &helds[i];
        uint8_t frame[TERN_HELD_LEN], tag[4] = {(uint8_t)(c->tag >> 24), (uint8_t)(c->tag >> 16),
                                                (uint8_t)(c->tag >> 8), (uint8_t)c->tag};
        struct tern_forward_head h;
        CHECK_EQ_I64(tern_forward_held_write(c->power, c->keeper, c->destination, tag, frame),
                     TERN_HELD_LEN);
        CHECK(memcmp(frame, c->frame, TERN_HELD_LEN) == 0);
        CHECK(tern_forward_frame(c->frame, TERN_HELD_LEN) && !tern_route_frame(c->frame, 1));
        CHECK(tern_forward_head_read(&h, c->frame, TERN_HELD_LEN));
        CHECK(h.hdr == TERN_HDR_HELD && h.hops == 0 && h.power == c->power && h.next == c->keeper &&
              h.destination == c->destination);
        CHECK(!tern_forward_listens(c->frame, TERN_HELD_LEN)); /* sent once */
    }
}

static void frames_the_specification_rejects_are_discarded(void) {
    for (size_t i = 0; i < COUNT(rejected); i++) {
        const struct rejected_case *c = &rejected[i];
        struct tern_poll q;
        struct tern_poll_answer a;
        struct tern_forward_head h;
        bool read = tern_poll_read(&q, c->frame, c->len) ||
                    tern_poll_answer_read(&a, c->frame, c->len) ||
                    (c->frame[0] == TERN_HDR_HELD && tern_forward_head_read(&h, c->frame, c->len));
        if (read) {
            fprintf(stderr, "read %s\n", c->why);
            check_failures++;
        }
    }
}

static void announces_say_their_keeper(void) {
    for (size_t i = 0; i < COUNT(announces); i++) {
        const struct announce_case *c = &announces[i];
        uint8_t frame[255], m[TERN_ANNOUNCE_LABEL_LEN + TERN_ROUTE_FRAME_MAX];
        struct tern_announce in = c->fields, a;
        struct tern_identity id;
        tern_identity_init(&id, c->seed);
        memcpy(in.key, id.key, 32);
        CHECK_EQ_I64(tern_announce_write(&in, frame), c->len);
        tern_identity_sign_blinded(&id, TERN_EPOCH, m, tern_announce_signed(frame, c->len, m),
                                   frame + c->len - TERN_ANNOUNCE_SIG);
        CHECK(memcmp(frame, c->frame, c->len) == 0);
        bool read = tern_announce_read(&a, c->frame, c->len);
        bool checks =
            read && tern_route_key_id(a.key) == a.sender && tern_address_valid(a.key) &&
            tern_address_verify(a.key, m, tern_announce_signed(c->frame, c->len, m), a.sig) &&
            a.sender != V_OTHER;
        CHECK(checks == c->taken);
        if (read) {
            CHECK(a.sleeps == c->fields.sleeps && a.keeper == c->fields.keeper);
            CHECK(a.named_count == c->fields.named_count && a.seq == c->fields.seq);
        }
        tern_identity_wipe(&id);
    }
}

static void sleep_content_is_written_and_read(void) {
    for (size_t i = 0; i < COUNT(sleeps); i++) {
        const struct sleep_case *c = &sleeps[i];
        uint16_t wake = 0xBEEF;
        if (c->has_wake) {
            uint8_t out[TERN_SLEEP_CONTENT_LEN];
            CHECK_EQ_I64(tern_sleep_content_write(c->wake, out), TERN_SLEEP_CONTENT_LEN);
            CHECK(memcmp(out, c->content, TERN_SLEEP_CONTENT_LEN) == 0);
        }
        CHECK(tern_sleep_content_read(c->content, c->len, &wake) == c->valid);
        CHECK(!c->valid || wake == c->wake);
    }
}

static void keepers_and_sources_reckon_time_as_the_specification_does(void) {
    for (size_t i = 0; i < COUNT(holds); i++) {
        CHECK(tern_sleep_holds(TERN_MS(holds[i].at_ms), holds[i].listen) == holds[i].holds);
    }
    for (size_t i = 0; i < COUNT(poll_ids); i++) {
        CHECK(tern_sleep_answers_poll(poll_ids[i].polled, poll_ids[i].last, poll_ids[i].id) ==
              poll_ids[i].answers);
    }
    for (size_t i = 0; i < COUNT(rests); i++) {
        CHECK(tern_sleep_may_take(rests[i].given_up, TERN_S(rests[i].since_s), rests[i].answered) ==
              rests[i].may_take);
    }
    for (size_t i = 0; i < COUNT(hold_fors); i++) {
        CHECK_EQ_I64(tern_sleep_hold_for(hold_fors[i].wake), TERN_S(hold_fors[i].hold_for));
    }
    for (size_t i = 0; i < COUNT(waits); i++) {
        CHECK_EQ_I64(tern_sleep_source_wait(TERN_MS(V_ACK_WAIT_MS), V_ACK_FACTOR, waits[i].metric,
                                            waits[i].wake),
                     TERN_MS(waits[i].wait_ms));
    }
}

/* --- A few nodes --- */

#define NODES 4
#define NB 8
#define DESTS 8
#define SLOTS 4
#define KEPT 16
#define NONE INT16_MIN
#define FULL 22
#define SNR_FLOOR_Q (-50)
#define BODY 24
#define WAKE 300

/* 0, a relay, sends; 1 and 3 are relays; 2 is a leaf that sleeps, nearer 1 than 3. */
enum { SRC = 0, KEEPER = 1, LEAF = 2, OTHER = 3 };

struct node {
    struct tern_route r;
    struct tern_forward f;
    struct tern_route_neighbour nb[NB];
    struct tern_route_dest dest[DESTS];
    struct tern_forward_slot slot[SLOTS];
    struct tern_forward_kept kept[KEPT];
    int got, acked, failed;
};

static struct {
    struct node node[NODES];
    int16_t snr[NODES][NODES];
    struct tern_lora lora;
    tern_time now;
    tern_time awake;          /* the leaf's time with its radio on */
    bool deaf[NODES];         /* a relay on firmware before sleeping leaves: it takes no poll */
    unsigned polls;           /* the leaf's, sent */
    unsigned keeper_messages; /* messages the keeper put on the air */
} net;

static uint32_t id_of(int i) { return 0x2000u + (uint32_t)i; }

static bool off(int k) {
    tern_time wake;
    return tern_forward_may_sleep(&net.node[k].f, net.now, &wake);
}

static void on_air(int from, const uint8_t *frame, size_t len, int8_t dbm) {
    net.polls += from == LEAF && frame[0] == TERN_HDR_POLL;
    net.keeper_messages += from == KEEPER && frame[0] == TERN_HDR_MESSAGE;
    for (int k = 0; k < NODES; k++) {
        struct node *y = &net.node[k];
        int snr = net.snr[from][k] == NONE ? NONE : net.snr[from][k] + 4 * (dbm - FULL);
        if (k == from || snr == NONE || snr < SNR_FLOOR_Q || off(k)) {
            continue;
        }
        if (net.deaf[k] && frame[0] == TERN_HDR_POLL) {
            continue;
        }
        if (tern_route_frame(frame, len)) {
            tern_route_heard(&y->r, net.now, frame, len, (int16_t)snr);
        } else {
            struct tern_forward_heard got;
            tern_forward_heard(&y->f, net.now, frame, len, (int16_t)snr, &got);
            if (got.got == TERN_FORWARD_MESSAGE) {
                uint8_t ack[TERN_ACK_LEN] = {TERN_HDR_ACK};
                int who = frame[TERN_FORWARD_HEAD + TERN_FORWARD_TAG];
                y->got++;
                memcpy(ack + TERN_FORWARD_HEAD, frame + TERN_FORWARD_HEAD, TERN_FORWARD_TAG);
                tern_forward_send(&y->f, net.now, id_of(who), ack, sizeof ack, false, got.back);
            } else if (got.got == TERN_FORWARD_ACK) {
                y->acked += tern_forward_acked(&y->f, frame + TERN_FORWARD_HEAD);
            }
        }
    }
}

static void run(tern_time until) {
    for (;;) {
        tern_time t = until + 1;
        for (int i = 0; i < NODES; i++) {
            const struct node *x = &net.node[i];
            tern_time due = tern_route_due(&x->r);
            due = tern_forward_due(&x->f) < due ? tern_forward_due(&x->f) : due;
            t = due < t ? due : t;
        }
        /* The leaf's radio, at least once a second, to see how long it is on. */
        t = net.now + TERN_S(1) < t ? net.now + TERN_S(1) : t;
        if (t > until) {
            break;
        }
        if (!off(LEAF)) {
            net.awake += (t > net.now ? t : net.now) - net.now;
        }
        /* Time moves on, as a board's clock does, however soon something says it is due. */
        net.now = t > net.now ? t : net.now + TERN_MS(1);
        for (int i = 0; i < NODES; i++) {
            struct node *x = &net.node[i];
            uint8_t frame[TERN_ROUTE_FRAME_MAX], tag[TERN_FORWARD_TAG], handle;
            enum tern_forward_kind kind;
            int8_t dbm = 0;
            size_t len;
            /* As the node drives them (ports/node/node.c): the forwarder first, and only when it
             * says it is due, then the router. */
            while (tern_forward_due(&x->f) <= net.now &&
                   (len = tern_forward_poll(&x->f, net.now, frame, &dbm, &kind, &handle)) != 0) {
                tern_forward_sent(&x->f, net.now, handle);
                on_air(i, frame, len, dbm);
            }
            while (tern_route_due(&x->r) <= net.now &&
                   (len = tern_route_poll(&x->r, net.now, frame, &dbm)) != 0) {
                tern_route_sent(&x->r, net.now);
                on_air(i, frame, len, dbm);
            }
            while (tern_forward_failed(&x->f, tag)) {
                x->failed++;
            }
        }
    }
    net.now = until;
}

static void link(int a, int b, int16_t snr) { net.snr[a][b] = net.snr[b][a] = snr; }

static void net_init(void) {
    memset(&net, 0, sizeof net);
    net.lora = tern_region_lora(tern_region(TERN_REGION_US915));
    for (int a = 0; a < NODES; a++) {
        struct node *x = &net.node[a];
        struct tern_forward_config fc = tern_forward_defaults();
        struct tern_route_config c = tern_route_defaults(&net.lora, FULL, -9, a != LEAF);
        if (a == LEAF) {
            c.sleep_wake = WAKE;
            c.sleep_listen_s = 2;
        }
        tern_route_init(&x->r, &c, id_of(a), x->nb, NB, x->dest, DESTS, 0,
                        0x9E3779B97F4A7C15ULL * (uint64_t)(a + 1), 0);
        tern_forward_init(&x->f, &fc, &x->r, x->slot, SLOTS, (uint64_t)(a + 7));
        if (a != LEAF) {
            tern_forward_keeper(&x->f, x->kept, KEPT);
        }
        for (int b = 0; b < NODES; b++) {
            net.snr[a][b] = NONE;
        }
    }
    link(SRC, KEEPER, 40);
    link(SRC, OTHER, 40);
    link(KEEPER, OTHER, 40);
    link(KEEPER, LEAF, 60);
    link(OTHER, LEAF, 20);
    run(TERN_S(1800));
}

static void message(int from, int to, uint8_t tag) {
    uint8_t frame[TERN_FORWARD_HEAD + BODY] = {TERN_HDR_MESSAGE};
    memset(frame + TERN_FORWARD_HEAD, tag, TERN_FORWARD_TAG);
    frame[TERN_FORWARD_HEAD + TERN_FORWARD_TAG] = (uint8_t)from;
    CHECK(tern_forward_send(&net.node[from].f, net.now, id_of(to), frame, sizeof frame, true,
                            INT8_MIN));
}

/* Once started, the leaf sleeps with the relay it hears best as its keeper; and the other relay,
 * which hears it too, reaches it through the keeper, never directly. */
static void a_leaf_sleeps_and_only_its_keeper_uses_it(void) {
    uint32_t keeper = 0, next = 0;
    uint16_t metric;
    net_init();
    CHECK(tern_route_sleeping(&net.node[LEAF].r, &keeper));
    CHECK(keeper == id_of(KEEPER));
    CHECK(tern_route_next(&net.node[OTHER].r, id_of(LEAF), &next, &metric));
    CHECK(next == id_of(KEEPER));
    CHECK(tern_route_next(&net.node[KEEPER].r, id_of(LEAF), &next, &metric));
    CHECK(next == id_of(LEAF));
}

/* A message for the leaf is held by its keeper while it sleeps, the node before told so, and
 * handed over when it polls; its acknowledgement reaches the source, which knew to wait. */
static void a_message_waits_for_its_leaf(void) {
    net_init();
    tern_forward_wake(&net.node[SRC].f, id_of(LEAF), WAKE);
    net.awake = 0;
    tern_time from = net.now;
    message(SRC, LEAF, 0xA1);
    run(net.now + TERN_S(20));
    CHECK_EQ_I64(net.node[KEEPER].f.counts.held, 1);
    CHECK_EQ_I64(net.node[LEAF].got, 0);
    CHECK_EQ_I64(net.node[SRC].f.counts.given_up, 0); /* the held notice ended its hop */
    run(net.now + TERN_S(WAKE + 30));
    CHECK_EQ_I64(net.node[LEAF].got, 1);
    CHECK_EQ_I64(net.node[KEEPER].f.counts.handed, 1);
    CHECK_EQ_I64(net.node[SRC].acked, 1);
    CHECK_EQ_I64(net.node[SRC].failed, 0);
    /* Asleep nearly all the time. */
    CHECK(net.awake * 100 < (net.now - from) * 5);
}

/* What the keeper's next answer to the leaf's poll says it holds. */
static int held_for_leaf(void) {
    const struct tern_route *r = &net.node[KEEPER].r;
    for (size_t i = 0; i < TERN_ROUTE_KEPT_LEAVES; i++) {
        if (r->keeps[i].leaf == id_of(LEAF)) {
            return r->keeps[i].held;
        }
    }
    return -1;
}

/* A message from the keeper itself waits for the leaf too, and is counted in the answer to its
 * poll, so the leaf listens for it: found on two boards, where the answer said nothing was held and
 * the leaf went back to sleep before the keeper's own message went. And no more polls than it
 * needs. */
static void the_keepers_own_message_waits_for_its_leaf(void) {
    net_init();
    tern_forward_wake(&net.node[KEEPER].f, id_of(LEAF), WAKE);
    unsigned before = net.polls;
    message(KEEPER, LEAF, 0xD4);
    run(net.now + TERN_S(20));
    CHECK_EQ_I64(net.node[LEAF].got, 0);
    CHECK_EQ_I64(held_for_leaf(), 1); /* what the answer to its next poll says */
    run(net.now + TERN_S(WAKE + 30));
    CHECK_EQ_I64(net.node[LEAF].got, 1);
    CHECK_EQ_I64(net.node[KEEPER].acked, 1);
    CHECK_EQ_I64(net.node[KEEPER].failed, 0);
    CHECK(net.polls - before <= 3);
}

/* The leaf's own message to its keeper: the keeper's acknowledgement, its own frame, is held for
 * the leaf's next poll, and the leaf's wait for it is long enough. */
static void the_keepers_acknowledgement_waits_for_its_leaf(void) {
    net_init();
    message(LEAF, KEEPER, 0xE5);
    run(net.now + TERN_S(2 * WAKE + 60));
    CHECK(net.node[KEEPER].got >= 1);
    CHECK_EQ_I64(net.node[LEAF].acked, 1);
    CHECK_EQ_I64(net.node[LEAF].failed, 0);
    CHECK_EQ_I64(net.node[KEEPER].got, 1); /* one try: its wait was long enough */
}

/* An hour asleep, with nothing for it, the leaf's radio is on a few percent of the time. */
static void a_leaf_with_nothing_for_it_is_nearly_always_off(void) {
    net_init();
    net.awake = 0;
    tern_time from = net.now;
    run(net.now + TERN_S(3600));
    CHECK(tern_route_sleeping(&net.node[LEAF].r, NULL));
    CHECK(net.awake * 100 < (net.now - from) * 3);
}

/* A keeper that goes is given up after POLL_TRIES polls unanswered, and the leaf sleeps again with
 * the other relay, to which a message then goes. */
static void a_leaf_whose_keeper_goes_takes_another(void) {
    uint32_t keeper = 0;
    net_init();
    link(KEEPER, LEAF, NONE);
    link(KEEPER, SRC, NONE);
    link(KEEPER, OTHER, NONE);
    run(net.now + TERN_S(1800));
    CHECK(tern_route_sleeping(&net.node[LEAF].r, &keeper));
    CHECK(keeper == id_of(OTHER));
    tern_forward_wake(&net.node[SRC].f, id_of(LEAF), WAKE);
    message(SRC, LEAF, 0xB2);
    run(net.now + TERN_S(2 * WAKE));
    CHECK_EQ_I64(net.node[LEAF].got, 1);
    CHECK_EQ_I64(net.node[SRC].acked, 1);
}

/* A frame held longer than HOLD_FOR is dropped. */
static void a_frame_held_too_long_is_dropped(void) {
    net_init();
    message(SRC, LEAF, 0xC3);
    run(net.now + TERN_S(5));
    link(KEEPER, LEAF, NONE); /* the leaf goes, and never polls again */
    link(OTHER, LEAF, NONE);
    run(net.now + TERN_S(2 * WAKE + 120));
    CHECK_EQ_I64(net.node[KEEPER].f.counts.held, 1);
    CHECK_EQ_I64(net.node[KEEPER].f.counts.held_late, 1);
    CHECK_EQ_I64(net.node[LEAF].got, 0);
}

/* Told not to sleep, a leaf asleep listens again at once, and its announces stop naming a keeper;
 * told to sleep again, it does, with the keeper it had. */
static void a_leaf_told_to_stop_listens_and_can_sleep_again(void) {
    uint32_t keeper = 0;
    tern_time wake;
    net_init();
    CHECK(tern_route_sleeping(&net.node[LEAF].r, NULL));
    tern_route_sleep_set(&net.node[LEAF].r, 0, 0);
    CHECK(!tern_route_sleeping(&net.node[LEAF].r, NULL));
    CHECK(!tern_forward_may_sleep(&net.node[LEAF].f, net.now, &wake));
    CHECK(tern_route_listen_until(&net.node[LEAF].r) == INT64_MAX);
    run(net.now + TERN_S(120));
    CHECK(!tern_route_sleeping(&net.node[LEAF].r, NULL));
    /* The keeper heard it announce awake: it no longer takes it for asleep, and holds nothing. */
    message(SRC, LEAF, 0xD4);
    run(net.now + TERN_S(30));
    CHECK_EQ_I64(net.node[LEAF].got, 1);
    CHECK_EQ_I64(net.node[KEEPER].f.counts.held, 0);
    tern_route_sleep_set(&net.node[LEAF].r, WAKE, 2);
    run(net.now + TERN_S(60));
    CHECK(tern_route_sleeping(&net.node[LEAF].r, &keeper));
    CHECK(keeper == id_of(KEEPER));
}

/* No relay it hears answers a poll, as none on earlier firmware does: the leaf gives each up in
 * turn, takes neither again while it rests, and listens all the time meanwhile, polling no more;
 * once the rests are over it tries again, and sleeps if a relay now answers. */
static void relays_that_do_not_keep_are_rested(void) {
    uint32_t keeper = 0, relay = 0;
    tern_time until = 0, wake;
    net_init();
    CHECK(tern_route_sleeping(&net.node[LEAF].r, &keeper));
    net.deaf[KEEPER] = net.deaf[OTHER] = true;
    net.polls = 0;
    run(net.now + TERN_S(1800));
    CHECK(!tern_route_sleeping(&net.node[LEAF].r, NULL));
    CHECK(!tern_forward_may_sleep(&net.node[LEAF].f, net.now, &wake));
    CHECK(tern_route_resting(&net.node[LEAF].r, net.now, &relay, &until));
    CHECK(relay == id_of(KEEPER)); /* given up first, so its rest ends first */
    /* No storm: POLL_TRIES to each relay, and none after. */
    CHECK(net.polls <= 2 * TERN_SLEEP_POLL_TRIES + 1);
    unsigned sent = net.polls;
    run(until - TERN_S(60));
    CHECK_EQ_I64(net.polls, sent);
    CHECK(!tern_route_sleeping(&net.node[LEAF].r, NULL));
    /* The relays are updated; the rests end, and the leaf sleeps again. */
    net.deaf[KEEPER] = net.deaf[OTHER] = false;
    run(until + TERN_S(1200));
    CHECK(tern_route_sleeping(&net.node[LEAF].r, &keeper));
    CHECK(keeper == id_of(KEEPER));
}

/* Restarts node `k` as a board does: its routes, and what its forwarder held, gone. A keeper given
 * what it kept, as a board keeps it in flash, takes back its sleeping leaves. */
static void restart(int k, const struct tern_route_kept_saved *saved, size_t n) {
    struct node *x = &net.node[k];
    struct tern_forward_config fc = tern_forward_defaults();
    struct tern_route_config c = tern_route_defaults(&net.lora, FULL, -9, k != LEAF);
    if (k == LEAF) {
        c.sleep_wake = WAKE;
        c.sleep_listen_s = 2;
    }
    tern_route_init(&x->r, &c, id_of(k), x->nb, NB, x->dest, DESTS, 0,
                    0x5851F42D4C957F2DULL * (uint64_t)(k + 3), net.now);
    tern_forward_init(&x->f, &fc, &x->r, x->slot, SLOTS, (uint64_t)(k + 17));
    if (k != LEAF) {
        tern_forward_keeper(&x->f, x->kept, KEPT);
    }
    tern_route_kept_restore(&x->r, net.now, saved, n);
    x->got = x->acked = x->failed = 0;
}

/* A keeper that restarts takes back the leaf it kept: what it then has for the leaf, here a first
 * contact's frame, for which nothing tells it the leaf sleeps, is held until the leaf polls, and
 * the leaf, its polls answered, keeps it as its keeper. Found on two boards: the keeper had no
 * route to the leaf, gave its frame up in four tries, and answered none of its polls. */
static void a_keeper_that_restarts_still_keeps_its_leaf(void) {
    struct tern_route_kept_saved saved[TERN_ROUTE_KEPT_LEAVES];
    uint32_t sum = 0, keeper = 0;
    net_init();
    size_t n = tern_route_kept_list(&net.node[KEEPER].r, saved, COUNT(saved), &sum);
    CHECK_EQ_I64(n, 1);
    CHECK(saved[0].leaf == id_of(LEAF));
    CHECK(tern_sleep_wake_s(saved[0].wake) == WAKE);
    restart(KEEPER, saved, n);
    message(KEEPER, LEAF, 0xF1);
    run(net.now + TERN_S(20));
    CHECK_EQ_I64(net.node[LEAF].got, 0);
    CHECK_EQ_I64(net.node[KEEPER].failed, 0);
    run(net.now + TERN_S(WAKE + 30));
    CHECK_EQ_I64(net.node[LEAF].got, 1);
    CHECK_EQ_I64(net.node[KEEPER].acked, 1);
    CHECK_EQ_I64(net.node[KEEPER].failed, 0);
    CHECK(tern_route_sleeping(&net.node[LEAF].r, &keeper));
    CHECK(keeper == id_of(KEEPER));
    /* What it lists again sums as before: nothing to save. */
    uint32_t again = 0;
    CHECK_EQ_I64(tern_route_kept_list(&net.node[KEEPER].r, saved, COUNT(saved), &again), 1);
    CHECK(again == sum);
}

/* Both restart, as two boards flashed together do: the keeper takes back the leaf it kept, the
 * leaf starts over and takes the keeper again, and only then does the keeper have a frame of its
 * own for it. The answer to the leaf's next poll says it is held, and the leaf listens for it. On
 * two boards the answer said none was held, the leaf slept, and the frame went to no one: the
 * forwarder, polled only when due, had not counted it. */
static void both_restart_and_the_keepers_own_frame_waits_for_the_next_poll(void) {
    struct tern_route_kept_saved saved[TERN_ROUTE_KEPT_LEAVES];
    uint32_t sum = 0, keeper = 0;
    net_init();
    size_t n = tern_route_kept_list(&net.node[KEEPER].r, saved, COUNT(saved), &sum);
    restart(KEEPER, saved, n);
    restart(LEAF, NULL, 0);
    for (tern_time limit = net.now + TERN_S(600);
         !tern_route_sleeping(&net.node[LEAF].r, &keeper) && net.now < limit;) {
        run(net.now + TERN_S(1));
    }
    CHECK(keeper == id_of(KEEPER));
    run(net.now + TERN_S(5)); /* asleep now, between polls */
    message(KEEPER, LEAF, 0xF3);
    run(net.now + TERN_S(WAKE + 30));
    CHECK_EQ_I64(net.node[LEAF].got, 1);
    CHECK_EQ_I64(net.node[KEEPER].acked, 1);
    CHECK_EQ_I64(net.node[KEEPER].failed, 0);
}

/* A frame of the keeper's own that comes just after it answered the leaf's poll saying nothing is
 * held: the leaf, told nothing, sleeps again at once, so the frame waits for its next poll and goes
 * once, then. On two boards it went at once, and twice more, to a leaf that was not listening. */
static void a_frame_after_the_answer_waits_for_the_next_poll(void) {
    net_init();
    tern_forward_wake(&net.node[KEEPER].f, id_of(LEAF), WAKE);
    unsigned polled = net.polls;
    while (net.polls == polled) {
        run(net.now + TERN_MS(10));
    }
    run(net.now + TERN_MS(300)); /* answered, saying nothing is held; the leaf asleep again */
    CHECK_EQ_I64(held_for_leaf(), 0);
    unsigned before = net.keeper_messages;
    message(KEEPER, LEAF, 0xF5);
    run(net.now + TERN_S(10));
    CHECK_EQ_I64(net.keeper_messages, before); /* not to a leaf that is not listening */
    CHECK_EQ_I64(held_for_leaf(), 1);
    run(net.now + TERN_S(WAKE + 30));
    CHECK_EQ_I64(net.node[LEAF].got, 1);
    CHECK_EQ_I64(net.keeper_messages, before + 1);
    CHECK_EQ_I64(net.node[KEEPER].acked, 1);
    CHECK_EQ_I64(net.node[KEEPER].failed, 0);
}

/* The same with nothing kept, as before: the frame is given up. */
static void a_keeper_that_restarts_with_nothing_kept_gives_up(void) {
    net_init();
    restart(KEEPER, NULL, 0);
    message(KEEPER, LEAF, 0xF2);
    run(net.now + TERN_S(60));
    CHECK_EQ_I64(net.node[KEEPER].failed, 1);
    CHECK_EQ_I64(net.node[LEAF].got, 0);
}

/* A source told nothing of the leaf's sleep, as one making first contact is: the keeper's held
 * notice tells it the frame waits for the leaf, and it waits for the answer, not giving the frame
 * up in its four tries. */
static void a_source_told_it_is_held_waits(void) {
    net_init();
    message(SRC, LEAF, 0xA7);
    run(net.now + TERN_S(20));
    CHECK_EQ_I64(net.node[KEEPER].f.counts.held, 1);
    run(net.now + TERN_S(WAKE + 30));
    CHECK_EQ_I64(net.node[LEAF].got, 1);
    CHECK_EQ_I64(net.node[SRC].acked, 1);
    CHECK_EQ_I64(net.node[SRC].failed, 0);
}

/* A leaf that does not sleep is a leaf as before: never off. */
/* A leaf an owner manages (draft/managing.md#a-node-that-sleeps): after the first request, which
 * waits for its wake, each answer and the next request take seconds, as the leaf polls fast; and
 * MANAGED_AWAKE after the last, it polls by its wake again. Without it, four requests took four
 * wakes, twenty minutes, on two boards. */
static void a_managed_leaf_answers_in_seconds_not_wakes(void) {
    net_init();
    tern_forward_wake(&net.node[SRC].f, id_of(LEAF), WAKE);
    tern_time from = net.now, until;
    for (int i = 0; i < 4; i++) {
        int leaf_got = net.node[LEAF].got, src_got = net.node[SRC].got;
        tern_time limit = net.now + TERN_S(WAKE + 60);
        message(SRC, LEAF, (uint8_t)(0x30 + i));
        while (net.node[LEAF].got == leaf_got && net.now < limit) {
            run(net.now + TERN_S(1));
        }
        CHECK_EQ_I64(net.node[LEAF].got, leaf_got + 1);
        tern_route_poll_fast(&net.node[LEAF].r, net.now, TERN_MANAGE_POLL_S, TERN_MANAGE_AWAKE_S);
        CHECK(tern_route_polling_fast(&net.node[LEAF].r, net.now, &until));
        message(LEAF, SRC, (uint8_t)(0x40 + i)); /* its answer */
        while (net.node[SRC].got == src_got && net.now < limit) {
            run(net.now + TERN_S(1));
        }
        CHECK_EQ_I64(net.node[SRC].got, src_got + 1);
    }
    /* One wake for the first, then seconds: well inside a second wake. */
    CHECK(net.now - from < TERN_S(WAKE + 90));
    CHECK_EQ_I64(net.node[SRC].failed, 0);
    CHECK_EQ_I64(net.node[LEAF].failed, 0);
    /* And then back to its wake: a poll or so each wake, once the time is up. */
    run(net.now + TERN_S(TERN_MANAGE_AWAKE_S + 10));
    CHECK(!tern_route_polling_fast(&net.node[LEAF].r, net.now, &until));
    unsigned before = net.polls;
    run(net.now + TERN_S(2 * WAKE));
    CHECK(net.polls - before <= 3);
}

static void a_leaf_that_does_not_sleep_never_may(void) {
    struct tern_route r;
    struct tern_forward f;
    struct tern_route_neighbour nb[2];
    struct tern_route_dest dest[2];
    struct tern_forward_slot slot[2];
    struct tern_lora lora = tern_region_lora(tern_region(TERN_REGION_US915));
    struct tern_route_config c = tern_route_defaults(&lora, FULL, -9, false);
    struct tern_forward_config fc = tern_forward_defaults();
    tern_time wake;
    tern_route_init(&r, &c, 7, nb, 2, dest, 2, 0, 1, 0);
    tern_forward_init(&f, &fc, &r, slot, 2, 1);
    CHECK(!tern_forward_may_sleep(&f, TERN_S(5000), &wake));
    CHECK(tern_route_listen_until(&r) == INT64_MAX);
}

int main(void) {
    RUN(parameters_are_the_specifications);
    RUN(polls_and_answers_are_written_and_read);
    RUN(held_notices_are_written_and_read);
    RUN(frames_the_specification_rejects_are_discarded);
    RUN(announces_say_their_keeper);
    RUN(sleep_content_is_written_and_read);
    RUN(keepers_and_sources_reckon_time_as_the_specification_does);
    RUN(a_leaf_that_does_not_sleep_never_may);
    RUN(a_leaf_sleeps_and_only_its_keeper_uses_it);
    RUN(a_message_waits_for_its_leaf);
    RUN(the_keepers_own_message_waits_for_its_leaf);
    RUN(the_keepers_acknowledgement_waits_for_its_leaf);
    RUN(a_leaf_with_nothing_for_it_is_nearly_always_off);
    RUN(a_leaf_whose_keeper_goes_takes_another);
    RUN(a_frame_held_too_long_is_dropped);
    RUN(a_leaf_told_to_stop_listens_and_can_sleep_again);
    RUN(relays_that_do_not_keep_are_rested);
    RUN(a_keeper_that_restarts_still_keeps_its_leaf);
    RUN(both_restart_and_the_keepers_own_frame_waits_for_the_next_poll);
    RUN(a_frame_after_the_answer_waits_for_the_next_poll);
    RUN(a_keeper_that_restarts_with_nothing_kept_gives_up);
    RUN(a_source_told_it_is_held_waits);
    RUN(a_managed_leaf_answers_in_seconds_not_wakes);
    return CHECK_DONE();
}
