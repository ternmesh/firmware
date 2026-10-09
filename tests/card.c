#include "tern/card.h"

#include <string.h>

#include "check.h"

/* Presence cards against the specification's vectors (tests/vectors/cards.json), and the table of
 * cards held: newest from each address, room made by forgetting the one heard longest ago, and
 * forgotten after CARD_KEPT. */

struct accepted_case {
    uint8_t seed[32];
    uint8_t address[32];
    uint32_t number;
    const char *name;
    size_t name_len;
    uint8_t hops;
    int8_t power;
    const uint8_t *signed_part;
    size_t signed_len;
    const uint8_t *frame;
    size_t len;
};
struct rejected_case {
    const char *why;
    const uint8_t *frame;
    size_t len;
};
struct delivery_case {
    const uint8_t *frame;
    size_t len;
    bool keep;
};

#include "cards.h"

#define COUNT(a) (sizeof(a) / sizeof(a)[0])

static enum tern_card_verdict receive(struct tern_cards *c, tern_time now, const uint8_t *frame,
                                      size_t len, size_t *index) {
    bool gone;
    uint8_t gone_address[TERN_ADDRESS_LEN];
    return tern_cards_receive(c, self_address, now, frame, len, index, &gone, gone_address);
}

static void accepted_cards(void) {
    for (size_t i = 0; i < COUNT(accepted); i++) {
        const struct accepted_case *c = &accepted[i];
        struct tern_identity id;
        uint8_t frame[TERN_CARD_MAX];
        tern_identity_init(&id, c->seed);
        CHECK(memcmp(id.address, c->address, 32) == 0);
        size_t len = tern_card_write(&id, c->number, (const uint8_t *)c->name, c->name_len, frame);
        CHECK(len == c->len);
        frame[1] = c->hops;
        frame[2] = (uint8_t)c->power;
        CHECK(memcmp(frame, c->frame, c->len) == 0);
        /* What was signed is the label, hdr, address, number and name: the vector's. */
        CHECK(c->signed_len == 12 + c->len - TERN_SIGNATURE_LEN - 2);
        CHECK(tern_address_verify(id.address, c->signed_part, c->signed_len,
                                  c->frame + c->len - TERN_SIGNATURE_LEN));
        tern_identity_wipe(&id);

        struct tern_card place[2];
        struct tern_cards t;
        size_t at = 9;
        tern_cards_init(&t, place, 2);
        CHECK(receive(&t, 5, c->frame, c->len, &at) == TERN_CARD_ACCEPTED);
        CHECK(at < 2 && place[at].held && place[at].heard == 5);
        CHECK(memcmp(place[at].address, c->address, 32) == 0 && place[at].number == c->number);
        CHECK(place[at].name_len == c->name_len &&
              memcmp(place[at].name, c->name, c->name_len) == 0);
    }
}

static void names_too_long_are_not_written(void) {
    struct tern_identity id;
    uint8_t frame[TERN_CARD_MAX], name[TERN_CARD_NAME_MAX + 1];
    memset(name, 'x', sizeof name);
    tern_identity_init(&id, accepted[0].seed);
    CHECK(tern_card_write(&id, 1, name, sizeof name, frame) == 0);
    CHECK(tern_card_write(&id, 1, (const uint8_t *)"\xff", 1, frame) == 0);
    CHECK(tern_card_write(&id, 1, name, 0, frame) == TERN_CARD_MIN);
    tern_identity_wipe(&id);
}

static void rejected_cards(void) {
    for (size_t i = 0; i < COUNT(rejected); i++) {
        struct tern_card place[2];
        struct tern_cards t;
        size_t at;
        tern_cards_init(&t, place, 2);
        if (receive(&t, 0, rejected[i].frame, rejected[i].len, &at) == TERN_CARD_ACCEPTED) {
            fprintf(stderr, "rejected: %s: accepted\n", rejected[i].why);
            check_failures++;
        }
        CHECK(!place[0].held && !place[1].held);
    }
}

static void relayed_card_is_still_good(void) {
    struct tern_card place[1];
    struct tern_cards t;
    size_t at;
    tern_cards_init(&t, place, 1);
    CHECK(receive(&t, 0, relayed, sizeof relayed, &at) == TERN_CARD_ACCEPTED);
}

static void deliveries_in_order(void) {
    struct tern_card place[4];
    struct tern_cards t;
    tern_cards_init(&t, place, 4);
    for (size_t i = 0; i < COUNT(deliveries); i++) {
        size_t at;
        bool kept = receive(&t, (tern_time)i, deliveries[i].frame, deliveries[i].len, &at) ==
                    TERN_CARD_ACCEPTED;
        if (kept != deliveries[i].keep) {
            fprintf(stderr, "delivery %zu: %s\n", i, kept ? "kept" : "discarded");
            check_failures++;
        }
    }
}

/* Two cards from two addresses, in a table of one: the second takes the first's place, and the
 * caller is told whose card went. Then neither is held a day later. */
static void room_and_forgetting(void) {
    struct tern_card place[1];
    struct tern_cards t;
    size_t at;
    bool gone = false;
    uint8_t gone_address[TERN_ADDRESS_LEN], forgot[TERN_ADDRESS_LEN];
    tern_cards_init(&t, place, 1);
    CHECK(tern_cards_due(&t, TERN_CARD_KEPT) == INT64_MAX);
    CHECK(tern_cards_receive(&t, self_address, 10, accepted[0].frame, accepted[0].len, &at, &gone,
                             gone_address) == TERN_CARD_ACCEPTED);
    CHECK(!gone);
    CHECK(tern_cards_receive(&t, self_address, 20, accepted[2].frame, accepted[2].len, &at, &gone,
                             gone_address) == TERN_CARD_ACCEPTED);
    CHECK(gone && memcmp(gone_address, accepted[0].address, 32) == 0);
    CHECK(memcmp(place[0].address, accepted[2].address, 32) == 0);

    CHECK(tern_cards_due(&t, TERN_CARD_KEPT) == 20 + TERN_CARD_KEPT);
    CHECK(!tern_cards_forget(&t, 20 + TERN_CARD_KEPT - 1, TERN_CARD_KEPT, forgot));
    CHECK(tern_cards_forget(&t, 20 + TERN_CARD_KEPT, TERN_CARD_KEPT, forgot));
    CHECK(memcmp(forgot, accepted[2].address, 32) == 0 && !place[0].held);
    CHECK(!tern_cards_forget(&t, 20 + TERN_CARD_KEPT, TERN_CARD_KEPT, forgot));

    /* Forgetting forgets the number: the same card is new again. */
    CHECK(tern_cards_receive(&t, self_address, 30, accepted[2].frame, accepted[2].len, &at, &gone,
                             gone_address) == TERN_CARD_ACCEPTED);
}

int main(void) {
    RUN(accepted_cards);
    RUN(names_too_long_are_not_written);
    RUN(rejected_cards);
    RUN(relayed_card_is_still_good);
    RUN(deliveries_in_order);
    RUN(room_and_forgetting);
    return CHECK_DONE();
}
