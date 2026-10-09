#include <string.h>

#include "check.h"
#include "tern/flood.h"
#include "tern/region.h"
#include "tern/route.h"

/* Frames for every node (draft/flooding.md): each rule against the specification's vectors, and
 * the flooder over a router whose neighbours the test sets by hand. */

#define NB 16
#define DESTS 4
#define SLOTS 8
#define FULL 22
#define LOWEST (-9)

struct head_case {
    uint8_t hdr, hops;
    int8_t power;
    uint8_t id[TERN_FLOOD_ID];
    const uint8_t *frame;
    size_t len;
};
struct rejected_case {
    const char *name;
    const uint8_t *frame;
    size_t len;
};
struct same_case {
    const char *name;
    const uint8_t *a;
    size_t a_len;
    const uint8_t *b;
    size_t b_len;
    bool same;
};
struct pass_case {
    bool relay;
    unsigned neighbours;
    uint8_t hops;
    int sends;
};
struct busy_case {
    uint32_t busy_ppm, drops_ppm;
};

struct draw_case {
    uint32_t busy_ppm, draw;
    bool drops;
};

struct radio_span {
    int64_t from, to;
};

struct share_ask {
    int64_t at;
    uint32_t least, most;
};

struct share_case {
    const char *name;
    const struct radio_span *radio;
    size_t radios;
    const struct share_ask *asks;
    size_t n;
};

struct copy_case {
    unsigned received;
    bool drops;
};
struct wait_case {
    uint8_t sf;
    uint32_t bw;
    uint32_t len;
    tern_time airtime, longest;
};
struct power_case {
    int8_t every;
    const int32_t *floors;
    size_t n;
    int8_t lowest, full, power;
};
struct seen_take {
    tern_time at;
    unsigned id;
};
struct seen_ask {
    tern_time at;
    unsigned id;
    bool seen;
};
struct seen_case {
    const char *name;
    const struct seen_take *takes;
    size_t n_takes;
    const struct seen_ask *asks;
    size_t n_asks;
};
struct allowance_frame {
    tern_time at, airtime;
    bool pays;
};
struct allowance_case {
    const char *name;
    uint32_t ppm;
    int window_s;
    uint8_t sf;
    uint32_t bw;
    const struct allowance_frame *frames;
    size_t n;
};

#include "flooding.h"

#define COUNT(a) (sizeof(a) / sizeof(a)[0])

/* A node: a router with nothing heard, and a flooder over it. */
struct node {
    struct tern_route r;
    struct tern_flood f;
    struct tern_route_neighbour nb[NB];
    struct tern_route_dest dest[DESTS];
    struct tern_flood_slot slot[SLOTS];
    uint8_t seen[TERN_FLOOD_SEEN][TERN_FLOOD_ID];
    struct tern_lora lora;
};

static void start(struct node *x, bool relay, uint64_t seed) {
    memset(x, 0, sizeof *x);
    x->lora = tern_region_lora(tern_region(TERN_REGION_US915));
    struct tern_route_config c = tern_route_defaults(&x->lora, FULL, LOWEST, relay);
    struct tern_flood_config fc = tern_flood_defaults();
    tern_route_init(&x->r, &c, 0x1000, x->nb, NB, x->dest, DESTS, 0, 7, 0);
    tern_flood_init(&x->f, &fc, &x->r, x->slot, SLOTS, x->seen, TERN_FLOOD_SEEN, seed, 0);
}

/* Gives a node so many relay neighbours whose links are up, each with this floor. */
static void neighbours(struct node *x, unsigned n, int32_t floor) {
    for (unsigned i = 0; i < n; i++) {
        x->nb[i] = (struct tern_route_neighbour){
            .id = 0x2000 + i, .used = true, .relay = true, .up = true, .floor = floor};
    }
}

/* A group frame's worth of bytes, told from others by `which`. */
static size_t make(uint8_t *frame, size_t len, uint8_t hops, unsigned which) {
    memset(frame, 0, len);
    frame[0] = TERN_HDR_GROUP;
    frame[1] = hops;
    frame[2] = 5;
    frame[3] = (uint8_t)(which >> 8);
    frame[4] = (uint8_t)which;
    return len;
}

static void test_heads(void) {
    for (size_t i = 0; i < COUNT(heads); i++) {
        const struct head_case *c = &heads[i];
        uint8_t id[TERN_FLOOD_ID];
        CHECK(tern_flood_frame(c->frame, c->len));
        CHECK(c->frame[0] == c->hdr && c->frame[1] == c->hops && (int8_t)c->frame[2] == c->power);
        tern_flood_id(c->frame, c->len, id);
        CHECK(memcmp(id, c->id, sizeof id) == 0);
    }
    for (size_t i = 0; i < COUNT(rejected); i++) {
        if (tern_flood_frame(rejected[i].frame, rejected[i].len)) {
            fprintf(stderr, "rejected %s: taken\n", rejected[i].name);
            check_failures++;
        }
    }
    for (size_t i = 0; i < COUNT(same_cases); i++) {
        const struct same_case *c = &same_cases[i];
        uint8_t a[TERN_FLOOD_ID], b[TERN_FLOOD_ID];
        tern_flood_id(c->a, c->a_len, a);
        tern_flood_id(c->b, c->b_len, b);
        if ((memcmp(a, b, sizeof a) == 0) != c->same) {
            fprintf(stderr, "same %s: not as the vector says\n", c->name);
            check_failures++;
        }
    }
}

static void test_rules(void) {
    struct tern_flood_config d = tern_flood_defaults();
    for (size_t i = 0; i < COUNT(pass_cases); i++) {
        const struct pass_case *c = &pass_cases[i];
        CHECK_EQ_I64(tern_flood_passes(c->relay, c->neighbours, c->hops, d.hops, d.sparse),
                     c->sends);
    }
    for (size_t i = 0; i < COUNT(power_cases); i++) {
        const struct power_case *c = &power_cases[i];
        CHECK_EQ_I64(tern_flood_power(c->every, c->floors, c->n, 10, c->lowest, c->full), c->power);
    }
    for (size_t i = 0; i < COUNT(allowance_cases); i++) {
        const struct allowance_case *c = &allowance_cases[i];
        struct tern_lora l = tern_region_lora(tern_region(TERN_REGION_US915));
        struct tern_flood_bucket b;
        l.sf = c->sf;
        l.bw_hz = c->bw;
        tern_flood_bucket_init(&b, c->ppm, TERN_S(c->window_s), tern_lora_airtime(&l, 255), 0);
        for (size_t k = 0; k < c->n; k++) {
            if (tern_flood_bucket_pays(&b, c->frames[k].at, c->frames[k].airtime) !=
                c->frames[k].pays) {
                fprintf(stderr, "allowance %s: frame %zu\n", c->name, k);
                check_failures++;
            }
        }
    }
}

/* The flooder, at each hops and number of relay neighbours the vectors give: what it sends a
 * frame on with is what the rule says. */
static void test_passes(void) {
    for (size_t i = 0; i < COUNT(pass_cases); i++) {
        const struct pass_case *c = &pass_cases[i];
        if (c->neighbours > NB) {
            continue;
        }
        struct node x;
        uint8_t frame[TERN_FLOOD_FRAME_MAX], out[TERN_FLOOD_FRAME_MAX];
        int8_t dbm;
        enum tern_flood_kind kind;
        uint8_t h;
        start(&x, c->relay, i + 1);
        neighbours(&x, c->neighbours, -160);
        size_t len = make(frame, 40, c->hops, 1);
        CHECK(tern_flood_heard(&x.f, 0, frame, len));
        tern_time due = tern_flood_due(&x.f);
        if (c->sends < 0) {
            CHECK(due == INT64_MAX);
            continue;
        }
        CHECK(due <= 8 * tern_lora_airtime(&x.lora, 40));
        CHECK(tern_flood_poll(&x.f, due, out, &dbm, &kind, &h) == len);
        CHECK(kind == TERN_FLOOD_RELAY && out[1] == c->sends && (int8_t)out[2] == dbm);
        CHECK(memcmp(out + TERN_FLOOD_HEAD, frame + TERN_FLOOD_HEAD, len - TERN_FLOOD_HEAD) == 0);
        tern_flood_sent(&x.f, due, h);
        CHECK(tern_flood_due(&x.f) == INT64_MAX);
        /* Sent, it is still seen: a copy that comes round is not passed on again. */
        CHECK(!tern_flood_heard(&x.f, due + 1, out, len));
        CHECK(tern_flood_due(&x.f) == INT64_MAX);
    }
}

static void test_copies(void) {
    for (size_t i = 0; i < COUNT(copy_cases); i++) {
        struct node x;
        uint8_t frame[TERN_FLOOD_FRAME_MAX];
        start(&x, true, 3);
        size_t len = make(frame, 60, 5, 2);
        CHECK(tern_flood_heard(&x.f, 0, frame, len));
        for (unsigned k = 1; k < copy_cases[i].received; k++) {
            frame[1] = (uint8_t)(4 - k % 2); /* a copy is a copy whatever its hops and power */
            frame[2] = (uint8_t)k;
            CHECK(!tern_flood_heard(&x.f, 1, frame, len));
        }
        CHECK((tern_flood_due(&x.f) == INT64_MAX) == copy_cases[i].drops);
    }

    /* A copy heard once the frame is with the caller: it is no longer wanted, and taking it back
     * returns what it was charged. */
    struct node x;
    uint8_t frame[TERN_FLOOD_FRAME_MAX], out[TERN_FLOOD_FRAME_MAX];
    int8_t dbm;
    enum tern_flood_kind kind;
    uint8_t h;
    start(&x, true, 4);
    size_t len = make(frame, 60, 5, 3);
    int64_t full = x.f.relay.have;
    CHECK(tern_flood_heard(&x.f, 0, frame, len));
    tern_time due = tern_flood_due(&x.f);
    CHECK(tern_flood_poll(&x.f, due, out, &dbm, &kind, &h) == len);
    CHECK(tern_flood_wanted(&x.f, h) && x.f.relay.have < full);
    CHECK(!tern_flood_heard(&x.f, due, frame, len));
    CHECK(!tern_flood_wanted(&x.f, h));
    tern_flood_withdrawn(&x.f, due, h);
    CHECK(x.f.relay.have == full && tern_flood_due(&x.f) == INT64_MAX);
    CHECK_EQ_U64(x.f.counts.cancelled, 1);
}

/* The wait before a frame is passed on: never longer than the vector's bound, and not one time. */
static void test_waits(void) {
    for (size_t i = 0; i < COUNT(wait_cases); i++) {
        const struct wait_case *c = &wait_cases[i];
        tern_time least = INT64_MAX, most = 0;
        for (uint64_t seed = 1; seed <= 200; seed++) {
            struct node x;
            uint8_t frame[TERN_FLOOD_FRAME_MAX];
            memset(&x, 0, sizeof x);
            x.lora = tern_region_lora(tern_region(TERN_REGION_US915));
            x.lora.sf = c->sf;
            x.lora.bw_hz = c->bw;
            struct tern_route_config rc = tern_route_defaults(&x.lora, FULL, LOWEST, true);
            struct tern_flood_config fc = tern_flood_defaults();
            tern_route_init(&x.r, &rc, 0x1000, x.nb, NB, x.dest, DESTS, 0, 7, 0);
            tern_flood_init(&x.f, &fc, &x.r, x.slot, SLOTS, x.seen, TERN_FLOOD_SEEN, seed, 0);
            if (seed == 1) {
                CHECK_EQ_I64(tern_lora_airtime(&x.lora, c->len), c->airtime);
            }
            CHECK(tern_flood_heard(&x.f, 0, frame, make(frame, c->len, 5, 4)));
            tern_time due = tern_flood_due(&x.f);
            least = due < least ? due : least;
            most = due > most ? due : most;
        }
        CHECK(least >= 0 && most <= c->longest && most > c->longest / 2 && least < c->longest / 2);
    }
}

static void test_seen(void) {
    for (size_t i = 0; i < COUNT(seen_cases); i++) {
        const struct seen_case *c = &seen_cases[i];
        struct node x;
        uint8_t frame[TERN_FLOOD_FRAME_MAX];
        size_t taken = 0;
        start(&x, false, 5);
        for (size_t k = 0; k < c->n_asks; k++) {
            const struct seen_ask *a = &c->asks[k];
            while (taken < c->n_takes && c->takes[taken].at <= a->at) {
                CHECK(tern_flood_heard(&x.f, c->takes[taken].at, frame,
                                       make(frame, 40, 5, c->takes[taken].id)));
                taken++;
            }
            /* A frame not seen is taken as seen by the asking, which the cases allow for: each
             * such id is asked after once, and last. */
            if (tern_flood_heard(&x.f, a->at, frame, make(frame, 40, 5, a->id)) == a->seen) {
                fprintf(stderr, "seen %s: ask %zu\n", c->name, k);
                check_failures++;
            }
        }
    }

    /* An id is held until TERN_FLOOD_SEEN more have come, and no longer. */
    struct node x;
    uint8_t frame[TERN_FLOOD_FRAME_MAX];
    start(&x, false, 6);
    for (unsigned k = 0; k <= TERN_FLOOD_SEEN; k++) {
        CHECK(tern_flood_heard(&x.f, 0, frame, make(frame, 40, 5, k)));
    }
    CHECK(!tern_flood_heard(&x.f, 0, frame, make(frame, 40, 5, 1)));
    CHECK(tern_flood_heard(&x.f, 0, frame, make(frame, 40, 5, 0)));
}

/* A node's own frames: with the hops a flood starts with, at once while the allowance lasts, and
 * then as it fills. The numbers are the vector's first allowance case. */
static void test_own(void) {
    struct node x;
    uint8_t frame[TERN_FLOOD_FRAME_MAX], out[TERN_FLOOD_FRAME_MAX];
    int8_t dbm;
    enum tern_flood_kind kind;
    uint8_t h;
    start(&x, false, 8);
    CHECK(!tern_flood_send(&x.f, 0, frame, make(frame, 30, 0, 0))); /* shorter than any */
    for (unsigned k = 0; k < 10; k++) {
        CHECK(tern_flood_send(&x.f, 0, frame, make(frame, 255, 0, k)));
        size_t len = tern_flood_poll(&x.f, 0, out, &dbm, &kind, &h);
        if (k < 9) {
            CHECK(len == 255 && kind == TERN_FLOOD_OWN && out[1] == 5 && dbm == FULL);
            CHECK((int8_t)out[2] == FULL && out[4] == k);
            tern_flood_sent(&x.f, 0, h);
        } else {
            CHECK(len == 0);
        }
    }
    CHECK_EQ_I64(tern_flood_due(&x.f), 41536000000LL);
    CHECK(tern_flood_poll(&x.f, 41535999999LL, out, &dbm, &kind, &h) == 0);
    CHECK(tern_flood_poll(&x.f, 41536000000LL, out, &dbm, &kind, &h) == 255);
    tern_flood_sent(&x.f, 41536000000LL, h);
    /* What a node sent comes back from its neighbours: seen, and nothing to it. */
    CHECK(!tern_flood_heard(&x.f, 41536000001LL, out, 255));
    CHECK_EQ_U64(x.f.counts.heard, 0);
}

/* A position to a group must leave the allowance room for a 255-byte frame of words, counting
 * what of the node's own still waits. Asking spends nothing. */
static void test_own_room(void) {
    struct node x;
    uint8_t frame[TERN_FLOOD_FRAME_MAX], out[TERN_FLOOD_FRAME_MAX];
    int8_t dbm;
    enum tern_flood_kind kind;
    uint8_t h;
    start(&x, false, 9);
    /* Full, the bucket holds 3 s of airtime: 9.35 frames of 255 bytes at US915, 321 ms each. A
     * 33-byte position is 70 ms. */
    CHECK(tern_flood_own_room(&x.f, 0, 33));
    CHECK(tern_flood_own_room(&x.f, 0, 33)); /* asking twice is asking once */
    /* One sent and paid for, and seven waiting: a position and a frame still fit, 9.22 of 9.35.
     * An eighth waiting, and they do not. */
    CHECK(tern_flood_send(&x.f, 0, frame, make(frame, 255, 0, 0)));
    CHECK(tern_flood_poll(&x.f, 0, out, &dbm, &kind, &h) == 255);
    tern_flood_sent(&x.f, 0, h);
    for (unsigned k = 1; k < 8; k++) {
        CHECK(tern_flood_send(&x.f, 0, frame, make(frame, 255, 0, k)));
    }
    CHECK(tern_flood_own_room(&x.f, 0, 33));
    CHECK(tern_flood_send(&x.f, 0, frame, make(frame, 255, 0, 8)));
    CHECK(!tern_flood_own_room(&x.f, 0, 33));
    /* Sent rather than waiting, they cost the same. */
    while (tern_flood_poll(&x.f, 0, out, &dbm, &kind, &h) != 0) {
        tern_flood_sent(&x.f, 0, h);
    }
    CHECK(!tern_flood_own_room(&x.f, 0, 33));
    /* And the bucket fills again. */
    CHECK(tern_flood_own_room(&x.f, TERN_S(600), 33));
}

/* A frame of this node's that is no longer to go: let go of while it waits, and not wanted if the
 * caller already has it, with what it was charged given back. */
static void test_cancel(void) {
    struct node x;
    uint8_t frame[TERN_FLOOD_FRAME_MAX], out[TERN_FLOOD_FRAME_MAX], id[TERN_FLOOD_ID];
    int8_t dbm;
    enum tern_flood_kind kind;
    uint8_t h;
    start(&x, false, 11);
    int64_t full = x.f.own.have;
    size_t len = make(frame, 60, 0, 1);
    tern_flood_id(frame, len, id);
    CHECK(!tern_flood_cancel(&x.f, 0, id));
    CHECK(tern_flood_send(&x.f, 0, frame, len));
    CHECK(tern_flood_cancel(&x.f, 0, id) && tern_flood_due(&x.f) == INT64_MAX);
    CHECK(!tern_flood_cancel(&x.f, 0, id));

    CHECK(tern_flood_send(&x.f, 0, frame, len));
    CHECK(tern_flood_poll(&x.f, 0, out, &dbm, &kind, &h) == len && x.f.own.have < full);
    CHECK(tern_flood_cancel(&x.f, 0, id) && !tern_flood_wanted(&x.f, h));
    tern_flood_withdrawn(&x.f, 0, h);
    CHECK(x.f.own.have == full && tern_flood_due(&x.f) == INT64_MAX);
    CHECK(tern_flood_poll(&x.f, 1, out, &dbm, &kind, &h) == 0);
}

/* Frames to pass on that the allowance cannot pay for are dropped, not kept. */
static void test_unpaid(void) {
    struct node x;
    uint8_t frame[TERN_FLOOD_FRAME_MAX], out[TERN_FLOOD_FRAME_MAX];
    int8_t dbm;
    enum tern_flood_kind kind;
    uint8_t h;
    unsigned sent = 0;
    start(&x, true, 9);
    /* 3% of a minute is 1.8 s: five frames of 320.768 ms. Eight come at one instant, and all
     * are looked at once every wait is over. */
    for (unsigned k = 0; k < 8; k++) {
        CHECK(tern_flood_heard(&x.f, 0, frame, make(frame, 255, 5, k)));
    }
    tern_time late = 8 * tern_lora_airtime(&x.lora, 255);
    while (tern_flood_poll(&x.f, late, out, &dbm, &kind, &h) != 0) {
        tern_flood_sent(&x.f, late, h);
        sent++;
    }
    CHECK_EQ_U64(sent, 5);
    CHECK_EQ_U64(x.f.counts.unpaid, 3);
    CHECK(tern_flood_due(&x.f) == INT64_MAX);
}

/* A busy relay passes fewer on: as often as the vectors say, over many frames. */
static void test_busy(void) {
    enum { FRAMES = 2000 };
    uint8_t frame[TERN_FLOOD_FRAME_MAX], out[TERN_FLOOD_FRAME_MAX];
    int8_t dbm;
    enum tern_flood_kind kind;
    uint8_t h;
    for (size_t i = 0; i < COUNT(busy_cases); i++) {
        const struct busy_case *c = &busy_cases[i];
        CHECK_EQ_U64(tern_flood_busy_drops(c->busy_ppm, 200000), c->drops_ppm);
        CHECK_EQ_U64(tern_flood_busy_drops(c->busy_ppm, 1000000), 0);

        /* With an allowance that always pays, so that only being busy drops a frame. */
        struct node x;
        start(&x, true, 11 + i);
        struct tern_flood_config fc = tern_flood_defaults();
        fc.relay_ppm = 1000000;
        tern_flood_init(&x.f, &fc, &x.r, x.slot, SLOTS, x.seen, TERN_FLOOD_SEEN, 11 + i, 0);
        unsigned sent = 0;
        for (unsigned k = 1; k <= FRAMES; k++) {
            /* A second apart, the radio on the air that share of all the time gone by. */
            tern_time now = TERN_S(k);
            tern_flood_radio(&x.f, now, now / 1000000 * c->busy_ppm);
            CHECK(tern_flood_heard(&x.f, now, frame, make(frame, 40, 5, k)));
            tern_time due = tern_flood_due(&x.f);
            CHECK(due < now + TERN_S(1));
            tern_flood_radio(&x.f, due, due / 1000000 * c->busy_ppm);
            if (tern_flood_poll(&x.f, due, out, &dbm, &kind, &h) != 0) {
                tern_flood_sent(&x.f, due, h);
                sent++;
            }
        }
        unsigned dropped = FRAMES - sent,
                 should = (unsigned)((uint64_t)FRAMES * c->drops_ppm / 1000000);
        CHECK_EQ_U64(x.f.counts.busy, dropped);
        CHECK_EQ_U64(x.f.counts.unpaid, 0);
        if (c->drops_ppm == 0 || c->drops_ppm == 1000000) {
            CHECK_EQ_U64(dropped, should);
        } else {
            CHECK(dropped + FRAMES / 20 >= should && dropped <= should + FRAMES / 20);
        }
    }
}

/* A draw below how often it drops, and the frame is dropped. */
static void test_draws(void) {
    for (size_t i = 0; i < COUNT(draw_cases); i++) {
        const struct draw_case *c = &draw_cases[i];
        CHECK((c->draw < tern_flood_busy_drops(c->busy_ppm, 200000)) == c->drops);
    }
}

/* The radio's time on, in all, up to `now`. */
static tern_time on_air(const struct share_case *c, tern_time now) {
    tern_time sum = 0;
    for (size_t i = 0; i < c->radios; i++) {
        tern_time to = c->radio[i].to < now ? c->radio[i].to : now;
        sum += to > c->radio[i].from ? to - c->radio[i].from : 0;
    }
    return sum;
}

/* The busy share, told of the radio every second: within what any span the draft allows finds. */
static void test_shares(void) {
    for (size_t i = 0; i < COUNT(share_cases); i++) {
        const struct share_case *c = &share_cases[i];
        for (size_t k = 0; k < c->n; k++) {
            struct node x;
            start(&x, true, 3);
            for (tern_time t = 0; t <= c->asks[k].at; t += TERN_S(1)) {
                tern_flood_radio(&x.f, t, on_air(c, t));
            }
            CHECK(x.f.busy >= c->asks[k].least && x.f.busy <= c->asks[k].most);
        }
    }

    /* Not told for more than a minute, it keeps count afresh: the half minute never idle that
     * went before is not spread over the gap, nor counted. */
    struct node x;
    start(&x, true, 3);
    tern_flood_radio(&x.f, 0, 0);
    tern_flood_radio(&x.f, TERN_S(30), TERN_S(30));
    CHECK_EQ_U64(x.f.busy, 1000000);
    tern_flood_radio(&x.f, TERN_S(300), TERN_S(30));
    CHECK_EQ_U64(x.f.busy, 0);
    tern_flood_radio(&x.f, TERN_S(310), TERN_S(35));
    CHECK_EQ_U64(x.f.busy, 500000);
}

/* Being busy drops nothing of a node's own, and what it drops is not charged to the allowance. */
static void test_busy_own(void) {
    struct node x;
    uint8_t frame[TERN_FLOOD_FRAME_MAX], out[TERN_FLOOD_FRAME_MAX];
    int8_t dbm;
    enum tern_flood_kind kind;
    uint8_t h;
    start(&x, true, 5);
    tern_flood_radio(&x.f, 0, 0);
    tern_flood_radio(&x.f, TERN_S(40), TERN_S(40)); /* never idle */
    for (unsigned k = 0; k < 6; k++) {
        CHECK(tern_flood_heard(&x.f, TERN_S(40), frame, make(frame, 255, 5, k)));
    }
    CHECK(tern_flood_send(&x.f, TERN_S(40), frame, make(frame, 60, 0, 99)));
    tern_time late = TERN_S(40) + 8 * tern_lora_airtime(&x.lora, 255);
    tern_flood_radio(&x.f, late, late);
    CHECK(tern_flood_poll(&x.f, late, out, &dbm, &kind, &h) == 60 && kind == TERN_FLOOD_OWN);
    tern_flood_sent(&x.f, late, h);
    CHECK(tern_flood_poll(&x.f, late, out, &dbm, &kind, &h) == 0);
    CHECK_EQ_U64(x.f.counts.busy, 6);
    CHECK_EQ_U64(x.f.counts.unpaid, 0);

    /* Idle for the next minute and more, it passes on as many as the allowance ever paid for. */
    tern_time t = late + TERN_S(70);
    tern_flood_radio(&x.f, t, late);
    tern_flood_radio(&x.f, t + TERN_S(31), late);
    t += TERN_S(31);
    unsigned sent = 0;
    for (unsigned k = 0; k < 8; k++) {
        CHECK(tern_flood_heard(&x.f, t, frame, make(frame, 255, 5, 100 + k)));
    }
    late = t + 8 * tern_lora_airtime(&x.lora, 255);
    tern_flood_radio(&x.f, late, TERN_S(40) + 8 * tern_lora_airtime(&x.lora, 255));
    while (tern_flood_poll(&x.f, late, out, &dbm, &kind, &h) != 0) {
        tern_flood_sent(&x.f, late, h);
        sent++;
    }
    CHECK_EQ_U64(sent, 5);
    CHECK_EQ_U64(x.f.counts.busy, 6);
}

/* How loud: as a frame for every neighbour, and for every relay a selected route goes through. */
static void test_power(void) {
    struct node x;
    uint8_t frame[TERN_FLOOD_FRAME_MAX], out[TERN_FLOOD_FRAME_MAX];
    int8_t dbm;
    enum tern_flood_kind kind;
    uint8_t h;
    start(&x, false, 10);
    neighbours(&x, 9, -400); /* nine near relays: a frame for every neighbour goes at -9 */
    x.nb[9] = (struct tern_route_neighbour){
        .id = 0x3000, .used = true, .relay = true, .up = true, .floor = 20};
    CHECK(tern_flood_send(&x.f, 0, frame, make(frame, 40, 0, 1)));
    CHECK(tern_flood_poll(&x.f, 0, out, &dbm, &kind, &h) == 40 && dbm == -9);
    tern_flood_sent(&x.f, 0, h);
    /* A route selected through the far one: 1.25 dBm and the margin, rounded up. */
    x.dest[0] = (struct tern_route_dest){.id = 0x4000, .used = true, .sel = 10};
    CHECK(tern_flood_send(&x.f, 0, frame, make(frame, 40, 0, 2)));
    CHECK(tern_flood_poll(&x.f, 0, out, &dbm, &kind, &h) == 40 && dbm == 12);
    CHECK((int8_t)out[2] == 12);
}

/* Several nodes, each hearing those the test joins it to. Nothing is lost and nothing collides:
 * this checks who sends and who is reached, not the radio. */
#define NET 10
static struct node net_node[NET];
static bool net_hears[NET][NET];
static unsigned net_got[NET], net_sent[NET];

static void net_start(unsigned n, unsigned crowd) {
    memset(net_hears, 0, sizeof net_hears);
    memset(net_got, 0, sizeof net_got);
    memset(net_sent, 0, sizeof net_sent);
    for (unsigned i = 0; i < n; i++) {
        start(&net_node[i], true, 0x1234 + i);
        neighbours(&net_node[i], crowd, -160);
    }
}

/* Runs until nobody has anything left to send. */
static void net_run(unsigned n) {
    for (;;) {
        unsigned who = n;
        tern_time at = INT64_MAX;
        for (unsigned i = 0; i < n; i++) {
            tern_time due = tern_flood_due(&net_node[i].f);
            if (due < at) {
                at = due;
                who = i;
            }
        }
        if (who == n) {
            return;
        }
        uint8_t frame[TERN_FLOOD_FRAME_MAX];
        int8_t dbm;
        enum tern_flood_kind kind;
        uint8_t h;
        size_t len = tern_flood_poll(&net_node[who].f, at, frame, &dbm, &kind, &h);
        if (len == 0) {
            continue;
        }
        tern_flood_sent(&net_node[who].f, at, h);
        net_sent[who]++;
        for (unsigned i = 0; i < n; i++) {
            if (net_hears[i][who]) {
                net_got[i] += tern_flood_heard(&net_node[i].f, at, frame, len);
            }
        }
    }
}

static void test_reach(void) {
    uint8_t frame[TERN_FLOOD_FRAME_MAX];

    /* A line of bridges: no hop is spent, and the frame goes the whole line, each node sending it
     * once and taking it once. */
    net_start(NET, 0);
    for (unsigned i = 0; i + 1 < NET; i++) {
        net_hears[i][i + 1] = net_hears[i + 1][i] = true;
    }
    CHECK(tern_flood_send(&net_node[0].f, 0, frame, make(frame, 50, 0, 1)));
    net_run(NET);
    for (unsigned i = 0; i < NET; i++) {
        CHECK_EQ_U64(net_got[i], i > 0);
        CHECK_EQ_U64(net_sent[i], 1);
    }

    /* The same line, each node in a crowd: four pass it on, the fifth takes it and does not,
     * and the sixth never hears of it. */
    net_start(NET, 9);
    for (unsigned i = 0; i + 1 < NET; i++) {
        net_hears[i][i + 1] = net_hears[i + 1][i] = true;
    }
    CHECK(tern_flood_send(&net_node[0].f, 0, frame, make(frame, 50, 0, 2)));
    net_run(NET);
    for (unsigned i = 0; i < NET; i++) {
        CHECK_EQ_U64(net_got[i], i >= 1 && i <= 5);
        CHECK_EQ_U64(net_sent[i], i <= 4);
    }

    /* Ten nodes that all hear each other: the first to pass it on is the last. Everyone has it
     * from the writer, and one copy more is enough for the rest to stay silent. */
    net_start(NET, 9);
    for (unsigned i = 0; i < NET; i++) {
        for (unsigned k = 0; k < NET; k++) {
            net_hears[i][k] = i != k;
        }
    }
    CHECK(tern_flood_send(&net_node[3].f, 0, frame, make(frame, 50, 0, 3)));
    net_run(NET);
    unsigned sent = 0;
    for (unsigned i = 0; i < NET; i++) {
        CHECK_EQ_U64(net_got[i], i != 3);
        sent += net_sent[i];
    }
    CHECK_EQ_U64(sent, 2);
}

int main(void) {
    RUN(test_heads);
    RUN(test_rules);
    RUN(test_passes);
    RUN(test_copies);
    RUN(test_waits);
    RUN(test_seen);
    RUN(test_own);
    RUN(test_own_room);
    RUN(test_unpaid);
    RUN(test_busy);
    RUN(test_draws);
    RUN(test_shares);
    RUN(test_busy_own);
    RUN(test_cancel);
    RUN(test_power);
    RUN(test_reach);
    return CHECK_DONE();
}
