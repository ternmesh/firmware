#ifndef HELTEC_V3_LINK_H
#define HELTEC_V3_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tern/address.h"
#include "tern/companion.h"
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
 * Several clients may drive the node at once, one on each connection: the USB port and a
 * Bluetooth central. Each has its own HELLO, sync and news count, and is answered alone; the
 * contacts and messages are the node's, and news of a change to them goes to every client.
 *
 * Nothing here touches the hardware, so tests/link.c runs it on a host. */

#define LINK_CONTACTS 16
#define LINK_MESSAGES 32
#define LINK_REFS 16    /* SENDs remembered, so one sent again is not sent twice */
#define LINK_ID_STEP 64 /* message ids set aside by one write to flash */
#define LINK_ASKED 8    /* addresses lately refused, remembered so each is news once a LINK_QUIET */
#define LINK_NEIGHBOURS 64
#define LINK_QUIET TERN_S(10) /* the least time between two news frames about one thing */
#define LINK_LOOK TERN_S(1)   /* how often the link looks for changes to tell */
#define LINK_SNR_STEP 4       /* quarter-dB: a neighbour's SNR moved this much is news */
#define LINK_LAPSE TERN_S(60) /* a serial client silent this long since its last answer is gone */

/* The connections: where a frame came from, and where one goes. */
enum { LINK_SERIAL, LINK_BLE, LINK_CONNS };

struct link_neighbour {
    uint32_t id;
    uint8_t role;   /* 0 leaf, 1 relay */
    int8_t snr;     /* quarter-dB */
    uint16_t heard; /* seconds since */
};

/* What main.c knows that the link does not keep: everything in SELF, NEIGHBOUR, AIRTIME and
 * POWER. */
struct link_view {
    uint8_t address[TERN_ADDRESS_LEN];
    uint8_t role;
    const char *region;
    int8_t power;
    uint32_t time; /* seconds since 1970, or 0 */
    size_t n_neighbours;
    struct link_neighbour neighbours[LINK_NEIGHBOURS];
    uint32_t period_s, allowed_ms, used_ms, wait_ms;
    uint16_t millivolts;
    uint8_t percent, power_flags;
};

struct link_host {
    void *ctx;
    const char *firmware;
    /* One frame to the client on a connection. */
    void (*out)(void *ctx, unsigned conn, const uint8_t *frame, size_t len);
    void (*view)(void *ctx, struct link_view *v);
    /* SET: 0, or the ERROR code to answer with. */
    uint8_t (*set)(void *ctx, const struct tern_companion_msg *m);
    void (*set_time)(void *ctx, uint32_t time);
    /* Whether the node shares a session with an address. */
    bool (*session)(void *ctx, const uint8_t address[TERN_ADDRESS_LEN]);
    /* Why a new message to an address would wait: a TERN_C_WAIT_ reason. */
    uint8_t (*why)(void *ctx, const uint8_t address[TERN_ADDRESS_LEN]);
    /* END_SESSION: ends the session with an address, if there is one, and lets go of whatever the
     * forwarder holds for it without a word: the link says what became of those messages, once
     * it has answered. 0, or the ERROR code to answer with. */
    uint8_t (*end_session)(void *ctx, const uint8_t address[TERN_ADDRESS_LEN]);
    /* The contacts, saved whole and loaded at start. */
    bool (*load)(void *ctx, void *buf, size_t len);
    bool (*save)(void *ctx, const void *buf, size_t len);
    /* The first message id not yet set aside, kept so that an id is never given twice
     * (link_init()). */
    bool (*load_ids)(void *ctx, uint32_t *next);
    bool (*save_ids)(void *ctx, uint32_t next);
};

struct link_contact {
    bool used;
    uint8_t address[TERN_ADDRESS_LEN];
    uint8_t name_len;
    uint8_t name[TERN_COMPANION_NAME_MAX];
};

struct link_message {
    bool used;
    bool taken; /* with the forwarder, which sends it: not to be handed over again */
    uint32_t id;
    uint8_t address[TERN_ADDRESS_LEN];
    uint32_t time;
    uint8_t flags, state, reason;
    uint16_t wait;
    uint8_t text_len;
    uint8_t text[TERN_COMPANION_TEXT_MAX];
};

struct link_ref {
    bool used;
    uint32_t ref, id;
    uint8_t to[TERN_ADDRESS_LEN];
    uint8_t digest[8]; /* of the text: the first bytes of its SHA-256 */
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

/* One client's connection, and what that client was last told, to tell it what changed. */
struct link_conn {
    bool open;
    /* How long the client may go without a request, counted from the answer to its last, before
     * the connection is taken as ended (draft/companion.md, "Going quiet"): LINK_LAPSE on a serial
     * port, which cannot see a client close it, and 0 on one that can. */
    tern_time lapse;
    size_t mtu; /* the ATT MTU over Bluetooth; 0 on a byte stream, which carries any frame */
    bool hello;
    uint8_t version;       /* the client's, from its HELLO: it is sent nothing a later one added */
    bool synced;           /* told everything since HELLO: link_tick tells what changed since */
    uint8_t news;          /* the count: the next news frame's seq */
    tern_time answered_at; /* when the last request was answered */

    struct link_told told[LINK_NEIGHBOURS];
    size_t n_told;
    uint8_t self_role;
    int8_t self_power;
    const char *self_region;
    struct tern_companion_msg air, power;
    tern_time air_at, power_at, look_at;
};

struct link {
    struct link_host host;
    struct link_conn conns[LINK_CONNS];
    struct link_conn *asker; /* the connection whose request is being answered */

    struct link_contact contacts[LINK_CONTACTS];
    struct link_message messages[LINK_MESSAGES];
    uint32_t next_id;
    uint32_t ids_saved; /* ids below this are set aside in flash: no restart gives them again */
    struct link_ref refs[LINK_REFS];
    size_t next_ref;
    /* The addresses ASKED last told of, and when: one that keeps asking is told of every
     * LINK_QUIET. When more than LINK_ASKED ask at once the oldest is forgotten, and may be told
     * of sooner. */
    struct link_asked asked[LINK_ASKED];

    struct link_view view; /* scratch, filled by the host */
};

/* Loads the contacts, and where the message ids had got to. Every connection starts closed.
 *
 * A message's id is greater than every one the node gave before, across restarts too
 * (draft/companion.md, "Messages"): a client asks for what is new by the greatest id it holds.
 * The messages themselves are not kept, so the ids are: LINK_ID_STEP of them are set aside in
 * flash at a time, and a restart begins after the last set aside, skipping the few not used. If
 * the write fails the id is given all the same and the next message tries again; a restart
 * before one succeeds may give those ids twice. */
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

/* Tells the client what has changed: neighbours, the air, power, the node itself. Call it often;
 * it looks at most every LINK_LOOK. A connection that has lapsed is ended here, and hears nothing
 * more until it says HELLO again. */
void link_tick(struct link *l, tern_time now);

/* The oldest message waiting that the forwarder has not been given, or NULL. */
struct link_message *link_outgoing(struct link *l);

/* What became of a message: its state, why it waits and for how long. A change to the state or
 * the reason is news. */
void link_state(struct link *l, uint32_t id, uint8_t state, uint8_t reason, uint16_t wait);

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

/* Every message to an address that waits for a session, given up: first contact failed. */
void link_unreachable(struct link *l, const uint8_t address[TERN_ADDRESS_LEN]);

/* A session started or ended with an address: news, if it is a contact. */
void link_session_changed(struct link *l, const uint8_t address[TERN_ADDRESS_LEN]);

/* Whether the user has saved an address as a contact: one the board takes first contact from
 * (draft/companion.md, "Who may make first contact"). */
bool link_contact(const struct link *l, const uint8_t address[TERN_ADDRESS_LEN]);

/* The board refused first contact from an address that proved itself, for a TERN_C_ASKED_
 * reason: news, for a client to offer to let it in. */
void link_asked(struct link *l, tern_time now, const uint8_t address[TERN_ADDRESS_LEN],
                uint8_t why);

#endif
