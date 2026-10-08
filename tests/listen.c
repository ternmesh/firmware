#include "tern/listen.h"

#include "check.h"
#include "tern/region.h"

/* Listening first against the specification's vectors (tests/vectors/forwarding.json, listens),
 * and what the vectors cannot hold. */

enum listen_radio { PREAMBLE, HEADER, END, SENT };
struct listen_event {
    tern_time at;
    enum listen_radio radio;
};
struct listen_ask {
    tern_time at;
    bool receiving;
};
struct listen_case {
    uint8_t sf;
    uint32_t bw_hz;
    const struct listen_event *events;
    size_t event_count;
    const struct listen_ask *asks;
    size_t ask_count;
};

#include "listening.h"

#define COUNT(a) (sizeof(a) / sizeof(a)[0])

/* A profile's modulation, as the vectors' cases are: the profiles' preamble and coding rate. */
static struct tern_lora modulation(uint8_t sf, uint32_t bw_hz) {
    struct tern_lora m = tern_lora_default(sf, bw_hz);
    m.preamble = TERN_PREAMBLE;
    return m;
}

static void tell(struct tern_listen *l, const struct listen_event *e) {
    switch (e->radio) {
    case PREAMBLE:
        tern_listen_preamble(l, e->at);
        break;
    case HEADER:
        tern_listen_header(l, e->at);
        break;
    case END:
    case SENT:
        tern_listen_over(l);
        break;
    }
}

/* Each ask is answered by a listener told every event at or before it, and no other. */
static void the_vectors_say_when_a_radio_is_receiving(void) {
    CHECK(COUNT(listen_cases) >= 27);
    for (size_t i = 0; i < COUNT(listen_cases); i++) {
        const struct listen_case *c = &listen_cases[i];
        struct tern_lora m = modulation(c->sf, c->bw_hz);
        for (size_t a = 0; a < c->ask_count; a++) {
            struct tern_listen l;
            tern_listen_init(&l);
            for (size_t e = 0; e < c->event_count && c->events[e].at <= c->asks[a].at; e++) {
                tell(&l, &c->events[e]);
            }
            bool got = tern_listen_receiving(&l, &m, c->asks[a].at);
            if (got != c->asks[a].receiving) {
                fprintf(stderr, "case %zu, ask %zu: receiving %d, expected %d\n", i, a, got,
                        c->asks[a].receiving);
            }
            CHECK(got == c->asks[a].receiving);
        }
    }
}

static void a_radio_that_has_said_nothing_is_not_receiving(void) {
    struct tern_listen l;
    struct tern_lora m = modulation(9, 500000);
    tern_listen_init(&l);
    CHECK(!tern_listen_receiving(&l, &m, 0));
    CHECK(!tern_listen_receiving(&l, &m, INT64_MAX));
}

/* A longer preamble is waited out for longer: a header cannot come before the preamble ends. */
static void the_wait_for_a_header_grows_with_the_preamble(void) {
    struct tern_listen l;
    struct tern_lora m = modulation(9, 500000);
    tern_time symbol = tern_lora_symbol(&m);
    tern_listen_init(&l);
    tern_listen_preamble(&l, 1000);
    m.preamble = 8;
    CHECK(tern_listen_receiving(&l, &m, 1000 + 21 * symbol - 1));
    CHECK(!tern_listen_receiving(&l, &m, 1000 + 21 * symbol));
    m.preamble = 32;
    CHECK(tern_listen_receiving(&l, &m, 1000 + 45 * symbol - 1));
    CHECK(!tern_listen_receiving(&l, &m, 1000 + 45 * symbol));
}

int main(void) {
    RUN(the_vectors_say_when_a_radio_is_receiving);
    RUN(a_radio_that_has_said_nothing_is_not_receiving);
    RUN(the_wait_for_a_header_grows_with_the_preamble);
    return CHECK_DONE();
}
