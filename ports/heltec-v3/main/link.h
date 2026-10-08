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
 * Nothing here touches the hardware, so tests/link.c runs it on a host. */

#define LINK_CONTACTS 16
#define LINK_MESSAGES 32
#define LINK_REFS 16 /* SENDs remembered, so one sent again is not sent twice */
#define LINK_NEIGHBOURS 64
#define LINK_QUIET TERN_S(10) /* the least time between two news frames about one thing */
#define LINK_LOOK TERN_S(1)   /* how often the link looks for changes to tell */
#define LINK_SNR_STEP 4       /* quarter-dB: a neighbour's SNR moved this much is news */
#define LINK_LAPSE TERN_S(60) /* a serial client silent this long since its last answer is gone */

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
    /* How long a client may go without a request, counted from the answer to its last, before
     * the connection is taken as ended (draft/companion.md, "Going quiet"): LINK_LAPSE on a serial
     * port, which cannot see a client close it, and 0 on one that can. */
    tern_time lapse;
    /* One frame to the client. */
    void (*out)(void *ctx, const uint8_t *frame, size_t len);
    void (*view)(void *ctx, struct link_view *v);
    /* SET: 0, or the ERROR code to answer with. */
    uint8_t (*set)(void *ctx, const struct tern_companion_msg *m);
    void (*set_time)(void *ctx, uint32_t time);
    /* Whether the node shares a session with an address. */
    bool (*session)(void *ctx, const uint8_t address[TERN_ADDRESS_LEN]);
    /* Why a new message to an address would wait: a TERN_C_WAIT_ reason. */
    uint8_t (*why)(void *ctx, const uint8_t address[TERN_ADDRESS_LEN]);
    /* The contacts, saved whole and loaded at start. */
    bool (*load)(void *ctx, void *buf, size_t len);
    bool (*save)(void *ctx, const void *buf, size_t len);
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

struct link_told {
    struct link_neighbour n;
    tern_time at;
};

struct link {
    struct link_host host;
    bool hello;
    bool synced;           /* told everything since HELLO: link_tick tells what changed since */
    uint8_t news;          /* the count: the next news frame's seq */
    tern_time answered_at; /* when the last request was answered */

    struct link_contact contacts[LINK_CONTACTS];
    struct link_message messages[LINK_MESSAGES];
    uint32_t next_id;
    struct link_ref refs[LINK_REFS];
    size_t next_ref;

    /* What the client was last told, to tell it what changed. */
    struct link_told told[LINK_NEIGHBOURS];
    size_t n_told;
    uint8_t self_role;
    int8_t self_power;
    const char *self_region;
    struct tern_companion_msg air, power;
    tern_time air_at, power_at, look_at;

    struct link_view view; /* scratch, filled by the host */
};

/* Loads the contacts. */
void link_init(struct link *l, const struct link_host *host);

/* One frame from the client. */
void link_receive(struct link *l, tern_time now, const uint8_t *frame, size_t len);

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

#endif
