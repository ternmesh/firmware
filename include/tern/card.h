#ifndef TERN_CARD_H
#define TERN_CARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tern/address.h"
#include "tern/flood.h"
#include "tern/time.h"

/* Presence cards: specification draft 0, draft/cards.md in ternmesh/spec.
 *
 * A card is how a node whose user chooses says, to the nodes near it, who it is: its address, in
 * clear, and a name, signed with the address's key. It is flooded (tern/flood.h) two hops, every
 * two hours or so, and only while the user has cards on. A node holds the newest card from each
 * address it hears, for its user to see as who is about.
 *
 * Like the rest of the core this keeps no clock and no flash: when to send, and the number that
 * only rises, are the caller's. The table of cards held is the caller's memory too. */

#define TERN_CARD_HDR TERN_HDR_CARD
#define TERN_CARD_NAME_MAX 31
#define TERN_CARD_MIN TERN_FLOOD_CARD_MIN
#define TERN_CARD_MAX TERN_FLOOD_CARD_MAX
#define TERN_CARD_HOPS TERN_FLOOD_CARD_HOPS
#define TERN_CARD_EVERY TERN_S(2 * 3600) /* between one node's cards, on average */
#define TERN_CARD_KEPT TERN_S(24 * 3600) /* a card not heard again for this long is forgotten */

/* Asking (draft/cards.md, "Asking"): a node asks those in earshot for their cards with one byte,
 * which nobody passes on; one with cards on answers with a card of hops 0, at a random time within
 * TERN_CARD_ASK_SPREAD, unless a card of its own went less than TERN_CARD_SOON before or one is
 * waiting to go. A node asks no more often than TERN_CARD_ASK_GAP. Every parameter is a guess. */
#define TERN_CARD_ASK 0x69                /* format 01, type 101 (a card), flags 001: the frame */
#define TERN_CARD_SOON TERN_S(15 * 60)    /* no answer within this long of the node's last card */
#define TERN_CARD_ASK_GAP TERN_S(10 * 60) /* between one node's asks */
#define TERN_CARD_ASK_SPREAD TERN_S(30)   /* within which a node answers */

/* Writes this node's card into out: `number` higher than any it has sent with this address, and
 * the name its user chose, UTF-8 of at most TERN_CARD_NAME_MAX bytes, or none. hops and power are
 * zeros, for the flood to fill in. Returns its length, 103 + name_len, or 0 if the name is too
 * long or not UTF-8. */
size_t tern_card_write(const struct tern_identity *id, uint32_t number, const uint8_t *name,
                       size_t name_len, uint8_t out[TERN_CARD_MAX]);

/* Whether a frame is an ask: exactly the one byte. A frame that starts with it and is longer is
 * not, and is discarded. */
static inline bool tern_card_ask(const uint8_t *frame, size_t len) {
    return len == 1 && frame[0] == TERN_CARD_ASK;
}

/* Whether a node whose cards are on answers an ask heard at `now`: not if one of its own cards
 * went on the air less than TERN_CARD_SOON before (`sent`, and when, `last`), nor if one is
 * waiting to go (`waiting`). An answer is a card like any other, and counts as one once it goes. */
static inline bool tern_card_answers(bool sent, tern_time last, bool waiting, tern_time now) {
    return !waiting && (!sent || now - last >= TERN_CARD_SOON);
}

/* Whether a node whose user asks it to ask, at `now`, sends an ask: not within TERN_CARD_ASK_GAP
 * of its last (`asked`, and when, `last`). */
static inline bool tern_card_may_ask(bool asked, tern_time last, tern_time now) {
    return !asked || now - last >= TERN_CARD_ASK_GAP;
}

/* A card held. */
struct tern_card {
    bool held;
    uint8_t name_len;
    uint8_t address[TERN_ADDRESS_LEN];
    uint32_t number;
    tern_time heard; /* when it was accepted */
    uint8_t name[TERN_CARD_NAME_MAX];
};

/* What became of a card received. */
enum tern_card_verdict {
    TERN_CARD_ACCEPTED = 1,
    TERN_CARD_MALFORMED, /* not a card's length or hdr, or a name that is not UTF-8 */
    TERN_CARD_REFUSED,   /* this node's own address, or not a valid one */
    TERN_CARD_FORGED,    /* the signature is not the address's */
    TERN_CARD_OLD,       /* its number is not higher than that of the card held from it */
};

/* The cards a node holds, in memory the caller gives. */
struct tern_cards {
    struct tern_card *card;
    size_t cap;
};

/* Starts a table over cap places, holding nothing. */
void tern_cards_init(struct tern_cards *c, struct tern_card *places, size_t cap);

/* Receives a card, as the flood hands it over, for a node whose address is `self`. If it is
 * accepted it is held, in place of any held from its address: `*index` says where. With no room,
 * the card heard longest ago is forgotten for it, and if that was another address's, `*gone` is
 * set and that address written to `gone_address`, for the caller to say it is forgotten. A table
 * of no places checks a card and holds nothing: `*index` is then 0, which is not a place. The
 * checks that cost least come first, so a card heard again costs no signature check. */
enum tern_card_verdict tern_cards_receive(struct tern_cards *c,
                                          const uint8_t self[TERN_ADDRESS_LEN], tern_time now,
                                          const uint8_t *frame, size_t len, size_t *index,
                                          bool *gone, uint8_t gone_address[TERN_ADDRESS_LEN]);

/* Forgets one card not heard again since `now - kept`, the one heard longest ago, and writes its
 * address. False when there is none to forget. Call it until it is false. */
bool tern_cards_forget(struct tern_cards *c, tern_time now, tern_time kept,
                       uint8_t address[TERN_ADDRESS_LEN]);

/* When tern_cards_forget() will next have one to forget, or INT64_MAX. */
tern_time tern_cards_due(const struct tern_cards *c, tern_time kept);

#endif
