#ifndef HELTEC_V3_DEMO_H
#define HELTEC_V3_DEMO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tern/unicast.h"

/* Bench pairing for the demo: two boards that share a passphrase share a session.
 *
 * This is a stopgap, not Tern. The specification has no first contact yet, and until it does
 * two boards need some way to share a session secret. Here each board is told the other's ID
 * (its MAC address) and a passphrase, and the secret is
 *
 *     SHA-256("tern demo pairing v1" || lower ID || higher ID || passphrase)
 *
 * and the board with the lower ID is the initiator. So the two always take different roles, and
 * a third board given the same passphrase gets a different secret.
 *
 * What the real handshake will not have is the same secret coming out every time, so a board
 * that started again from counter 0 would reuse message keys and nonces. Against that:
 *
 * - The session is saved after every message: a frame is sent only once the counter it uses is
 *   saved, and a received message is shown only once its counter is saved as received. A restart
 *   carries on from where it was, never repeats a counter, and never accepts a frame twice.
 * - A board remembers every secret it has paired with, and will not pair with one again.
 *
 * What neither can catch is a board whose flash was erased being paired with an old passphrase.
 * After erasing, choose a new one.
 *
 * Nothing here touches the hardware, so tests/demo.c runs it on a host with a fake store. */

#define DEMO_ID_LEN 6

/* Saving and loading whole records by name. On the board, NVS. */
struct demo_store {
    void *ctx;
    bool (*load)(void *ctx, const char *key, void *buf, size_t len);
    bool (*save)(void *ctx, const char *key, const void *buf, size_t len);
};

#define DEMO_MAX_PAIRINGS 32

struct demo_state {
    uint32_t magic, size; /* checked on load: a record from another build is not used */
    uint8_t role;         /* enum tern_role, or 0 before pairing */
    uint8_t peer[DEMO_ID_LEN];
    uint32_t sent, heard; /* messages, for display */
    struct tern_session session;
};

struct demo_used {
    uint32_t count;
    uint8_t fingerprint[DEMO_MAX_PAIRINGS][8];
};

struct demo {
    struct demo_store store;
    struct demo_state s;
    struct demo_used used;
};

enum demo_result {
    DEMO_OK = 0,
    DEMO_UNPAIRED,     /* no session yet: pair first */
    DEMO_SAME_ID,      /* the ID given is this board's own */
    DEMO_REUSED,       /* this board has paired with that board and passphrase before */
    DEMO_FULL,         /* it has paired DEMO_MAX_PAIRINGS times: erase it, start afresh */
    DEMO_STORE_FAILED, /* the session could not be saved, so nothing was sent */
    DEMO_SPENT,        /* every counter used; pair again with a new passphrase */
    DEMO_TOO_LONG,     /* more than TERN_UNICAST_MAX_PLAINTEXT bytes */
};

/* Loads whatever was saved. */
void demo_start(struct demo *d, const struct demo_store *store);

/* Starts a session with the board whose ID is peer, from a passphrase both are given. Replaces
 * any session this board had. */
enum demo_result demo_pair(struct demo *d, const uint8_t self[DEMO_ID_LEN],
                           const uint8_t peer[DEMO_ID_LEN], const char *passphrase);

/* Seals msg into frame (at least len + 16 bytes) and saves the session. Send the frame only if
 * this returns DEMO_OK. */
enum demo_result demo_seal(struct demo *d, const uint8_t *msg, size_t len, uint8_t *frame);

enum demo_heard {
    DEMO_HEARD_MESSAGE,   /* from the paired board: msg, len and counter are set */
    DEMO_HEARD_UNSAVED,   /* from the paired board, but the session could not be saved, so the
                             message is not shown: after a restart it could be accepted again */
    DEMO_HEARD_OTHER,     /* not for this session */
    DEMO_HEARD_FORGED,    /* its tag was ours, but it did not authenticate */
    DEMO_HEARD_MALFORMED, /* not a Tern unicast frame at all */
};

/* What a received frame was. msg must hold TERN_UNICAST_MAX_PLAINTEXT bytes. */
enum demo_heard demo_open(struct demo *d, const uint8_t *frame, size_t len, uint8_t *msg,
                          size_t *msg_len, uint32_t *counter);

#endif
