#ifndef HELTEC_V3_POWER_H
#define HELTEC_V3_POWER_H

#include <stdbool.h>
#include <stdint.h>

/* The battery, as the board's measuring circuit gives it, and what to tell the user of it.
 *
 * Heltec's schematics put the battery through a 390k and 100k divider onto GPIO1, behind a switch
 * GPIO37 turns on so the divider does not drain the battery between readings. Which way GPIO37
 * turns it on changed between revisions: low on the V3 and V3.1, whose switch is a P-channel FET
 * held off by a pull-up, and high on the V3.2, which drives that FET through an NPN transistor.
 * Rather than ask which board it is, the board reads with GPIO37 each way once: the way that is
 * off reads near nothing, so the higher reading is the battery's, and the board remembers which
 * way that was (power_pick()). Between readings GPIO37 is left the other way.
 *
 * Nothing here touches the hardware, so tests/power.c runs it on a host. */

#define POWER_NONE_MV 2500 /* less than this at the battery is no battery: none is fitted */
#define POWER_UNKNOWN 255  /* a percentage that is not known */

struct power_sense {
    bool known;        /* which way GPIO37 turns the circuit on has been found */
    bool high_enables; /* and it is high */
};

/* The battery's millivolts from a reading with GPIO37 low and one with it high, learning which
 * way turns the circuit on if that is not yet known; 0 if neither reads a battery. Once it is
 * known, only that reading is looked at, and the other may be anything. */
uint16_t power_pick(struct power_sense *s, uint16_t at_low_mv, uint16_t at_high_mv);

/* A lithium polymer cell's charge, roughly, from its voltage at rest: an estimate, good to ten
 * percent or so, that reads high while it charges. POWER_UNKNOWN below POWER_NONE_MV. */
uint8_t power_percent(uint16_t mv);

#endif
