#include "tern/duty.h"

#include <stdlib.h>

#include "check.h"

/* The regulator's limit on transmitting (draft/phy.md): no period of the window's length may
 * hold more time on air than the limit, however the frames fall in it. */

#define S 1000000000LL
#define MS 1000000LL
#define HOUR (3600 * S)

static void no_limit_allows_everything(void) {
    struct tern_duty d;
    tern_duty_init(&d, 1000000, 3600);
    for (int i = 0; i < 100000; i++) {
        CHECK(tern_duty_allows(&d, i * MS, 400 * MS));
        tern_duty_charge(&d, i * MS, 400 * MS);
    }
    CHECK_EQ_I64(tern_duty_used(&d, 0), 0);
}

static void ten_percent_of_an_hour_is_six_minutes(void) {
    struct tern_duty d;
    tern_time now = 0;
    int sent = 0;
    tern_duty_init(&d, 100000, 3600);
    /* One-second frames back to back. */
    while (tern_duty_allows(&d, now, S)) {
        tern_duty_charge(&d, now, S);
        now += S;
        sent++;
    }
    CHECK_EQ_I64(sent, 360);
    CHECK_EQ_I64(tern_duty_used(&d, now), 360 * S);
    CHECK(!tern_duty_allows(&d, now, 1));
    CHECK(tern_duty_allows(&d, now, 0));
}

/* A frame stops counting an hour after it began, and by a minute later the account has let it
 * go. */
static void airtime_comes_back_after_the_window(void) {
    struct tern_duty d;
    tern_duty_init(&d, 100000, 3600);
    tern_duty_charge(&d, 30 * S, 360 * S);
    CHECK(!tern_duty_allows(&d, 30 * S + HOUR - 1, S));
    CHECK(tern_duty_allows(&d, 30 * S + HOUR + 60 * S, 360 * S));
    CHECK_EQ_I64(tern_duty_used(&d, 30 * S + HOUR + 60 * S), 0);
}

static void a_frame_longer_than_the_limit_is_never_allowed(void) {
    struct tern_duty d;
    tern_duty_init(&d, 100000, 3600);
    CHECK(!tern_duty_allows(&d, 0, 361 * S));
    CHECK(!tern_duty_allows(&d, 0, -1));
    CHECK(tern_duty_allows(&d, 0, 360 * S));
}

static void a_long_silence_empties_the_account(void) {
    struct tern_duty d;
    tern_duty_init(&d, 100000, 3600);
    tern_duty_charge(&d, 0, 360 * S);
    CHECK_EQ_I64(tern_duty_used(&d, 1000 * HOUR), 0);
    tern_duty_charge(&d, 1000 * HOUR, 5 * S);
    CHECK_EQ_I64(tern_duty_used(&d, 1000 * HOUR + S), 5 * S);
}

static void a_clock_that_goes_backwards_forgives_nothing(void) {
    struct tern_duty d;
    tern_duty_init(&d, 100000, 3600);
    tern_duty_charge(&d, 10 * HOUR, 360 * S);
    CHECK(!tern_duty_allows(&d, 0, S));
    CHECK(!tern_duty_allows(&d, -5 * HOUR, S));
    tern_duty_charge(&d, 0, S);
    CHECK_EQ_I64(tern_duty_used(&d, 10 * HOUR), 361 * S);
}

/* The rule itself, checked the slow way: a node that sends whenever it is allowed, at random
 * times and lengths, never puts more than the limit into any period of the window's length. And
 * the account is not much stricter than the rule: such a node gets most of what it is allowed. */
static void no_window_ever_holds_more_than_the_limit(void) {
    enum { MAX = 20000 };
    static tern_time at[MAX], air[MAX];
    static const struct {
        uint32_t ppm, window_s;
    } rules[] = {{100000, 3600}, {10000, 3600}, {1000, 3600}, {100000, 20}};

    srand(1);
    for (size_t r = 0; r < sizeof rules / sizeof rules[0]; r++) {
        struct tern_duty d;
        tern_time window = rules[r].window_s * S, limit = window / 1000000 * rules[r].ppm;
        tern_time now = 0, most = 0;
        int n = 0;
        tern_duty_init(&d, rules[r].ppm, rules[r].window_s);
        while (now < 6 * window && n < MAX) {
            tern_time len = (10 + rand() % 400) * MS * (tern_time)rules[r].window_s / 3600;
            if (tern_duty_allows(&d, now, len)) {
                tern_duty_charge(&d, now, len);
                at[n] = now;
                air[n] = len;
                n++;
            }
            now += window / 20000 * (1 + rand() % 12);
        }
        CHECK(n < MAX);
        /* Every window that matters begins with a frame. */
        for (int i = 0; i < n; i++) {
            tern_time sum = 0;
            for (int j = i; j < n && at[j] < at[i] + window; j++) {
                sum += air[j];
            }
            CHECK(sum <= limit);
            most = sum > most ? sum : most;
        }
        CHECK(most > limit / 10 * 9);
    }
}

int main(void) {
    RUN(no_limit_allows_everything);
    RUN(ten_percent_of_an_hour_is_six_minutes);
    RUN(airtime_comes_back_after_the_window);
    RUN(a_frame_longer_than_the_limit_is_never_allowed);
    RUN(a_long_silence_empties_the_account);
    RUN(a_clock_that_goes_backwards_forgives_nothing);
    RUN(no_window_ever_holds_more_than_the_limit);
    return CHECK_DONE();
}
