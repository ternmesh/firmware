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

/* Whether the battery is charging, and whether it is empty, from its readings one after another.
 *
 * The board has no wire from its charger to the chip, so charging is inferred from the voltage:
 * plugging a charger in raises the cell's voltage at once by the charging current through its
 * internal resistance, typically 30 to 80 mV, and then it climbs; unplugging drops it by as much.
 * So a step up of POWER_STEP_MV or more between two readings is taken for a charger, and a step
 * down, or the voltage falling POWER_SAG_MV below the highest reading since, for its going. It is
 * an estimate: a full battery plugged in takes no current and shows no step, and readings taken
 * while the radio sends sag, so the caller does not read then.
 *
 * Empty is two readings in a row below POWER_EMPTY_MV while not charging: one low reading may be
 * a sag under load. */
#define POWER_STEP_MV 30
#define POWER_SAG_MV 40
#define POWER_EMPTY_MV 3300   /* below this the board turns itself off */
#define POWER_RESTART_MV 3450 /* and above this, charged a little, it turns on again */
#define POWER_LOW_PERCENT 10  /* at this or below, the screen says the battery is low */

struct power_watch {
    uint16_t last, peak; /* millivolts: the last reading, and the highest while charging */
    bool charging;
    uint8_t low; /* readings in a row below POWER_EMPTY_MV */
};

/* Takes a reading: millivolts, or 0 for no battery, which forgets everything. */
void power_watch(struct power_watch *w, uint16_t mv);

/* Whether the battery is too low to run on: the board should turn itself off. */
bool power_empty(const struct power_watch *w);

#endif
