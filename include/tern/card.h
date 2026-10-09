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

/* Writes this node's card into out: `number` higher than any it has sent with this address, and
 * the name its user chose, UTF-8 of at most TERN_CARD_NAME_MAX bytes, or none. hops and power are
 * zeros, for the flood to fill in. Returns its length, 103 + name_len, or 0 if the name is too
 * long or not UTF-8. */
size_t tern_card_write(const struct tern_identity *id, uint32_t number, const uint8_t *name,
                       size_t name_len, uint8_t out[TERN_CARD_MAX]);

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
 * set and that address written to `gone_address`, for the caller to say it is forgotten. The
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
