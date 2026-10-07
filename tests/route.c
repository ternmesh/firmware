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
    size_t len;
    uint8_t frame[255];
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
    bool promise_passed, names_none, had_margin;
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
struct kept_case {
    struct tern_route_fd fd;
    struct tern_route_choice routes[4];
    int selected;
    struct tern_route_choice offered;
    int replaces;
};

#include "routing.h"

#define COUNT(a) (sizeof(a) / sizeof(a)[0])

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
        uint8_t frame[255];
        struct tern_announce a;
        CHECK_EQ_I64(tern_announce_write(&c->fields, frame), c->len);
        CHECK(memcmp(frame, c->frame, c->len) == 0);
        CHECK(tern_route_frame(c->frame, c->len));
        CHECK(tern_announce_read(&a, c->frame, c->len));
        CHECK(a.sender == c->fields.sender && a.number == c->fields.number);
        CHECK(a.seq == c->fields.seq && a.relay == c->fields.relay);
        CHECK(a.promise == c->fields.promise && a.round == c->fields.round);
        CHECK(a.power == c->fields.power);
        CHECK(a.named_count == c->fields.named_count && a.route_count == c->fields.route_count);
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
 * discards it is left exactly as it was. */
static void frames_the_specification_rejects_change_nothing(void) {
    const struct tern_region *us = tern_region(TERN_REGION_US915);
    struct tern_lora lora = tern_region_lora(us);
    struct tern_route_config config = tern_route_defaults(&lora, 22, -9, true);
    for (size_t i = 0; i < COUNT(rejected); i++) {
        struct tern_route_neighbour nb[4];
        struct tern_route_dest dest[4];
        struct tern_route r, before;
        tern_route_init(&r, &config, VECTOR_OWN_ID, nb, 4, dest, 4, 0, 7, 0);
        r.interval = TERN_S(64); /* so that anything taken as a request shows */
        before = r;
        tern_route_heard(&r, TERN_S(1), rejected[i].frame, rejected[i].len, 40);
        before.now = r.now;
        if (memcmp(&r, &before, sizeof r) != 0 || nb[0].used || dest[0].used) {
            fprintf(stderr, "took %s\n", rejected[i].why);
            check_failures++;
        }
    }
    /* And the frame the last of them was made from is taken. */
    {
        struct tern_route_neighbour nb[4];
        struct tern_route_dest dest[4];
        struct tern_route r;
        tern_route_init(&r, &config, VECTOR_OWN_ID, nb, 4, dest, 4, 0, 7, 0);
        tern_route_heard(&r, TERN_S(1), announces[1].frame, announces[1].len, 40);
        CHECK(nb[0].used && nb[0].id == announces[1].fields.sender);
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
        CHECK(strcmp(names[tern_route_numbering(c->last, c->number, c->promise_passed,
                                                c->names_none, c->had_margin)],
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

int main(void) {
    RUN(ids_come_from_addresses);
    RUN(sequence_numbers_wrap);
    RUN(promises_round_up);
    RUN(announces_are_written_and_read);
    RUN(requests_are_written_and_read);
    RUN(frames_the_specification_rejects_change_nothing);
    RUN(floors_average_and_margins_round_down);
    RUN(links_come_up_and_stay_within_the_band);
    RUN(margins_are_withdrawn_after_eight_rounds);
    RUN(a_neighbour_that_starts_again_is_told_from_a_late_frame);
    RUN(a_link_costs_the_reference_frame);
    RUN(feasibility_is_babels);
    RUN(the_lowest_feasible_metric_is_selected_with_hysteresis);
    RUN(the_route_least_worth_keeping_gives_way);
    return CHECK_DONE();
}
