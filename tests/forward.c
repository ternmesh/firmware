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

#include "forwarding.h"

#define COUNT(a) (sizeof(a) / sizeof(a)[0])

struct node {
    struct tern_route r;
    struct tern_forward f;
    struct tern_route_neighbour nb[NB];
    struct tern_route_dest dest[DESTS];
    struct tern_forward_slot slot[SLOTS];
    bool relay;
    bool deaf;       /* hears no frame that follows routes */
    bool silent;     /* acknowledges nothing */
    int got;         /* messages for it received, copies and all */
    int acked;       /* acknowledgements for its messages */
    int failed;      /* messages of its own given up on */
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

int main(void) {
    RUN(heads_are_written_and_read);
    RUN(frames_the_specification_rejects_change_nothing);
    RUN(a_hop_ends_when_the_specification_says);
    RUN(messages_that_share_a_tag_are_told_apart);
    RUN(slots_need_not_be_cleared_and_are_no_more_than_a_handle_names);
    RUN(an_answer_goes_loud_enough_for_the_node_it_answers);
    RUN(a_frame_goes_louder_each_time);
    RUN(a_message_goes_along_the_line_and_is_acknowledged);
    RUN(a_leaf_passes_nothing_on);
    RUN(a_hop_unheard_is_sent_again_louder_and_then_given_up);
    RUN(a_message_unacknowledged_is_sent_again_and_every_copy_arrives);
    RUN(a_frame_given_up_on_goes_another_way);
    RUN(a_frame_already_being_passed_on_is_not_taken_twice);
    RUN(a_frame_that_has_come_far_enough_stops);
    RUN(a_message_with_no_route_waits_and_is_given_up);
    return CHECK_DONE();
}
