#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "demo.h"
#include "tern/forward.h"
#include "tern/region.h"
#include "tern/route.h"

/* First contact along a route (draft/first-contact.md, draft/forwarding.md): boards as the Heltec
 * V3 port has them, with the node, the router and the forwarder joined as its main loop joins
 * them, over a channel that takes no time and loses only what a test tells it to. Two boards
 * that do not hear each other make a session through a third, which learns nothing of it.
 *
 * tests/forward.c has the forwarder's rules with frames made by hand, and tests/demo.c the node's
 * with frames handed straight across. This is the two together, with real handshakes. */

#define NODES 3
#define NB 8
#define DESTS 8
#define SLOTS 6
#define RECORDS 12
#define NONE INT16_MIN
#define FULL 22
#define SNR_FLOOR_Q (-50) /* SF9 */

struct store {
    char keys[RECORDS][16];
    uint8_t data[RECORDS][sizeof(struct demo_state)];
    size_t lens[RECORDS];
    uint64_t rng;
};

static int find(struct store *s, const char *key) {
    for (int i = 0; i < RECORDS; i++) {
        if (strcmp(s->keys[i], key) == 0) {
            return i;
        }
    }
    return -1;
}

static bool store_load(void *ctx, const char *key, void *buf, size_t len) {
    struct store *s = ctx;
    int i = find(s, key);
    if (i < 0 || s->lens[i] != len) {
        return false;
    }
    memcpy(buf, s->data[i], len);
    return true;
}

static bool store_save(void *ctx, const char *key, const void *buf, size_t len) {
    struct store *s = ctx;
    int i = find(s, key);
    if (i < 0) {
        i = find(s, "");
    }
    strcpy(s->keys[i], key);
    memcpy(s->data[i], buf, len);
    s->lens[i] = len;
    return true;
}

static bool store_random(void *ctx, uint8_t *buf, size_t len) {
    struct store *s = ctx;
    for (size_t i = 0; i < len; i++) {
        s->rng = s->rng * 6364136223846793005u + 1442695040888963407u;
        buf[i] = (uint8_t)(s->rng >> 56);
    }
    return true;
}

struct node {
    struct store flash;
    struct demo demo;
    struct tern_route r;
    struct tern_forward f;
    struct tern_route_neighbour nb[NB];
    struct tern_route_dest dest[DESTS];
    struct tern_forward_slot slot[SLOTS];
    uint32_t id;
    /* The handshake frame the forwarder keeps for this board, as main.c has it. */
    bool kept;
    uint8_t kept_hdr, kept_tag[TERN_FORWARD_TAG];
    int gave_up;         /* handshakes this board began and gave up */
    int paired, refused; /* what it made of handshakes */
    int heard;           /* messages shown */
    int acked;           /* acknowledgements for its messages that checked */
    int contact_sent[5]; /* first-contact frames of its own on the air, by number */
    int passed_on;       /* frames of others' on the air */
    char last[32];
    /* The message it waits on an acknowledgement for. */
    int ack_slot;
    uint32_t ack_counter;
};

static struct {
    struct node node[NODES];
    int16_t snr[NODES][NODES];
    struct tern_lora lora;
    tern_time now;
    /* The next frame with this hdr sent to the node it is for is not heard by it, once. */
    uint8_t lose_hdr;
    int lost;
} net;

static void net_init(const bool relay[NODES]) {
    memset(&net, 0, sizeof net);
    net.lora = tern_region_lora(tern_region(TERN_REGION_US915));
    for (int a = 0; a < NODES; a++) {
        struct node *x = &net.node[a];
        struct demo_store st = {&x->flash, store_load, store_save, store_random};
        struct tern_route_config c = tern_route_defaults(&net.lora, FULL, -9, relay[a]);
        struct tern_forward_config fc = tern_forward_defaults();
        x->flash.rng = 0x72656c6179u + (uint64_t)a;
        CHECK(demo_start(&x->demo, &st, DEMO_HOLD));
        x->id = tern_route_id(x->demo.id.address);
        tern_route_init(&x->r, &c, x->id, x->nb, NB, x->dest, DESTS, 0,
                        0x9E3779B97F4A7C15ULL * (uint64_t)(a + 1), 0);
        tern_forward_init(&x->f, &fc, &x->r, x->slot, SLOTS, (uint64_t)(a + 7));
        x->ack_slot = -1;
        for (int b = 0; b < NODES; b++) {
            net.snr[a][b] = NONE;
        }
    }
}

static void link(int a, int b) { net.snr[a][b] = net.snr[b][a] = 40; }

static void let_go(struct node *x) {
    if (x->kept) {
        (void)tern_forward_done(&x->f, x->kept_hdr, x->kept_tag);
        x->kept = false;
    }
}

static bool send_contact(struct node *x, const uint8_t *frame, size_t len, int8_t back) {
    bool keep = frame[0] == TERN_HDR_CONTACT_1 || frame[0] == TERN_HDR_CONTACT_1 + 2;
    if (keep) {
        let_go(x);
    }
    if (!tern_forward_send(&x->f, net.now, tern_contact_destination(frame), frame, len, keep,
                           back)) {
        return false;
    }
    if (keep) {
        x->kept = true;
        x->kept_hdr = frame[0];
        memcpy(x->kept_tag, frame + TERN_FORWARD_HEAD, TERN_FORWARD_TAG);
    }
    return true;
}

static void contact(int from, int to) {
    struct node *x = &net.node[from];
    uint8_t frame[TERN_CONTACT_MAX_FRAME];
    size_t len;
    CHECK(demo_contact(&x->demo, net.node[to].demo.id.address, net.now, frame, &len) == DEMO_OK);
    CHECK(send_contact(x, frame, len, INT8_MIN));
}

static void say(int from, int to, const char *text) {
    struct node *x = &net.node[from];
    uint8_t frame[TERN_UNICAST_MAX_FRAME];
    size_t len = strlen(text);
    int slot = demo_peer(&x->demo, net.node[to].demo.id.address);
    CHECK(slot >= 0);
    x->ack_slot = slot;
    x->ack_counter = x->demo.s[slot].session.tx.next;
    CHECK(demo_seal(&x->demo, slot, (const uint8_t *)text, len, frame) == DEMO_OK);
    CHECK(tern_forward_send(&x->f, net.now, net.node[to].id, frame, len + TERN_UNICAST_OVERHEAD,
                            true, INT8_MIN));
}

static void heard(struct node *y, const uint8_t *frame, size_t len, int16_t snr) {
    uint8_t msg[TERN_UNICAST_MAX_PLAINTEXT + 1];
    struct demo_received got;
    struct tern_forward_heard routed;
    if (tern_route_frame(frame, len)) {
        tern_route_heard(&y->r, net.now, frame, len, snr);
        return;
    }
    tern_forward_heard(&y->f, net.now, frame, len, snr, &routed);
    if (routed.got == TERN_FORWARD_ACK && y->ack_slot >= 0 &&
        demo_acked(&y->demo, y->ack_slot, y->ack_counter, frame, len)) {
        y->acked += tern_forward_acked(&y->f, frame + TERN_FORWARD_HEAD);
    }
    if (routed.got != TERN_FORWARD_MESSAGE && routed.got != TERN_FORWARD_CONTACT) {
        return;
    }
    switch (demo_receive(&y->demo, net.now, frame, len, msg, &got)) {
    case DEMO_HEARD_MESSAGE:
        y->heard++;
        memcpy(y->last, msg, got.msg_len < sizeof y->last - 1 ? got.msg_len : sizeof y->last - 1);
        break;
    case DEMO_HEARD_PAIRED:
        y->paired++;
        break;
    case DEMO_HEARD_REFUSED:
        y->refused++;
        break;
    default:
        break;
    }
    if (y->demo.h.phase != DEMO_INITIATING) {
        let_go(y);
    }
    if (got.reply_len != 0) {
        (void)send_contact(y, got.reply, got.reply_len, routed.back);
    }
    for (size_t i = 0; i < got.acks; i++) {
        (void)tern_forward_send(&y->f, net.now, tern_route_id(y->demo.s[got.ack_slot[i]].peer),
                                got.ack[i], sizeof got.ack[i], false, routed.back);
    }
}

static void on_air(int from, const uint8_t *frame, size_t len, int8_t dbm) {
    struct tern_forward_head h;
    uint32_t deaf = 0; /* the node that does not hear this frame, if it is the one to lose */
    if (net.lose_hdr != 0 && frame[0] == net.lose_hdr && tern_forward_head_read(&h, frame, len) &&
        h.next == h.destination) {
        net.lose_hdr = 0;
        net.lost++;
        deaf = h.destination;
    }
    for (int k = 0; k < NODES; k++) {
        int snr = net.snr[from][k] == NONE ? NONE : net.snr[from][k] + 4 * (dbm - FULL);
        if (k != from && net.node[k].id != deaf && snr != NONE && snr >= SNR_FLOOR_Q) {
            heard(&net.node[k], frame, len, (int16_t)snr);
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
        if (t > until) {
            break;
        }
        net.now = t > net.now ? t : net.now;
        for (int i = 0; i < NODES; i++) {
            struct node *x = &net.node[i];
            uint8_t frame[TERN_ROUTE_FRAME_MAX], tag[TERN_FORWARD_TAG], handle, hdr;
            enum tern_forward_kind kind;
            int8_t dbm = 0;
            size_t len;
            while (tern_route_due(&x->r) <= net.now &&
                   (len = tern_route_poll(&x->r, net.now, frame, &dbm)) != 0) {
                tern_route_sent(&x->r, net.now);
                on_air(i, frame, len, dbm);
            }
            while ((len = tern_forward_poll(&x->f, net.now, frame, &dbm, &kind, &handle)) != 0) {
                if (kind == TERN_FORWARD_OWN && tern_forward_contact(frame[0])) {
                    x->contact_sent[frame[0] - 0x50]++;
                }
                x->passed_on += kind == TERN_FORWARD_RELAY;
                tern_forward_sent(&x->f, net.now, handle);
                on_air(i, frame, len, dbm);
            }
            while (tern_forward_failed(&x->f, tag)) {
            }
            while (tern_forward_contact_failed(&x->f, &hdr, tag)) {
                if (x->kept && hdr == x->kept_hdr && memcmp(tag, x->kept_tag, sizeof tag) == 0) {
                    x->kept = false;
                    x->gave_up += demo_abandon(&x->demo);
                }
            }
            (void)demo_tick(&x->demo, net.now);
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

/* 0 - 1 - 2: the ends are leaves and do not hear each other; the one between is a relay. */
static void ends_and_a_relay(void) {
    static const bool relay[NODES] = {false, true, false};
    uint32_t next;
    uint16_t metric;
    net_init(relay);
    link(0, 1);
    link(1, 2);
    run(TERN_S(1200));
    CHECK(tern_route_next(&net.node[0].r, net.node[2].id, &next, &metric) &&
          next == net.node[1].id);
    CHECK(tern_route_next(&net.node[2].r, net.node[0].id, &next, &metric) &&
          next == net.node[1].id);
}

static bool session(int a, int b) {
    return demo_peer(&net.node[a].demo, net.node[b].demo.id.address) >= 0;
}

static void two_boards_out_of_earshot_make_a_session_through_a_relay(void) {
    ends_and_a_relay();
    contact(0, 2);
    run(net.now + TERN_S(30));
    CHECK(session(0, 2) && session(2, 0));
    CHECK_EQ_I64(net.node[0].paired, 1);
    CHECK_EQ_I64(net.node[2].paired, 1);
    /* Each frame once from its end and once from the relay, which keeps nothing of it. */
    CHECK_EQ_I64(net.node[0].contact_sent[1], 1);
    CHECK_EQ_I64(net.node[2].contact_sent[2], 1);
    CHECK_EQ_I64(net.node[0].contact_sent[3], 1);
    CHECK_EQ_I64(net.node[2].contact_sent[4], 1);
    CHECK_EQ_I64(net.node[1].passed_on, 4);
    CHECK_EQ_I64((int64_t)demo_peers(&net.node[1].demo), 0);
    CHECK(net.node[1].demo.h.phase == DEMO_IDLE);
    for (int i = 0; i < NODES; i++) {
        CHECK_EQ_I64(net.node[i].f.counts.given_up, 0);
        CHECK_EQ_I64(in_hand(i), 0);
    }

    /* And the session carries a message each way, acknowledged, the same way round. */
    say(0, 2, "over the hill");
    run(net.now + TERN_S(30));
    CHECK_EQ_I64(net.node[2].heard, 1);
    CHECK(strcmp(net.node[2].last, "over the hill") == 0);
    CHECK_EQ_I64(net.node[0].acked, 1);
    say(2, 0, "and back");
    run(net.now + TERN_S(30));
    CHECK_EQ_I64(net.node[0].heard, 1);
    CHECK(strcmp(net.node[0].last, "and back") == 0);
    CHECK_EQ_I64(net.node[2].acked, 1);
}

/* The last hop of each frame is sent once, so a frame lost there is not sent again by the relay:
 * the board that began the handshake sends its own frame again, and the other answers again. */
static void a_frame_lost_on_its_last_hop_is_made_good_from_the_start(void) {
    for (int n = 1; n <= 4; n++) {
        ends_and_a_relay();
        net.lose_hdr = (uint8_t)(0x50 + n);
        contact(0, 2);
        run(net.now + TERN_S(120));
        CHECK_EQ_I64(net.lost, 1);
        CHECK(session(0, 2) && session(2, 0));
        /* What was lost cost one more of the initiator's frames, and the answer to it if that
         * was what was lost. */
        CHECK_EQ_I64(net.node[0].contact_sent[1], n <= 2 ? 2 : 1);
        CHECK_EQ_I64(net.node[2].contact_sent[2], n == 2 ? 2 : 1);
        CHECK_EQ_I64(net.node[0].contact_sent[3], n >= 3 ? 2 : 1);
        CHECK_EQ_I64(net.node[2].contact_sent[4], n == 4 ? 2 : 1);
        CHECK_EQ_I64(net.node[0].gave_up, 0);
        CHECK_EQ_I64(net.node[1].f.counts.sent_again, 0);
        for (int i = 0; i < NODES; i++) {
            CHECK_EQ_I64(in_hand(i), 0);
        }
        say(0, 2, "made it");
        run(net.now + TERN_S(30));
        CHECK_EQ_I64(net.node[2].heard, 1);
        CHECK_EQ_I64(net.node[0].acked, 1);
    }
}

/* A board that will not take the one asking says nothing after message_3. The one asking tries
 * as often as a message is tried and gives up, and nobody holds that against the relay. */
static void a_board_that_refuses_is_asked_no_more_than_a_message_is_sent(void) {
    static const bool relay[NODES] = {true, true, true};
    net_init(relay);
    link(0, 1);
    link(1, 2);
    run(TERN_S(1200));
    /* The relay and one end have a session, so that end takes no stranger. */
    contact(1, 2);
    run(net.now + TERN_S(30));
    CHECK(session(1, 2) && session(2, 1));

    contact(0, 2);
    run(net.now + TERN_S(240));
    CHECK(!session(0, 2) && !session(2, 0));
    CHECK_EQ_I64(net.node[2].refused, 1);
    CHECK_EQ_I64(net.node[0].contact_sent[1], 1);
    CHECK_EQ_I64(net.node[0].contact_sent[3], 4);
    CHECK_EQ_I64(net.node[2].contact_sent[4], 1); /* the one to the relay */
    CHECK_EQ_I64(net.node[0].gave_up, 1);
    CHECK(net.node[0].demo.h.phase == DEMO_IDLE);
    CHECK_EQ_I64(net.node[1].f.counts.given_up, 0);
    CHECK_EQ_I64(in_hand(0), 0);
    CHECK(session(1, 2) && session(2, 1));
}

/* With nobody to pass its frames on, a board that begins a handshake asks for a route as often
 * as it would have sent, and gives up. Here it takes no default route. */
static void a_board_with_no_route_gives_up(void) {
    static const bool relay[NODES] = {false, true, false};
    net_init(relay);
    net.node[0].r.config.default_hops = 0;
    link(0, 1);
    run(TERN_S(1200));
    contact(0, 2);
    run(net.now + TERN_S(120));
    CHECK_EQ_I64(net.node[0].contact_sent[1], 0);
    CHECK_EQ_I64(net.node[0].gave_up, 1);
    CHECK_EQ_I64(in_hand(0), 0);
}

/* A leaf with no route hands its frames to its nearest relay, which has none on: the relay passes
 * nothing on, and the leaf gives up all the same, holding nothing. */
static void a_leaf_with_no_route_tries_its_relay_and_gives_up(void) {
    static const bool relay[NODES] = {false, true, false};
    net_init(relay);
    link(0, 1);
    run(TERN_S(1200));
    contact(0, 2);
    run(net.now + TERN_S(120));
    CHECK(net.node[0].contact_sent[1] > 0);
    CHECK_EQ_I64(net.node[0].gave_up, 1);
    CHECK_EQ_I64(in_hand(0), 0);
    CHECK_EQ_I64(in_hand(1), 0);
}

int main(void) {
    RUN(two_boards_out_of_earshot_make_a_session_through_a_relay);
    RUN(a_frame_lost_on_its_last_hop_is_made_good_from_the_start);
    RUN(a_board_that_refuses_is_asked_no_more_than_a_message_is_sent);
    RUN(a_board_with_no_route_gives_up);
    RUN(a_leaf_with_no_route_tries_its_relay_and_gives_up);
    return CHECK_DONE();
}
