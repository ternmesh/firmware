#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "tern/region.h"
#include "tern/route.h"

/* Routers together (draft/routing.md): a handful of them over a channel that loses nothing and
 * takes no time, which is enough to see whether routes form, heal and stay loop-free. What a real
 * channel does to them is the simulator's to say (ternmesh/sim). */

#define NODES 12
#define NB 16
#define DESTS 32
#define NONE INT16_MIN
#define FULL 22
#define SNR_FLOOR_Q (-50) /* SF9 */

struct node {
    struct tern_route r;
    struct tern_route_neighbour nb[NB];
    struct tern_route_dest dest[DESTS];
    bool on;
    bool relay;
    tern_time airtime;
    int announces, requests;
};

struct net {
    int n;
    struct node node[NODES];
    int16_t snr[NODES][NODES]; /* [from][to] at full power, in quarters of a decibel, or NONE */
    struct tern_lora lora;
    tern_time now;
    size_t nb_cap, dest_cap;
    tern_time silent_max;
    int loops;            /* times a check found one */
    tern_time loop_last;  /* and when it last did */
    tern_time loop_since; /* when the loops there are now began, or -1 */
    tern_time loop_longest;
};

static uint32_t id_of(int i) { return 0x1000u + (uint32_t)i; }

static void start(struct net *net, int i, uint16_t seq) {
    struct node *x = &net->node[i];
    struct tern_route_config c = tern_route_defaults(&net->lora, FULL, -9, x->relay);
    c.silent_max = net->silent_max ? net->silent_max : c.silent_max;
    tern_route_init(&x->r, &c, id_of(i), x->nb, net->nb_cap, x->dest, net->dest_cap, seq,
                    0x9E3779B97F4A7C15ULL * (uint64_t)(i + 1) + (uint64_t)net->now, net->now);
    x->on = true;
}

static void net_init(struct net *net, int n) {
    memset(net, 0, sizeof *net);
    net->n = n;
    net->lora = tern_region_lora(tern_region(TERN_REGION_US915));
    net->nb_cap = NB;
    net->dest_cap = DESTS;
    net->loop_since = -1;
    for (int a = 0; a < n; a++) {
        net->node[a].relay = true;
        for (int b = 0; b < n; b++) {
            net->snr[a][b] = NONE;
        }
    }
}

static void link(struct net *net, int a, int b, int16_t snr_q) {
    net->snr[a][b] = net->snr[b][a] = snr_q;
}

static void start_all(struct net *net) {
    for (int i = 0; i < net->n; i++) {
        start(net, i, 0);
    }
}

/* Follows next hops from every node to every other: a frame may be dropped where a route is
 * missing, but must never come back to a node it has been through. */
static void check_no_loops(struct net *net) {
    int before = net->loops;
    for (int d = 0; d < net->n; d++) {
        for (int s = 0; s < net->n; s++) {
            bool been[NODES] = {false};
            int at = s;
            while (at != d && net->node[at].on) {
                uint32_t next;
                uint16_t metric;
                if (been[at]) {
                    net->loops++;
                    net->loop_last = net->now;
                    break;
                }
                been[at] = true;
                if (!tern_route_next(&net->node[at].r, id_of(d), &next, &metric)) {
                    break;
                }
                at = (int)(next - 0x1000u);
            }
        }
    }
    if (net->loops != before) {
        net->loop_since = net->loop_since < 0 ? net->now : net->loop_since;
    } else if (net->loop_since >= 0) {
        tern_time lasted = net->now - net->loop_since;
        net->loop_longest = lasted > net->loop_longest ? lasted : net->loop_longest;
        net->loop_since = -1;
    }
}

static void run(struct net *net, tern_time until) {
    for (;;) {
        tern_time t = until + 1;
        for (int i = 0; i < net->n; i++) {
            if (net->node[i].on && tern_route_due(&net->node[i].r) < t) {
                t = tern_route_due(&net->node[i].r);
            }
        }
        if (t > until) {
            break;
        }
        net->now = t > net->now ? t : net->now;
        for (int i = 0; i < net->n; i++) {
            struct node *x = &net->node[i];
            uint8_t frame[TERN_ROUTE_FRAME_MAX];
            int8_t dbm = 0;
            size_t len;
            while (x->on && tern_route_due(&x->r) <= net->now &&
                   (len = tern_route_poll(&x->r, net->now, frame, &dbm)) != 0) {
                x->airtime += tern_lora_airtime(&net->lora, (uint32_t)len);
                x->announces += frame[0] == TERN_HDR_ANNOUNCE;
                x->requests += frame[0] == TERN_HDR_REQUEST;
                CHECK(dbm >= -9 && dbm <= FULL);
                tern_route_sent(&x->r, net->now);
                for (int k = 0; k < net->n; k++) {
                    /* Quieter than full power, a frame arrives that much weaker. */
                    int snr = net->snr[i][k] == NONE ? NONE : net->snr[i][k] + 4 * (dbm - FULL);
                    if (k != i && net->node[k].on && snr != NONE && snr >= SNR_FLOOR_Q) {
                        tern_route_heard(&net->node[k].r, net->now, frame, len, (int16_t)snr);
                    }
                }
            }
        }
        check_no_loops(net);
    }
    net->now = until;
}

/* The hops a frame from `s` takes to reach `d` along the routes, or -1 if it does not arrive. */
static int hops(struct net *net, int s, int d) {
    int at = s, n = 0;
    while (at != d) {
        uint32_t next;
        uint16_t metric;
        if (n > net->n || !net->node[at].on ||
            !tern_route_next(&net->node[at].r, id_of(d), &next, &metric)) {
            return -1;
        }
        at = (int)(next - 0x1000u);
        n++;
    }
    return n;
}

static uint16_t metric(struct net *net, int s, int d) {
    uint32_t next;
    uint16_t m = 0;
    return tern_route_next(&net->node[s].r, id_of(d), &next, &m) ? m : TERN_ROUTE_INF;
}

static struct net net; /* too big for the stack of some hosts */

/* The same on every host, which rand() is not. */
static uint64_t rnd_state = 1;

static int rnd(void) {
    rnd_state = rnd_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)(rnd_state >> 33);
}

static void two_nodes_find_each_other(void) {
    uint16_t cost;
    net_init(&net, 2);
    link(&net, 0, 1, 40);
    start_all(&net);
    cost = net.node[0].r.cost;
    CHECK_EQ_I64(cost, 70);
    CHECK_EQ_I64(hops(&net, 0, 1), -1);
    run(&net, TERN_S(60));
    CHECK_EQ_I64(hops(&net, 0, 1), 1);
    CHECK_EQ_I64(hops(&net, 1, 0), 1);
    CHECK_EQ_I64(metric(&net, 0, 1), cost);
    /* Heard 22.5 dB above the floor at full power: that much margin, each way. */
    CHECK(net.node[0].nb[0].up && net.node[0].nb[0].theirs == 128 + 22);
    CHECK_EQ_I64(net.node[0].nb[0].floor, 16 * FULL - 4 * (40 - SNR_FLOOR_Q));
}

static void a_line_is_routed_end_to_end(void) {
    net_init(&net, 6);
    for (int i = 0; i + 1 < 6; i++) {
        link(&net, i, i + 1, 0);
    }
    start_all(&net);
    run(&net, TERN_S(300));
    for (int s = 0; s < 6; s++) {
        for (int d = 0; d < 6; d++) {
            CHECK_EQ_I64(hops(&net, s, d), abs(s - d));
            if (s != d) {
                CHECK_EQ_I64(metric(&net, s, d), abs(s - d) * net.node[0].r.cost);
            }
        }
    }
    CHECK_EQ_I64(net.loops, 0);
}

static void a_link_heard_one_way_is_not_used(void) {
    net_init(&net, 2);
    net.snr[0][1] = 40; /* 1 hears 0, and 0 hears nothing */
    start_all(&net);
    run(&net, TERN_S(600));
    CHECK_EQ_I64(hops(&net, 0, 1), -1);
    CHECK_EQ_I64(hops(&net, 1, 0), -1);
    CHECK(net.node[1].nb[0].used && !net.node[1].nb[0].up);
}

static void the_shorter_way_round_a_ring_is_taken(void) {
    net_init(&net, 7);
    for (int i = 0; i < 7; i++) {
        link(&net, i, (i + 1) % 7, 20);
    }
    start_all(&net);
    run(&net, TERN_S(600));
    for (int s = 0; s < 7; s++) {
        for (int d = 0; d < 7; d++) {
            int direct = abs(s - d);
            CHECK_EQ_I64(hops(&net, s, d), direct < 7 - direct ? direct : 7 - direct);
        }
    }
    CHECK_EQ_I64(net.loops, 0);
}

static void a_quiet_network_goes_quiet(void) {
    int before = 0, after = 0;
    net_init(&net, 5);
    for (int a = 0; a < 5; a++) {
        for (int b = a + 1; b < 5; b++) {
            link(&net, a, b, 40);
        }
    }
    start_all(&net);
    run(&net, TERN_S(3600));
    for (int i = 0; i < 5; i++) {
        before += net.node[i].announces;
        CHECK_EQ_I64(net.node[i].r.interval, TERN_S(512));
        CHECK_EQ_I64(net.node[i].requests, 0);
    }
    run(&net, TERN_S(2 * 3600));
    for (int i = 0; i < 5; i++) {
        after += net.node[i].announces;
    }
    /* Seven intervals an hour, each node suppressed in two of three of them at most, and the
     * others suppressing it in some: between a third of them and all. */
    CHECK(after - before >= 5 * 7 / 3 - 1);
    CHECK(after - before <= 5 * 8);
    CHECK_EQ_I64(hops(&net, 0, 4), 1);
}

/* A relay that goes is found out only by its silence, in this draft: its neighbours forget it a
 * day and two of its promises on, and the routes go the other way round. */
static void a_node_gone_is_forgotten_and_routed_around(void) {
    net_init(&net, 6);
    for (int i = 0; i < 6; i++) {
        link(&net, i, (i + 1) % 6, 20);
    }
    start_all(&net);
    run(&net, TERN_S(600));
    CHECK_EQ_I64(hops(&net, 0, 2), 2);
    net.node[1].on = false;
    run(&net, TERN_S(600) + TERN_S(26 * 3600));
    CHECK_EQ_I64(hops(&net, 0, 2), 4);
    CHECK_EQ_I64(hops(&net, 0, 1), -1);
    CHECK_EQ_I64(hops(&net, 3, 1), -1);
    CHECK_EQ_I64(net.loops, 0);
}

/* A node that restarts has lost its sequence number, and its neighbours hold routes to it at the
 * old one that nothing it now says is feasible against. They ask, and it takes the seq they ask
 * for. */
static void a_node_that_restarts_is_routed_to_again(void) {
    int asked = 0;
    net_init(&net, 5);
    for (int i = 0; i + 1 < 5; i++) {
        link(&net, i, i + 1, 20);
    }
    for (int i = 0; i < 5; i++) {
        start(&net, i, i == 4 ? 700 : 0);
    }
    run(&net, TERN_S(600));
    CHECK_EQ_I64(hops(&net, 0, 4), 4);
    start(&net, 4, 0);
    run(&net, TERN_S(900));
    CHECK_EQ_I64(hops(&net, 0, 4), 4);
    CHECK_EQ_I64(hops(&net, 4, 0), 4);
    CHECK(tern_route_newer(net.node[4].r.seq, 700));
    for (int i = 0; i < 5; i++) {
        asked += net.node[i].requests;
    }
    CHECK(asked > 0);
    CHECK_EQ_I64(net.loops, 0);
}

static void a_leaf_is_reached_but_never_routed_through(void) {
    net_init(&net, 4);
    link(&net, 0, 1, 20); /* relay 0, leaf 1, relay 2, and relay 3 beyond it */
    link(&net, 1, 2, 20);
    link(&net, 2, 3, 20);
    net.node[1].relay = false;
    start_all(&net);
    run(&net, TERN_S(900));
    CHECK_EQ_I64(hops(&net, 0, 1), 1);
    CHECK_EQ_I64(hops(&net, 3, 1), 2);
    CHECK_EQ_I64(hops(&net, 1, 3), 2);
    CHECK_EQ_I64(hops(&net, 1, 0), 1);
    CHECK_EQ_I64(hops(&net, 0, 2), -1);
    CHECK_EQ_I64(hops(&net, 2, 0), -1);
    CHECK_EQ_I64(net.node[1].r.urgent_count, 0);
}

static void routing_keeps_to_its_cap(void) {
    tern_time full_frame;
    net_init(&net, 8);
    for (int a = 0; a < 8; a++) {
        for (int b = a + 1; b < 8; b++) {
            if ((a * 7 + b * 3) % 4 != 0) {
                link(&net, a, b, 10);
            }
        }
    }
    start_all(&net);
    full_frame = tern_lora_airtime(&net.lora, 255);
    /* Nothing settles: a node restarts every minute. */
    for (int m = 1; m <= 120; m++) {
        run(&net, TERN_S(60) * m);
        start(&net, m % 8, 0);
        for (int i = 0; i < 8; i++) {
            /* Since it last started, no more than the cap, and what its two buckets held. The
             * restarts forgive nothing here: airtime is counted across them. */
            CHECK(net.node[i].airtime <= TERN_S(60) * m / 200 + 2 * full_frame * (m / 8 + 2));
        }
    }
}

static void a_crowd_turns_its_announces_down(void) {
    int8_t dbm = FULL;
    net_init(&net, 12);
    for (int a = 0; a < 12; a++) {
        for (int b = a + 1; b < 12; b++) {
            link(&net, a, b, 60); /* 27.5 dB above the floor */
        }
    }
    start_all(&net);
    run(&net, TERN_S(1800));
    for (int a = 0; a < 12; a++) {
        const struct tern_route *r = &net.node[a].r;
        int up = 0;
        for (int k = 0; k < NB; k++) {
            up += r->nb[k].used && r->nb[k].up;
        }
        CHECK_EQ_I64(up, 11);
        CHECK_EQ_I64(hops(&net, a, (a + 5) % 12), 1);
    }
    /* Every floor is 27.5 dB under full power, so with 10 dB of margin: 17.5 dB down, rounded up
     * to a whole dBm. */
    {
        struct node *x = &net.node[0];
        uint8_t frame[TERN_ROUTE_FRAME_MAX];
        x->r.asked = true;
        while (tern_route_poll(&x->r, tern_route_due(&x->r), frame, &dbm) == 0) {
        }
        CHECK_EQ_I64(dbm, 5);
    }
}

static void tables_too_small_hold_what_they_can(void) {
    int routed = 0;
    net_init(&net, 10);
    net.nb_cap = 3;
    net.dest_cap = 4;
    for (int a = 0; a < 10; a++) {
        for (int b = a + 1; b < 10; b++) {
            if (b - a <= 4) {
                link(&net, a, b, 20);
            }
        }
    }
    start_all(&net);
    run(&net, TERN_S(3600));
    for (int s = 0; s < 10; s++) {
        for (int d = 0; d < 10; d++) {
            routed += s != d && hops(&net, s, d) > 0;
        }
    }
    /* A link needs each end to have kept the other, so there are few; but what is held is right. */
    CHECK(routed > 0);
    CHECK(routed <= 10 * 4);
    CHECK_EQ_I64(net.loops, 0);
}

static void random_mesh(struct net *m, int16_t base[NODES][NODES]) {
    net_init(m, NODES);
    for (int a = 0; a < NODES; a++) {
        link(m, a, (a + 1) % NODES, 20);
        for (int b = a + 2; b < NODES; b++) {
            if (rnd() % 4 == 0) {
                link(m, a, b, (int16_t)(rnd() % 60 - 20));
            }
        }
    }
    memcpy(base, m->snr, sizeof m->snr);
}

/* Every few seconds a third of the links are deaf, one way or the other. */
static void deafen(struct net *m, int16_t base[NODES][NODES]) {
    for (int a = 0; a < NODES; a++) {
        for (int b = 0; b < NODES; b++) {
            m->snr[a][b] = rnd() % 3 == 0 ? NONE : base[a][b];
        }
    }
}

static int unreached(struct net *m) {
    int n = 0;
    for (int s = 0; s < m->n; s++) {
        for (int d = 0; d < m->n; d++) {
            n += hops(m, s, d) < 0;
        }
    }
    return n;
}

/* The property the design stands on: whatever is lost on the air, no frame is ever sent round in
 * a circle. Announces here are lost as they would be on a busy channel, and neighbours are
 * forgotten after two minutes' silence, so links come and go and routes with them. */
static void routes_never_loop_whatever_is_lost(void) {
    static int16_t base[NODES][NODES];
    int asked = 0;
    rnd_state = 12345;
    random_mesh(&net, base);
    net.silent_max = TERN_S(120);
    start_all(&net);
    for (int step = 1; step <= 4000; step++) {
        deafen(&net, base);
        run(&net, TERN_S(5) * step);
    }
    for (int i = 0; i < NODES; i++) {
        asked += net.node[i].requests;
    }
    CHECK(asked > 0); /* or nothing was ever starved, and little was tested */
    CHECK_EQ_I64(net.loops, 0);
    memcpy(net.snr, base, sizeof base);
    run(&net, net.now + TERN_S(3600));
    CHECK_EQ_I64(unreached(&net), 0);
    CHECK_EQ_I64(net.loops, 0);
}

/* A node that restarts has lost what it announced, which is what the rule above rests on: until
 * its neighbours hear that it has, a route can lead back through it. So it says it is starting, and
 * waits; a neighbour that hears none of that can still loop. What is checked here is that it
 * passes: with nodes restarting and changing role all the while, the network left alone heals, and
 * nothing loops once it has. Over five seeds, three had no loop and the longest lasted 82 s. */
static void restarts_heal(void) {
    static int16_t base[NODES][NODES];
    rnd_state = 999;
    random_mesh(&net, base);
    start_all(&net);
    for (int step = 1; step <= 2000; step++) {
        deafen(&net, base);
        if (rnd() % 20 == 0) {
            int i = rnd() % NODES;
            net.node[i].relay = rnd() % 4 != 0;
            start(&net, i, (uint16_t)rnd());
        }
        run(&net, TERN_S(5) * step);
    }
    memcpy(net.snr, base, sizeof base);
    for (int i = 0; i < NODES; i++) {
        if (!net.node[i].relay) {
            net.node[i].relay = true;
            start(&net, i, (uint16_t)rnd());
        }
    }
    run(&net, net.now + TERN_S(3600));
    CHECK_EQ_I64(unreached(&net), 0);
    CHECK(net.loop_last < net.now - TERN_S(3000));
    CHECK(net.loop_longest < TERN_S(120));
}

int main(void) {
    RUN(two_nodes_find_each_other);
    RUN(a_line_is_routed_end_to_end);
    RUN(a_link_heard_one_way_is_not_used);
    RUN(the_shorter_way_round_a_ring_is_taken);
    RUN(a_quiet_network_goes_quiet);
    RUN(a_node_gone_is_forgotten_and_routed_around);
    RUN(a_node_that_restarts_is_routed_to_again);
    RUN(a_leaf_is_reached_but_never_routed_through);
    RUN(routing_keeps_to_its_cap);
    RUN(a_crowd_turns_its_announces_down);
    RUN(tables_too_small_hold_what_they_can);
    RUN(routes_never_loop_whatever_is_lost);
    RUN(restarts_heal);
    return CHECK_DONE();
}
