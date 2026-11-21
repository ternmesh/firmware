#include <string.h>

#include "check.h"
#include "tern/forward.h"
#include "tern/region.h"
#include "tern/route.h"

/* The frames that follow routes (draft/forwarding.md), over routers that have settled: a handful
 * of nodes on a channel that takes no time and loses only what a test says. What a real channel
 * does to them is the simulator's to say (ternmesh/sim). */

#define NODES 6
#define NB 8
#define DESTS 8
#define SLOTS 4
#define NONE INT16_MIN
#define FULL 22
#define SNR_FLOOR_Q (-50) /* SF9 */
#define BODY 24           /* a tag, twelve bytes of message and a check */

struct head_case {
    struct tern_forward_head head;
    uint8_t tag[TERN_FORWARD_TAG];
    uint8_t frame[TERN_FORWARD_FRAME_MAX];
    size_t len;
};
struct rejected_case {
    const char *why;
    uint8_t frame[TERN_FORWARD_FRAME_MAX];
    size_t len;
};
struct hop_case {
    const char *why;
    uint8_t sent[TERN_FORWARD_FRAME_MAX];
    size_t sent_len;
    uint8_t heard[TERN_FORWARD_FRAME_MAX];
    size_t heard_len;
    bool ends;
};
struct wait_case {
    const char *why;
    uint8_t sent[TERN_FORWARD_FRAME_MAX];
    size_t len;
    bool listens;
};
struct entry_case {
    const char *why;
    uint8_t frame[TERN_FORWARD_FRAME_MAX];
    size_t len;
    uint32_t id;
    int16_t snr_q;
    uint8_t hops;
    uint8_t passed[TERN_FORWARD_FRAME_MAX]; /* from the destination on */
    size_t passed_len;                      /* 0: dropped */
};
struct answer_case {
    const char *why;
    uint8_t frame[TERN_FORWARD_FRAME_MAX];
    size_t len;
    uint32_t id;
    int16_t snr_q;
    uint8_t flags;
    uint8_t answer[TERN_FORWARD_FRAME_MAX]; /* from the destination on */
    size_t answer_len;                      /* 0: not answered */
};
struct back_case {
    int8_t power;
    int16_t snr_q;
    uint8_t sf;
    int8_t lowest, full, needs;
};
struct power_case {
    int8_t neighbour, back;
    uint8_t tries;
    int8_t full, power;
};

struct again_case {
    bool source; /* started again by its source, not sent again by a hop */
    uint8_t sf;
    uint32_t bw_hz;
    uint32_t len;
    tern_time airtime, longest;
};

struct acked_case {
    const char *why;
    uint8_t hops;
    bool leaf;
    uint32_t nearest, source; /* nearest 0: none */
    int n;
    struct {
        uint32_t next;
        bool relay, usable, feasible;
        uint16_t metric;
    } routes[4];
    int selected;   /* -1: none */
    uint32_t after; /* 0: none */
};

#include "forwarding.h"

#define COUNT(a) (sizeof(a) / sizeof(a)[0])

struct node {
    struct tern_route r;
    struct tern_forward f;
    struct tern_route_neighbour nb[NB];
    struct tern_route_dest dest[DESTS];
    struct tern_forward_slot slot[SLOTS];
    bool relay;
    bool deaf;      /* hears no frame that follows routes */
    bool silent;    /* acknowledges nothing */
    int got;        /* messages for it received, copies and all */
    int acked;      /* acknowledgements for its messages */
    int failed;     /* messages of its own given up on */
    int contacts;   /* first-contact frames for it received */
    int answered;   /* and of them, answers to frames of its own */
    int unanswered; /* first-contact frames of its own given up on */
    int traced;     /* answers to its traces */
    int untraced;   /* traces of its own given up on, and of them those never begun */
    int unbegun;
    uint8_t answer[TERN_FORWARD_FRAME_MAX]; /* the last answer to a trace of its own */
    size_t answer_len;
    int sent[3];     /* by kind */
    int8_t power[8]; /* what its last frames went at */
    int powers;
};

struct net {
    int n;
    struct node node[NODES];
    int16_t snr[NODES][NODES];
    struct tern_lora lora;
    tern_time now;
};

static struct net net;

static uint32_t id_of(int i) { return 0x1000u + (uint32_t)i; }

static void net_init(int n, const bool *relay) {
    memset(&net, 0, sizeof net);
    net.n = n;
    net.lora = tern_region_lora(tern_region(TERN_REGION_US915));
    for (int a = 0; a < n; a++) {
        struct node *x = &net.node[a];
        struct tern_route_config c;
        struct tern_forward_config fc = tern_forward_defaults();
        x->relay = relay ? relay[a] : true;
        c = tern_route_defaults(&net.lora, FULL, -9, x->relay);
        tern_route_init(&x->r, &c, id_of(a), x->nb, NB, x->dest, DESTS, 0,
                        0x9E3779B97F4A7C15ULL * (uint64_t)(a + 1), 0);
        tern_forward_init(&x->f, &fc, &x->r, x->slot, SLOTS, (uint64_t)(a + 7));
        for (int b = 0; b < n; b++) {
            net.snr[a][b] = NONE;
        }
    }
}

static void link(int a, int b) { net.snr[a][b] = net.snr[b][a] = 40; }

static void message(int from, int to, uint8_t tag) {
    uint8_t frame[TERN_FORWARD_HEAD + BODY] = {TERN_HDR_MESSAGE};
    memset(frame + TERN_FORWARD_HEAD, tag, TERN_FORWARD_TAG);
    frame[TERN_FORWARD_HEAD + TERN_FORWARD_TAG] = (uint8_t)from; /* a test's frame says who */
    CHECK(tern_forward_send(&net.node[from].f, net.now, id_of(to), frame, sizeof frame, true,
                            INT8_MIN));
}

/* A first-contact frame as a test has it: the right length, a tag, and who it is from where a
 * test's message says so. Its answer is the handshake's next frame, with the same tag. */
static void contact(int from, int to, uint8_t hdr, uint8_t tag, bool tracked, int8_t back) {
    uint8_t frame[TERN_CONTACT_LEN(TERN_HDR_CONTACT_1 + 2)] = {hdr};
    memset(frame + TERN_FORWARD_HEAD, tag, TERN_FORWARD_TAG);
    frame[TERN_FORWARD_HEAD + TERN_FORWARD_TAG] = (uint8_t)from;
    CHECK(tern_forward_send(&net.node[from].f, net.now, id_of(to), frame, TERN_CONTACT_LEN(hdr),
                            tracked, back));
}

static void on_air(int from, const uint8_t *frame, size_t len, int8_t dbm) {
    for (int k = 0; k < net.n; k++) {
        struct node *y = &net.node[k];
        int snr = net.snr[from][k] == NONE ? NONE : net.snr[from][k] + 4 * (dbm - FULL);
        if (k == from || snr == NONE || snr < SNR_FLOOR_Q) {
            continue;
        }
        if (tern_route_frame(frame, len)) {
            tern_route_heard(&y->r, net.now, frame, len, (int16_t)snr);
        } else if (!y->deaf) {
            struct tern_forward_heard got;
            tern_forward_heard(&y->f, net.now, frame, len, (int16_t)snr, &got);
            if (got.got == TERN_FORWARD_MESSAGE) {
                uint8_t ack[TERN_ACK_LEN] = {TERN_HDR_ACK};
                int who = frame[TERN_FORWARD_HEAD + TERN_FORWARD_TAG];
                y->got++;
                memcpy(ack + TERN_FORWARD_HEAD, frame + TERN_FORWARD_HEAD, TERN_FORWARD_TAG);
                if (!y->silent) {
                    tern_forward_send(&y->f, net.now, id_of(who), ack, sizeof ack, false, got.back);
                }
            } else if (got.got == TERN_FORWARD_ACK) {
                y->acked += tern_forward_acked(&y->f, frame + TERN_FORWARD_HEAD);
            } else if (got.got == TERN_FORWARD_TRACE) {
                if (!y->silent) {
                    (void)tern_forward_trace_answer(&y->f, net.now, frame, len, (int16_t)snr,
                                                    TERN_TRACE_BY_DESTINATION, got.back);
                }
            } else if (got.got == TERN_FORWARD_TRACED) {
                y->traced++;
                memcpy(y->answer, frame, len);
                y->answer_len = len;
            } else if (got.got == TERN_FORWARD_CONTACT) {
                int who = frame[TERN_FORWARD_HEAD + TERN_FORWARD_TAG];
                y->contacts++;
                if (frame[0] > TERN_HDR_CONTACT_1) {
                    y->answered += tern_forward_done(&y->f, (uint8_t)(frame[0] - 1),
                                                     frame + TERN_FORWARD_HEAD);
                }
                if (frame[0] < TERN_HDR_CONTACT_4 && !y->silent) {
                    contact(k, who, (uint8_t)(frame[0] + 1), frame[TERN_FORWARD_HEAD], false,
                            got.back);
                }
            }
        }
    }
}

static void run(tern_time until) {
    for (;;) {
        tern_time t = until + 1;
        for (int i = 0; i < net.n; i++) {
            const struct node *x = &net.node[i];
            tern_time due = tern_route_due(&x->r);
            due = tern_forward_due(&x->f) < due ? tern_forward_due(&x->f) : due;
            t = due < t ? due : t;
        }
        if (t > until) {
            break;
        }
        net.now = t > net.now ? t : net.now;
        for (int i = 0; i < net.n; i++) {
            struct node *x = &net.node[i];
            uint8_t frame[TERN_ROUTE_FRAME_MAX], tag[TERN_FORWARD_TAG], handle;
            enum tern_forward_kind kind;
            int8_t dbm = 0;
            size_t len;
            while (tern_route_due(&x->r) <= net.now &&
                   (len = tern_route_poll(&x->r, net.now, frame, &dbm)) != 0) {
                tern_route_sent(&x->r, net.now);
                on_air(i, frame, len, dbm);
            }
            while ((len = tern_forward_poll(&x->f, net.now, frame, &dbm, &kind, &handle)) != 0) {
                CHECK(dbm >= -9 && dbm <= FULL);
                x->sent[kind]++;
                x->power[x->powers++ % 8] = dbm;
                tern_forward_sent(&x->f, net.now, handle);
                on_air(i, frame, len, dbm);
            }
            while (tern_forward_failed(&x->f, tag)) {
                x->failed++;
            }
            while (tern_forward_contact_failed(&x->f, &handle, tag)) {
                x->unanswered++;
            }
            uint32_t to;
            bool began;
            while (tern_forward_trace_failed(&x->f, tag, &to, &began)) {
                x->untraced++;
                x->unbegun += !began;
            }
        }
    }
    net.now = until;
}

static int in_hand(int i) {
    int n = 0;
    for (int k = 0; k < SLOTS; k++) {
        n += net.node[i].slot[k].state != TERN_FORWARD_FREE;
    }
    return n;
}

/* 0 - 1 - 2 - 3, settled. */
static void line(void) {
    net_init(4, NULL);
    link(0, 1);
    link(1, 2);
    link(2, 3);
    run(TERN_S(1200));
}

static void heads_are_written_and_read(void) {
    for (size_t i = 0; i < COUNT(heads); i++) {
        const struct head_case *c = &heads[i];
        uint8_t frame[TERN_FORWARD_HEAD];
        struct tern_forward_head h;
        tern_forward_head_write(&c->head, frame);
        CHECK(memcmp(frame, c->frame, sizeof frame) == 0);
        CHECK(tern_forward_head_read(&h, c->frame, c->len));
        CHECK(h.hdr == c->head.hdr && h.hops == c->head.hops && h.power == c->head.power &&
              h.next == c->head.next && h.destination == c->head.destination);
        CHECK(memcmp(c->frame + TERN_FORWARD_HEAD, c->tag, TERN_FORWARD_TAG) == 0);
    }
}

/* A forwarder that takes a frame takes a slot for it, or says it is for this node: one that
 * discards it does neither. */
static void frames_the_specification_rejects_change_nothing(void) {
    for (size_t i = 0; i < COUNT(rejected); i++) {
        struct tern_forward_head h;
        struct tern_forward_heard got;
        if (tern_forward_head_read(&h, rejected[i].frame, rejected[i].len)) {
            fprintf(stderr, "read %s\n", rejected[i].why);
            check_failures++;
        }
        net_init(1, NULL);
        tern_forward_heard(&net.node[0].f, 0, rejected[i].frame, rejected[i].len, 40, &got);
        CHECK(got.got == TERN_FORWARD_NOTHING && net.node[0].slot[0].state == TERN_FORWARD_FREE);
    }
}

static void a_hop_ends_when_the_specification_says(void) {
    for (size_t i = 0; i < COUNT(hop_cases); i++) {
        const struct hop_case *c = &hop_cases[i];
        if (tern_forward_ends(c->sent, c->sent_len, c->heard, c->heard_len) != c->ends) {
            fprintf(stderr, "wrong for %s\n", c->why);
            check_failures++;
        }
    }
}

static void a_node_listens_when_the_specification_says(void) {
    for (size_t i = 0; i < COUNT(wait_cases); i++) {
        const struct wait_case *c = &wait_cases[i];
        if (tern_forward_listens(c->sent, c->len) != c->listens) {
            fprintf(stderr, "wrong for %s\n", c->why);
            check_failures++;
        }
    }
}

/* Two messages for one node with one tag are two messages: a relay holding the first takes the
 * second, and hearing the second passed on does not end the first's hop. */
static void messages_that_share_a_tag_are_told_apart(void) {
    uint8_t one[TERN_FORWARD_HEAD + BODY] = {0}, two[TERN_FORWARD_HEAD + BODY] = {0};
    struct tern_forward_head h = {TERN_HDR_MESSAGE, 32, FULL, 0, 0};
    line();
    h.next = id_of(1);
    h.destination = id_of(3);
    tern_forward_head_write(&h, one);
    tern_forward_head_write(&h, two);
    memset(one + TERN_FORWARD_HEAD, 0x77, TERN_FORWARD_TAG);
    memset(two + TERN_FORWARD_HEAD, 0x77, TERN_FORWARD_TAG);
    two[sizeof two - 1] = 1;
    tern_forward_heard(&net.node[1].f, net.now, one, sizeof one, 40,
                       &(struct tern_forward_heard){0});
    tern_forward_heard(&net.node[1].f, net.now, two, sizeof two, 40,
                       &(struct tern_forward_heard){0});
    CHECK_EQ_I64(in_hand(1), 2);
    one[1] = 31;
    two[1] = 30;
    CHECK(!tern_forward_ends(one, sizeof one, two, sizeof two));
}

/* Slots come as the caller's memory was left, and no more are used than a handle can name. */
static void slots_need_not_be_cleared_and_are_no_more_than_a_handle_names(void) {
    static struct tern_forward_slot many[300];
    struct tern_forward f;
    struct tern_forward_config fc = tern_forward_defaults();
    uint8_t frame[TERN_FORWARD_HEAD + BODY] = {0};
    struct tern_forward_head h = {TERN_HDR_MESSAGE, 31, FULL, 0, 0};
    struct tern_forward_heard got;
    line();
    memset(many, 0xA5, sizeof many);
    tern_forward_init(&f, &fc, &net.node[1].r, many, 300, 1);
    CHECK_EQ_I64(f.cap, TERN_FORWARD_SLOTS_MAX);
    h.next = id_of(1);
    h.destination = id_of(3);
    tern_forward_head_write(&h, frame);
    tern_forward_heard(&f, net.now, frame, sizeof frame, 40, &got);
    CHECK_EQ_I64(f.counts.passed_on, 1);
    CHECK(many[TERN_FORWARD_SLOTS_MAX].state == 0xA5); /* past the last used: untouched */
}

static void an_answer_goes_loud_enough_for_the_node_it_answers(void) {
    for (size_t i = 0; i < COUNT(backs); i++) {
        const struct back_case *c = &backs[i];
        struct tern_lora lora = tern_region_lora(tern_region(TERN_REGION_US915));
        struct tern_route_config config;
        struct tern_route_neighbour nb[1];
        struct tern_route_dest dest[1];
        struct tern_route r;
        lora.sf = c->sf;
        config = tern_route_defaults(&lora, c->full, c->lowest, true);
        tern_route_init(&r, &config, 0x1000, nb, 1, dest, 1, 0, 7, 0);
        CHECK_EQ_I64(tern_route_power_back(&r, c->power, c->snr_q), c->needs);
    }
}

static void a_frame_goes_louder_each_time(void) {
    for (size_t i = 0; i < COUNT(power_cases); i++) {
        const struct power_case *c = &power_cases[i];
        CHECK_EQ_I64(tern_forward_power(c->neighbour, c->back, c->tries, 3, c->full), c->power);
    }
}

static void a_message_goes_along_the_line_and_is_acknowledged(void) {
    line();
    message(0, 3, 0x11);
    run(TERN_S(1260));
    CHECK_EQ_I64(net.node[3].got, 1);
    CHECK_EQ_I64(net.node[0].acked, 1);
    CHECK_EQ_I64(net.node[0].failed, 0);
    /* Sent once by each node on the way, and the acknowledgement once by each on the way back. */
    CHECK_EQ_I64(net.node[0].sent[TERN_FORWARD_OWN], 1);
    CHECK_EQ_I64(net.node[1].sent[TERN_FORWARD_RELAY], 1);
    CHECK_EQ_I64(net.node[2].sent[TERN_FORWARD_RELAY], 1);
    CHECK_EQ_I64(net.node[3].sent[TERN_FORWARD_REPLY], 1);
    CHECK_EQ_I64(net.node[2].sent[TERN_FORWARD_REPLY], 1);
    CHECK_EQ_I64(net.node[1].sent[TERN_FORWARD_REPLY], 1);
    for (int i = 0; i < 4; i++) {
        CHECK_EQ_I64(in_hand(i), 0);
    }
}

/* A handshake's four frames cross two relays, two each way, hop by hop as a message does. The
 * last hop of each is sent once: nothing is heard after it. */
static void first_contact_goes_along_the_line_and_back(void) {
    line();
    contact(0, 3, TERN_HDR_CONTACT_1, 0x51, true, INT8_MIN);
    run(net.now + TERN_S(60));
    CHECK_EQ_I64(net.node[3].contacts, 2);
    CHECK_EQ_I64(net.node[0].contacts, 2);
    CHECK_EQ_I64(net.node[0].answered, 1); /* its first frame, which it kept, by the second */
    CHECK_EQ_I64(net.node[0].sent[TERN_FORWARD_OWN], 2);
    CHECK_EQ_I64(net.node[3].sent[TERN_FORWARD_OWN], 2);
    CHECK_EQ_I64(net.node[1].sent[TERN_FORWARD_RELAY], 4);
    CHECK_EQ_I64(net.node[2].sent[TERN_FORWARD_RELAY], 4);
    for (int i = 0; i < 4; i++) {
        CHECK_EQ_I64(net.node[i].f.counts.sent_again, 0);
        CHECK_EQ_I64(net.node[i].f.counts.given_up, 0);
        CHECK_EQ_I64(in_hand(i), 0);
    }
}

/* A node that does not answer is asked as often as a message is sent, and then no more. Its
 * silence says nothing of the link to it: a node may choose not to answer. */
static void first_contact_unanswered_is_tried_again_and_no_hop_is_given_up(void) {
    net_init(2, NULL);
    link(0, 1);
    run(TERN_S(1200));
    net.node[1].silent = true;
    contact(0, 1, TERN_HDR_CONTACT_1 + 2, 0x53, true, INT8_MIN);
    run(net.now + TERN_S(120));
    CHECK_EQ_I64(net.node[0].sent[TERN_FORWARD_OWN], 4);
    CHECK_EQ_I64(net.node[1].contacts, 4);
    CHECK_EQ_I64(net.node[0].unanswered, 1);
    CHECK_EQ_I64(net.node[0].failed, 0);
    CHECK_EQ_I64(net.node[0].f.counts.sent_again, 0);
    CHECK_EQ_I64(net.node[0].f.counts.given_up, 0);
    CHECK_EQ_I64(in_hand(0), 0);
}

/* A relay that hears nothing of a first-contact frame it passed on sends it again, as it does a
 * message; and the node that began it starts it again when no answer comes. */
static void first_contact_lost_on_the_way_is_sent_again(void) {
    line();
    net.node[2].deaf = true;
    contact(0, 3, TERN_HDR_CONTACT_1, 0x51, true, INT8_MIN);
    run(net.now + TERN_S(20));
    CHECK(net.node[1].f.counts.sent_again >= 1);
    CHECK_EQ_I64(net.node[3].contacts, 0);
    net.node[2].deaf = false;
    run(net.now + TERN_S(120));
    CHECK(net.node[3].contacts >= 1);
    CHECK_EQ_I64(net.node[0].answered, 1);
    CHECK_EQ_I64(net.node[0].unanswered, 0);
    CHECK_EQ_I64(in_hand(0), 0);
}

static void a_leaf_passes_nothing_on(void) {
    static const bool relay[] = {true, false, true};
    net_init(3, relay);
    link(0, 1);
    link(1, 2);
    run(TERN_S(1200));
    {
        /* Made by hand: node 0 has no route through a leaf to send it by. */
        uint8_t frame[TERN_FORWARD_HEAD + BODY] = {0};
        struct tern_forward_head h = {TERN_HDR_MESSAGE, 32, FULL, id_of(1), id_of(2)};
        tern_forward_head_write(&h, frame);
        on_air(0, frame, sizeof frame, FULL);
    }
    run(TERN_S(1260));
    CHECK_EQ_I64(net.node[1].sent[TERN_FORWARD_RELAY], 0);
    CHECK_EQ_I64(net.node[2].got, 0);
}

static void a_hop_unheard_is_sent_again_louder_and_then_given_up(void) {
    line();
    net.node[1].deaf = true;
    message(0, 3, 0x22);
    run(net.now + TERN_S(4)); /* less than the wait for its acknowledgement */
    CHECK_EQ_I64(net.node[0].sent[TERN_FORWARD_OWN], 1);
    run(net.now + TERN_S(1)); /* the hop's wait has passed */
    CHECK_EQ_I64(net.node[0].sent[TERN_FORWARD_OWN], 2);
    CHECK_EQ_I64(net.node[0].power[1], net.node[0].power[0] + 3);
    run(net.now + TERN_S(120));
    /* Four tries of the message, none acknowledged, and then it is given up. */
    CHECK_EQ_I64(net.node[0].failed, 1);
    CHECK_EQ_I64(net.node[0].acked, 0);
    CHECK_EQ_I64(net.node[3].got, 0);
    CHECK_EQ_I64(in_hand(0), 0);
    CHECK(net.node[0].sent[TERN_FORWARD_OWN] >= 4 && net.node[0].sent[TERN_FORWARD_OWN] <= 12);
}

/* How long node 0's forwarder, started again with this seed and jitter, holds a frame back once
 * its hop's wait has passed with nothing heard. */
static tern_time held_back(uint64_t seed, uint8_t retry_jitter, bool source, size_t *len) {
    struct node *x = &net.node[0];
    struct tern_forward_config fc = tern_forward_defaults();
    uint8_t frame[TERN_FORWARD_FRAME_MAX], handle;
    enum tern_forward_kind kind;
    int8_t dbm;
    fc.retry_jitter = retry_jitter;
    tern_forward_init(&x->f, &fc, &x->r, x->slot, SLOTS, seed);
    message(0, 3, 0x44);
    *len = tern_forward_poll(&x->f, net.now, frame, &dbm, &kind, &handle);
    CHECK(*len != 0);
    tern_forward_sent(&x->f, net.now, handle);
    if (source) {
        /* Its next hop is heard passing it on, so what is waited for is its acknowledgement. */
        struct tern_forward_heard got;
        frame[1]--;
        tern_forward_heard(&x->f, net.now, frame, *len, 40, &got);
    }
    tern_time waited = tern_forward_due(&x->f); /* when the hop, or the message, is given up on */
    CHECK(waited > net.now + (source ? TERN_S(5) : 0));
    if (tern_forward_poll(&x->f, waited, frame, &dbm, &kind, &handle) != 0) {
        return 0; /* sent again at once */
    }
    return tern_forward_due(&x->f) - waited;
}

/* The specification's RETRY_JITTER: two nodes whose frames met wait as long as each other, and
 * would meet again at every try if each did not then wait a time of its own. */
static void a_frame_sent_again_waits_a_time_of_its_own(void) {
    tern_time first = -1;
    bool differ = false;
    size_t len;
    line();
    for (int source = 0; source < 2; source++) {
        first = -1;
        differ = false;
        for (uint64_t seed = 1; seed <= 32; seed++) {
            tern_time held = held_back(seed, tern_forward_defaults().retry_jitter, source, &len);
            tern_time air = tern_lora_airtime(&net.lora, (uint32_t)len);
            CHECK(held >= 0 && held <= 4 * air);
            differ = differ || (first >= 0 && held != first);
            first = held;
            CHECK_EQ_I64(held_back(seed, 0, source, &len), 0);
        }
        CHECK(differ);
    }
    CHECK_EQ_I64(tern_forward_defaults().retry_jitter, 4);
}

/* agains: the longest a frame may wait is RETRY_JITTER of its airtimes, as the core reckons
 * airtime; and on the profile and the frame these tests run with, no wait is longer, for a hop
 * sent again or a message started again. */
static void a_wait_is_no_longer_than_the_specification_allows(void) {
    int at_this_profile = 0;
    line();
    for (size_t i = 0; i < COUNT(again_cases); i++) {
        const struct again_case *c = &again_cases[i];
        struct tern_lora l = net.lora;
        l.sf = c->sf;
        l.bw_hz = c->bw_hz;
        l.ldro = TERN_LDRO_AUTO;
        CHECK_EQ_I64(tern_lora_airtime(&l, c->len), c->airtime);
        CHECK_EQ_I64(tern_forward_defaults().retry_jitter * tern_lora_airtime(&l, c->len),
                     c->longest);
        if (c->sf != net.lora.sf || c->bw_hz != net.lora.bw_hz ||
            c->len != TERN_FORWARD_HEAD + BODY) {
            continue;
        }
        at_this_profile++;
        for (uint64_t seed = 1; seed <= 64; seed++) {
            size_t len;
            tern_time held = held_back(seed, tern_forward_defaults().retry_jitter, c->source, &len);
            CHECK_EQ_U64(len, c->len);
            CHECK(held >= 0 && held <= c->longest);
        }
    }
    CHECK_EQ_I64(at_this_profile, 2); /* the hop's case and the source's */
}

static void a_message_unacknowledged_is_sent_again_and_every_copy_arrives(void) {
    line();
    net.node[3].silent = true;
    message(0, 3, 0x33);
    run(net.now + TERN_S(120));
    CHECK_EQ_I64(net.node[0].failed, 1);
    /* Node 2 sends what it is given three times, hearing nothing back, and takes no second copy
     * while it still has the first: so threes, and fewer than the source's four tries make. */
    CHECK(net.node[3].got >= 3 && net.node[3].got < 4 * 3 && net.node[3].got % 3 == 0);
    CHECK(net.node[2].f.counts.given_up >= 1);
    CHECK_EQ_I64(in_hand(0) + in_hand(1) + in_hand(2), 0);
}

/* 0 reaches 3 through 1 or through 2. */
static void a_frame_given_up_on_goes_another_way(void) {
    static const bool relay[] = {true, true, true, true};
    int first, other;
    net_init(4, relay);
    link(0, 1);
    link(0, 2);
    link(1, 3);
    link(2, 3);
    run(TERN_S(1200));
    {
        uint32_t next;
        uint16_t metric;
        CHECK(tern_route_next(&net.node[0].r, id_of(3), &next, &metric));
        first = (int)(next - 0x1000u);
        other = first == 1 ? 2 : 1;
    }
    /* Node 0 passes a frame on for a neighbour, so that only the hop's own wait is at work. */
    net.node[first].deaf = true;
    {
        uint8_t frame[TERN_FORWARD_HEAD + BODY] = {0};
        struct tern_forward_head h = {TERN_HDR_MESSAGE, 32, FULL, id_of(0), id_of(3)};
        tern_forward_head_write(&h, frame);
        memset(frame + TERN_FORWARD_HEAD, 0x44, TERN_FORWARD_TAG);
        frame[TERN_FORWARD_HEAD + TERN_FORWARD_TAG] = 0;
        net.node[3].silent = true;
        tern_forward_heard(&net.node[0].f, net.now, frame, sizeof frame, 40,
                           &(struct tern_forward_heard){0});
    }
    run(net.now + TERN_S(60));
    CHECK_EQ_I64(net.node[0].sent[TERN_FORWARD_RELAY], 3 + 1); /* three to the first, one on */
    CHECK_EQ_I64(net.node[0].f.counts.salvaged, 1);
    CHECK_EQ_I64(net.node[other].sent[TERN_FORWARD_RELAY] > 0, 1);
    CHECK(net.node[3].got >= 1);
}

static void a_frame_already_being_passed_on_is_not_taken_twice(void) {
    uint8_t frame[TERN_FORWARD_HEAD + BODY] = {0};
    struct tern_forward_head h = {TERN_HDR_MESSAGE, 32, FULL, 0, 0};
    line();
    h.next = id_of(1);
    h.destination = id_of(3);
    tern_forward_head_write(&h, frame);
    memset(frame + TERN_FORWARD_HEAD, 0x55, TERN_FORWARD_TAG);
    for (int k = 0; k < 3; k++) {
        tern_forward_heard(&net.node[1].f, net.now, frame, sizeof frame, 40,
                           &(struct tern_forward_heard){0});
    }
    CHECK_EQ_I64(in_hand(1), 1);
    CHECK_EQ_I64(net.node[1].f.counts.passed_on, 1);
}

static void a_frame_that_has_come_far_enough_stops(void) {
    uint8_t frame[TERN_FORWARD_HEAD + BODY] = {0};
    struct tern_forward_head h = {TERN_HDR_MESSAGE, 1, FULL, 0, 0};
    line();
    h.next = id_of(1);
    h.destination = id_of(3);
    tern_forward_head_write(&h, frame);
    tern_forward_heard(&net.node[1].f, net.now, frame, sizeof frame, 40,
                       &(struct tern_forward_heard){0});
    CHECK_EQ_I64(in_hand(1), 0);
    CHECK_EQ_I64(net.node[1].f.counts.hop_limit, 1);
}

static void a_message_with_no_route_waits_and_is_given_up(void) {
    net_init(2, NULL);
    run(TERN_S(600)); /* neither hears the other */
    message(0, 1, 0x66);
    CHECK_EQ_I64(in_hand(0), 1);
    run(net.now + TERN_S(60));
    CHECK_EQ_I64(net.node[0].failed, 1);
    CHECK_EQ_I64(net.node[0].sent[TERN_FORWARD_OWN], 0);
    CHECK_EQ_I64(in_hand(0), 0);
}

static void a_relay_adds_its_entry_as_the_specification_says(void) {
    for (size_t i = 0; i < COUNT(entry_cases); i++) {
        const struct entry_case *c = &entry_cases[i];
        uint8_t out[TERN_FORWARD_FRAME_MAX];
        struct tern_forward_head h;
        size_t n;
        CHECK(tern_forward_head_read(&h, c->frame, c->len));
        n = tern_forward_entry_pass(c->frame, c->len, c->id, c->snr_q, out);
        if (n != (c->passed_len ? c->passed_len + 7 : 0) ||
            (n && (out[1] != c->hops || memcmp(out + 7, c->passed, c->passed_len) != 0))) {
            fprintf(stderr, "wrong for %s\n", c->why);
            check_failures++;
        }
    }
}

static void a_trace_is_answered_as_the_specification_says(void) {
    for (size_t i = 0; i < COUNT(answer_cases); i++) {
        const struct answer_case *c = &answer_cases[i];
        uint8_t out[TERN_FORWARD_FRAME_MAX];
        size_t n = tern_forward_answer_write(c->frame, c->len, c->id, c->snr_q, c->flags, 32, out);
        if (n != (c->answer_len ? c->answer_len + 7 : 0) ||
            (n && (out[1] != 32 || memcmp(out + 7, c->answer, c->answer_len) != 0))) {
            fprintf(stderr, "wrong for %s\n", c->why);
            check_failures++;
        }
    }
}

static uint32_t entry_id(const uint8_t *frame, size_t k) {
    const uint8_t *e = frame + TERN_TRACE_ANSWER_MIN + k * TERN_TRACE_ENTRY;
    return (uint32_t)e[0] << 24 | (uint32_t)e[1] << 16 | (uint32_t)e[2] << 8 | e[3];
}

static void trace(int from, int to, uint8_t tag) {
    uint8_t t[TERN_FORWARD_TAG];
    memset(t, tag, sizeof t);
    CHECK(tern_forward_trace(&net.node[from].f, net.now, id_of(to), t));
}

/* Out across two relays and back: each node on the way adds its entry, the destination its own
 * last on the way out, and the source, which ends the trace, has the whole of both ways. */
static void a_trace_goes_along_the_line_and_back(void) {
    line();
    trace(0, 3, 0x71);
    CHECK(tern_forward_tracing(&net.node[0].f, id_of(3)));
    run(net.now + TERN_S(60));
    CHECK_EQ_I64(net.node[0].traced, 1);
    CHECK_EQ_I64(net.node[0].untraced, 0);
    CHECK(!tern_forward_tracing(&net.node[0].f, id_of(3)));
    CHECK_EQ_I64(net.node[0].answer[TERN_FORWARD_MIN], TERN_TRACE_BY_DESTINATION);
    CHECK_EQ_I64(net.node[0].answer[TERN_FORWARD_MIN + 1], 3);
    CHECK_EQ_I64(net.node[0].answer_len, TERN_TRACE_ANSWER_MIN + 5 * TERN_TRACE_ENTRY);
    CHECK_EQ_I64(entry_id(net.node[0].answer, 0), id_of(1));
    CHECK_EQ_I64(entry_id(net.node[0].answer, 1), id_of(2));
    CHECK_EQ_I64(entry_id(net.node[0].answer, 2), id_of(3));
    CHECK_EQ_I64(entry_id(net.node[0].answer, 3), id_of(2));
    CHECK_EQ_I64(entry_id(net.node[0].answer, 4), id_of(1));
    for (int i = 0; i < 4; i++) {
        CHECK_EQ_I64(net.node[i].f.counts.sent_again, 0);
        CHECK_EQ_I64(in_hand(i), 0);
    }
}

/* One trace to a destination at a time, and a trace not answered is given up, once, not begun
 * again. */
static void a_trace_unanswered_is_given_up_once(void) {
    uint8_t t[TERN_FORWARD_TAG] = {0x72, 0x72, 0x72, 0x72};
    line();
    net.node[3].silent = true;
    trace(0, 3, 0x72);
    CHECK(!tern_forward_trace(&net.node[0].f, net.now, id_of(3), t));
    run(net.now + TERN_S(120));
    CHECK_EQ_I64(net.node[0].traced, 0);
    CHECK_EQ_I64(net.node[0].untraced, 1);
    CHECK_EQ_I64(net.node[0].unbegun, 0);
    CHECK_EQ_I64(net.node[0].sent[TERN_FORWARD_OWN], 1);
    CHECK_EQ_I64(in_hand(0), 0);
}

static void a_trace_with_no_route_is_not_begun(void) {
    net_init(2, NULL);
    run(TERN_S(600)); /* neither hears the other */
    trace(0, 1, 0x73);
    run(net.now + TERN_S(60));
    CHECK_EQ_I64(net.node[0].untraced, 1);
    CHECK_EQ_I64(net.node[0].unbegun, 1);
    CHECK_EQ_I64(net.node[0].sent[TERN_FORWARD_OWN], 0);
    CHECK_EQ_I64(in_hand(0), 0);
}

/* A relay with no route on answers, from where the trace got to, with its own entry last. */
static void a_relay_with_no_route_answers(void) {
    uint8_t frame[TERN_TRACE_MIN] = {0};
    struct tern_forward_head h = {TERN_HDR_TRACE, 32, FULL, id_of(1), 0x7777u};
    struct tern_forward_heard got;
    line();
    tern_forward_head_write(&h, frame);
    memset(frame + TERN_FORWARD_HEAD, 0x74, TERN_FORWARD_TAG);
    frame[TERN_TRACE_FROM + 2] = 0x10; /* from id_of(0) */
    tern_forward_heard(&net.node[1].f, net.now, frame, sizeof frame, 40, &got);
    CHECK_EQ_I64(got.got, TERN_FORWARD_NOTHING);
    CHECK_EQ_I64(in_hand(1), 1);
    run(net.now + TERN_S(10));
    CHECK_EQ_I64(net.node[0].traced, 0); /* no trace of its own: an answer it ignores */
    CHECK_EQ_I64(net.node[1].sent[TERN_FORWARD_OWN], 1);
    CHECK_EQ_I64(in_hand(1), 0);
}

/* No more than TERN_TRACE_ANSWERS a minute, and as many again a minute later. */
/* The vector's destination, its routes to the message's source each through a neighbour of its
 * own, sends its acknowledgement to the neighbour it says. Feasibility makes no difference. */
static void an_acknowledgement_goes_the_way_the_specification_says(void) {
    struct tern_lora lora = tern_region_lora(tern_region(TERN_REGION_US915));
    for (size_t i = 0; i < COUNT(acked_cases); i++) {
        const struct acked_case *c = &acked_cases[i];
        struct tern_route_config config = tern_route_defaults(&lora, 22, -9, !c->leaf);
        struct tern_route_neighbour nb[6];
        struct tern_route_dest dest[2];
        struct tern_route r;
        uint32_t next = 0;
        uint16_t metric;
        bool sent;
        int n = 0;
        if (!c->nearest) {
            config.default_hops = 0; /* takes no default route */
        }
        tern_route_init(&r, &config, 0x77777777u, nb, 6, dest, 2, 0, 1, 0);
        r.starting = 0;
        dest[0] = (struct tern_route_dest){.id = c->source, .used = true};
        for (int k = 0; k < c->n; k++) {
            nb[n] = (struct tern_route_neighbour){.id = c->routes[k].next,
                                                  .used = true,
                                                  .relay = c->routes[k].relay,
                                                  .up = true,
                                                  .out_of_use = !c->routes[k].usable,
                                                  .theirs = 200,
                                                  .floor = 16 * 10,
                                                  .promise = TERN_S(600)};
            n++;
            dest[0].e[k] =
                (struct tern_route_entry){.slot = (uint8_t)n,
                                          .seq = c->routes[k].feasible ? 2 : 1,
                                          .metric = (uint16_t)(c->routes[k].metric - r.cost)};
        }
        dest[0].fd = (struct tern_route_fd){.has = true, .seq = 1, .metric = 0};
        dest[0].sel = c->selected < 0 ? 0 : (uint8_t)(c->selected + 1);
        if (c->nearest) {
            bool kept = false;
            for (int k = 0; k < n; k++) {
                kept |= nb[k].id == c->nearest;
            }
            if (!kept) {
                /* The nearest relay, which the destination holds no route through. */
                nb[n++] = (struct tern_route_neighbour){.id = c->nearest,
                                                        .used = true,
                                                        .relay = true,
                                                        .up = true,
                                                        .theirs = 200,
                                                        .floor = 16 * -20,
                                                        .promise = TERN_S(600)};
            }
        }
        sent = tern_route_back(&r, c->source, c->hops < 32, &next, &metric);
        CHECK(sent == (c->after != 0));
        CHECK(!sent || next == c->after);
    }
}

/* The first field test's drive: a relay at a desk (0), a relay on a patio (1) and a leaf (2) start
 * together, every link strong, and then the leaf goes where the desk does not hear it, nor it the
 * desk, and the patio hears both. The leaf no longer uses the desk, and hands what it has to its
 * nearest relay; the desk still holds its link to the leaf up. */
static const struct tern_route_neighbour *kept_by(int at, int i) {
    for (int k = 0; k < NB; k++) {
        if (net.node[at].nb[k].used && net.node[at].nb[k].id == id_of(i)) {
            return &net.node[at].nb[k];
        }
    }
    return NULL;
}

static void drive(void) {
    static const bool relay[] = {true, true, false};
    net_init(3, relay);
    link(0, 1);
    link(0, 2);
    link(1, 2);
    run(TERN_S(1200));
    net.snr[0][2] = net.snr[2][0] = NONE;
    for (int k = 0; k < NB; k++) {
        if (net.node[2].nb[k].used && net.node[2].nb[k].id == id_of(0)) {
            net.node[2].nb[k].out_of_use = true; /* as its solicits would have it */
        }
    }
}

static void an_acknowledgement_goes_back_the_way_the_message_came(void) {
    uint32_t next;
    uint16_t metric;
    drive();
    CHECK(tern_route_next(&net.node[0].r, id_of(2), &next, &metric) && next == id_of(2));
    message(2, 0, 0x61);
    run(net.now + TERN_S(30));
    CHECK_EQ_I64(net.node[0].got, 1);   /* through the patio */
    CHECK_EQ_I64(net.node[2].acked, 1); /* and its acknowledgement back that way */
    CHECK_EQ_I64(net.node[2].failed, 0);
}

static void a_leaf_gone_quiet_is_taken_out_of_use_and_reached_another_way(void) {
    const struct tern_route_neighbour *leaf;
    tern_time quiet;
    drive();
    leaf = kept_by(0, 2);
    CHECK(leaf != NULL && leaf->up && !leaf->out_of_use);
    quiet = leaf->heard + 2 * leaf->promise;
    /* Lost: the desk sends them direct, and gives hops up there, two for each. */
    message(0, 2, 0x62);
    run(net.now + TERN_S(120));
    message(0, 2, 0x64);
    run(net.now + TERN_S(120));
    CHECK_EQ_I64(net.node[2].got, 0);
    CHECK_EQ_I64(net.node[0].failed, 2);
    CHECK(leaf->lost >= 3);
    CHECK(!leaf->out_of_use || net.now > quiet);
    run(quiet + TERN_S(1));
    CHECK(leaf->out_of_use);
    CHECK(leaf->up); /* its link kept */
    /* The desk asks after the leaf; its newer seq comes back through the patio. */
    run(net.now + TERN_S(120));
    message(0, 2, 0x63);
    run(net.now + TERN_S(60));
    CHECK_EQ_I64(net.node[2].got, 1);
    CHECK_EQ_I64(net.node[0].acked, 1);
}

static void a_node_answers_few_traces_a_minute(void) {
    uint8_t frame[TERN_TRACE_MIN] = {0};
    struct tern_forward_head h = {TERN_HDR_TRACE, 32, FULL, id_of(1), id_of(1)};
    line();
    tern_forward_head_write(&h, frame);
    frame[TERN_TRACE_FROM + 2] = 0x10;
    for (int k = 0; k < TERN_TRACE_ANSWERS + 1; k++) {
        frame[TERN_FORWARD_HEAD] = (uint8_t)k;
        CHECK_EQ_I64(tern_forward_trace_answer(&net.node[1].f, net.now, frame, sizeof frame, 40,
                                               TERN_TRACE_BY_DESTINATION, INT8_MIN),
                     k < TERN_TRACE_ANSWERS);
        run(net.now + TERN_S(2));
    }
    /* Each kind counted apart. */
    CHECK(tern_forward_trace_answer(&net.node[1].f, net.now, frame, sizeof frame, 40,
                                    TERN_TRACE_BY_RELAY, INT8_MIN));
    run(net.now + TERN_S(60));
    CHECK(tern_forward_trace_answer(&net.node[1].f, net.now, frame, sizeof frame, 40,
                                    TERN_TRACE_BY_DESTINATION, INT8_MIN));
}

int main(void) {
    RUN(heads_are_written_and_read);
    RUN(frames_the_specification_rejects_change_nothing);
    RUN(a_hop_ends_when_the_specification_says);
    RUN(a_node_listens_when_the_specification_says);
    RUN(messages_that_share_a_tag_are_told_apart);
    RUN(slots_need_not_be_cleared_and_are_no_more_than_a_handle_names);
    RUN(an_answer_goes_loud_enough_for_the_node_it_answers);
    RUN(a_frame_goes_louder_each_time);
    RUN(a_message_goes_along_the_line_and_is_acknowledged);
    RUN(first_contact_goes_along_the_line_and_back);
    RUN(first_contact_unanswered_is_tried_again_and_no_hop_is_given_up);
    RUN(first_contact_lost_on_the_way_is_sent_again);
    RUN(a_leaf_passes_nothing_on);
    RUN(a_hop_unheard_is_sent_again_louder_and_then_given_up);
    RUN(a_frame_sent_again_waits_a_time_of_its_own);
    RUN(a_wait_is_no_longer_than_the_specification_allows);
    RUN(a_message_unacknowledged_is_sent_again_and_every_copy_arrives);
    RUN(a_frame_given_up_on_goes_another_way);
    RUN(a_frame_already_being_passed_on_is_not_taken_twice);
    RUN(a_frame_that_has_come_far_enough_stops);
    RUN(a_message_with_no_route_waits_and_is_given_up);
    RUN(a_relay_adds_its_entry_as_the_specification_says);
    RUN(a_trace_is_answered_as_the_specification_says);
    RUN(a_trace_goes_along_the_line_and_back);
    RUN(a_trace_unanswered_is_given_up_once);
    RUN(a_trace_with_no_route_is_not_begun);
    RUN(a_relay_with_no_route_answers);
    RUN(a_node_answers_few_traces_a_minute);
    RUN(an_acknowledgement_goes_the_way_the_specification_says);
    RUN(an_acknowledgement_goes_back_the_way_the_message_came);
    RUN(a_leaf_gone_quiet_is_taken_out_of_use_and_reached_another_way);
    return CHECK_DONE();
}
