#ifndef HELTEC_V3_DEMO_H
#define HELTEC_V3_DEMO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tern/unicast.h"

/* Bench pairing for the demo: two boards that share a passphrase share a session.
 *
 * This is a stopgap, not Tern. The specification has no first contact yet, and until it does
 * two boards need some way to share a session secret; here it is SHA-256 of a passphrase. That
 * brings a danger the real handshake will not have: the same passphrase gives the same secret
 * every time, so a board that started again from counter 0 would reuse message keys and nonces.
 * Everything below exists to stop that:
 *
 * - The session is saved after every message, before the frame is sent, so a restart carries
 *   on from where it was and never repeats a counter.
 * - A board remembers every passphrase it has paired with, and will not pair with one again.
 * - Each board also listens for frames sent in its own direction. Hearing one means the other
 *   board took the same role, so both are using the same keys; it stops sending at once.
 *
 * What none of it can catch is a board whose flash was erased being paired with an old
 * passphrase. After erasing, choose a new one.
 *
 * Nothing here touches the hardware, so tests/demo.c runs it on a host with a fake store. */

/* Saving and loading whole records by name. On the board, NVS. */
struct demo_store {
    void *ctx;
    bool (*load)(void *ctx, const char *key, void *buf, size_t len);
    bool (*save)(void *ctx, const char *key, const void *buf, size_t len);
};

#define DEMO_MAX_PASSPHRASES 32

struct demo_state {
    uint32_t magic, size; /* checked on load: a record from another build is not used */
    uint8_t role;         /* enum tern_role, or 0 before pairing */
    bool conflict;        /* the other board has the same role: never send again */
    uint32_t sent, heard; /* messages, for display */
    struct tern_session session;
    struct tern_unicast_rx mirror; /* receives in this board's own direction, to catch a clash */
};

struct demo_used {
    uint32_t count;
    uint8_t fingerprint[DEMO_MAX_PASSPHRASES][8];
};

struct demo {
    struct demo_store store;
    struct demo_state s;
    struct demo_used used;
};

enum demo_result {
    DEMO_OK = 0,
    DEMO_UNPAIRED,     /* no session yet: pair first */
    DEMO_REUSED,       /* this board has paired with that passphrase before */
    DEMO_FULL,         /* it has paired DEMO_MAX_PASSPHRASES times: erase it, start afresh */
    DEMO_STORE_FAILED, /* the session could not be saved, so nothing was sent */
    DEMO_CONFLICT,     /* both boards took the same role; this one no longer sends */
    DEMO_SPENT,        /* every counter used; pair again with a new passphrase */
    DEMO_TOO_LONG,     /* more than TERN_UNICAST_MAX_PLAINTEXT bytes */
};

/* Loads whatever was saved. */
void demo_start(struct demo *d, const struct demo_store *store);

/* Starts a session from a passphrase, as initiator or responder: the two boards must take
 * different roles. Replaces any session this board had. */
enum demo_result demo_pair(struct demo *d, enum tern_role role, const char *passphrase);

/* Seals msg into frame (at least len + 16 bytes) and saves the session. Send the frame only if
 * this returns DEMO_OK. */
enum demo_result demo_seal(struct demo *d, const uint8_t *msg, size_t len, uint8_t *frame);

enum demo_heard {
    DEMO_HEARD_MESSAGE,   /* from the paired board: msg, len and counter are set */
    DEMO_HEARD_CLASH,     /* sent with this board's own keys: the roles clash */
    DEMO_HEARD_OTHER,     /* not for this session */
    DEMO_HEARD_FORGED,    /* its tag was ours, but it did not authenticate */
    DEMO_HEARD_MALFORMED, /* not a Tern unicast frame at all */
};

/* What a received frame was. msg must hold TERN_UNICAST_MAX_PLAINTEXT bytes. */
enum demo_heard demo_open(struct demo *d, const uint8_t *frame, size_t len, uint8_t *msg,
                          size_t *msg_len, uint32_t *counter);

#endif
