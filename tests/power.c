#include "power.h"

#include "check.h"

/* The Heltec V3 port's battery (ports/heltec-v3/main/power.c): which reading is the battery's,
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

int main(void) {
    RUN(a_v3_2_is_found_by_its_reading);
    RUN(a_v3_1_is_found_even_when_off_is_not_quite_off);
    RUN(no_battery_teaches_nothing);
    RUN(a_voltage_is_a_charge);
    return CHECK_DONE();
}
