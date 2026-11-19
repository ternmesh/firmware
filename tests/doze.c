#include "doze.h"

#include "check.h"
#include "tern/sleep.h"

/* When a sleeping leaf's radio is put to sleep and woken (ports/node/doze.c). */

static void a_gap_worth_it_puts_the_radio_to_sleep(void) {
    struct doze d;
    tern_time until = 0;
    doze_init(&d, 0);
    CHECK(doze_should(&d, TERN_S(10), true, TERN_S(70), INT64_MAX, false, &until));
    CHECK_EQ_I64(until, TERN_S(70) - DOZE_LEAD);
    /* Woken for the flooder too, whichever is first. */
    CHECK(doze_should(&d, TERN_S(10), true, TERN_S(70), TERN_S(20), false, &until));
    CHECK_EQ_I64(until, TERN_S(20) - DOZE_LEAD);
}

static void not_while_it_may_not_or_is_busy_or_the_gap_is_short(void) {
    struct doze d;
    tern_time until = 0;
    doze_init(&d, 0);
    CHECK(!doze_should(&d, TERN_S(10), false, TERN_S(70), INT64_MAX, false, &until));
    CHECK(!doze_should(&d, TERN_S(10), true, TERN_S(70), INT64_MAX, true, &until));
    CHECK(!doze_should(&d, TERN_S(10), true, TERN_S(10) + DOZE_MIN - 1, INT64_MAX, false, &until));
    CHECK(doze_should(&d, TERN_S(10), true, TERN_S(10) + DOZE_MIN, INT64_MAX, false, &until));
    doze_begin(&d, TERN_S(10), until);
    CHECK(!doze_should(&d, TERN_S(10), true, TERN_S(70), INT64_MAX, false, &until));
}

static void a_radio_that_will_not_wake_is_left_on(void) {
    struct doze d;
    tern_time until = 0;
    doze_init(&d, 0);
    d.failures = DOZE_FAILURES;
    CHECK(!doze_should(&d, TERN_S(10), true, TERN_S(70), INT64_MAX, false, &until));
}

static void it_wakes_on_time_for_work_or_when_sleep_stops(void) {
    struct doze d;
    doze_init(&d, 0);
    CHECK(!doze_wake_now(&d, TERN_S(1), TERN_S(1), true)); /* awake already */
    doze_begin(&d, TERN_S(10), TERN_S(60));
    CHECK(!doze_wake_now(&d, TERN_S(30), TERN_S(61), true));
    CHECK(doze_wake_now(&d, TERN_S(60), INT64_MAX, true));
    CHECK(doze_wake_now(&d, TERN_S(30), TERN_S(30) + DOZE_LEAD, true)); /* something to send */
    CHECK(doze_wake_now(&d, TERN_S(30), INT64_MAX, false));             /* sleep turned off */
}

static void the_time_off_is_counted(void) {
    struct doze d;
    doze_init(&d, TERN_S(100));
    CHECK_EQ_I64(doze_on_permille(&d, TERN_S(100)), 1000);
    doze_begin(&d, TERN_S(110), TERN_S(200));
    CHECK_EQ_I64(doze_off(&d, TERN_S(150)), TERN_S(40));
    doze_end(&d, TERN_S(190));
    CHECK_EQ_I64(doze_off(&d, TERN_S(200)), TERN_S(80));
    CHECK_EQ_I64(doze_on_permille(&d, TERN_S(200)), 200);
    doze_end(&d, TERN_S(300)); /* not dozing: nothing more counted */
    CHECK_EQ_I64(doze_off(&d, TERN_S(300)), TERN_S(80));
    CHECK_EQ_I64(d.dozes, 1);
}

static void the_loop_turns_at_most_a_slice_and_no_later_than_waking(void) {
    struct doze d;
    doze_init(&d, 0);
    CHECK_EQ_I64(doze_slice_ms(&d, 0), 1);
    doze_begin(&d, 0, TERN_S(10));
    CHECK_EQ_I64(doze_slice_ms(&d, 0), DOZE_SLICE_MS);
    CHECK_EQ_I64(doze_slice_ms(&d, TERN_S(10) - TERN_MS(30)), 30);
    CHECK_EQ_I64(doze_slice_ms(&d, TERN_S(10) - TERN_US(10)), 1);
    CHECK_EQ_I64(doze_slice_ms(&d, TERN_S(11)), 1);
}

static void the_command_takes_a_wake_and_a_listen(void) {
    uint16_t w = 0;
    uint8_t l = 0;
    CHECK(doze_parse("300", &w, &l));
    CHECK_EQ_I64(w, 300);
    CHECK_EQ_I64(l, TERN_SLEEP_LISTEN_MIN_S);
    CHECK(doze_parse(" 3600 5 ", &w, &l));
    CHECK_EQ_I64(w, 3600);
    CHECK_EQ_I64(l, 5);
    CHECK(doze_parse("900 255", &w, &l));
    CHECK(!doze_parse("299", &w, &l));  /* under WAKE_MIN */
    CHECK(!doze_parse("3601", &w, &l)); /* over WAKE_MAX */
    CHECK(!doze_parse("300 1", &w, &l));
    CHECK(!doze_parse("300 256", &w, &l));
    CHECK(!doze_parse("", &w, &l));
    CHECK(!doze_parse("five", &w, &l));
    CHECK(!doze_parse("300 2 7", &w, &l));
    CHECK(!doze_parse("9999999999", &w, &l));
}

static void the_setting_keeps_both_and_reads_nonsense_as_off(void) {
    uint16_t w = 1;
    uint8_t l = 1;
    doze_setting_read(doze_setting(600, 4), &w, &l);
    CHECK_EQ_I64(w, 600);
    CHECK_EQ_I64(l, 4);
    /* A listen is kept while it does not sleep, for when it does again (SET 9, version 15). */
    doze_setting_read(doze_setting(0, 9), &w, &l);
    CHECK_EQ_I64(w, 0);
    CHECK_EQ_I64(l, 9);
    doze_setting_read(0, &w, &l);
    CHECK_EQ_I64(w, 0);
    doze_setting_read(0xFFFFFFFFu, &w, &l); /* flash never written */
    CHECK_EQ_I64(w, 0);
    doze_setting_read(doze_setting(30, 2), &w, &l); /* a wake the specification no longer allows */
    CHECK_EQ_I64(w, 0);
}

int main(void) {
    RUN(a_gap_worth_it_puts_the_radio_to_sleep);
    RUN(not_while_it_may_not_or_is_busy_or_the_gap_is_short);
    RUN(a_radio_that_will_not_wake_is_left_on);
    RUN(it_wakes_on_time_for_work_or_when_sleep_stops);
    RUN(the_time_off_is_counted);
    RUN(the_loop_turns_at_most_a_slice_and_no_later_than_waking);
    RUN(the_command_takes_a_wake_and_a_listen);
    RUN(the_setting_keeps_both_and_reads_nonsense_as_off);
    return CHECK_DONE();
}
