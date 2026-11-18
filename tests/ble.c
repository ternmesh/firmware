#include "ble.h"

#include "check.h"

/* The passkey a port draws for a pairing (ble.h): from a generator that may fail, which then
 * gives no passkey at all rather than one it did not draw. */

struct script {
    const uint32_t *values; /* what the generator gives, in turn */
    const bool *gives;      /* and whether it gives it */
    int at;
};

static bool scripted(void *ctx, uint32_t *r) {
    struct script *s = ctx;
    int i = s->at++;
    if (!s->gives[i]) {
        return false; /* *r left as it was: whatever the stack held */
    }
    *r = s->values[i];
    return true;
}

static void a_draw_gives_a_passkey(void) {
    const uint32_t values[] = {123456789u};
    const bool gives[] = {true};
    struct script s = {values, gives, 0};
    uint32_t key = 0;
    CHECK(ble_passkey_draw(scripted, &s, &key));
    CHECK_EQ_U64(key, 123456789u % 1000000u);
    CHECK_EQ_U64(s.at, 1);
}

/* Failures and draws past the last whole million are drawn again. */
static void failures_are_drawn_again(void) {
    const uint32_t values[] = {0, 0xFFFFFFFFu, 0, 42};
    const bool gives[] = {false, true, false, true};
    struct script s = {values, gives, 0};
    uint32_t key = 7;
    CHECK(ble_passkey_draw(scripted, &s, &key));
    CHECK_EQ_U64(key, 42);
    CHECK_EQ_U64(s.at, 4);
}

/* A generator that keeps failing gives no passkey, and the key is not written. */
static void a_generator_that_fails_gives_none(void) {
    uint32_t values[BLE_PASSKEY_TRIES] = {0};
    bool gives[BLE_PASSKEY_TRIES] = {false};
    struct script s = {values, gives, 0};
    uint32_t key = 0xFFFFFFFFu;
    CHECK(!ble_passkey_draw(scripted, &s, &key));
    CHECK_EQ_U64(key, 0xFFFFFFFFu);
    CHECK_EQ_U64(s.at, BLE_PASSKEY_TRIES);
}

int main(void) {
    RUN(a_draw_gives_a_passkey);
    RUN(failures_are_drawn_again);
    RUN(a_generator_that_fails_gives_none);
    return CHECK_DONE();
}
