#include "tern/card.h"

#include <string.h>

#include "tern/companion.h"

#define LABEL "tern v0 card"
#define LABEL_LEN 12
#define ADDRESS_AT TERN_FLOOD_HEAD
#define NUMBER_AT (ADDRESS_AT + TERN_ADDRESS_LEN)
#define NAME_AT (NUMBER_AT + 4)

/* M = "tern v0 card" || hdr || address || number || name. */
static size_t signed_part(const uint8_t *frame, size_t name_len, uint8_t *m) {
    memcpy(m, LABEL, LABEL_LEN);
    m[LABEL_LEN] = frame[0];
    memcpy(m + LABEL_LEN + 1, frame + ADDRESS_AT, TERN_ADDRESS_LEN + 4 + name_len);
    return LABEL_LEN + 1 + TERN_ADDRESS_LEN + 4 + name_len;
}

size_t tern_card_write(const struct tern_identity *id, uint32_t number, const uint8_t *name,
                       size_t name_len, uint8_t out[TERN_CARD_MAX]) {
    uint8_t m[LABEL_LEN + 1 + TERN_ADDRESS_LEN + 4 + TERN_CARD_NAME_MAX];
    if (name_len > TERN_CARD_NAME_MAX || !tern_companion_utf8(name, name_len)) {
        return 0;
    }
    out[0] = TERN_CARD_HDR;
    out[1] = 0;
    out[2] = 0;
    memcpy(out + ADDRESS_AT, id->address, TERN_ADDRESS_LEN);
    out[NUMBER_AT] = (uint8_t)(number >> 24);
    out[NUMBER_AT + 1] = (uint8_t)(number >> 16);
    out[NUMBER_AT + 2] = (uint8_t)(number >> 8);
    out[NUMBER_AT + 3] = (uint8_t)number;
    if (name_len) {
        memcpy(out + NAME_AT, name, name_len);
    }
    size_t m_len = signed_part(out, name_len, m);
    tern_identity_sign(id, m, m_len, out + NAME_AT + name_len);
    return NAME_AT + name_len + TERN_SIGNATURE_LEN;
}

void tern_cards_init(struct tern_cards *c, struct tern_card *places, size_t cap) {
    c->card = places;
    c->cap = cap;
    for (size_t i = 0; i < cap; i++) {
        places[i].held = false;
    }
}

static struct tern_card *held_from(struct tern_cards *c, const uint8_t *address) {
    for (size_t i = 0; i < c->cap; i++) {
        if (c->card[i].held && memcmp(c->card[i].address, address, TERN_ADDRESS_LEN) == 0) {
            return &c->card[i];
        }
    }
    return NULL;
}

enum tern_card_verdict tern_cards_receive(struct tern_cards *c,
                                          const uint8_t self[TERN_ADDRESS_LEN], tern_time now,
                                          const uint8_t *frame, size_t len, size_t *index,
                                          bool *gone, uint8_t gone_address[TERN_ADDRESS_LEN]) {
    uint8_t m[LABEL_LEN + 1 + TERN_ADDRESS_LEN + 4 + TERN_CARD_NAME_MAX];
    *gone = false;
    if (len < TERN_CARD_MIN || len > TERN_CARD_MAX || frame[0] != TERN_CARD_HDR) {
        return TERN_CARD_MALFORMED;
    }
    const uint8_t *address = frame + ADDRESS_AT;
    size_t name_len = len - TERN_CARD_MIN;
    if (!tern_companion_utf8(frame + NAME_AT, name_len)) {
        return TERN_CARD_MALFORMED;
    }
    if (memcmp(address, self, TERN_ADDRESS_LEN) == 0) {
        return TERN_CARD_REFUSED;
    }
    uint32_t number = (uint32_t)frame[NUMBER_AT] << 24 | (uint32_t)frame[NUMBER_AT + 1] << 16 |
                      (uint32_t)frame[NUMBER_AT + 2] << 8 | frame[NUMBER_AT + 3];
    /* Step 4 before 2 and 3: a card no newer than the one held is discarded whatever else is true
     * of it, and that costs no point decoded and no signature checked. */
    struct tern_card *held = held_from(c, address);
    if (held != NULL && number <= held->number) {
        return TERN_CARD_OLD;
    }
    if (!tern_address_valid(address)) {
        return TERN_CARD_REFUSED;
    }
    size_t m_len = signed_part(frame, name_len, m);
    if (!tern_address_verify(address, m, m_len, frame + NAME_AT + name_len)) {
        return TERN_CARD_FORGED;
    }

    if (held == NULL) {
        for (size_t i = 0; i < c->cap && held == NULL; i++) {
            if (!c->card[i].held) {
                held = &c->card[i];
            }
        }
    }
    if (held == NULL) {
        if (c->cap == 0) {
            return TERN_CARD_ACCEPTED; /* checked, and nowhere to hold it */
        }
        held = &c->card[0];
        for (size_t i = 1; i < c->cap; i++) {
            if (c->card[i].heard < held->heard) {
                held = &c->card[i];
            }
        }
        *gone = true;
        memcpy(gone_address, held->address, TERN_ADDRESS_LEN);
    }
    held->held = true;
    memcpy(held->address, address, TERN_ADDRESS_LEN);
    held->number = number;
    held->heard = now;
    held->name_len = (uint8_t)name_len;
    memcpy(held->name, frame + NAME_AT, name_len);
    *index = (size_t)(held - c->card);
    return TERN_CARD_ACCEPTED;
}

static struct tern_card *oldest(const struct tern_cards *c) {
    struct tern_card *o = NULL;
    for (size_t i = 0; i < c->cap; i++) {
        if (c->card[i].held && (o == NULL || c->card[i].heard < o->heard)) {
            o = &c->card[i];
        }
    }
    return o;
}

bool tern_cards_forget(struct tern_cards *c, tern_time now, tern_time kept,
                       uint8_t address[TERN_ADDRESS_LEN]) {
    struct tern_card *o = oldest(c);
    if (o == NULL || now - o->heard < kept) {
        return false;
    }
    memcpy(address, o->address, TERN_ADDRESS_LEN);
    o->held = false;
    return true;
}

tern_time tern_cards_due(const struct tern_cards *c, tern_time kept) {
    const struct tern_card *o = oldest(c);
    return o == NULL ? INT64_MAX : o->heard + kept;
}
