#include "demo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

/* The Heltec V3 port's node (ports/heltec-v3/main/demo.c), which has no hardware in it: its
 * identity, first contact over frames that may be lost, and the session that follows. What
 * matters most is that no restart, loss or failure makes a board send two frames with the same
 * keys and counter, or leaves the two boards unable to meet again. */

/* A board's flash: a few named records, and a switch to make saving fail. And its random
 * number generator, which here is only different for each board and never repeats. */
struct store {
    char keys[4][16];
    uint8_t data[4][sizeof(struct demo_state)];
    size_t lens[4];
    bool broken;
    uint64_t rng;
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

static bool store_random(void *ctx, uint8_t *buf, size_t len) {
    struct store *s = ctx;
    for (size_t i = 0; i < len; i++) {
        s->rng = s->rng * 6364136223846793005u + 1442695040888963407u;
        buf[i] = (uint8_t)(s->rng >> 56);
    }
    return true;
}

#define RETRY 1000 /* the time the initiator waits for an answer; the tests' clock is their own */

/* A board: its flash, and what is in its RAM, which a restart loses. */
struct board {
    struct store flash;
    struct demo ram;
};

static bool boot(struct board *b) {
    struct demo_store st = {&b->flash, store_load, store_save, store_random};
    memset(&b->ram, 0xA5, sizeof b->ram); /* RAM holds nothing useful at power-on */
    return demo_start(&b->ram, &st, RETRY);
}

static struct board *new_board(void) {
    static struct board boards[56];
    static size_t used;
    if (used == sizeof boards / sizeof boards[0]) {
        fprintf(stderr, "out of boards\n");
        abort();
    }
    struct board *b = &boards[used++];
    memset(b, 0, sizeof *b);
    b->flash.rng = 0x7465726eu + used;
    CHECK(boot(b));
    return b;
}

/* A frame on its way from one board to another. */
struct frame {
    uint8_t data[TERN_UNICAST_MAX_FRAME];
    size_t len;
};

/* Hands a frame to a board, and puts its answer, if any, in the frame's place. */
static enum demo_heard deliver(struct board *to, tern_time now, struct frame *f,
                               struct demo_received *out) {
    uint8_t msg[TERN_UNICAST_MAX_PLAINTEXT];
    enum demo_heard h = demo_receive(&to->ram, now, f->data, f->len, msg, out);
    memcpy(f->data, out->reply, out->reply_len);
    f->len = out->reply_len;
    return h;
}

/* First contact from a to b with nothing lost. Returns what b made of message_3. */
static enum demo_heard contact(struct board *a, struct board *b, tern_time now) {
    struct frame f;
    struct demo_received r;
    CHECK(demo_contact(&a->ram, b->ram.id.address, now, f.data, &f.len) == DEMO_OK);
    CHECK(f.len == 45);
    CHECK(deliver(b, now, &f, &r) == DEMO_HEARD_CONTACT && f.len == 53);
    CHECK(deliver(a, now, &f, &r) == DEMO_HEARD_CONTACT && f.len == 73);
    enum demo_heard at_b = deliver(b, now, &f, &r);
    if (at_b == DEMO_HEARD_PAIRED) {
        CHECK(f.len == 17 && memcmp(r.peer, a->ram.id.address, 32) == 0);
        CHECK(deliver(a, now, &f, &r) == DEMO_HEARD_PAIRED && f.len == 0);
        CHECK(memcmp(r.peer, b->ram.id.address, 32) == 0);
    }
    return at_b;
}

/* Seals text on one board and hands the frame to another; returns what the second made of it. */
static enum demo_heard say(struct board *from, struct board *to, const char *text,
                           uint32_t *counter) {
    uint8_t frame[TERN_UNICAST_MAX_FRAME], msg[TERN_UNICAST_MAX_PLAINTEXT];
    struct demo_received r;
    size_t len = strlen(text);
    CHECK(demo_seal(&from->ram, (const uint8_t *)text, len, frame) == DEMO_OK);
    enum demo_heard h = demo_receive(&to->ram, 0, frame, len + TERN_UNICAST_OVERHEAD, msg, &r);
    if (h == DEMO_HEARD_MESSAGE) {
        CHECK(r.msg_len == len && memcmp(msg, text, len) == 0);
        *counter = r.counter;
    }
    return h;
}

static void each_board_has_its_own_identity_and_keeps_it(void) {
    struct board *a = new_board(), *b = new_board();
    uint8_t address[TERN_ADDRESS_LEN];
    memcpy(address, a->ram.id.address, sizeof address);
    CHECK(tern_address_valid(address));
    CHECK(memcmp(address, b->ram.id.address, sizeof address) != 0);
    CHECK(boot(a));
    CHECK(memcmp(address, a->ram.id.address, sizeof address) == 0);
}

/* An address that changed at every start would be no address. */
static void board_that_cannot_save_its_identity_does_not_start(void) {
    static struct board b;
    b.flash.broken = true;
    CHECK(!boot(&b));
    b.flash.broken = false;
    CHECK(boot(&b));
}

static void board_with_no_session_sends_nothing(void) {
    struct board *a = new_board();
    uint8_t frame[32];
    CHECK(demo_seal(&a->ram, (const uint8_t *)"x", 1, frame) == DEMO_UNPAIRED);
}

static void first_contact_gives_both_a_session(void) {
    struct board *a = new_board(), *b = new_board();
    uint32_t n;
    CHECK(contact(a, b, 0) == DEMO_HEARD_PAIRED);
    CHECK(a->ram.s.role == TERN_INITIATOR && b->ram.s.role == TERN_RESPONDER);
    CHECK(say(a, b, "one", &n) == DEMO_HEARD_MESSAGE && n == 0);
    CHECK(say(a, b, "two", &n) == DEMO_HEARD_MESSAGE && n == 1);
    CHECK(say(b, a, "back", &n) == DEMO_HEARD_MESSAGE && n == 0);
}

static void own_and_invalid_addresses_are_refused(void) {
    struct board *a = new_board();
    uint8_t frame[TERN_CONTACT_MAX_FRAME], neutral[TERN_ADDRESS_LEN] = {1};
    size_t len;
    CHECK(demo_contact(&a->ram, a->ram.id.address, 0, frame, &len) == DEMO_OWN_ADDRESS);
    CHECK(demo_contact(&a->ram, neutral, 0, frame, &len) == DEMO_BAD_ADDRESS);
    CHECK(a->ram.h.phase == DEMO_IDLE);
}

/* The handshake names nobody in clear, so a board it is not for cannot tell, and stays silent. */
static void third_board_hears_nothing(void) {
    struct board *a = new_board(), *b = new_board(), *c = new_board();
    struct frame f, g;
    struct demo_received r;
    uint32_t n;
    CHECK(demo_contact(&a->ram, b->ram.id.address, 0, f.data, &f.len) == DEMO_OK);
    for (int i = 0; i < 4; i++) {
        g = f;
        CHECK(deliver(c, 0, &g, &r) == DEMO_HEARD_OTHER && g.len == 0);
        CHECK(c->ram.h.phase == DEMO_IDLE);
        (void)deliver(i % 2 ? a : b, 0, &f, &r);
    }
    CHECK(say(a, c, "for b", &n) == DEMO_HEARD_OTHER);
    CHECK(say(a, b, "for b", &n) == DEMO_HEARD_MESSAGE);
}

/* Runs first contact from a to b, losing the frame numbered `lost` (1 to 4) the first time it is
 * sent. The initiator's clock brings it round again. */
static void contact_losing(struct board *a, struct board *b, int lost) {
    struct frame f, again;
    struct demo_received r;
    tern_time now = 0;
    int sent = 0;
    bool dropped = false;
    CHECK(demo_contact(&a->ram, b->ram.id.address, now, f.data, &f.len) == DEMO_OK);
    for (int steps = 0; steps < 40 && a->ram.s.role == 0; steps++) {
        if (f.len == 0) {
            /* Nothing on the air: time passes until the initiator sends again. */
            now += RETRY;
            CHECK(demo_tick(&a->ram, now, again.data, &again.len) == DEMO_TICK_RESEND);
            CHECK(demo_tick(&b->ram, now, f.data, &f.len) == DEMO_TICK_NONE);
            f = again;
        }
        int n = f.data[0] - 0x50;
        sent++;
        if (n == lost && !dropped) {
            dropped = true;
            f.len = 0;
            continue;
        }
        (void)deliver(n % 2 ? b : a, now, &f, &r);
    }
    CHECK(dropped);
    /* One frame lost costs one more of the initiator's, and the answer to it if it had one. */
    CHECK(sent == (lost == 1 ? 5 : lost == 4 ? 6 : lost == 2 ? 6 : 5));
}

static void a_lost_frame_is_sent_again(void) {
    for (int lost = 1; lost <= 4; lost++) {
        struct board *a = new_board(), *b = new_board();
        uint32_t n;
        contact_losing(a, b, lost);
        CHECK(a->ram.s.role == TERN_INITIATOR && b->ram.s.role == TERN_RESPONDER);
        CHECK(say(a, b, "made it", &n) == DEMO_HEARD_MESSAGE && n == 0);
        CHECK(say(b, a, "so did i", &n) == DEMO_HEARD_MESSAGE && n == 0);
    }
}

/* The specification forbids computing an answer twice: a repeat gets the same bytes. */
static void a_repeat_gets_the_same_answer(void) {
    struct board *a = new_board(), *b = new_board();
    struct frame m1, m2, m3, m4, f;
    struct demo_received r;
    CHECK(demo_contact(&a->ram, b->ram.id.address, 0, m1.data, &m1.len) == DEMO_OK);
    m2 = m1;
    CHECK(deliver(b, 0, &m2, &r) == DEMO_HEARD_CONTACT);
    f = m1;
    CHECK(deliver(b, 1, &f, &r) == DEMO_HEARD_CONTACT);
    CHECK(f.len == m2.len && memcmp(f.data, m2.data, f.len) == 0);
    m3 = m2;
    CHECK(deliver(a, 1, &m3, &r) == DEMO_HEARD_CONTACT);
    f = m2;
    CHECK(deliver(a, 2, &f, &r) == DEMO_HEARD_CONTACT);
    CHECK(f.len == m3.len && memcmp(f.data, m3.data, f.len) == 0);
    m4 = m3;
    CHECK(deliver(b, 2, &m4, &r) == DEMO_HEARD_PAIRED);
    f = m3;
    CHECK(deliver(b, 3, &f, &r) == DEMO_HEARD_CONTACT);
    CHECK(f.len == m4.len && memcmp(f.data, m4.data, f.len) == 0);
    CHECK(b->ram.s.heard == 0 && b->ram.s.session.tx.next == 0); /* the session is untouched */
    /* Once the responder has forgotten the handshake, the repeat is nothing to it. */
    CHECK(demo_tick(&b->ram, 2 + DEMO_HOLD(RETRY), f.data, &f.len) == DEMO_TICK_NONE);
    f = m3;
    CHECK(deliver(b, 2 + DEMO_HOLD(RETRY), &f, &r) == DEMO_HEARD_OTHER && f.len == 0);
}

static void initiator_gives_up_and_keeps_what_it_had(void) {
    struct board *a = new_board(), *b = new_board(), *c = new_board();
    struct frame f;
    uint32_t n;
    tern_time now = 0;
    CHECK(contact(a, b, now) == DEMO_HEARD_PAIRED);
    CHECK(demo_contact(&a->ram, c->ram.id.address, now, f.data, &f.len) == DEMO_OK);
    for (int i = 1; i < DEMO_TRIES; i++) {
        CHECK(demo_tick(&a->ram, now + RETRY - 1, f.data, &f.len) == DEMO_TICK_NONE);
        now += RETRY;
        CHECK(demo_tick(&a->ram, now, f.data, &f.len) == DEMO_TICK_RESEND && f.len == 45);
    }
    now += RETRY;
    CHECK(demo_tick(&a->ram, now, f.data, &f.len) == DEMO_TICK_GAVE_UP && f.len == 0);
    CHECK(demo_tick(&a->ram, now + RETRY, f.data, &f.len) == DEMO_TICK_NONE);
    CHECK(say(a, b, "still here", &n) == DEMO_HEARD_MESSAGE && n == 0);
}

static void responder_forgets_a_handshake_never_finished(void) {
    struct board *a = new_board(), *b = new_board(), *c = new_board();
    struct frame f, g;
    struct demo_received r;
    CHECK(demo_contact(&a->ram, b->ram.id.address, 0, f.data, &f.len) == DEMO_OK);
    CHECK(deliver(b, 0, &f, &r) == DEMO_HEARD_CONTACT);
    /* While it waits, another board's message_1 is not answered. */
    CHECK(demo_contact(&c->ram, b->ram.id.address, 0, g.data, &g.len) == DEMO_OK);
    f = g;
    CHECK(deliver(b, 1, &f, &r) == DEMO_HEARD_OTHER && f.len == 0);
    CHECK(demo_tick(&b->ram, DEMO_HOLD(RETRY) - 1, f.data, &f.len) == DEMO_TICK_NONE);
    CHECK(demo_tick(&b->ram, DEMO_HOLD(RETRY), f.data, &f.len) == DEMO_TICK_LAPSED);
    CHECK(deliver(b, DEMO_HOLD(RETRY), &g, &r) == DEMO_HEARD_CONTACT && g.len == 53);
}

/* A board with a session is not taken over by whoever asks. */
static void stranger_is_refused_until_accepted(void) {
    struct board *a = new_board(), *b = new_board(), *c = new_board();
    struct frame f;
    struct demo_received r;
    uint32_t n;
    CHECK(contact(a, b, 0) == DEMO_HEARD_PAIRED);

    CHECK(demo_contact(&c->ram, b->ram.id.address, 10, f.data, &f.len) == DEMO_OK);
    CHECK(deliver(b, 10, &f, &r) == DEMO_HEARD_CONTACT);
    CHECK(deliver(c, 10, &f, &r) == DEMO_HEARD_CONTACT);
    CHECK(deliver(b, 10, &f, &r) == DEMO_HEARD_REFUSED && f.len == 0);
    CHECK(memcmp(r.peer, c->ram.id.address, 32) == 0);
    CHECK(say(a, b, "still ours", &n) == DEMO_HEARD_MESSAGE && n == 0);

    demo_accept(&b->ram, 100);
    CHECK(contact(c, b, 100) == DEMO_HEARD_REFUSED); /* the time given is when it closes */
    demo_accept(&b->ram, 200);
    CHECK(contact(c, b, 199) == DEMO_HEARD_PAIRED);
    CHECK(say(c, b, "hello", &n) == DEMO_HEARD_MESSAGE && n == 0);
    CHECK(say(a, b, "gone", &n) == DEMO_HEARD_OTHER);
    /* Accepting one is not accepting the next. */
    CHECK(contact(a, b, 199) == DEMO_HEARD_REFUSED);
}

/* Its own peer may always come again: that is how two boards recover when only one of them took
 * up the session. And a new handshake is a new session, with new keys. */
static void peer_may_make_contact_again(void) {
    struct board *a = new_board(), *b = new_board();
    uint8_t old[TERN_UNICAST_MAX_FRAME], msg[TERN_UNICAST_MAX_PLAINTEXT];
    struct demo_received r;
    uint32_t n;
    CHECK(contact(a, b, 0) == DEMO_HEARD_PAIRED);
    CHECK(demo_seal(&a->ram, (const uint8_t *)"old", 3, old) == DEMO_OK);
    CHECK(contact(a, b, 0) == DEMO_HEARD_PAIRED);
    CHECK(contact(b, a, 0) == DEMO_HEARD_PAIRED);
    CHECK(b->ram.s.role == TERN_INITIATOR && a->ram.s.role == TERN_RESPONDER);
    CHECK(demo_receive(&b->ram, 0, old, 26, msg, &r) == DEMO_HEARD_OTHER);
    CHECK(say(a, b, "new", &n) == DEMO_HEARD_MESSAGE && n == 0);
}

static void restart_carries_on_counting(void) {
    struct board *a = new_board(), *b = new_board();
    uint32_t n;
    CHECK(contact(a, b, 0) == DEMO_HEARD_PAIRED);
    CHECK(say(a, b, "before", &n) == DEMO_HEARD_MESSAGE && n == 0);
    CHECK(boot(a));
    CHECK(boot(b));
    CHECK(say(a, b, "after", &n) == DEMO_HEARD_MESSAGE && n == 1);
}

static void restart_abandons_a_handshake(void) {
    struct board *a = new_board(), *b = new_board();
    struct frame f;
    struct demo_received r;
    CHECK(demo_contact(&a->ram, b->ram.id.address, 0, f.data, &f.len) == DEMO_OK);
    CHECK(deliver(b, 0, &f, &r) == DEMO_HEARD_CONTACT);
    CHECK(boot(a));
    CHECK(a->ram.h.phase == DEMO_IDLE);
    CHECK(deliver(a, 0, &f, &r) == DEMO_HEARD_OTHER && f.len == 0);
}

static void receiver_restart_still_refuses_replays(void) {
    struct board *a = new_board(), *b = new_board();
    uint8_t frame[TERN_UNICAST_MAX_FRAME], msg[TERN_UNICAST_MAX_PLAINTEXT];
    struct demo_received r;
    CHECK(contact(a, b, 0) == DEMO_HEARD_PAIRED);
    CHECK(demo_seal(&a->ram, (const uint8_t *)"once", 4, frame) == DEMO_OK);
    CHECK(demo_receive(&b->ram, 0, frame, 27, msg, &r) == DEMO_HEARD_MESSAGE);
    CHECK(boot(b));
    CHECK(demo_receive(&b->ram, 0, frame, 27, msg, &r) != DEMO_HEARD_MESSAGE);
}

/* A message whose reception could not be saved is not shown, so it is shown once at most. */
static void unsaved_reception_is_not_shown(void) {
    struct board *a = new_board(), *b = new_board();
    uint8_t frame[TERN_UNICAST_MAX_FRAME], msg[TERN_UNICAST_MAX_PLAINTEXT];
    struct demo_received r;
    CHECK(contact(a, b, 0) == DEMO_HEARD_PAIRED);
    CHECK(demo_seal(&a->ram, (const uint8_t *)"once", 4, frame) == DEMO_OK);
    b->flash.broken = true;
    memset(msg, 0x55, sizeof msg);
    CHECK(demo_receive(&b->ram, 0, frame, 27, msg, &r) == DEMO_HEARD_UNSAVED);
    CHECK(msg[0] == 0);
    CHECK(r.acks == 0); /* nor acknowledged: nobody was shown it */
    b->flash.broken = false;
    CHECK(boot(b));
    CHECK(demo_receive(&b->ram, 0, frame, 27, msg, &r) == DEMO_HEARD_MESSAGE);
    CHECK(demo_receive(&b->ram, 0, frame, 27, msg, &r) != DEMO_HEARD_MESSAGE);
}

/* And with no restart between, the board is as it was before the frame came: its sender's next
 * try is taken, not answered as a copy of a message that was never shown. */
static void unsaved_reception_is_taken_when_it_comes_again(void) {
    struct board *a = new_board(), *b = new_board();
    uint8_t frame[TERN_UNICAST_MAX_FRAME], msg[TERN_UNICAST_MAX_PLAINTEXT];
    struct demo_received r;
    CHECK(contact(a, b, 0) == DEMO_HEARD_PAIRED);
    CHECK(demo_seal(&a->ram, (const uint8_t *)"once", 4, frame) == DEMO_OK);
    b->flash.broken = true;
    CHECK(demo_receive(&b->ram, 0, frame, 27, msg, &r) == DEMO_HEARD_UNSAVED);
    CHECK_EQ_U64(b->ram.s.heard, 0);
    b->flash.broken = false;
    CHECK(demo_receive(&b->ram, 0, frame, 27, msg, &r) == DEMO_HEARD_MESSAGE);
    CHECK(r.msg_len == 4 && memcmp(msg, "once", 4) == 0 && r.acks == 1);
    CHECK_EQ_U64(b->ram.s.heard, 1);
}

/* A message heard comes with its acknowledgement, which only the board that sent it takes, and
 * only for that message. Heard again, it is not shown again, and is acknowledged again, after a
 * restart too. */
static void a_message_is_acknowledged_and_a_copy_again(void) {
    struct board *a = new_board(), *b = new_board(), *c = new_board();
    uint8_t frame[TERN_UNICAST_MAX_FRAME], msg[TERN_UNICAST_MAX_PLAINTEXT];
    uint8_t ack[TERN_UNICAST_ACK_LEN];
    struct demo_received r;
    CHECK(contact(a, b, 0) == DEMO_HEARD_PAIRED);
    CHECK(demo_seal(&a->ram, (const uint8_t *)"hi", 2, frame) == DEMO_OK);
    CHECK(demo_receive(&b->ram, 0, frame, 25, msg, &r) == DEMO_HEARD_MESSAGE);
    CHECK(r.acks == 1 && r.counter == 0);
    CHECK(memcmp(r.peer, a->ram.id.address, TERN_ADDRESS_LEN) == 0);
    memcpy(ack, r.ack[0], sizeof ack);
    CHECK(ack[0] == TERN_UNICAST_ACK_HDR && memcmp(&ack[11], &frame[11], 4) == 0);

    CHECK(demo_acked(&a->ram, 0, ack, sizeof ack));
    CHECK(!demo_acked(&a->ram, 1, ack, sizeof ack)); /* a message a has not sent */
    CHECK(!demo_acked(&b->ram, 0, ack, sizeof ack)); /* nor is it b's to take */
    CHECK(!demo_acked(&c->ram, 0, ack, sizeof ack)); /* nor a board's with no session */
    ack[18] ^= 0x01;
    CHECK(!demo_acked(&a->ram, 0, ack, sizeof ack));
    ack[18] ^= 0x01;

    for (int i = 0; i < 2; i++) {
        memset(msg, 0x55, sizeof msg);
        CHECK(demo_receive(&b->ram, 0, frame, 25, msg, &r) == DEMO_HEARD_COPY);
        CHECK(r.msg_len == 0 && msg[0] == 0x55 && r.counter == 0);
        CHECK(r.acks == 1 && memcmp(r.ack[0], ack, sizeof ack) == 0);
        CHECK_EQ_U64(b->ram.s.heard, 1);
        CHECK(boot(b));
    }
    /* A board the message was not for acknowledges nothing. */
    CHECK(demo_receive(&c->ram, 0, frame, 25, msg, &r) == DEMO_HEARD_OTHER && r.acks == 0);
}

static void failed_save_sends_nothing(void) {
    struct board *a = new_board(), *b = new_board();
    uint8_t frame[32];
    uint32_t n;
    CHECK(contact(a, b, 0) == DEMO_HEARD_PAIRED);
    a->flash.broken = true;
    CHECK(demo_seal(&a->ram, (const uint8_t *)"lost", 4, frame) == DEMO_STORE_FAILED);
    /* The frame was never sent, so after a restart its counter may be used, once. */
    a->flash.broken = false;
    CHECK(boot(a));
    CHECK(say(a, b, "found", &n) == DEMO_HEARD_MESSAGE && n == 0);
}

/* A responder that cannot save the new session does not send message_4, so the initiator never
 * takes it up either, and both are left with the session they had. */
static void session_that_cannot_be_saved_is_not_started(void) {
    struct board *a = new_board(), *b = new_board();
    struct frame f;
    struct demo_received r;
    uint32_t n;
    CHECK(contact(a, b, 0) == DEMO_HEARD_PAIRED);
    CHECK(say(a, b, "first", &n) == DEMO_HEARD_MESSAGE && n == 0);

    CHECK(demo_contact(&a->ram, b->ram.id.address, 0, f.data, &f.len) == DEMO_OK);
    CHECK(deliver(b, 0, &f, &r) == DEMO_HEARD_CONTACT);
    CHECK(deliver(a, 0, &f, &r) == DEMO_HEARD_CONTACT);
    b->flash.broken = true;
    CHECK(deliver(b, 0, &f, &r) == DEMO_HEARD_UNPAIRED && f.len == 0);
    b->flash.broken = false;
    CHECK(say(a, b, "second", &n) == DEMO_HEARD_MESSAGE && n == 1);

    /* The same for the initiator, at message_4. The responder has moved on by then, and the
     * initiator comes back to it as its peer. */
    CHECK(demo_contact(&a->ram, b->ram.id.address, 0, f.data, &f.len) == DEMO_OK);
    CHECK(deliver(b, 0, &f, &r) == DEMO_HEARD_CONTACT);
    CHECK(deliver(a, 0, &f, &r) == DEMO_HEARD_CONTACT);
    CHECK(deliver(b, 0, &f, &r) == DEMO_HEARD_PAIRED);
    a->flash.broken = true;
    CHECK(deliver(a, 0, &f, &r) == DEMO_HEARD_UNPAIRED);
    a->flash.broken = false;
    CHECK(say(a, b, "to the old session", &n) == DEMO_HEARD_OTHER);
    CHECK(contact(a, b, 0) == DEMO_HEARD_PAIRED);
    CHECK(say(a, b, "third", &n) == DEMO_HEARD_MESSAGE && n == 0);
}

static void record_from_another_build_is_not_used(void) {
    struct board *a = new_board(), *b = new_board();
    uint8_t address[TERN_ADDRESS_LEN];
    CHECK(contact(a, b, 0) == DEMO_HEARD_PAIRED);
    memcpy(address, a->ram.id.address, sizeof address);
    int i = find(&a->flash, "session");
    a->flash.data[i][4] ^= 1; /* the recorded size no longer matches */
    CHECK(boot(a));
    CHECK(a->ram.s.role == 0);
    CHECK(memcmp(address, a->ram.id.address, sizeof address) == 0);
    CHECK(contact(a, b, 0) == DEMO_HEARD_PAIRED);
}

static void frames_that_are_not_terns_are_malformed(void) {
    struct board *a = new_board();
    uint8_t msg[TERN_UNICAST_MAX_PLAINTEXT], junk[20] = {0x99};
    struct demo_received r;
    CHECK(demo_receive(&a->ram, 0, junk, sizeof junk, msg, &r) == DEMO_HEARD_MALFORMED);
    CHECK(demo_receive(&a->ram, 0, junk, 0, msg, &r) == DEMO_HEARD_MALFORMED);
    junk[0] = 0x51; /* a message_1 of the wrong length */
    CHECK(demo_receive(&a->ram, 0, junk, sizeof junk, msg, &r) == DEMO_HEARD_OTHER);
    CHECK(r.reply_len == 0);
}

int main(void) {
    RUN(each_board_has_its_own_identity_and_keeps_it);
    RUN(board_that_cannot_save_its_identity_does_not_start);
    RUN(board_with_no_session_sends_nothing);
    RUN(first_contact_gives_both_a_session);
    RUN(own_and_invalid_addresses_are_refused);
    RUN(third_board_hears_nothing);
    RUN(a_lost_frame_is_sent_again);
    RUN(a_repeat_gets_the_same_answer);
    RUN(initiator_gives_up_and_keeps_what_it_had);
    RUN(responder_forgets_a_handshake_never_finished);
    RUN(stranger_is_refused_until_accepted);
    RUN(peer_may_make_contact_again);
    RUN(restart_carries_on_counting);
    RUN(restart_abandons_a_handshake);
    RUN(receiver_restart_still_refuses_replays);
    RUN(unsaved_reception_is_not_shown);
    RUN(unsaved_reception_is_taken_when_it_comes_again);
    RUN(a_message_is_acknowledged_and_a_copy_again);
    RUN(failed_save_sends_nothing);
    RUN(session_that_cannot_be_saved_is_not_started);
    RUN(record_from_another_build_is_not_used);
    RUN(frames_that_are_not_terns_are_malformed);
    return CHECK_DONE();
}
