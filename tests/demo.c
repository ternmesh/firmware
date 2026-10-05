#include "demo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

/* The Heltec V3 port's bench pairing (ports/heltec-v3/main/demo.c), which has no hardware in it.
 * What matters most here is that no restart, mistake or failure makes a board send two frames
 * with the same keys and counter. */

/* A board's flash: a few named records, and a switch to make saving fail. */
struct store {
    char keys[4][16];
    uint8_t data[4][sizeof(struct demo_state) + sizeof(struct demo_used)];
    size_t lens[4];
    bool broken;
};

static int find(struct store *s, const char *key) {
    for (int i = 0; i < 4; i++) {
        if (strcmp(s->keys[i], key) == 0) {
            return i;
        }
    }
    return -1;
}

static bool store_load(void *ctx, const char *key, void *buf, size_t len) {
    struct store *s = ctx;
    int i = find(s, key);
    if (i < 0 || s->lens[i] != len) {
        return false;
    }
    memcpy(buf, s->data[i], len);
    return true;
}

static bool store_save(void *ctx, const char *key, const void *buf, size_t len) {
    struct store *s = ctx;
    if (s->broken) {
        return false;
    }
    int i = find(s, key);
    if (i < 0) {
        i = find(s, "");
    }
    strcpy(s->keys[i], key);
    memcpy(s->data[i], buf, len);
    s->lens[i] = len;
    return true;
}

/* A board: its flash, and what is in its RAM, which a restart loses. */
struct board {
    struct store flash;
    struct demo ram;
};

static void boot(struct board *b) {
    struct demo_store st = {&b->flash, store_load, store_save};
    memset(&b->ram, 0xA5, sizeof b->ram); /* RAM holds nothing useful at power-on */
    demo_start(&b->ram, &st);
}

static struct board *new_board(void) {
    static struct board boards[16];
    static size_t used;
    if (used == sizeof boards / sizeof boards[0]) {
        fprintf(stderr, "out of boards\n");
        abort();
    }
    struct board *b = &boards[used++];
    memset(b, 0, sizeof *b);
    boot(b);
    return b;
}

/* Seals text on one board and hands the frame to another; returns what the second made of it. */
static enum demo_heard say(struct board *from, struct board *to, const char *text,
                           uint32_t *counter) {
    uint8_t frame[TERN_UNICAST_MAX_FRAME], msg[TERN_UNICAST_MAX_PLAINTEXT];
    size_t len = strlen(text), got = 0;
    CHECK(demo_seal(&from->ram, (const uint8_t *)text, len, frame) == DEMO_OK);
    enum demo_heard h = demo_open(&to->ram, frame, len + 16, msg, &got, counter);
    if (h == DEMO_HEARD_MESSAGE) {
        CHECK(got == len && memcmp(msg, text, len) == 0);
    }
    return h;
}

static void unpaired_board_sends_nothing(void) {
    struct board *a = new_board();
    uint8_t frame[32];
    CHECK(demo_seal(&a->ram, (const uint8_t *)"x", 1, frame) == DEMO_UNPAIRED);
}

static void paired_boards_talk_both_ways(void) {
    struct board *a = new_board(), *b = new_board();
    uint32_t n;
    CHECK(demo_pair(&a->ram, TERN_INITIATOR, "correct horse") == DEMO_OK);
    CHECK(demo_pair(&b->ram, TERN_RESPONDER, "correct horse") == DEMO_OK);
    CHECK(say(a, b, "one", &n) == DEMO_HEARD_MESSAGE && n == 0);
    CHECK(say(a, b, "two", &n) == DEMO_HEARD_MESSAGE && n == 1);
    CHECK(say(b, a, "back", &n) == DEMO_HEARD_MESSAGE && n == 0);
}

static void restart_carries_on_counting(void) {
    struct board *a = new_board(), *b = new_board();
    uint32_t n;
    CHECK(demo_pair(&a->ram, TERN_INITIATOR, "battery staple") == DEMO_OK);
    CHECK(demo_pair(&b->ram, TERN_RESPONDER, "battery staple") == DEMO_OK);
    CHECK(say(a, b, "before", &n) == DEMO_HEARD_MESSAGE && n == 0);
    boot(a);
    boot(b);
    CHECK(say(a, b, "after", &n) == DEMO_HEARD_MESSAGE && n == 1);
}

static void receiver_restart_still_refuses_replays(void) {
    struct board *a = new_board(), *b = new_board();
    uint8_t frame[TERN_UNICAST_MAX_FRAME], msg[TERN_UNICAST_MAX_PLAINTEXT];
    size_t len;
    uint32_t n;
    CHECK(demo_pair(&a->ram, TERN_INITIATOR, "replay") == DEMO_OK);
    CHECK(demo_pair(&b->ram, TERN_RESPONDER, "replay") == DEMO_OK);
    CHECK(demo_seal(&a->ram, (const uint8_t *)"once", 4, frame) == DEMO_OK);
    CHECK(demo_open(&b->ram, frame, 20, msg, &len, &n) == DEMO_HEARD_MESSAGE);
    boot(b);
    CHECK(demo_open(&b->ram, frame, 20, msg, &len, &n) != DEMO_HEARD_MESSAGE);
}

static void passphrase_is_never_paired_twice(void) {
    struct board *a = new_board();
    CHECK(demo_pair(&a->ram, TERN_INITIATOR, "only once") == DEMO_OK);
    CHECK(demo_pair(&a->ram, TERN_INITIATOR, "only once") == DEMO_REUSED);
    CHECK(demo_pair(&a->ram, TERN_RESPONDER, "only once") == DEMO_REUSED);
    boot(a);
    CHECK(demo_pair(&a->ram, TERN_INITIATOR, "only once") == DEMO_REUSED);
    CHECK(demo_pair(&a->ram, TERN_INITIATOR, "another") == DEMO_OK);
}

static void list_of_passphrases_fills_up(void) {
    struct board *a = new_board();
    char p[16];
    for (int i = 0; i < DEMO_MAX_PASSPHRASES; i++) {
        snprintf(p, sizeof p, "p%d", i);
        CHECK(demo_pair(&a->ram, TERN_INITIATOR, p) == DEMO_OK);
    }
    CHECK(demo_pair(&a->ram, TERN_INITIATOR, "one more") == DEMO_FULL);
}

static void same_role_on_both_is_caught_and_stops_sending(void) {
    struct board *a = new_board(), *b = new_board();
    uint8_t frame[32];
    uint32_t n;
    CHECK(demo_pair(&a->ram, TERN_INITIATOR, "clash") == DEMO_OK);
    CHECK(demo_pair(&b->ram, TERN_INITIATOR, "clash") == DEMO_OK);
    CHECK(say(a, b, "hello", &n) == DEMO_HEARD_CLASH);
    CHECK(demo_seal(&b->ram, (const uint8_t *)"x", 1, frame) == DEMO_CONFLICT);
    boot(b);
    CHECK(demo_seal(&b->ram, (const uint8_t *)"x", 1, frame) == DEMO_CONFLICT);
}

static void failed_save_sends_nothing(void) {
    struct board *a = new_board(), *b = new_board();
    uint8_t frame[32];
    uint32_t n;
    CHECK(demo_pair(&a->ram, TERN_INITIATOR, "flaky flash") == DEMO_OK);
    CHECK(demo_pair(&b->ram, TERN_RESPONDER, "flaky flash") == DEMO_OK);
    a->flash.broken = true;
    CHECK(demo_seal(&a->ram, (const uint8_t *)"lost", 4, frame) == DEMO_STORE_FAILED);
    CHECK(demo_pair(&a->ram, TERN_INITIATOR, "new one") == DEMO_STORE_FAILED);
    /* The frame was never sent, so after a restart its counter may be used, once. */
    a->flash.broken = false;
    boot(a);
    CHECK(say(a, b, "found", &n) == DEMO_HEARD_MESSAGE && n == 0);
    /* And the passphrase whose pairing failed was not marked used. */
    CHECK(demo_pair(&a->ram, TERN_INITIATOR, "new one") == DEMO_OK);
}

static void record_from_another_build_is_not_used(void) {
    struct board *a = new_board();
    CHECK(demo_pair(&a->ram, TERN_INITIATOR, "old build") == DEMO_OK);
    int i = find(&a->flash, "session");
    a->flash.data[i][4] ^= 1; /* the recorded size no longer matches */
    boot(a);
    CHECK(a->ram.s.role == 0);
    /* The passphrase list survives it, which is what keeps that session from starting over. */
    CHECK(demo_pair(&a->ram, TERN_INITIATOR, "old build") == DEMO_REUSED);
}

int main(void) {
    RUN(unpaired_board_sends_nothing);
    RUN(paired_boards_talk_both_ways);
    RUN(restart_carries_on_counting);
    RUN(receiver_restart_still_refuses_replays);
    RUN(passphrase_is_never_paired_twice);
    RUN(list_of_passphrases_fills_up);
    RUN(same_role_on_both_is_caught_and_stops_sending);
    RUN(failed_save_sends_nothing);
    RUN(record_from_another_build_is_not_used);
    return CHECK_DONE();
}
