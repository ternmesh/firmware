#ifndef TERN_SLEEP_H
#define TERN_SLEEP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tern/time.h"

/* Leaves that sleep: specification draft 0, draft/sleeping.md in ternmesh/spec.
 *
 * A leaf that turns its radio off between wakes polls a relay it chooses, its keeper, which holds
 * the leaf's frames while it sleeps and hands them over when it asks. The router (tern/route.h)
 * keeps the polls, their answers and the announce's `keeper`; the forwarder (tern/forward.h) keeps
 * what a keeper holds and the held notice it answers with. This header has the rules both use, and
 * the frame for the node that tells a leaf's contacts how long it sleeps. */

#define TERN_SLEEP_WAKE_MIN_S 300   /* WAKE_MIN: the shortest wake */
#define TERN_SLEEP_WAKE_MAX_S 3600  /* WAKE_MAX: the longest */
#define TERN_SLEEP_LISTEN_MIN_S 2   /* LISTEN_MIN: the shortest listen */
#define TERN_SLEEP_HOLD_LEAF 16     /* HOLD_LEAF: frames a keeper holds for each leaf, at least */
#define TERN_SLEEP_HOLD_EXTRA_S 60  /* HOLD_EXTRA */
#define TERN_SLEEP_ANNOUNCE_S 600   /* SLEEP_ANNOUNCE: the longest between a sleeping leaf's */
#define TERN_SLEEP_POLL_TRIES 6     /* POLL_TRIES: unanswered in a row before it changes keeper */
#define TERN_SLEEP_POLL_RETRY_S 10  /* POLL_RETRY: after a poll unanswered */
#define TERN_SLEEP_KEEPER_BAND_DB 6 /* KEEPER_BAND */

#define TERN_SLEEP_KIND 0x04 /* the first byte of a message for the node that says it sleeps */
#define TERN_SLEEP_CONTENT_LEN 3

/* The rules are inline here, with no source file of their own: the simulator (ternmesh/sim) links
 * a fixed list of the core's source files, and a new one would leave it without them. */

/* What a wake code, in an announce's promise encoding, says in seconds; 0 for 0xFFFF, which is
 * not a wake time. */
static inline uint32_t tern_sleep_wake_s(uint16_t code) {
    if (code == 0xFFFF) {
        return 0;
    }
    return (code & 0x8000u) ? (uint32_t)(code & 0x7FFFu) * 60u : code;
}

/* Whether a wake code is one a poll may give: from WAKE_MIN to WAKE_MAX. */
static inline bool tern_sleep_wake_ok(uint16_t code) {
    uint32_t s = tern_sleep_wake_s(code);
    return code != 0xFFFF && s >= TERN_SLEEP_WAKE_MIN_S && s <= TERN_SLEEP_WAKE_MAX_S;
}

/* The node's sleep content: its wake, or 0 for not sleeping. Writes TERN_SLEEP_CONTENT_LEN bytes.
 */
static inline size_t tern_sleep_content_write(uint16_t wake, uint8_t out[TERN_SLEEP_CONTENT_LEN]) {
    out[0] = TERN_SLEEP_KIND;
    out[1] = (uint8_t)(wake >> 8);
    out[2] = (uint8_t)wake;
    return TERN_SLEEP_CONTENT_LEN;
}

/* Reads one: false, for a receiver to ignore, for one of any other length, with `wake` 0xFFFF, or
 * not 0 but outside WAKE_MIN to WAKE_MAX. */
static inline bool tern_sleep_content_read(const uint8_t *content, size_t len, uint16_t *wake) {
    uint16_t w;
    if (len != TERN_SLEEP_CONTENT_LEN || content[0] != TERN_SLEEP_KIND) {
        return false;
    }
    w = (uint16_t)(content[1] << 8 | content[2]);
    if (w != 0 && !tern_sleep_wake_ok(w)) {
        return false;
    }
    *wake = w;
    return true;
}

/* Whether a keeper holds a frame for its leaf `since` after it received the leaf's last poll,
 * which gave `listen_s`: while the leaf is not awake, from `listen_s` on. */
static inline bool tern_sleep_holds(tern_time since, uint8_t listen_s) {
    return since >= TERN_S(listen_s);
}

/* How long a keeper holds a frame for a leaf whose wake is `wake`: 2 × w + HOLD_EXTRA. */
static inline tern_time tern_sleep_hold_for(uint16_t wake) {
    return TERN_S(2 * (int64_t)tern_sleep_wake_s(wake) + TERN_SLEEP_HOLD_EXTRA_S);
}

/* What a source waits for each try of a message to a destination that sleeps with wake `wake`,
 * the route's metric being `metric`, given forwarding's ACK_WAIT and ACK_FACTOR. */
static inline tern_time tern_sleep_source_wait(tern_time ack_wait, uint8_t ack_factor,
                                               uint16_t metric, uint16_t wake) {
    return ack_wait + (tern_time)ack_factor * metric * TERN_MS(1) +
           TERN_S((int64_t)tern_sleep_wake_s(wake) + TERN_SLEEP_LISTEN_MIN_S);
}

#endif
