#include "tern/route.h"

#include <string.h>

#include "check.h"
#include "tern/region.h"

/* The rules of draft/routing.md, each against the specification's vectors. How routers behave
 * together is in tests/mesh.c. */

struct id_case {
    uint8_t address[32];
    uint32_t id;
};
struct newer_case {
    uint16_t a, b;
    bool newer;
};
struct promise_case {
    uint64_t seconds;
    uint16_t code;
    int64_t read_seconds; /* -1 for no promise */
};
struct announce_case {
    struct tern_announce fields;
    uint8_t seed[32];
    uint8_t address[32]; /* the signer's */
    size_t len;
    uint8_t frame[255];
};
struct verified_case {
    int announce;
    bool held;
    uint8_t held_address[32];
    bool takes;
};
struct request_case {
    struct tern_request fields;
    size_t len;
    uint8_t frame[255];
};
struct rejected_case {
    const char *why;
    size_t len;
    uint8_t frame[255];
};
struct floor_case {
    uint8_t sf;
    int8_t full;
    int steps;
    struct {
        int8_t power;
        int16_t snr_q;
        int32_t floor;
        uint8_t margin;
    } heard[8];
};
struct link_case {
    bool has_own;
    int32_t own;
    uint8_t theirs;
    bool up;
};
struct named_case {
    uint16_t named, number, round;
    bool withdrawn;
};
struct numbering_case {
    uint16_t last, number;
    bool promise_passed, starting, was_starting;
    const char *does;
};
struct cost_case {
    const char *profile;
    uint16_t cost;
};
struct feasible_case {
    struct tern_route_fd fd;
    uint16_t seq, metric;
    bool feasible;
};
struct selection_case {
    struct tern_route_fd fd;
    int n;
    struct tern_route_choice routes[4];
    int selected, selects;
};
struct place_case {
    int n;
    struct {
        int32_t floor;
        bool up;
    } neighbours[4];
    int32_t floor;
    int replaces;
};
struct kept_case {
    struct tern_route_fd fd;
    struct tern_route_choice routes[4];
    int selected;
    struct tern_route_choice offered;
    int replaces;
};

struct default_case {
    int n;
    struct {
        int32_t floor;
        bool up, relay;
    } neighbours[4];
    bool leaf, starting;
    uint32_t busy;
    int tried_count;
    int tried[4];
    int next;
    uint16_t link_cost, metric;
};

#include "routing.h"

#define COUNT(a) (sizeof(a) / sizeof(a)[0])

/* Ed25519 under an identity, as a board's router signs and checks. */
static void sign_ed25519(void *ctx, const uint8_t *m, size_t len, uint8_t sig[TERN_ANNOUNCE_SIG]) {
    tern_identity_sign(ctx, m, len, sig);
}

static bool verify_ed25519(void *ctx, const uint8_t address[TERN_ADDRESS_LEN], const uint8_t *m,
                           size_t len, const uint8_t sig[TERN_ANNOUNCE_SIG]) {
    (void)ctx;
    return tern_address_valid(address) && tern_address_verify(address, m, len, sig);
}

/* A router that signs and checks, as the node with that seed. */
static void route_signed(struct tern_route *r, struct tern_identity *id, const uint8_t seed[32]) {
    tern_identity_init(id, seed);
    tern_route_auth(
        r, &(struct tern_route_auth){
               .address = id->address, .ctx = id, .sign = sign_ed25519, .verify = verify_ed25519});
}

static void ids_come_from_addresses(void) {
    for (size_t i = 0; i < COUNT(ids); i++) {
        CHECK_EQ_U64(tern_route_id(ids[i].address), ids[i].id);
    }
}

static void sequence_numbers_wrap(void) {
    for (size_t i = 0; i < COUNT(newers); i++) {
        CHECK(tern_route_newer(newers[i].a, newers[i].b) == newers[i].newer);
    }
}

static void promises_round_up(void) {
    for (size_t i = 0; i < COUNT(promises); i++) {
        const struct promise_case *p = &promises[i];
        CHECK_EQ_I64(tern_route_promise_code(p->seconds), p->code);
        CHECK_EQ_I64(tern_route_promise_time(p->code),
                     p->read_seconds < 0 ? TERN_ROUTE_NO_PROMISE : TERN_S(p->read_seconds));
    }
}

static void announces_are_written_and_read(void) {
    for (size_t i = 0; i < COUNT(announces); i++) {
        const struct announce_case *c = &announces[i];
        uint8_t frame[255], m[TERN_ANNOUNCE_LABEL_LEN + TERN_ROUTE_FRAME_MAX];
        struct tern_announce a;
        struct tern_identity id;
        tern_identity_init(&id, c->seed);
        CHECK(memcmp(id.address, c->address, 32) == 0);
        CHECK_EQ_I64(tern_announce_write(&c->fields, frame), c->len);
        tern_identity_sign(&id, m, tern_announce_signed(frame, c->len, m),
                           frame + c->len - TERN_ANNOUNCE_SIG);
        CHECK(memcmp(frame, c->frame, c->len) == 0);
        CHECK(verify_ed25519(NULL, c->address, m, tern_announce_signed(c->frame, c->len, m),
                             c->frame + c->len - TERN_ANNOUNCE_SIG));
        CHECK(tern_route_frame(c->frame, c->len));
        CHECK(tern_announce_read(&a, c->frame, c->len));
        CHECK(a.sender == c->fields.sender && a.number == c->fields.number);
        CHECK(a.seq == c->fields.seq && a.relay == c->fields.relay);
        CHECK(a.starting == c->fields.starting);
        CHECK(a.promise == c->fields.promise && a.round == c->fields.round);
        CHECK(a.power == c->fields.power);
        CHECK(a.named_count == c->fields.named_count && a.route_count == c->fields.route_count);
        CHECK(a.carries_address == c->fields.carries_address);
        CHECK(!a.carries_address || memcmp(a.address, c->address, 32) == 0);
        CHECK(memcmp(a.sig, c->frame + c->len - TERN_ANNOUNCE_SIG, TERN_ANNOUNCE_SIG) == 0);
        for (int k = 0; k < a.named_count; k++) {
            CHECK(a.named[k].id == c->fields.named[k].id);
            CHECK(a.named[k].margin == c->fields.named[k].margin);
        }
        for (int k = 0; k < a.route_count; k++) {
            CHECK(a.routes[k].destination == c->fields.routes[k].destination);
            CHECK(a.routes[k].seq == c->fields.routes[k].seq);
            CHECK(a.routes[k].metric == c->fields.routes[k].metric);
        }
    }
}

static void requests_are_written_and_read(void) {
    for (size_t i = 0; i < COUNT(requests); i++) {
        const struct request_case *c = &requests[i];
        uint8_t frame[255];
        struct tern_request q;
        CHECK_EQ_I64(tern_request_write(&c->fields, frame), c->len);
        CHECK(memcmp(frame, c->frame, c->len) == 0);
        CHECK(tern_request_read(&q, c->frame, c->len));
        CHECK(q.next == c->fields.next && q.count == c->fields.count);
        for (int k = 0; k < q.count; k++) {
            CHECK(q.asks[k].destination == c->fields.asks[k].destination);
            CHECK(q.asks[k].seq == c->fields.asks[k].seq);
            CHECK(q.asks[k].hops == c->fields.asks[k].hops);
        }
    }
}

/* A router that takes a frame gains a neighbour, or changes its Trickle interval: one that
 * discards it is left exactly as it was, whether or not it held the sender's address. */
static void frames_the_specification_rejects_change_nothing(void) {
    const struct tern_region *us = tern_region(TERN_REGION_US915);
    struct tern_lora lora = tern_region_lora(us);
    struct tern_route_config config = tern_route_defaults(&lora, 22, -9, true);
    static const uint8_t own_seed[32] = {0x77};
    for (int holding = 0; holding < 2; holding++) {
        for (size_t i = 0; i < COUNT(rejected); i++) {
            struct tern_route_neighbour nb[4], nb_before[4];
            struct tern_route_dest dest[4], dest_before[4];
            struct tern_route r, before;
            struct tern_identity id;
            tern_route_init(&r, &config, VECTOR_OWN_ID, nb, 4, dest, 4, 0, 7, 0);
            route_signed(&r, &id, own_seed);
            if (holding) { /* the address of the sender of announces[1] and [3] */
                tern_route_heard(&r, TERN_S(1), announces[3].frame, announces[3].len, 40);
                CHECK(nb[0].used && nb[0].has_address);
            }
            r.interval = TERN_S(64); /* so that anything taken as a request shows */
            before = r;
            memcpy(nb_before, nb, sizeof nb);
            memcpy(dest_before, dest, sizeof dest);
            tern_route_heard(&r, TERN_S(2), rejected[i].frame, rejected[i].len, 40);
            before.now = r.now;
            if (memcmp(&r, &before, sizeof r) != 0 || memcmp(nb, nb_before, sizeof nb) != 0 ||
                memcmp(dest, dest_before, sizeof dest) != 0) {
                fprintf(stderr, "took %s%s\n", rejected[i].why, holding ? ", holding" : "");
                check_failures++;
            }
        }
    }
    /* And the frame the last of them was made from is taken. */
    {
        struct tern_route_neighbour nb[4];
        struct tern_route_dest dest[4];
        struct tern_route r;
        struct tern_identity id;
        tern_route_init(&r, &config, VECTOR_OWN_ID, nb, 4, dest, 4, 0, 7, 0);
        route_signed(&r, &id, own_seed);
        tern_route_heard(&r, TERN_S(1), announces[3].frame, announces[3].len, 40);
        CHECK(nb[0].used && nb[0].id == announces[3].fields.sender);
        CHECK(nb[0].has_address && memcmp(nb[0].address, announces[3].address, 32) == 0);
    }
}

/* Whether an announce is taken depends on the address held for its sender: one that carries none
 * is checked with the one held, and with none held there is nothing to check it with. */
static void an_announce_is_taken_only_if_it_can_be_checked(void) {
    const struct tern_region *us = tern_region(TERN_REGION_US915);
    struct tern_lora lora = tern_region_lora(us);
    struct tern_route_config config = tern_route_defaults(&lora, 22, -9, true);
    static const uint8_t own_seed[32] = {0x77};
    for (size_t i = 0; i < COUNT(verifieds); i++) {
        const struct verified_case *c = &verifieds[i];
        const struct announce_case *a = &announces[c->announce];
        struct tern_route_neighbour nb[4];
        struct tern_route_dest dest[4];
        struct tern_route r;
        struct tern_identity id;
        tern_route_init(&r, &config, VECTOR_OWN_ID, nb, 4, dest, 4, 0, 7, 0);
        route_signed(&r, &id, own_seed);
        if (c->held) { /* a neighbour already, its last announce the one before this */
            nb[0] = (struct tern_route_neighbour){.id = a->fields.sender,
                                                  .used = true,
                                                  .relay = a->fields.relay,
                                                  .number = (uint16_t)(a->fields.number - 1),
                                                  .named = (uint16_t)(a->fields.number - 1),
                                                  .promise = TERN_S(600),
                                                  .has_address = true};
            memcpy(nb[0].address, c->held_address, 32);
        }
        tern_route_heard(&r, TERN_S(1), a->frame, a->len, 40);
        bool took = nb[0].used && nb[0].id == a->fields.sender && nb[0].number == a->fields.number;
        if (took != c->takes) {
            fprintf(stderr, "verified case %zu: took %d\n", i, took);
            check_failures++;
        }
        if (took) {
            CHECK(nb[0].has_address && memcmp(nb[0].address, a->address, 32) == 0);
        }
    }
}

/* What goes on the air is signed, and carries the address while the node is starting and while a
 * neighbour gives it no margin, and not once every neighbour does. */
static void a_router_signs_and_carries_its_address_while_it_is_needed(void) {
    const struct tern_region *us = tern_region(TERN_REGION_US915);
    struct tern_lora lora = tern_region_lora(us);
    struct tern_route_config config = tern_route_defaults(&lora, 22, -9, false);
    static const uint8_t seed_a[32] = {0x0A}, seed_b[32] = {0x0B};
    struct tern_route_neighbour nb_a[4], nb_b[4];
    struct tern_route_dest dest_a[4], dest_b[4];
    struct tern_route a, b;
    struct tern_identity id_a, id_b;
    uint8_t frame[TERN_ROUTE_FRAME_MAX];
    int8_t dbm;
    tern_time now = 0;
    bool carried_after = false, settled = false;
    int after = 0;
    tern_identity_init(&id_a, seed_a);
    tern_identity_init(&id_b, seed_b);
    tern_route_init(&a, &config, tern_route_id(id_a.address), nb_a, 4, dest_a, 4, 0, 1, 0);
    tern_route_init(&b, &config, tern_route_id(id_b.address), nb_b, 4, dest_b, 4, 0, 2, 0);
    route_signed(&a, &id_a, seed_a);
    route_signed(&b, &id_b, seed_b);
    for (int step = 0; step < 4000 && after < 200; step++) {
        struct tern_route *from = step % 2 ? &b : &a, *to = step % 2 ? &a : &b;
        tern_time due = tern_route_due(from);
        now = due > now ? due : now;
        size_t len;
        while ((len = tern_route_poll(from, now, frame, &dbm)) != 0) {
            struct tern_announce x;
            uint8_t m[TERN_ANNOUNCE_LABEL_LEN + TERN_ROUTE_FRAME_MAX];
            tern_route_sent(from, now);
            if (frame[0] != TERN_HDR_ANNOUNCE) {
                tern_route_heard(to, now, frame, len, 40);
                continue;
            }
            CHECK(tern_announce_read(&x, frame, len));
            CHECK(verify_ed25519(NULL, from == &a ? id_a.address : id_b.address, m,
                                 tern_announce_signed(frame, len, m), x.sig));
            if (x.starting) {
                CHECK(x.carries_address);
            }
            if (settled && x.carries_address) {
                carried_after = true; /* once both name each other, it stops */
            }
            tern_route_heard(to, now, frame, len, 40);
        }
        settled = settled || (!a.starting && !b.starting && nb_a[0].used && nb_a[0].theirs != 0 &&
                              nb_b[0].used && nb_b[0].theirs != 0);
        after += settled;
    }
    CHECK(settled);
    CHECK(!carried_after);
    CHECK(nb_a[0].has_address && memcmp(nb_a[0].address, id_b.address, 32) == 0);
    CHECK(nb_b[0].has_address && memcmp(nb_b[0].address, id_a.address, 32) == 0);
}

/* One announce from `id`, heard at `snr` decibels, naming this node with `margin` if that is not
 * 0. Sent at 0 dBm, so the floor it gives is the modulation's floor less `snr`. */
static void hear(struct tern_route *r, uint32_t id, int snr, uint8_t margin) {
    uint8_t frame[TERN_ROUTE_FRAME_MAX];
    struct tern_announce a = {.sender = id, .number = 1, .seq = 1, .promise = 60, .round = 1};
    if (margin) {
        a.named_count = 1;
        a.named[0] = (struct tern_announce_named){.id = VECTOR_OWN_ID, .margin = margin};
    }
    tern_route_heard(r, r->now + TERN_S(1), frame, tern_announce_write(&a, frame),
                     (int16_t)(4 * snr));
}

static bool keeps(const struct tern_route_neighbour *nb, size_t cap, uint32_t id) {
    for (size_t i = 0; i < cap; i++) {
        if (nb[i].used && nb[i].id == id) {
            return true;
        }
    }
    return false;
}

static void a_full_table_keeps_the_nearest_and_every_link_that_is_up(void) {
    const struct tern_region *us = tern_region(TERN_REGION_US915);
    struct tern_lora lora = tern_region_lora(us);
    struct tern_route_config config = tern_route_defaults(&lora, 22, -9, true);
    struct tern_route_neighbour nb[2];
    struct tern_route_dest dest[8];
    struct tern_route r;

    /* Neither link up: the one further off gives way, to a node 6 dB nearer and no less. */
    tern_route_init(&r, &config, VECTOR_OWN_ID, nb, 2, dest, 8, 0, 7, 0);
    hear(&r, 0xA, 0, 0);
    hear(&r, 0xB, 10, 0);
    hear(&r, 0xC, 5, 0);
    CHECK(keeps(nb, 2, 0xA) && keeps(nb, 2, 0xB) && !keeps(nb, 2, 0xC));
    hear(&r, 0xC, 6, 0);
    CHECK(!keeps(nb, 2, 0xA) && keeps(nb, 2, 0xB) && keeps(nb, 2, 0xC));
    /* And what was held through the one that went has gone with it. */
    CHECK(!tern_route_next(&r, 0xA, &(uint32_t){0}, &(uint16_t){0}));

    /* A link that is up stays, however far off: the other gives way. */
    tern_route_init(&r, &config, VECTOR_OWN_ID, nb, 2, dest, 8, 0, 7, 0);
    hear(&r, 0xA, 0, 200);
    hear(&r, 0xB, 10, 0);
    hear(&r, 0xC, 30, 0);
    CHECK(keeps(nb, 2, 0xA) && !keeps(nb, 2, 0xB) && keeps(nb, 2, 0xC));

    /* With every link up there is no room, whoever asks. */
    tern_route_init(&r, &config, VECTOR_OWN_ID, nb, 2, dest, 8, 0, 7, 0);
    hear(&r, 0xA, 0, 200);
    hear(&r, 0xB, 10, 200);
    hear(&r, 0xC, 30, 0);
    CHECK(keeps(nb, 2, 0xA) && keeps(nb, 2, 0xB) && !keeps(nb, 2, 0xC));
}

static void a_full_table_gives_the_place_the_specification_says(void) {
    for (size_t i = 0; i < COUNT(places); i++) {
        const struct place_case *c = &places[i];
        struct tern_route_neighbour nb[4] = {0};
        for (int k = 0; k < c->n; k++) {
            nb[k] = (struct tern_route_neighbour){
                .used = true, .floor = c->neighbours[k].floor, .up = c->neighbours[k].up};
        }
        CHECK_EQ_I64(tern_route_place(nb, (size_t)c->n, c->floor), c->replaces);
    }
}

static void floors_average_and_margins_round_down(void) {
    for (size_t i = 0; i < COUNT(floors); i++) {
        const struct floor_case *c = &floors[i];
        int32_t floor = 0;
        for (int k = 0; k < c->steps; k++) {
            floor = tern_route_floor(k == 0, floor, c->heard[k].power, c->heard[k].snr_q, c->sf);
            CHECK_EQ_I64(floor, c->heard[k].floor);
            CHECK_EQ_I64(tern_route_margin(c->full, floor), c->heard[k].margin);
        }
    }
}

static void links_come_up_and_stay_within_the_band(void) {
    bool up = false;
    for (size_t i = 0; i < COUNT(links); i++) {
        /* A node with no floor for a neighbour has not heard it, and has no margin from it. */
        up = links[i].has_own && tern_route_link_up(up, links[i].own, links[i].theirs);
        CHECK(up == links[i].up);
    }
}

static void margins_are_withdrawn_after_eight_rounds(void) {
    for (size_t i = 0; i < COUNT(nameds); i++) {
        const struct named_case *c = &nameds[i];
        CHECK(tern_route_withdrawn(c->named, c->number, c->round) == c->withdrawn);
    }
}

static void a_neighbour_that_starts_again_is_told_from_a_late_frame(void) {
    static const char *const names[] = {"take", "discard", "again"};
    for (size_t i = 0; i < COUNT(numberings); i++) {
        const struct numbering_case *c = &numberings[i];
        CHECK(strcmp(names[tern_route_numbering(c->last, c->number, c->promise_passed, c->starting,
                                                c->was_starting)],
                     c->does) == 0);
    }
}

static void a_link_costs_the_reference_frame(void) {
    for (size_t i = 0; i < COUNT(costs); i++) {
        bool found = false;
        for (int id = 1; id < TERN_REGION_END; id++) {
            const struct tern_region *g = tern_region((enum tern_region_id)id);
            if (strcmp(g->name, costs[i].profile) == 0) {
                struct tern_lora lora = tern_region_lora(g);
                CHECK_EQ_I64(tern_route_link_cost(&lora), costs[i].cost);
                found = true;
            }
        }
        CHECK(found);
    }
}

static void feasibility_is_babels(void) {
    for (size_t i = 0; i < COUNT(feasibles); i++) {
        const struct feasible_case *c = &feasibles[i];
        CHECK(tern_route_feasible(&c->fd, c->seq, c->metric) == c->feasible);
    }
}

static void the_lowest_feasible_metric_is_selected_with_hysteresis(void) {
    for (size_t i = 0; i < COUNT(selections); i++) {
        const struct selection_case *c = &selections[i];
        CHECK_EQ_I64(tern_route_select(&c->fd, c->routes, c->n, c->selected), c->selects);
    }
}

static void the_route_least_worth_keeping_gives_way(void) {
    for (size_t i = 0; i < COUNT(kepts); i++) {
        const struct kept_case *c = &kepts[i];
        CHECK_EQ_I64(tern_route_replace(&c->fd, c->routes, c->selected, &c->offered), c->replaces);
    }
}

/* A neighbour's id here is its index plus one. */
static void a_leaf_with_no_route_takes_its_nearest_relay(void) {
    for (size_t i = 0; i < COUNT(defaults); i++) {
        const struct default_case *c = &defaults[i];
        struct tern_route_neighbour nb[4] = {0};
        uint32_t tried[4];
        for (int k = 0; k < c->n; k++) {
            nb[k] = (struct tern_route_neighbour){.id = (uint32_t)k + 1,
                                                  .used = true,
                                                  .relay = c->neighbours[k].relay,
                                                  .up = c->neighbours[k].up,
                                                  .floor = c->neighbours[k].floor};
        }
        for (int k = 0; k < c->tried_count; k++) {
            tried[k] = (uint32_t)c->tried[k] + 1;
        }
        struct tern_lora l = tern_region_lora(tern_region(TERN_REGION_US915));
        struct tern_route_config d = tern_route_defaults(&l, 20, -9, false);
        CHECK_EQ_I64(tern_route_default(nb, (size_t)c->n, c->leaf, c->starting, c->busy,
                                        d.default_busy_ppm, tried, c->tried_count),
                     c->next);
        CHECK_EQ_U64(tern_route_default_metric(d.default_hops, c->link_cost), c->metric);
    }
    /* A guard of 1000000 is none: a leaf whose radio was never idle still takes its relay. */
    struct tern_route_neighbour relay = {.id = 1, .used = true, .relay = true, .up = true};
    CHECK_EQ_I64(tern_route_default(&relay, 1, true, false, 1000000, 1000000, NULL, 0), 0);
    CHECK_EQ_I64(tern_route_default(&relay, 1, true, false, 999999, 999999, NULL, 0), -1);
}

int main(void) {
    RUN(ids_come_from_addresses);
    RUN(a_leaf_with_no_route_takes_its_nearest_relay);
    RUN(sequence_numbers_wrap);
    RUN(promises_round_up);
    RUN(announces_are_written_and_read);
    RUN(requests_are_written_and_read);
    RUN(frames_the_specification_rejects_change_nothing);
    RUN(an_announce_is_taken_only_if_it_can_be_checked);
    RUN(a_router_signs_and_carries_its_address_while_it_is_needed);
    RUN(floors_average_and_margins_round_down);
    RUN(links_come_up_and_stay_within_the_band);
    RUN(margins_are_withdrawn_after_eight_rounds);
    RUN(a_neighbour_that_starts_again_is_told_from_a_late_frame);
    RUN(a_full_table_gives_the_place_the_specification_says);
    RUN(a_full_table_keeps_the_nearest_and_every_link_that_is_up);
    RUN(a_link_costs_the_reference_frame);
    RUN(feasibility_is_babels);
    RUN(the_lowest_feasible_metric_is_selected_with_hysteresis);
    RUN(the_route_least_worth_keeping_gives_way);
    return CHECK_DONE();
}
