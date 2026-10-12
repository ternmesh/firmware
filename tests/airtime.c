#include "tern/airtime.h"

#include <string.h>

#include "check.h"
#include "tern/companion.h"
#include "tern/flood.h"
#include "tern/forward.h"
#include "tern/region.h"
#include "tern/route.h"

/* The airtime budget (draft/airtime.md): each rule against the specification's vectors, the
 * budget and the flooder's allowances spending together as the vectors have them, and the
 * forwarder and the flooder keeping to a budget. */

enum allowance { ALLOWANCE_NONE, ALLOWANCE_OWN, ALLOWANCE_RELAY };

struct parts_case {
    bool relay;
    uint32_t own_ppm, own_window_s;
    int64_t relayed_ppm, relayed_window_s; /* -1 for none */
    uint32_t routing_ppm, learning_ppm, most_ppm;
};
struct class_case {
    uint8_t hdr;
    bool own;
    enum tern_airtime_part part;
    enum allowance allowance;
    enum tern_airtime_unpaid unpaid;
};
struct spend_frame {
    tern_time at;
    uint8_t hdr;
    bool own;
    uint32_t len;
    tern_time airtime;
    bool sent;
};
struct spend_case {
    const char *name;
    bool relay;
    enum tern_region_id profile;
    const struct spend_frame *frames;
    size_t n;
};
struct estimate_waiting {
    uint8_t hdr;
    uint32_t len;
    tern_time airtime;
    uint16_t wait_s;
};
struct estimate_case {
    const char *name;
    bool relay;
    enum tern_region_id profile;
    const struct spend_frame *spent;
    size_t n_spent;
    tern_time at;
    const struct estimate_waiting *waiting;
    size_t n_waiting;
};
struct reason_case {
    bool route, session;
    uint32_t region_wait_s, budget_wait_s;
    uint8_t reason;
    uint16_t wait;
};

#include "airtime.h"

#define COUNT(a) (sizeof(a) / sizeof(a)[0])
#define WAITING_MAX 8

/* A node as the vectors have one: its budget, and the flooder's two allowances beside it, every
 * bucket full at time 0. */
struct node {
    struct tern_lora lora;
    struct tern_airtime a;
    struct tern_airtime_bucket flood_own, flood_relay;
};

static void start(struct node *x, bool relay, enum tern_region_id profile) {
    struct tern_airtime_config c = tern_airtime_defaults();
    struct tern_flood_config fc = tern_flood_defaults();
    x->lora = tern_region_lora(tern_region(profile));
    tern_time longest = tern_lora_airtime(&x->lora, 255);
    tern_airtime_init(&x->a, &c, &x->lora, relay, 0);
    tern_airtime_bucket_init(&x->flood_own, fc.own_ppm, fc.own_window, longest, 0);
    tern_airtime_bucket_init(&x->flood_relay, fc.relay_ppm, fc.relay_window, longest, 0);
}

/* The allowance that pays for a frame too, or NULL. */
static struct tern_airtime_bucket *allowance(struct node *x, uint8_t hdr, bool own) {
    return tern_airtime_flooded(hdr) ? own ? &x->flood_own : &x->flood_relay : NULL;
}

/* Offers a frame: sent if the budget and its allowance both let it go, and then charged to both;
 * charged to neither if not. */
static bool offer(struct node *x, tern_time at, uint8_t hdr, bool own, tern_time air) {
    struct tern_airtime_bucket *fl = allowance(x, hdr, own);
    bool sent = tern_airtime_pays(&x->a, at, hdr, own, air) &&
                (fl == NULL || tern_airtime_bucket_holds(fl, at, air));
    if (sent) {
        tern_airtime_charge(&x->a, at, hdr, own, air);
        if (fl != NULL) {
            tern_airtime_bucket_charge(fl, at, air);
        }
    }
    return sent;
}

static void test_parts(void) {
    struct tern_airtime_config c = tern_airtime_defaults();
    struct tern_lora l = tern_region_lora(tern_region(TERN_REGION_EU868));
    for (size_t i = 0; i < COUNT(parts_cases); i++) {
        const struct parts_case *p = &parts_cases[i];
        struct tern_route_config rc = tern_route_defaults(&l, 22, -9, p->relay);
        struct tern_airtime a;
        tern_airtime_init(&a, &c, &l, p->relay, 0);
        CHECK_EQ_I64(c.own_ppm, p->own_ppm);
        CHECK_EQ_I64(c.own_window, TERN_S(p->own_window_s));
        CHECK_EQ_I64(a.relay, p->relayed_ppm >= 0);
        if (p->relayed_ppm >= 0) {
            CHECK_EQ_I64(c.relay_ppm, p->relayed_ppm);
            CHECK_EQ_I64(c.relay_window, TERN_S(p->relayed_window_s));
        }
        /* Routing's part is the router's cap, as the router keeps it. */
        uint32_t learning = p->relay ? rc.learn_ppm : 0;
        CHECK_EQ_I64(rc.announce_ppm + rc.request_ppm, p->routing_ppm);
        CHECK_EQ_I64(learning, p->learning_ppm);
        CHECK_EQ_I64(c.own_ppm + (p->relay ? c.relay_ppm + learning : 0) + rc.announce_ppm +
                         rc.request_ppm,
                     p->most_ppm);
    }
}

static void test_classes(void) {
    for (size_t i = 0; i < COUNT(class_cases); i++) {
        const struct class_case *c = &class_cases[i];
        enum allowance fl = !tern_airtime_flooded(c->hdr) ? ALLOWANCE_NONE
                            : c->own                      ? ALLOWANCE_OWN
                                                          : ALLOWANCE_RELAY;
        CHECK_EQ_I64(tern_airtime_part(c->hdr, c->own), c->part);
        CHECK_EQ_I64(fl, c->allowance);
        CHECK_EQ_I64(tern_airtime_unpaid(c->hdr, c->own), c->unpaid);
    }
}

static void test_spends(void) {
    for (size_t i = 0; i < COUNT(spend_cases); i++) {
        const struct spend_case *c = &spend_cases[i];
        struct node x;
        start(&x, c->relay, c->profile);
        for (size_t k = 0; k < c->n; k++) {
            const struct spend_frame *f = &c->frames[k];
            CHECK_EQ_I64(tern_lora_airtime(&x.lora, f->len), f->airtime);
            if (offer(&x, f->at, f->hdr, f->own, f->airtime) != f->sent) {
                fprintf(stderr, "spends %s: frame %zu\n", c->name, k);
                check_failures++;
            }
        }
    }
}

static void test_estimates(void) {
    for (size_t i = 0; i < COUNT(estimate_cases); i++) {
        const struct estimate_case *c = &estimate_cases[i];
        struct node x;
        struct tern_airtime_waiting w[WAITING_MAX];
        uint16_t wait_s[WAITING_MAX];
        start(&x, c->relay, c->profile);
        for (size_t k = 0; k < c->n_spent; k++) {
            const struct spend_frame *f = &c->spent[k];
            (void)offer(&x, f->at, f->hdr, f->own, tern_lora_airtime(&x.lora, f->len));
        }
        CHECK(c->n_waiting <= WAITING_MAX);
        for (size_t k = 0; k < c->n_waiting && k < WAITING_MAX; k++) {
            CHECK_EQ_I64(tern_lora_airtime(&x.lora, c->waiting[k].len), c->waiting[k].airtime);
            w[k] = (struct tern_airtime_waiting){c->waiting[k].hdr, c->waiting[k].airtime};
        }
        struct node before = x;
        tern_airtime_waits(&x.a, &x.flood_own, c->at, w, c->n_waiting, wait_s);
        for (size_t k = 0; k < c->n_waiting && k < WAITING_MAX; k++) {
            if (wait_s[k] != c->waiting[k].wait_s) {
                fprintf(stderr, "estimates %s: frame %zu waits %u s, expected %u\n", c->name, k,
                        wait_s[k], c->waiting[k].wait_s);
                check_failures++;
            }
        }
        /* Asking spends nothing. */
        CHECK(memcmp(&before, &x, sizeof x) == 0);
    }
}

static void test_reasons(void) {
    for (size_t i = 0; i < COUNT(reason_cases); i++) {
        const struct reason_case *c = &reason_cases[i];
        uint16_t wait = 1;
        CHECK_EQ_I64(
            tern_airtime_reason(c->route, c->session, c->region_wait_s, c->budget_wait_s, &wait),
            c->reason);
        CHECK_EQ_I64(wait, c->wait);
    }
    CHECK_EQ_I64(TERN_C_WAIT_BUDGET, 4);
}

/* A bucket a very long time unfilled is full, not overflowed; and one charged past empty is empty,
 * not owing. */
static void a_bucket_stays_within_itself(void) {
    struct tern_airtime_bucket b;
    tern_airtime_bucket_init(&b, 80000, TERN_S(60), TERN_MS(400), 0);
    tern_airtime_bucket_charge(&b, 0, TERN_S(10));
    CHECK_EQ_I64(b.have, 0);
    CHECK_EQ_I64(tern_airtime_bucket_wait(&b, 0, TERN_MS(1)), TERN_MS(1) * 1000000 / 80000);
    CHECK(tern_airtime_bucket_holds(&b, INT64_MAX / 2, TERN_S(4)));
    CHECK_EQ_I64(b.have, b.most);
    tern_airtime_bucket_give(&b, INT64_MAX / 2, TERN_S(1));
    CHECK_EQ_I64(b.have, b.most);
}

/* A leaf has no part for frames passed on: it would drop any but an acknowledgement, and charges
 * nothing for one. */
static void a_leaf_passes_nothing_on(void) {
    struct tern_airtime a;
    struct tern_airtime_config c = tern_airtime_defaults();
    struct tern_lora l = tern_region_lora(tern_region(TERN_REGION_EU868));
    tern_airtime_init(&a, &c, &l, false, 0);
    CHECK(tern_airtime_bucket(&a, TERN_HDR_MESSAGE, false) == NULL);
    CHECK(!tern_airtime_pays(&a, 0, TERN_HDR_MESSAGE, false, TERN_MS(100)));
    CHECK(tern_airtime_pays(&a, 0, TERN_HDR_ACK, false, TERN_MS(60)));
    CHECK_EQ_I64(tern_airtime_pays_at(&a, 0, TERN_HDR_GROUP, false, TERN_MS(60)), INT64_MAX);
    CHECK(tern_airtime_pays(&a, 0, TERN_HDR_ANNOUNCE, true, TERN_S(100)));
}

/* --- The forwarder and the flooder, each holding to a budget --- */

#define NB 4
#define DESTS 4
#define SLOTS 8
#define ME 0x1000
#define NEXT 0x2000
#define FAR NEXT /* a route to the neighbour itself, which a node starting may use */

struct fwd {
    struct tern_route r;
    struct tern_forward f;
    struct tern_airtime a;
    struct tern_route_neighbour nb[NB];
    struct tern_route_dest dest[DESTS];
    struct tern_forward_slot slot[SLOTS];
    struct tern_lora lora;
};

/* A node with one neighbour, NEXT, whose link is up, and a route to it. */
static void fwd_start(struct fwd *x, bool relay) {
    memset(x, 0, sizeof *x);
    x->lora = tern_region_lora(tern_region(TERN_REGION_EU868));
    struct tern_route_config rc = tern_route_defaults(&x->lora, 22, -9, relay);
    struct tern_forward_config fc = tern_forward_defaults();
    struct tern_airtime_config ac = tern_airtime_defaults();
    tern_route_init(&x->r, &rc, ME, x->nb, NB, x->dest, DESTS, 0, 7, 0);
    x->nb[0] = (struct tern_route_neighbour){.id = NEXT, .used = true, .relay = true, .up = true};
    x->dest[0] = (struct tern_route_dest){.id = FAR, .used = true, .sel = 1};
    x->dest[0].e[0].slot = 1;
    tern_forward_init(&x->f, &fc, &x->r, x->slot, SLOTS, 3);
    tern_airtime_init(&x->a, &ac, &x->lora, relay, 0);
    tern_forward_budget(&x->f, &x->a);
}

/* Polls and says it went, at `now`. Returns the length, 0 for nothing, and the kind. */
static size_t fwd_go(struct fwd *x, tern_time now, enum tern_forward_kind *kind) {
    uint8_t frame[TERN_FORWARD_FRAME_MAX], h;
    int8_t dbm;
    size_t len = tern_forward_poll(&x->f, now, frame, &dbm, kind, &h);
    if (len != 0) {
        tern_forward_sent(&x->f, now, h);
    }
    return len;
}

static size_t message(uint8_t *frame, uint8_t which) {
    memset(frame, 0, 255);
    frame[0] = TERN_HDR_MESSAGE;
    frame[TERN_FORWARD_HEAD] = which;
    return 255;
}

/* A node's own messages wait for its own part, and the forwarder says when they will go: not
 * before the part holds the first of them. */
static void the_forwarder_holds_its_own(void) {
    struct fwd x;
    enum tern_forward_kind kind;
    uint8_t frame[255];
    fwd_start(&x, false);
    tern_time air = tern_lora_airtime(&x.lora, 255);
    /* 6 s of the own part goes in fourteen 255-byte frames; the next waits. */
    x.a.own.have = 0;
    CHECK(tern_forward_send(&x.f, 0, FAR, frame, message(frame, 1), false, INT8_MIN));
    CHECK(tern_forward_send(&x.f, 0, FAR, frame, message(frame, 2), false, INT8_MIN));
    CHECK_EQ_I64(fwd_go(&x, 0, &kind), 0);
    tern_time due = tern_forward_due(&x.f);
    CHECK_EQ_I64(due, air * 1000000 / 10000); /* 40.7808 s */
    struct tern_airtime_waiting w[SLOTS];
    uint8_t h[SLOTS];
    uint16_t wait_s[SLOTS];
    size_t n = tern_forward_own_waiting(&x.f, w, h, SLOTS);
    CHECK_EQ_I64(n, 2);
    tern_airtime_waits(&x.a, NULL, 0, w, n, wait_s);
    CHECK_EQ_I64(wait_s[0], 41);
    CHECK_EQ_I64(wait_s[1], 82);
    CHECK_EQ_I64(fwd_go(&x, due - 1, &kind), 0);
    CHECK_EQ_I64(fwd_go(&x, due, &kind), 255);
    CHECK_EQ_I64(kind, TERN_FORWARD_OWN);
    CHECK_EQ_I64(x.a.own.have, 0); /* charged as it went */
    /* An acknowledgement of its own goes with the part empty, and leaves it empty. */
    uint8_t ack[TERN_ACK_LEN] = {TERN_HDR_ACK};
    CHECK(tern_forward_send(&x.f, due, FAR, ack, sizeof ack, false, INT8_MIN));
    CHECK_EQ_I64(fwd_go(&x, due, &kind), TERN_ACK_LEN);
    CHECK_EQ_I64(kind, TERN_FORWARD_REPLY);
    CHECK_EQ_I64(x.a.own.have, 0);
}

/* A relay drops a frame passed on that its relayed part cannot pay for, and its own still go. */
static void the_forwarder_drops_what_it_cannot_pay_for(void) {
    struct fwd x;
    enum tern_forward_kind kind;
    uint8_t frame[255];
    struct tern_forward_heard got;
    fwd_start(&x, true);
    x.a.relayed.have = 0;
    message(frame, 7);
    tern_forward_head_write(&(struct tern_forward_head){TERN_HDR_MESSAGE, 5, 10, ME, FAR}, frame);
    tern_forward_heard(&x.f, 0, frame, 255, 20, &got);
    CHECK_EQ_I64(x.f.counts.passed_on, 1);
    CHECK(tern_forward_send(&x.f, 0, FAR, frame, message(frame, 1), false, INT8_MIN));
    tern_time later = TERN_S(1);
    CHECK_EQ_I64(fwd_go(&x, later, &kind), 255);
    CHECK_EQ_I64(kind, TERN_FORWARD_OWN);
    CHECK_EQ_I64(x.f.counts.unpaid, 1);
    CHECK_EQ_I64(fwd_go(&x, later, &kind), 0); /* nothing left to pass on */
}

/* A frame is paid for as it is handed over, so a caller taking two before either has gone cannot
 * have both paid for with the same airtime; one withdrawn is given back. */
static void the_forwarder_charges_as_it_hands_over(void) {
    struct fwd x;
    enum tern_forward_kind kind;
    uint8_t frame[255], out[TERN_FORWARD_FRAME_MAX], h;
    int8_t dbm;
    fwd_start(&x, false);
    tern_time air = tern_lora_airtime(&x.lora, 255);
    x.a.own.have = air * 1000000; /* one frame's worth */
    CHECK(tern_forward_send(&x.f, 0, FAR, frame, message(frame, 1), false, INT8_MIN));
    CHECK(tern_forward_send(&x.f, 0, FAR, frame, message(frame, 2), false, INT8_MIN));
    CHECK_EQ_I64(tern_forward_poll(&x.f, 0, out, &dbm, &kind, &h), 255);
    CHECK_EQ_I64(x.a.own.have, 0);
    uint8_t h2;
    CHECK_EQ_I64(tern_forward_poll(&x.f, 0, out, &dbm, &kind, &h2), 0);
    tern_forward_withdrawn(&x.f, 0, h);
    CHECK_EQ_I64(x.a.own.have, air * 1000000);
    CHECK_EQ_I64(tern_forward_poll(&x.f, 0, out, &dbm, &kind, &h2), 255);
    CHECK_EQ_I64(out[TERN_FORWARD_HEAD], 1); /* the same one, still first */
}

/* A group frame of the node's own waits for its allowance and the own part together: here the
 * part, which holds less. */
static void the_flooder_waits_for_both(void) {
    struct fwd x;
    struct tern_flood fl;
    struct tern_flood_slot fs[SLOTS];
    uint8_t seen[TERN_FLOOD_SEEN][TERN_FLOOD_ID];
    uint8_t frame[TERN_FLOOD_FRAME_MAX], h;
    int8_t dbm;
    enum tern_flood_kind kind;
    struct tern_flood_config fc = tern_flood_defaults();
    fwd_start(&x, false);
    tern_time air = tern_lora_airtime(&x.lora, 255);
    tern_flood_init(&fl, &fc, &x.r, fs, SLOTS, seen, TERN_FLOOD_SEEN, 5, 0);
    tern_flood_budget(&fl, &x.a);
    x.a.own.have = 0;
    memset(frame, 0, sizeof frame);
    frame[0] = TERN_HDR_GROUP;
    CHECK(tern_flood_send(&fl, 0, frame, 255));
    CHECK_EQ_I64(tern_flood_poll(&fl, 0, frame, &dbm, &kind, &h), 0);
    tern_time due = tern_flood_due(&fl);
    CHECK_EQ_I64(due, air * 1000000 / 10000);
    CHECK_EQ_I64(fl.own.have, fl.own.most); /* charged to neither */
    CHECK_EQ_I64(tern_flood_poll(&fl, due, frame, &dbm, &kind, &h), 255);
    CHECK_EQ_I64(x.a.own.have, 0);
    CHECK_EQ_I64(fl.own.have, fl.own.most - air * 1000000);
    /* Withdrawn, it is given back to both. */
    tern_flood_withdrawn(&fl, due, h);
    CHECK_EQ_I64(fl.own.have, fl.own.most);
    CHECK_EQ_I64(x.a.own.have, air * 1000000);
}

int main(void) {
    RUN(test_parts);
    RUN(test_classes);
    RUN(test_spends);
    RUN(test_estimates);
    RUN(test_reasons);
    RUN(a_bucket_stays_within_itself);
    RUN(a_leaf_passes_nothing_on);
    RUN(the_forwarder_holds_its_own);
    RUN(the_forwarder_drops_what_it_cannot_pay_for);
    RUN(the_forwarder_charges_as_it_hands_over);
    RUN(the_flooder_waits_for_both);
    return CHECK_DONE();
}
