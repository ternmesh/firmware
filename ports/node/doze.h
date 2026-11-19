#ifndef TERN_NODE_DOZE_H
#define TERN_NODE_DOZE_H

#include <stdbool.h>
#include <stdint.h>

#include "tern/time.h"

/* A leaf that sleeps (the specification's "Leaves that sleep", tern/sleep.h) turns its radio off
 * between polls: the node decides when, from what the core says may be (tern_forward_may_sleep()),
 * and keeps count of how long the radio has been off, for 'sleep' and 'status' to say.
 *
 * Its radio is put to sleep only for a gap worth it (DOZE_MIN), and woken DOZE_LEAD before it is
 * next needed: waking takes a reset of the chip and its set-up again, a few milliseconds.
 *
 * Nothing here touches the hardware, so tests/doze.c runs it on a host. */

#define DOZE_MIN TERN_MS(500) /* the shortest gap the radio is put to sleep for */
#define DOZE_LEAD TERN_MS(50) /* how long before it is needed it is woken */
#define DOZE_SLICE_MS 100     /* the loop's turn while the radio sleeps, at most */

struct doze {
    bool dozing;       /* the radio is asleep */
    tern_time since;   /* when it went to sleep */
    tern_time until;   /* when it is to be woken, DOZE_LEAD before it is needed */
    tern_time started; /* when counting began */
    tern_time off;     /* its time asleep before `since` */
    uint32_t dozes;    /* times it has gone to sleep */
    uint32_t failures; /* times it would not wake; after DOZE_FAILURES it is left on */
};

#define DOZE_FAILURES 3

void doze_init(struct doze *d, tern_time now);

/* Whether the radio goes to sleep now: `may` and `wake_at` as tern_forward_may_sleep() gave them,
 * `other_due` the soonest anything else that sends wants it (the flooder), and `busy` true while
 * a frame is on the air or waiting to be. If so, *until is when it is to be woken. */
bool doze_should(const struct doze *d, tern_time now, bool may, tern_time wake_at,
                 tern_time other_due, bool busy, tern_time *until);

/* Whether a radio asleep is woken now: its time has come, `due` (the soonest the router, the
 * forwarder or the flooder wants it) is within DOZE_LEAD, or the node no longer sleeps. */
bool doze_wake_now(const struct doze *d, tern_time now, tern_time due, bool sleeping);

void doze_begin(struct doze *d, tern_time now, tern_time until);
void doze_end(struct doze *d, tern_time now);

/* The radio's time asleep since counting began, the present sleep included, and the share of
 * that time it was on, in parts per thousand. */
tern_time doze_off(const struct doze *d, tern_time now);
uint32_t doze_on_permille(const struct doze *d, tern_time now);

/* How long the loop may wait before its next turn while the radio sleeps: DOZE_SLICE_MS, or less
 * if it is to be woken sooner; at least 1. */
uint32_t doze_slice_ms(const struct doze *d, tern_time now);

/* Reads the 'sleep on' command's arguments, "<seconds> [<listen seconds>]": a wake from WAKE_MIN
 * to WAKE_MAX, as its promise code, and a listen from LISTEN_MIN to 255, LISTEN_MIN if not given.
 * False for anything else. */
bool doze_parse(const char *args, uint16_t *wake, uint8_t *listen);

/* The settings' word for sleeping (node.c): the wake's code in the low 16 bits, 0 for not sleeping,
 * and the listen in the next 8, kept while it does not sleep. Read back, a wake or a listen the
 * specification does not allow is 0: not sleeping, and the least listen. */
uint32_t doze_setting(uint16_t wake, uint8_t listen);
void doze_setting_read(uint32_t word, uint16_t *wake, uint8_t *listen);

#endif
