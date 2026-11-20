#ifndef HELTEC_V3_LINK_H
#define HELTEC_V3_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "capacity.h"
#include "tern/address.h"
#include "tern/card.h"
#include "tern/companion.h"
#include "tern/crypto.h"
#include "tern/group.h"
#include "tern/position.h"
#include "tern/time.h"

/* The node as a client sees it over the companion link (draft/companion.md in ternmesh/spec), and
 * the link's half of the conversation: requests answered, news sent.
 *
 * This is the node model docs/ui.md describes, in the part of it the board cannot read off its
 * radio and router at any moment: the contacts the user saved and the messages, with what became
 * of each. The rest - the board's own settings, its neighbours, its time on the air - main.c
 * hands over as a link_view whenever the link asks, the way it fills the bench screen's
 * node_status, and the link tells a client what changed since it last said.
 *
 * Messages go to the forwarder from main.c, which asks for the next one waiting
 * (link_outgoing()) and says what became of it (link_taken(), link_state()). A client never reaches
 * past this into the protocol.
 *
 * The groups the node holds are here too, with their keys, since they are the user's as contacts
 * are: made, joined, named and left by a client. A group's frames are sealed and opened in main.c
 * with the keys this hands it (link_groups(), link_group_of()), and what is written to a group or
 * received from one, and each invite, is a message among the rest, with an id from the same
 * count.
 *
 * Positions are here too (draft/positions.md): the fix a client gives, whom the user shares it
 * with and how exactly, and the last position received from each contact and from each member of
 * each group. The link decides which position is due where (link_position_next()) and writes it;
 * main.c seals it, hands it over and says what became of it. None of this is saved: after a
 * restart the node shares with nobody until a client turns sharing on again, and a sync says so.
 *
 * The cards the node holds from others are here too (draft/cards.md): main.c hands over each card
 * the flood brings (link_card_received()), and the link keeps the newest from each address for
 * CARD_KEPT and tells clients of them as who is about. Its own card, and whether it sends one, are
 * main.c's: they are settings, and SELF says them (struct link_view).
 *
 * The cards held are saved too, each in its place, so that a restart, or a new image copied on,
 * does not empty who is about until each node's next card, hours away. A board has no clock that
 * runs while it is off, so each is saved with the time it was heard by the node's clock as a
 * client last set it (struct link_view), once there is one: after a restart, the first time a
 * client sets the clock, each card is as old as that says, and one older than CARD_KEPT is
 * forgotten. A card saved before any client set the clock cannot be dated, and is held for
 * CARD_KEPT from the restart.
 *
 * Several clients may drive the node at once, one on each connection: the USB port and a
 * Bluetooth central. Each has its own HELLO, sync and news count, and is answered alone; the
 * contacts and messages are the node's, and news of a change to them goes to every client.
 *
 * Nothing here touches the hardware, so tests/link.c runs it on a host. */

#define LINK_CONTACTS NODE_CONTACTS
#define LINK_GROUPS NODE_GROUPS
#define LINK_MESSAGES 32
#define LINK_REFS 16       /* SENDs remembered, so one sent again is not sent twice */
#define LINK_ID_STEP 64    /* message ids set aside by one write to flash */
#define LINK_COUNT_STEP 64 /* and counts for the node's own group frames */
/* The writers of every group as flash keeps them, at most: for each group its id, how many
 * writers, and each (tern_group_keep()). */
#define LINK_WRITERS_MAX (LINK_GROUPS * (TERN_COMPANION_GROUP + 1 + TERN_GROUP_KEPT))
/* A message as flash keeps it, at most: what does not change, then its text. */
#define LINK_SAVED_HEAD (17 + TERN_ADDRESS_LEN + TERN_COMPANION_GROUP + TERN_GROUP_SECRET)
#define LINK_SAVED_MAX (LINK_SAVED_HEAD + TERN_COMPANION_TEXT_MAX)
/* Saved messages taken out of flash, at most, for one new one: two of the shortest take more
 * room there than one of the longest. */
#define LINK_UNSAVE 2
#define LINK_ASKED 8 /* addresses lately refused, remembered so each is news once a LINK_QUIET */
#define LINK_NEIGHBOURS 64
#define LINK_QUIET TERN_S(10)  /* the least time between two news frames about one thing */
#define LINK_LOOK TERN_S(1)    /* how often the link looks for changes to tell */
#define LINK_SNR_STEP 4        /* quarter-dB: a neighbour's SNR moved this much is news */
#define LINK_LAPSE TERN_S(60)  /* a serial client silent this long since its last answer is gone */
#define LINK_GROUP_POSITIONS 8 /* members' positions held for each group */
#define LINK_CARDS NODE_CARDS  /* cards held from others: who is about */
#define LINK_ROUTES NODE_ROUTES /* how messages went, for the latest of them */
#define LINK_TRACES 4          /* traces out at once (TRACE) */
/* A card held, as saved: its version, the name's length, its number, when it was heard in seconds
 * since 1970 or 0 if the node did not know, its address and its name. */
#define LINK_CARD_SAVED_HEAD (1 + 1 + 4 + 4 + TERN_ADDRESS_LEN)
#define LINK_CARD_SAVED_MAX (LINK_CARD_SAVED_HEAD + TERN_CARD_NAME_MAX)
/* How much of each conversation the user has forgotten (FORGET, FORGET_GROUP): records for
 * addresses and groups together, saved as one: a version, then for each its kind, `through` and
 * the address or the group's id. */
#define LINK_FORGOTTEN TERN_C_FORGOTTEN_KEPT
#define LINK_FORGOTTEN_SAVED_EACH (1 + 4 + TERN_ADDRESS_LEN)
#define LINK_FORGOTTEN_SAVED_MAX (1 + LINK_FORGOTTEN * LINK_FORGOTTEN_SAVED_EACH)

/* The connections: where a frame came from, and where one goes. The third is the board's own:
 * what is typed at its console is asked of the link as a client would ask it, so that a group
 * made there is made as one made from a phone is. */
enum { LINK_SERIAL, LINK_BLE, LINK_BOARD, LINK_CONNS };

/* An owner's requests over the mesh (draft/managing.md) are answered on the board's own connection,
 * borrowed while link_manage() reads one: that connection never syncs, so nothing of it but a few
 * fields needs keeping, and a connection of its own would cost RAM a classic ESP32 does not have.
 * While borrowed its frames are packed into answers, messages for the owner's node, rather than
 * handed to the host. */
#define LINK_MESH LINK_BOARD
#define LINK_OWNERS TERN_C_OWNERS

struct link_neighbour {
    uint32_t id;
    uint8_t role;   /* 0 leaf, 1 relay */
    int8_t snr;     /* quarter-dB */
    uint16_t heard; /* seconds since */
};

/* What main.c knows that the link does not keep: everything in SELF, NEIGHBOUR, AIRTIME and
 * POWER, and the state of the board's satellite receiver, which FIX carries. */
struct link_view {
    uint8_t address[TERN_ADDRESS_LEN];
    uint8_t role;
    const char *region;
    int8_t power;
    uint8_t most;    /* 1 if `power` is the most the node may send at, which it works out */
    uint8_t pairing; /* how a client pairs over Bluetooth: a TERN_C_PAIRING_ */
    uint8_t locked;  /* 1 if the node's Bluetooth is locked (draft/managing.md) */
    uint32_t time;   /* seconds since 1970, or 0 */
    uint8_t cards;   /* 1 if the node sends cards, 0 if not */
    uint8_t card_name_len;
    uint8_t card_name[TERN_COMPANION_NAME_MAX];
    size_t n_neighbours;
    struct link_neighbour neighbours[LINK_NEIGHBOURS];
    uint32_t period_s, allowed_ms, used_ms, wait_ms;
    uint16_t millivolts;
    uint8_t percent, power_flags;
    uint8_t receiver; /* a TERN_C_RECEIVER_ state: none, off, searching or fixed */
    /* Sleeping, as SLEEP says it (version 15): the wake and listen set, in seconds, what the node
     * does now (a TERN_C_SLEEP_), its keeper or the relay it rests, and the seconds until it tries
     * that relay again. */
    uint16_t wake;
    uint8_t listen;
    uint8_t sleep_state;
    uint32_t keeper;
    uint16_t retry;
};

/* What became of saving a message: saved, refused for want of room, or not written for any other
 * reason. */
enum link_saved { LINK_SAVED, LINK_NO_ROOM, LINK_NOT_SAVED };

struct link_host {
    void *ctx;
    const char *firmware;
    /* INFO's board and release: the hardware a client finds an image for, and the version of the
     * firmware, empty if it has none. The board is NULL or empty if the node cannot be updated at
     * all. */
    const char *board, *release;
    /* How a client updates the node, INFO's update: a TERN_C_UPDATES_. A client before version 12
     * knows only the link's way, and is told no board unless the node updates that way. */
    uint8_t updates;
    /* The longest image the node has room for: 0 if it cannot be updated over the link. */
    uint32_t update_room;
    /* The release keys an image over the link must be signed by (tern/release.h), n_release_keys
     * of them: an UPDATE_BEGIN whose signature verifies with none is refused, unless
     * take_unsigned, which only a build for a developer sets (CONFIG_TERN_UPDATE_UNSIGNED), since
     * the node then takes an image from anyone who can pair. */
    const uint8_t (*release_keys)[32];
    size_t n_release_keys;
    bool take_unsigned;
    /* One frame to the client on a connection. */
    void (*out)(void *ctx, unsigned conn, const uint8_t *frame, size_t len);
    void (*view)(void *ctx, struct link_view *v);
    /* SET: 0, or the ERROR code to answer with. */
    uint8_t (*set)(void *ctx, const struct tern_companion_msg *m);
    void (*set_time)(void *ctx, uint32_t time);
    /* Whether the node shares a session with an address. */
    bool (*session)(void *ctx, const uint8_t address[TERN_ADDRESS_LEN]);
    /* How long an address said it sleeps between wakes, in seconds, in the session the node
     * shares with it: 0 for not, or with no session. CONTACT's wake (version 15). May be NULL. */
    uint16_t (*wake)(void *ctx, const uint8_t address[TERN_ADDRESS_LEN]);
    /* Why a new message to an address would wait: a TERN_C_WAIT_ reason. */
    uint8_t (*why)(void *ctx, const uint8_t address[TERN_ADDRESS_LEN]);
    /* END_SESSION: ends the session with an address, if there is one, and lets go of whatever the
     * forwarder holds for it without a word: the link says what became of those messages, once
     * it has answered. 0, or the ERROR code to answer with. */
    uint8_t (*end_session)(void *ctx, const uint8_t address[TERN_ADDRESS_LEN]);
    /* The contacts (struct link_contact), saved as one record and loaded at start: as many places
     * as are in use up to the last, and one at least. Loading gives the length, up to cap, or 0
     * with nothing kept: a build that holds more contacts than the one that saved them loads
     * them all. */
    size_t (*load)(void *ctx, void *buf, size_t cap);
    bool (*save)(void *ctx, const void *buf, size_t len);
    /* The groups (struct link_group_saved), the same way. */
    size_t (*load_groups)(void *ctx, void *buf, size_t cap);
    bool (*save_groups)(void *ctx, const void *buf, size_t len);
    /* Bytes fit to make a key from: a new group's secret. False if there are none to be had. */
    bool (*random)(void *ctx, uint8_t *buf, size_t len);
    /* The first message id not yet set aside, kept so that an id is never given twice
     * (link_init()). */
    bool (*load_ids)(void *ctx, uint32_t *next);
    bool (*save_ids)(void *ctx, uint32_t next);
    /* The first count for a group frame not yet set aside, kept so that the node never seals
     * two frames with one count (link_group_count()). */
    bool (*load_count)(void *ctx, uint32_t *next);
    bool (*save_count)(void *ctx, uint32_t next);
    /* The writers the node has accepted group frames from, saved whole, up to LINK_WRITERS_MAX
     * bytes, so that a restart does not have it read old frames again. Loading gives the
     * length, or 0 with nothing kept; saving a length of 0 forgets them. */
    size_t (*load_writers)(void *ctx, uint8_t *buf, size_t cap);
    bool (*save_writers)(void *ctx, const uint8_t *buf, size_t len);
    /* The forgotten records (struct link_forgotten), saved whole, up to LINK_FORGOTTEN_SAVED_MAX
     * bytes, each time one grows: what the board must always be able to save, as contacts are.
     * Loading gives the length, or 0 with nothing kept. */
    size_t (*load_forgotten)(void *ctx, uint8_t *buf, size_t cap);
    bool (*save_forgotten)(void *ctx, const uint8_t *buf, size_t len);
    /* The messages, each saved alone by its place among LINK_MESSAGES, in two parts: what is
     * written once, up to LINK_SAVED_MAX bytes, and a word for what changes after. Loading gives
     * the length, or 0 with nothing kept there; saving a length of 0 forgets what the place
     * held. A host with no room to spare says so: the link then takes its oldest out of flash
     * and asks again. A message not saved, for that or because the write failed, is held until
     * a restart, and a write that failed takes no other message out of flash. */
    /* The cards held, each saved alone by its place among LINK_CARDS, up to LINK_CARD_SAVED_MAX
     * bytes, only if the host has room to spare for them: a card is never why something the node
     * must keep cannot be saved. Loading gives the length, or 0 with nothing kept there; saving a
     * length of 0 forgets what the place held. */
    size_t (*load_card)(void *ctx, size_t place, uint8_t *buf, size_t cap);
    bool (*save_card)(void *ctx, size_t place, const uint8_t *buf, size_t len);
    size_t (*load_message)(void *ctx, size_t place, uint8_t *buf, size_t cap);
    enum link_saved (*save_message)(void *ctx, size_t place, const uint8_t *buf, size_t len);
    bool (*load_state)(void *ctx, size_t place, uint64_t *state);
    bool (*save_state)(void *ctx, size_t place, uint64_t state);
    /* An update (draft/companion.md, "Updating the firmware"). Begin readies room for an image of
     * `size` bytes, abandoning any update begun before, and is false if it cannot; write puts
     * bytes of it in place, in order, and is false if they could not be written, when the update
     * is abandoned; run is asked once the image is whole and its digest right, and is 0 if the
     * board will run it, restarting once the answer is out, or the ERROR code to answer with. */
    bool (*update_begin)(void *ctx, uint32_t size);
    bool (*update_write)(void *ctx, uint32_t offset, const uint8_t *data, size_t len);
    uint8_t (*update_run)(void *ctx);
    /* OFF: 0 if the board will turn off once the answer is out, or the ERROR code to answer with.
     * Every connection drops then, and nothing on the link turns the board on again. */
    uint8_t (*off)(void *ctx);
    /* UPDATE_BOOTLOADER: 0 if the board will restart into its bootloader's update once the answer
     * is out, or the ERROR code to answer with. Every connection drops then. */
    uint8_t (*bootloader)(void *ctx);
    /* WIPE: 0 if the board will erase everything it holds, its identity and Bluetooth bonds among
     * it, and restart as new once the answer is out, or the ERROR code to answer with. */
    uint8_t (*wipe)(void *ctx);
    /* RESTART: 0 if the board will restart once the answer is out, or the ERROR code to answer
     * with. Every connection drops then. */
    uint8_t (*restart)(void *ctx);
    /* TRACE (version 18): begins a trace to the routing id `to`, which link_traced() will end, as
     * `id`. 0, or the ERROR code to answer with: TERN_C_ERR_REFUSED for the node's own routing id,
     * TERN_C_ERR_NOT_NOW for one it already traces, TERN_C_ERR_FULL with no room. May be NULL. */
    uint8_t (*trace)(void *ctx, uint32_t id, uint32_t to);
    /* The owners' addresses (draft/managing.md, "Owners"), as save_owners last wrote them: the
     * bytes read, at most cap. */
    size_t (*load_owners)(void *ctx, uint8_t *buf, size_t cap);
    bool (*save_owners)(void *ctx, const uint8_t *buf, size_t len);
};

/* An update under way: the image's size and digest, how much of it the node holds, and the digest
 * of that much. */
struct link_update {
    bool on;
    uint32_t size, held;
    uint8_t digest[TERN_SHA256_LEN];
    struct tern_sha256 sha;
};

struct link_contact {
    bool used;
    uint8_t address[TERN_ADDRESS_LEN];
    uint8_t name_len;
    uint8_t name[TERN_COMPANION_NAME_MAX];
};

/* A group as flash keeps it: its secret and the user's name for it. */
struct link_group_saved {
    bool used;
    uint8_t secret[TERN_GROUP_SECRET];
    uint8_t name_len;
    uint8_t name[TERN_COMPANION_NAME_MAX];
};

struct link_group {
    bool used;
    uint8_t id[TERN_COMPANION_GROUP]; /* what a client knows it by */
    uint8_t name_len;
    uint8_t name[TERN_COMPANION_NAME_MAX];
    struct tern_group g;
};

/* What a message is: one to or from an address, one to or from a group, or an invite to a group,
 * to or from an address. */
enum link_kind {
    LINK_KIND_MESSAGE,
    LINK_KIND_GROUP,
    LINK_KIND_INVITE,
    LINK_KIND_MANAGE,
    LINK_KIND_ANSWER,
    LINK_KIND_NOTICE /* to an owner, that the owners changed: its text is all after the kind */
};

/* A MANAGE's request to another node, and an answer to an owner, are messages for the node of
 * kinds 0x05 and 0x06 (draft/managing.md): kept and sent as the others are, so that each is tried
 * again and its end is known, but never saved, shown or synced. A MANAGE's state is told in STATE
 * news; an answer's is told to no one. Their text is the bytes after the kind. */
#define LINK_TEXT_ROOM TERN_COMPANION_TEXT_MAX
#define LINK_MANAGE_KIND 0x05
#define LINK_ANSWER_KIND 0x06

/* Whether a message is one of the user's, to show and sync. */
static inline bool link_for_user(uint8_t kind) { return kind <= LINK_KIND_INVITE; }

struct link_message {
    bool used;
    bool taken; /* with the forwarder or the flooder, which sends it: not to be handed over again */
    bool saved; /* in flash, so what becomes of it is written there too */
    uint8_t kind;
    uint32_t id;
    uint8_t address[TERN_ADDRESS_LEN];   /* not a group message's */
    uint8_t group[TERN_COMPANION_GROUP]; /* a group message's and an invite's */
    uint32_t from;                       /* a received group message's writer, as it claimed */
    uint8_t secret[TERN_GROUP_SECRET];   /* a received invite's, kept for JOIN */
    uint32_t time;
    uint8_t flags, state, reason;
    uint16_t wait;
    uint8_t text_len; /* an invite's is the group's name */
    uint8_t text[LINK_TEXT_ROOM];
};

/* How much of a conversation the user has forgotten: every message and invite with an address,
 * or every message to a group, whose id is `through` or less (draft/companion.md, "Forgotten").
 * A group's record has its id at the start of `key`, and the rest zero. */
struct link_forgotten {
    bool used;
    bool group;
    uint32_t through;
    uint8_t key[TERN_ADDRESS_LEN];
};

struct link_ref {
    bool used;
    bool group; /* a SEND_GROUP's: `to` begins with the group's id, and the rest is zero */
    uint32_t ref, id;
    uint8_t to[TERN_ADDRESS_LEN];
    uint8_t digest[8]; /* of the text: the first bytes of its SHA-256 */
};

/* How a message went (ROUTE, version 18), as draft/companion.md has it: for one received, how the
 * frame that brought it was heard; for one sent, the neighbour it last went to, how many tries
 * went on the air, and once delivered how its acknowledgement was heard and the round trip. Kept
 * for the latest LINK_ROUTES messages, and not saved. */
struct link_route {
    uint32_t id; /* the message's; 0 for a free place */
    uint32_t via;
    uint32_t took; /* ms */
    int16_t rssi;
    uint8_t hops;
    uint8_t goes;
    int8_t snr;
};

struct link_asked {
    bool used;
    uint8_t address[TERN_ADDRESS_LEN];
    tern_time at;
};

struct link_told {
    struct link_neighbour n;
    tern_time at;
};

/* The node's own position: the more recent of the last its receiver gave (link_receiver_fix())
 * and the last a client gave it (SET_POSITION), as draft/positions.md has a node take it. */
struct link_fix {
    bool have;
    uint8_t source;     /* TERN_C_SOURCE_RECEIVER or _CLIENT, while it has one */
    int32_t lat, lon;   /* 10^-7 degree */
    int16_t altitude;   /* above the ellipsoid; TERN_C_NO_ALTITUDE for none */
    uint16_t accuracy;  /* metres, 0 for none */
    uint8_t satellites; /* the receiver's fix's, 0 for a client's */
    tern_time at;       /* when the fix was taken: when it came, less its age */
};

/* Sharing the node's position with one contact or group (SHARE, SHARE_GROUP), and what has gone
 * to it. */
struct link_share {
    uint8_t precision; /* 1 to 24, or 0 when off */
    uint8_t fields;    /* TERN_C_SHARE_ALTITUDE and _ACCURACY */
    uint16_t interval; /* seconds */
    tern_time until;   /* when it ends, or 0 for when it is turned off */
    bool sent;         /* a position has gone since it was turned on or last changed */
    bool gone;         /* a position has gone since it was turned on: a stopped one is owed */
    bool stop;         /* sharing ended after a position went: a stopped one is to go, once */
    bool in_flight;    /* a contact's: the last that went is neither acknowledged nor given up */
    tern_time last;    /* when the last went */
    tern_time go_at;   /* when one that is due goes, after its random wait; 0 for not yet drawn */
    struct tern_position cell;         /* the last that went: its cell */
    uint8_t address[TERN_ADDRESS_LEN]; /* a contact's, kept so a stopped one can follow removal */
};

/* A position received in a group, from a routing id: what a member claimed. */
struct link_member_position {
    bool used;
    uint32_t from;
    struct tern_position position;
    tern_time at;
};

/* A position due to go, as link_position_next() gives it: where, and its plaintext. */
struct link_position_out {
    bool group;   /* else a contact's */
    size_t place; /* among the contacts, or the groups */
    uint8_t address[TERN_ADDRESS_LEN];
    uint8_t plaintext[TERN_POSITION_MAX];
    size_t len;
    struct tern_position cell;
};

/* One client's connection, and what that client was last told, to tell it what changed. */
struct link_conn {
    bool open;
    /* How long the client may go without a request, counted from the answer to its last, before
     * the connection is taken as ended (draft/companion.md, "Going quiet"): LINK_LAPSE on a serial
     * port, which cannot see a client close it, and 0 on one that can. */
    tern_time lapse;
    size_t mtu; /* the ATT MTU over Bluetooth; 0 on a byte stream, which carries any frame */
    bool hello;
    bool mesh;             /* an owner's, over the mesh: told news only in answer to SYNC */
    uint8_t version;       /* the client's, from its HELLO: it is sent nothing a later one added */
    bool synced;           /* told everything since HELLO: link_tick tells what changed since */
    uint8_t news;          /* the count: the next news frame's seq */
    tern_time answered_at; /* when the last request was answered */

    struct link_told told[LINK_NEIGHBOURS];
    size_t n_told;
    uint8_t self_role;
    int8_t self_power;
    uint8_t self_most;
    uint8_t self_pairing;
    uint8_t self_locked;
    const char *self_region;
    uint8_t self_cards;
    uint8_t self_card_name_len;
    uint8_t self_card_name[TERN_COMPANION_NAME_MAX];
    struct tern_companion_msg air, power, fix;
    /* What SLEEP last told, of what makes it news: not a whole message, for the room. */
    uint16_t sleep_wake;
    uint8_t sleep_listen, sleep_state;
    uint32_t sleep_keeper;
    tern_time air_at, power_at, fix_at, look_at;
};

/* An owner, and its connection over the mesh while LINK_MESH is not answering it: whether its
 * client has said HELLO, in what version, and the news count. Kept in RAM only, so after a restart
 * an owner says HELLO again. */
struct link_owner {
    bool used;
    uint8_t address[TERN_ADDRESS_LEN];
    bool hello;
    uint8_t version, news;
};

struct link {
    struct link_host host;
    struct link_conn conns[LINK_CONNS];
    struct link_conn *asker; /* the connection whose request is being answered */

    struct link_contact contacts[LINK_CONTACTS];
    /* Each contact's routing id, by its place: 0 until link_contact_id() works it out, and again
     * whenever the place is given another address. */
    uint32_t contact_ids[LINK_CONTACTS];
    struct link_group groups[LINK_GROUPS];
    struct link_message messages[LINK_MESSAGES];
    struct link_forgotten forgotten[LINK_FORGOTTEN];
    uint32_t next_id;
    uint32_t ids_saved;    /* ids below this are set aside in flash: no restart gives them again */
    uint32_t next_count;   /* the count of the next group frame this node seals, for any group */
    uint32_t counts_saved; /* counts below this are set aside in flash, as ids are */
    bool writers_changed;  /* a group frame was accepted, or a group left, since the last save */
    /* Whether the cards have been dated since the start (date_cards()): when each was heard is read
     * back from flash then, not kept, as the smallest boards have no RAM to spare for it. Beside
     * the flag above, in what was padding. */
    bool cards_dated;
    struct link_ref refs[LINK_REFS];
    size_t next_ref;
    /* The addresses ASKED last told of, and when: one that keeps asking is told of every
     * LINK_QUIET. When more than LINK_ASKED ask at once the oldest is forgotten, and may be told
     * of sooner. */
    struct link_asked asked[LINK_ASKED];
    struct link_update update; /* what a screen may show of it, too */

    struct link_fix fix;
    struct link_share contact_shares[LINK_CONTACTS]; /* by the contact's place */
    struct tern_position_held contact_positions[LINK_CONTACTS];
    struct link_share group_shares[LINK_GROUPS];
    struct link_member_position group_positions[LINK_GROUPS][LINK_GROUP_POSITIONS];

    struct tern_card card_places[LINK_CARDS];
    struct tern_cards cards;

    struct link_route routes[LINK_ROUTES];
    size_t next_route;
    uint32_t traces[LINK_TRACES]; /* the ids of traces out, 0 for a free place */

    struct link_owner owners[LINK_OWNERS];
    /* The answer being packed for the owner LINK_MESH answers: frames, each after its length. */
    bool mesh_syncing; /* LINK_MESH is being told what a SYNC tells */
    const uint8_t *answer_to;
    uint8_t answer[TERN_COMPANION_MANAGED_MAX];
    size_t answer_len;
    /* When a request from a non-owner was last refused, if one has been: one each
     * TERN_MANAGE_REFUSE_GAP, whoever asks. */
    bool refused;
    tern_time refused_at;

    struct link_view view; /* scratch, filled by the host */
};

/* Loads the contacts, the groups, the messages, and where the message ids had got to. Every
 * connection starts closed.
 *
 * A message's id is greater than every one the node gave before, across restarts too
 * (draft/companion.md, "Messages"): a client asks for what is new by the greatest id it holds.
 * A message the host could not save is lost at a restart, so the ids are kept apart from the
 * messages: LINK_ID_STEP of them are set aside in flash at a time, and a restart begins after
 * the last set aside, skipping the few not used. If the write fails the id is given all the same
 * and the next message tries again; a restart before one succeeds may give those ids twice.
 *
 * A message is saved as it is kept, and again, in a word, when it is handed over to be sent,
 * when its state changes and when it is read. One that was waiting when the node restarted and
 * had not been handed over waits still, and goes. One to an address that had been handed over is
 * not delivered: the node no longer holds its frame or listens for its acknowledgement, so it
 * has given it up, and whether it arrived is not known. A group message that was waiting goes,
 * handed over or not, since it is on the air only once and is sent from then. The SENDs
 * remembered (LINK_REFS) are not saved: one sent again after a restart is sent twice. */
void link_init(struct link *l, const struct link_host *host);

/* A connection opened: a client connected, or a port that one may open at any time. It starts
 * before HELLO. `lapse` and `mtu` are as in struct link_conn. */
void link_open(struct link *l, unsigned conn, tern_time lapse, size_t mtu);

/* The connection's ATT MTU changed, as a Bluetooth central may ask after connecting. */
void link_mtu(struct link *l, unsigned conn, size_t mtu);

/* A client went: nothing more is sent on the connection until it is opened again. */
void link_close(struct link *l, unsigned conn);

/* One frame from the client on a connection. */
void link_receive(struct link *l, unsigned conn, tern_time now, const uint8_t *frame, size_t len);

/* Tells the client what has changed: neighbours, the air, power, the node itself and its fix.
 * Call it often; it looks at most every LINK_LOOK. A connection that has lapsed is ended here,
 * and hears nothing more until it says HELLO again. */
void link_tick(struct link *l, tern_time now);

/* When the last request from any client was answered, or 0 if none has been. */
tern_time link_answered_at(const struct link *l);

/* The oldest message waiting that the forwarder has not been given, or NULL. */
struct link_message *link_outgoing(struct link *l);

/* What became of a message: its state, why it waits and for how long. A change to the state or
 * the reason is news. */
void link_state(struct link *l, uint32_t id, uint8_t state, uint8_t reason, uint16_t wait);

/* How a message went (ROUTE): kept, replacing what was kept for it, and told to every client
 * that was told of the message. Only for a message, group message or invite of the user's. */
void link_route(struct link *l, const struct link_route *r);

/* A trace begun by TRACE has ended, as TRACED tells every client: `result` a TERN_C_TRACED_,
 * `entries` the trace answer's, `n` of them, five bytes each. Its STATE before, waiting or sent,
 * is link_state()'s, as a message's is. */
void link_traced(struct link *l, uint32_t id, uint32_t to, uint8_t result, uint8_t out,
                 uint32_t took, int8_t snr, const uint8_t *entries, size_t n);

/* Whether a message handed over is still to go: false once it has come to rest some other way,
 * as when its group is left or its session ended, and then the board lets go of its frame. */
bool link_wanted(struct link *l, uint32_t id);

/* A message has been handed to the forwarder, and is not to be handed over again. Its state
 * stays waiting until main.c learns more: delivered when its destination's acknowledgement comes
 * back, not delivered when it is given up. Going on the air does not make it sent: the draft
 * forbids claiming more than the node knows, and a message is sent only once a neighbour has been
 * heard passing it on, which the board does not yet report (README.md, "The companion
 * link"). */
void link_taken(struct link *l, uint32_t id);

/* A message that did not come from a client: one received, or one sent from the console. Text
 * that is not UTF-8, or is longer than a MESSAGE carries, is cut to what is. Returns its id, or
 * 0 if it was not kept: empty, or no room. */
uint32_t link_add(struct link *l, const uint8_t address[TERN_ADDRESS_LEN], uint32_t time,
                  uint8_t state, uint8_t reason, const uint8_t *text, size_t len);

/* The groups the node holds, for tern_group_open(): LINK_GROUPS places, NULL where there is no
 * group. */
void link_groups(struct link *l, struct tern_group *out[LINK_GROUPS]);

/* The count for a group frame the board is about to seal: greater than every one given before,
 * across restarts too, and one for all the node's groups, so that leaving a group and taking it
 * again cannot bring a count back. LINK_COUNT_STEP are set aside in flash at a time, and a
 * restart begins after the last set aside. False if flash would not take the next ones: the
 * frame is then not sealed, since a count given twice has members refuse the second. */
bool link_group_count(struct link *l, uint32_t *count);

/* The board accepted a group frame, of words or not: the groups' writers are saved at once,
 * before anything is done with the frame, so that no restart, however sudden, can have the same
 * frame accepted a second time. False if flash did not take them: the board then drops the frame
 * as if it had not come, and they are tried again with the next, or by link_keep_writers(). */
bool link_group_heard(struct link *l);

/* Saves the groups' writers if they have changed since they last were saved: the link calls it
 * when a group is left, and the board, no more often than it will write to flash, to try again
 * what flash did not take. True if flash has them as they are, false if it did not take them. */
bool link_keep_writers(struct link *l);

/* The keys to seal a waiting group message with, or NULL if its group has been left since. */
const struct tern_group *link_group_of(struct link *l, const struct link_message *x);

/* The plaintext of a waiting invite, for tern_unicast_seal_node(): its length, or 0 if its group
 * has been left since. */
size_t link_invite_write(struct link *l, const struct link_message *x,
                         uint8_t out[TERN_GROUP_INVITE_MAX]);

/* A message received from a group: `place` is the group's among link_groups(), and `from` the
 * routing id its frame gave. Returns its id, or 0 if it was not kept. */
uint32_t link_add_group(struct link *l, size_t place, uint32_t from, uint32_t time,
                        const uint8_t *text, size_t len);

/* An invite received from an address the node shares a session with. It is kept, with its secret,
 * for the user to take or leave: the group is held only on JOIN. Returns its id, or 0 if it was
 * not kept. */
uint32_t link_add_invite(struct link *l, const uint8_t from[TERN_ADDRESS_LEN], uint32_t time,
                         const uint8_t secret[TERN_GROUP_SECRET], const uint8_t *name,
                         size_t name_len);

/* Every received message whose id is `through` or less has been read: what READ does, for when the
 * user saw them on the board itself. Each one newly read is news to every client. */
void link_read(struct link *l, uint32_t through);

/* Every message to an address that waits for a session, given up: first contact failed. */
void link_unreachable(struct link *l, const uint8_t address[TERN_ADDRESS_LEN]);

/* A session started or ended with an address: news, if it is a contact. */
void link_session_changed(struct link *l, const uint8_t address[TERN_ADDRESS_LEN]);

/* An address told the node a new wake (draft/sleeping.md, "Telling the source"): news, if it is a
 * contact. */
void link_wake_changed(struct link *l, const uint8_t address[TERN_ADDRESS_LEN]);

/* What sleeping does may have changed: SLEEP to every client told one, at once, if anything in it
 * but its retry did. link_tick() looks too, every LINK_LOOK. */
void link_sleep_news(struct link *l);

/* Whether the user has saved an address as a contact: one the board takes first contact from
 * (draft/companion.md, "Who may make first contact"). */
bool link_contact(const struct link *l, const uint8_t address[TERN_ADDRESS_LEN]);

/* The routing id of the contact in that place (draft/routing.md, "Routing ids"), 0 for a place
 * that holds none. Worked out from its address the first time, which takes two scalar
 * multiplications, and kept. */
uint32_t link_contact_id(struct link *l, size_t place);

/* The board refused first contact from an address that proved itself, for a TERN_C_ASKED_
 * reason: news, for a client to offer to let it in. */
/* Whether an address is one of the node's owners (draft/managing.md). */
bool link_owner(const struct link *l, const uint8_t address[TERN_ADDRESS_LEN]);

/* A request to manage this node, the content of a message for the node of kind 0x05 after the
 * kind, from `from`, with whom the node shares a session. One from an address that is not an owner
 * is refused with ERROR 15 in an answer of its own, at most once each TERN_MANAGE_REFUSE_GAP
 * whoever asks, and otherwise let go; false is returned for it, and *refused (if not NULL) says
 * whether it was refused. An owner's is answered as a
 * request over a link is, on LINK_MESH, but for what may not be asked over the mesh, which is
 * answered ERROR 13; the answer, and the news a SYNC sends, are packed into messages to `from` of
 * kind 0x06, which go as the node's other messages do. */
bool link_manage(struct link *l, tern_time now, const uint8_t from[TERN_ADDRESS_LEN],
                 const uint8_t *request, size_t len, bool *refused);

/* An answer from a node this one manages: a message for the node of kind 0x06, `content` after
 * the kind. It is told to every client as MANAGED, if it is an answer at all. */
void link_managed(struct link *l, const uint8_t from[TERN_ADDRESS_LEN], const uint8_t *content,
                  size_t len);

/* A notice that the owners of the node at `from` changed: a message for the node of kind 0x07,
 * kind included. It is told to every client as OWNER_CHANGED, if it is a notice at all. */
void link_owners_notice(struct link *l, const uint8_t from[TERN_ADDRESS_LEN],
                        const uint8_t *content, size_t len);

void link_asked(struct link *l, tern_time now, const uint8_t address[TERN_ADDRESS_LEN],
                uint8_t why);

/* --- Positions ------------------------------------------------------------------------------ */

/* The next position due to go, if any, at `now`: to a contact the node shares a session with
 * whose last is not still on its way, or, with `groups`, to a group. A position is due as
 * draft/positions.md says, after a random wait of up to an eighth of the interval, and a stopped
 * one is due once sharing that sent one has ended. Nothing is marked as gone until
 * link_position_sent(). */
bool link_position_next(struct link *l, tern_time now, bool groups, struct link_position_out *out);

/* A position from link_position_next() went to the forwarder or the flooder at `now`. */
void link_position_sent(struct link *l, const struct link_position_out *out, tern_time now);

/* Whether a position to a group, handed to the flooder and not yet on the air, is still to go:
 * the group in that place is still the one with that id, and sharing with it is still on, or
 * the position is the stopped one. A position that is not is let go of, so that leaving a group,
 * or no longer sharing with it, sends it nothing more. */
bool link_group_position_wanted(const struct link *l, size_t place,
                                const uint8_t id[TERN_COMPANION_GROUP], bool stopped);

/* The last position to a contact was acknowledged, or given up: the next may go. */
void link_position_done(struct link *l, const uint8_t address[TERN_ADDRESS_LEN]);

/* A message for the node whose first byte is TERN_POSITION_KIND, from an address the node shares
 * a session with, at the session's `counter`. Taken only from a contact; news if what is held
 * changes. */
void link_position_received(struct link *l, tern_time now, const uint8_t from[TERN_ADDRESS_LEN],
                            uint32_t counter, const uint8_t *plaintext, size_t len);

/* The same from a group frame for the node: `place` is the group's among link_groups(), and
 * `from` the routing id the frame gave. */
void link_group_position_received(struct link *l, tern_time now, size_t place, uint32_t from,
                                  const uint8_t *plaintext, size_t len);

/* A fix from the board's own satellite receiver, made at `now`, one the receiver says is valid:
 * the node's from then on, being the most recent it has. Clients are told (FIX) at once if it is
 * the first of the receiver's since a client's, or if the receiver's state in the view changed
 * with it, and otherwise at most every LINK_QUIET. The receiver's state alone, a fix lost or the
 * receiver turned off, link_tick() tells of. */
void link_receiver_fix(struct link *l, tern_time now, int32_t lat, int32_t lon, int16_t altitude,
                       uint16_t accuracy, uint8_t satellites);

/* --- Cards ---------------------------------------------------------------------------------- */

/* A card the flood brought, new to this node. It is checked and, if accepted, held in place of
 * any from its address, and news; a card it makes room by forgetting is news too. The verdict says
 * whether a relay should pass it on: not one that is anything but accepted, or this node's own. */
enum tern_card_verdict link_card_received(struct link *l, tern_time now, const uint8_t *frame,
                                          size_t len);

#endif
