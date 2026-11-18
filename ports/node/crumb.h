#ifndef TERN_NODE_CRUMB_H
#define TERN_NODE_CRUMB_H

#include <stdbool.h>
#include <stdint.h>

/* Where the node's loop was, kept in memory a restart does not clear (plat_crumb()), so that after
 * the watchdog has restarted a board that stopped, the board can say which step it stopped in.
 *
 * The loop marks each step it goes into (crumb_at()), and counts its passes (crumb_pass()): two
 * stores, cheap enough for every step of every pass. At a start, crumb_boot() keeps where it was
 * if the watchdog made the restart, and starts the marks again. Power coming on leaves the memory
 * holding anything: what does not check out is cleared.
 *
 * Nothing here touches the hardware, so tests/crumb.c runs it on a host. */

enum crumb_step {
    CRUMB_NONE = 0,
    CRUMB_STARTING, /* node_main(), before the loop */
    CRUMB_RADIO,
    CRUMB_BEACON,
    CRUMB_CONTACT,
    CRUMB_OUTGOING,
    CRUMB_FORWARD,
    CRUMB_CARD,
    CRUMB_FLOOD,
    CRUMB_ROUTE,
    CRUMB_WRITERS,
    CRUMB_CONSOLE,
    CRUMB_BLE,
    CRUMB_BUTTON,
    CRUMB_SCREEN,
    CRUMB_BATTERY,
    CRUMB_RECEIVER,
    CRUMB_RESTART,  /* restart_if_due() */
    CRUMB_TX_CHECK, /* a frame that never said it had gone */
    CRUMB_YIELD,    /* the LED, and the platform's other tasks */
    CRUMB_HALTED,   /* halt(): it could not start */
    CRUMB_STEPS
};

struct crumb {
    uint32_t magic;
    uint32_t pass;      /* the loop's passes since the board started */
    uint8_t step;       /* enum crumb_step: where the loop is */
    uint8_t last_step;  /* where it was when the watchdog last restarted it; CRUMB_NONE if never */
    uint16_t restarts;  /* the watchdog's restarts since power came on */
    uint32_t last_pass; /* the pass it was in then */
    uint32_t check;     /* over the fields a start keeps, so that power coming on clears them */
};

/* At a start: `watchdog` if the watchdog made it. Clears what does not check out; then, after the
 * watchdog, keeps where the loop was (last_step, last_pass) and counts the restart. True if there
 * is a step to report from this start: the watchdog made it, and the marks before it were sound. */
bool crumb_boot(struct crumb *c, bool watchdog);

/* Through volatile, so that the compiler neither drops a mark the next one overwrites nor moves it
 * past the step it marks, which it may when that step is inlined into the loop. */
static inline void crumb_at(struct crumb *c, enum crumb_step step) {
    ((volatile struct crumb *)c)->step = (uint8_t)step;
}
static inline void crumb_pass(struct crumb *c) {
    volatile struct crumb *v = c;
    v->pass = v->pass + 1;
}

/* The step's name, as poll_*() functions are named in node.c; "?" for one not known. */
const char *crumb_name(uint8_t step);

#endif
