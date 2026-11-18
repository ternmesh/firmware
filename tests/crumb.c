#include "crumb.h"

#include <string.h>

#include "check.h"

/* Where the node's loop was when the watchdog restarted it (ports/node/crumb.c). */

/* Power coming on leaves the memory holding anything: nothing is reported, and it starts clean. */
static void power_on_clears_garbage(void) {
    struct crumb c;
    memset(&c, 0xA5, sizeof c);
    CHECK(!crumb_boot(&c, false));
    CHECK_EQ_I64(c.restarts, 0);
    CHECK_EQ_I64(c.last_step, CRUMB_NONE);
    CHECK_EQ_I64(c.step, CRUMB_STARTING);
    CHECK_EQ_I64(c.pass, 0);
}

/* Garbage that a watchdog reset reports as its reason (a chip that says so after power came on
 * with RAM unset) is not believed either. */
static void garbage_is_not_reported_after_the_watchdog(void) {
    struct crumb c;
    memset(&c, 0x3C, sizeof c);
    CHECK(!crumb_boot(&c, true));
    CHECK_EQ_I64(c.restarts, 0);
}

static void the_watchdog_keeps_the_step_and_the_pass(void) {
    struct crumb c;
    memset(&c, 0, sizeof c);
    (void)crumb_boot(&c, false);
    for (int i = 0; i < 1234; i++) {
        crumb_pass(&c);
        crumb_at(&c, CRUMB_RADIO);
        crumb_at(&c, CRUMB_FORWARD);
    }
    CHECK(crumb_boot(&c, true));
    CHECK_EQ_I64(c.last_step, CRUMB_FORWARD);
    CHECK_EQ_I64(c.last_pass, 1234);
    CHECK_EQ_I64(c.restarts, 1);
    CHECK_EQ_I64(c.step, CRUMB_STARTING);
    CHECK_EQ_I64(c.pass, 0);

    /* A second time, in another step: counted, and the newer kept. */
    crumb_pass(&c);
    crumb_at(&c, CRUMB_BATTERY);
    CHECK(crumb_boot(&c, true));
    CHECK_EQ_I64(c.last_step, CRUMB_BATTERY);
    CHECK_EQ_I64(c.last_pass, 1);
    CHECK_EQ_I64(c.restarts, 2);
}

/* A restart the board made itself, not the watchdog's, reports nothing and forgets nothing. */
static void other_restarts_keep_the_last_report(void) {
    struct crumb c;
    memset(&c, 0, sizeof c);
    (void)crumb_boot(&c, false);
    crumb_at(&c, CRUMB_BLE);
    CHECK(crumb_boot(&c, true));
    crumb_at(&c, CRUMB_CONSOLE);
    CHECK(!crumb_boot(&c, false));
    CHECK_EQ_I64(c.last_step, CRUMB_BLE);
    CHECK_EQ_I64(c.restarts, 1);
}

/* A kept field that changed (memory that did not hold) clears the lot. */
static void a_damaged_record_is_cleared(void) {
    struct crumb c;
    memset(&c, 0, sizeof c);
    (void)crumb_boot(&c, false);
    crumb_at(&c, CRUMB_CARD);
    CHECK(crumb_boot(&c, true));
    c.last_pass ^= 0x10;
    CHECK(!crumb_boot(&c, true));
    CHECK_EQ_I64(c.restarts, 0);
    CHECK_EQ_I64(c.last_step, CRUMB_NONE);

    /* A step past the last is not believed. */
    (void)crumb_boot(&c, false);
    c.step = CRUMB_STEPS;
    CHECK(!crumb_boot(&c, true));
}

static void the_count_stops_at_its_most(void) {
    struct crumb c;
    memset(&c, 0, sizeof c);
    (void)crumb_boot(&c, false);
    for (unsigned i = 0; i < 70000u; i++) {
        crumb_at(&c, CRUMB_ROUTE);
        (void)crumb_boot(&c, true);
    }
    CHECK_EQ_I64(c.restarts, UINT16_MAX);
}

static void every_step_has_a_name(void) {
    for (unsigned s = 0; s < CRUMB_STEPS; s++) {
        const char *n = crumb_name((uint8_t)s);
        CHECK(n != NULL && n[0] != '\0' && strcmp(n, "?") != 0);
    }
    CHECK(strcmp(crumb_name(CRUMB_STEPS), "?") == 0);
    CHECK(strcmp(crumb_name(CRUMB_FORWARD), "poll_forward") == 0);
}

int main(void) {
    RUN(power_on_clears_garbage);
    RUN(garbage_is_not_reported_after_the_watchdog);
    RUN(the_watchdog_keeps_the_step_and_the_pass);
    RUN(other_restarts_keep_the_last_report);
    RUN(a_damaged_record_is_cleared);
    RUN(the_count_stops_at_its_most);
    RUN(every_step_has_a_name);
    return CHECK_DONE();
}
