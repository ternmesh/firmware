#include "power.h"

#include "check.h"

/* The node's battery (ports/node/power.c): which reading is the battery's,
 * whichever board revision it is, and the charge a voltage is shown as. */

/* A V3.2: GPIO37 high turns the circuit on, and low leaves only the divider's pull to ground. */
static void a_v3_2_is_found_by_its_reading(void) {
    struct power_sense s = {0};
    CHECK_EQ_I64(power_pick(&s, 12, 3910), 3910);
    CHECK(s.known && s.high_enables);
    CHECK_EQ_I64(power_pick(&s, 4000, 3900),
                 3900); /* known now: the low reading is not looked at */
}

/* A V3 or V3.1: low turns it on. With 3.3 V on the switch's gate against the battery's 4.2, the
 * way that is meant to be off may still pass a little: the higher reading is still the battery. */
static void a_v3_1_is_found_even_when_off_is_not_quite_off(void) {
    struct power_sense s = {0};
    CHECK_EQ_I64(power_pick(&s, 4120, 2700), 4120);
    CHECK(s.known && !s.high_enables);
    CHECK_EQ_I64(power_pick(&s, 3800, 0), 3800);
}

static void no_battery_teaches_nothing(void) {
    struct power_sense s = {0};
    CHECK_EQ_I64(power_pick(&s, 10, 30), 0);
    CHECK(!s.known);
    CHECK_EQ_I64(power_pick(&s, 3700, 5), 3700); /* fitted later: learnt then */
    CHECK(s.known && !s.high_enables);
    CHECK_EQ_I64(power_pick(&s, 900, 0), 0); /* taken out again */
    CHECK(s.known);
}

static void a_voltage_is_a_charge(void) {
    CHECK_EQ_I64(power_percent(0), POWER_UNKNOWN);
    CHECK_EQ_I64(power_percent(POWER_NONE_MV - 1), POWER_UNKNOWN);
    CHECK_EQ_I64(power_percent(POWER_NONE_MV), 0);
    CHECK_EQ_I64(power_percent(3300), 0);
    CHECK_EQ_I64(power_percent(3800), 50);
    CHECK_EQ_I64(power_percent(3825), 55);
    CHECK_EQ_I64(power_percent(4200), 100);
    CHECK_EQ_I64(power_percent(4350), 100); /* charging, or a cell that holds more */
    CHECK_EQ_I64(power_percent(UINT16_MAX), 100);
    unsigned last = 0;
    for (unsigned mv = POWER_NONE_MV; mv <= 4300; mv++) {
        unsigned p = power_percent((uint16_t)mv);
        CHECK(p >= last && p <= 100); /* never less charge for more voltage */
        last = p;
    }
}

static void a_step_up_is_a_charger(void) {
    struct power_watch w = {0};
    power_watch(&w, 3700);
    power_watch(&w, 3695); /* running down */
    CHECK(!w.charging);
    power_watch(&w, 3750); /* plugged in: 55 mV at once */
    CHECK(w.charging);
    power_watch(&w, 3752);
    power_watch(&w, 3760); /* and climbing */
    CHECK(w.charging);
    power_watch(&w, 3740); /* noise is not enough to stop it */
    CHECK(w.charging);
    power_watch(&w, 3700); /* unplugged: a step down */
    CHECK(!w.charging);
}

static void a_slow_fall_is_not_charging(void) {
    struct power_watch w = {0};
    power_watch(&w, 3700);
    power_watch(&w, 3735); /* a reading after a sag, say */
    CHECK(w.charging);
    for (uint16_t mv = 3725; mv > 3680; mv -= 10) {
        power_watch(&w, mv); /* ten at a time, no step, but well below the highest */
    }
    CHECK(!w.charging);
}

static void small_changes_are_not_a_charger(void) {
    struct power_watch w = {0};
    uint16_t mv[] = {3800, 3790, 3810, 3795, 3820, 3800};
    for (unsigned i = 0; i < sizeof mv / sizeof mv[0]; i++) {
        power_watch(&w, mv[i]);
        CHECK(!w.charging);
    }
}

static void empty_takes_two_low_readings(void) {
    struct power_watch w = {0};
    power_watch(&w, 3400);
    power_watch(&w, 3290); /* one low reading: a sag, perhaps */
    CHECK(!power_empty(&w));
    power_watch(&w, 3310);
    power_watch(&w, 3290);
    CHECK(!power_empty(&w));
    power_watch(&w, 3280);
    CHECK(power_empty(&w));
    power_watch(&w, 0); /* taken out */
    CHECK(!power_empty(&w));
    CHECK(!w.charging && w.last == 0);
}

static void a_charger_on_an_empty_battery_is_not_empty(void) {
    struct power_watch w = {0};
    power_watch(&w, 3250);
    power_watch(&w, 3240);
    CHECK(power_empty(&w));
    power_watch(&w, 3290); /* plugged in, still under 3300 */
    CHECK(w.charging);
    CHECK(!power_empty(&w));
    power_watch(&w, 3295);
    CHECK(!power_empty(&w));
}

int main(void) {
    RUN(a_v3_2_is_found_by_its_reading);
    RUN(a_v3_1_is_found_even_when_off_is_not_quite_off);
    RUN(no_battery_teaches_nothing);
    RUN(a_voltage_is_a_charge);
    RUN(a_step_up_is_a_charger);
    RUN(a_slow_fall_is_not_charging);
    RUN(small_changes_are_not_a_charger);
    RUN(empty_takes_two_low_readings);
    RUN(a_charger_on_an_empty_battery_is_not_empty);
    return CHECK_DONE();
}
