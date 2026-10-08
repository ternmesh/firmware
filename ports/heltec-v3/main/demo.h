#ifndef HELTEC_V3_DEMO_H
#define HELTEC_V3_DEMO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tern/address.h"
#include "tern/contact.h"
#include "tern/time.h"
#include "tern/unicast.h"

/* The demo's node: an identity, first contact with other boards, and the sessions it gives.
 *
 * A board makes its identity the first time it starts, and keeps it. Its address is what another
 * board is told to reach it. One board starts first contact with the other's address
 * (demo_contact()); the handshake's four frames go over the air, and both come out with a
 * session (tern/contact.h, tern/unicast.h). A board holds up to DEMO_PEERS sessions, one with
 * each peer; first contact with a peer it has a session with replaces that session.
 *
 * What the specification leaves open, the demo decides for itself, and none of it is Tern yet:
 *
 * - Lost frames. The initiator sends message_1, and later message_3, up to DEMO_TRIES times,
 *   `retry` apart, and then gives up. The responder never sends unasked: a frame it has already
 *   answered gets the same answer again, byte for byte, never computed twice.
 * - Whom to accept. A board with no session accepts whoever contacts it. One with a session
 *   accepts a peer it has again, and anyone else only while demo_accept() has it open. It learns
 *   who is asking only from message_3, so it answers every message_1 meant for it, and stays
 *   silent after message_3 if it refuses.
 * - How many. A board that holds DEMO_PEERS sessions takes no new peer until one is forgotten
 *   (demo_forget()): it does not choose whom to drop.
 * - One handshake at a time. While one is under way, another message_1 is not answered.
 *
 * A session is saved after every message of its own: a frame is sent only once the counter it uses
 * is saved, and a received message is shown, and acknowledged, only once its counter is saved as
 * received. A
 * restart carries on from where it was, never repeats a counter, and never accepts a frame
 * twice. A handshake is not saved: a restart in the middle of one abandons it.
 *
 * Nothing here touches the hardware, so tests/demo.c runs it on a host with a fake store. */

#define DEMO_TRIES 4 /* times the initiator sends each of its frames */
#define DEMO_ACKS 2
#define DEMO_PEERS 8 /* sessions a board holds */
/* How long a responder keeps a handshake, or its last answer, with nothing new heard. */
#define DEMO_HOLD(retry) (2 * DEMO_TRIES * (retry))

/* What the board provides: whole records saved and loaded by name (on the board, NVS), and
 * random bytes fit for keys. */
struct demo_store {
    void *ctx;
    bool (*load)(void *ctx, const char *key, void *buf, size_t len);
    bool (*save)(void *ctx, const char *key, const void *buf, size_t len);
    bool (*random)(void *ctx, uint8_t *buf, size_t len);
};

struct demo_state {
    uint32_t magic, size; /* checked on load: a record from another build is not used */
    uint8_t role;         /* enum tern_role, or 0 with no session */
    uint8_t peer[TERN_ADDRESS_LEN];
    uint32_t sent, heard; /* messages, for display */
    struct tern_session session;
};

enum demo_phase {
    DEMO_IDLE = 0,
    DEMO_INITIATING, /* message_1 or message_3 sent, waiting for the answer */
    DEMO_RESPONDING, /* message_2 sent, waiting for message_3 */
    DEMO_ANSWERED,   /* message_4 sent and the session started; kept to answer a repeat */
};

struct demo_handshake {
    uint8_t phase; /* enum demo_phase */
    struct tern_contact c;
    /* The last frame processed and the frame sent in answer, to answer a repeat with. */
    uint8_t in[TERN_CONTACT_MAX_FRAME], out[TERN_CONTACT_MAX_FRAME];
    size_t in_len, out_len;
    uint8_t tries;   /* the initiator's: times `out` has been sent */
    tern_time next;  /* the initiator's: when to send `out` again, or give up */
    tern_time until; /* the responder's: when to forget the handshake */
};

struct demo {
    struct demo_store store;
    tern_time retry;
    struct tern_identity id;
    struct demo_state s[DEMO_PEERS]; /* a slot with role 0 holds none */
    int last; /* the slot of the session last made or used, or -1 with none: where a board with
                 only a button sends */
    struct demo_handshake h;
    tern_time accept_until; /* contact from a new peer is accepted before this */
};

enum demo_result {
    DEMO_OK = 0,
    DEMO_UNPAIRED,     /* no session yet: make contact first */
    DEMO_OWN_ADDRESS,  /* the address given is this board's own */
    DEMO_BAD_ADDRESS,  /* not a valid address */
    DEMO_NO_RANDOM,    /* the board gave no random bytes, so no handshake was started */
    DEMO_STORE_FAILED, /* the session could not be saved, so nothing was sent */
    DEMO_SPENT,        /* every counter used; make contact again */
    DEMO_TOO_LONG,     /* more than TERN_UNICAST_MAX_PLAINTEXT bytes */
    DEMO_FULL,         /* DEMO_PEERS sessions already, none of them with this address */
};

/* Loads the identity and the sessions, making and saving an identity if there is none. retry is
 * how long the initiator waits for an answer before sending again: the board knows how long its
 * frames take. False if there is no identity and one could not be made or saved; the board
 * should not run. */
bool demo_start(struct demo *d, const struct demo_store *store, tern_time retry);

/* Starts first contact with the board whose address is peer, abandoning any handshake under
 * way: writes message_1's frame (TERN_CONTACT_MAX_FRAME bytes are enough) to send. The session
 * this board has with that peer, if any, stays until the new one is made. */
enum demo_result demo_contact(struct demo *d, const uint8_t peer[TERN_ADDRESS_LEN], tern_time now,
                              uint8_t *frame, size_t *len);

/* Lets a board that has a session accept contact from a new peer until the time given. */
void demo_accept(struct demo *d, tern_time until);

/* The slot of the session with that address, or -1 if there is none; and how many sessions the
 * board holds. */
int demo_peer(const struct demo *d, const uint8_t address[TERN_ADDRESS_LEN]);
size_t demo_peers(const struct demo *d);

/* Ends the session in that slot and erases it from the store. False, and the session kept, if
 * the store would not take it: a session forgotten only until the next restart would come back. */
bool demo_forget(struct demo *d, int slot);

/* Seals msg into frame (at least len + 23 bytes) for the peer in that slot, as the message whose
 * counter is that session's tx.next before the call, and saves the session. Send the frame only
 * if this returns DEMO_OK. The ten bytes after its first are the forwarder's to fill in
 * (tern/forward.h). */
enum demo_result demo_seal(struct demo *d, int slot, const uint8_t *msg, size_t len,
                           uint8_t *frame);

/* Whether a frame is the acknowledgement, by the peer in that slot, of the message this board
 * sent it with that counter. */
bool demo_acked(const struct demo *d, int slot, uint32_t counter, const uint8_t *frame, size_t len);

enum demo_heard {
    DEMO_HEARD_MESSAGE,   /* from the peer: msg, msg_len, counter and peer are set */
    DEMO_HEARD_COPY,      /* a message already heard, sent again: counter is set, and it is
                             acknowledged again, not shown again */
    DEMO_HEARD_UNSAVED,   /* from the peer, but the session could not be saved, so the message
                             is not shown or acknowledged, and is accepted when it comes again */
    DEMO_HEARD_OTHER,     /* not for this board: someone else's session or handshake */
    DEMO_HEARD_FORGED,    /* its tag was ours, but it did not authenticate */
    DEMO_HEARD_MALFORMED, /* not a Tern frame this code knows */
    DEMO_HEARD_CONTACT,   /* a step of a handshake, or a repeat of one */
    DEMO_HEARD_PAIRED,    /* the handshake is complete: a session with `peer` has started */
    DEMO_HEARD_REFUSED,   /* `peer` proved who it is, and this board does not accept it */
    DEMO_HEARD_FULL,      /* `peer` proved who it is, and there is no room for another session */
    DEMO_HEARD_FAILED,    /* a frame of this board's handshake failed a check; it is abandoned */
    DEMO_HEARD_UNPAIRED,  /* the handshake completed but the session could not be saved; the
                             board is as it was before */
};

struct demo_received {
    size_t msg_len;
    uint32_t counter;
    uint8_t peer[TERN_ADDRESS_LEN];
    int slot; /* the session a message or copy came in, or a handshake made; -1 otherwise */
    /* A frame to send in answer, if reply_len is not 0. Any verdict may come with one. */
    uint8_t reply[TERN_CONTACT_MAX_FRAME];
    size_t reply_len;
    /* The acknowledgements to send, by the forwarder, each to the peer in ack_slot: one with a
     * message, and with a copy one for each message it is a copy of, which is one unless two
     * messages, of one session or of two, share a tag. More than DEMO_ACKS of them never happens
     * to honest peers. */
    uint8_t ack[DEMO_ACKS][TERN_UNICAST_ACK_LEN];
    int ack_slot[DEMO_ACKS];
    size_t acks;
};

/* What a received frame was. msg must hold TERN_UNICAST_MAX_PLAINTEXT bytes. */
enum demo_heard demo_receive(struct demo *d, tern_time now, const uint8_t *frame, size_t len,
                             uint8_t *msg, struct demo_received *out);

enum demo_tick {
    DEMO_TICK_NONE,
    DEMO_TICK_RESEND,  /* no answer yet: frame is to be sent again */
    DEMO_TICK_GAVE_UP, /* no answer after DEMO_TRIES: the handshake this board began is over */
    DEMO_TICK_LAPSED,  /* a handshake another board began never finished, and is forgotten */
};

/* Moves time on. Call it often, but not while a frame is on the air: a frame it returns is to be
 * sent now. frame must hold TERN_CONTACT_MAX_FRAME bytes. */
enum demo_tick demo_tick(struct demo *d, tern_time now, uint8_t *frame, size_t *len);

#endif
