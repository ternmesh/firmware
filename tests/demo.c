#include "demo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "tern/route.h"

/* The Heltec V3 port's node (ports/heltec-v3/main/demo.c), which has no hardware in it: its
 * identity, first contact over frames that may be lost, and the session that follows. What
 * matters most is that no restart, loss or failure makes a board send two frames with the same
 * keys and counter, or leaves the two boards unable to meet again. */

/* A board's flash: a few named records, and a switch to make saving fail. And its random
 * number generator, which here is only different for each board and never repeats. */
enum { RECORDS = 1 + DEMO_PEERS }; /* an identity, and a record for each session */

struct store {
    char keys[RECORDS][16];
    uint8_t data[RECORDS][sizeof(struct demo_state)];
    size_t lens[RECORDS];
    bool broken;
    uint64_t rng;
};

static int find(struct store *s, const char *key) {
    for (int i = 0; i < RECORDS; i++) {
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

#define HOLD                                                                                       \
    1000 /* how long a handshake is kept with nothing heard; the tests' clock is their own */

/* A board: its flash, and what is in its RAM, which a restart loses. */
struct board {
    struct store flash;
    struct demo ram;
};

static bool boot(struct board *b) {
    struct demo_store st = {&b->flash, store_load, store_save, store_random};
    memset(&b->ram, 0xA5, sizeof b->ram); /* RAM holds nothing useful at power-on */
    return demo_start(&b->ram, &st, HOLD);
}

static struct board *new_board(void) {
    static struct board boards[80];
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
    CHECK(f.len == 56);
    CHECK(deliver(b, now, &f, &r) == DEMO_HEARD_CONTACT && f.len == 60);
    CHECK(deliver(a, now, &f, &r) == DEMO_HEARD_CONTACT && f.len == 80);
    enum demo_heard at_b = deliver(b, now, &f, &r);
    if (at_b == DEMO_HEARD_PAIRED) {
        CHECK(f.len == 24 && memcmp(r.peer, a->ram.id.address, 32) == 0);
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
    /* To the board it is said to, or, with no session with that one, to whoever it last spoke
     * with: the other board then overhears. */
    int slot = demo_peer(&from->ram, to->ram.id.address);
    CHECK(demo_seal(&from->ram, slot >= 0 ? slot : from->ram.last, (const uint8_t *)text, len,
                    frame) == DEMO_OK);
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
    CHECK(demo_seal(&a->ram, 0, (const uint8_t *)"x", 1, frame) == DEMO_UNPAIRED);
}

static void first_contact_gives_both_a_session(void) {
    struct board *a = new_board(), *b = new_board();
    uint32_t n;
    CHECK(contact(a, b, 0) == DEMO_HEARD_PAIRED);
    CHECK(a->ram.s[0].role == TERN_INITIATOR && b->ram.s[0].role == TERN_RESPONDER);
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
 * sent. The forwarder, which the test stands in for, keeps the frame the initiator last sent and
 * brings it round again. */
static void contact_losing(struct board *a, struct board *b, int lost) {
    struct frame f, kept;
    struct demo_received r;
    tern_time now = 0;
    int sent = 0;
    bool dropped = false;
    CHECK(demo_contact(&a->ram, b->ram.id.address, now, f.data, &f.len) == DEMO_OK);
    for (int steps = 0; steps < 40 && a->ram.s[0].role == 0; steps++) {
        if (f.len == 0) {
            /* Nothing on the air: time passes until the initiator's frame goes again. */
            now += HOLD / 4;
            CHECK(demo_tick(&a->ram, now) == DEMO_TICK_NONE);
            CHECK(demo_tick(&b->ram, now) == DEMO_TICK_NONE);
            f = kept;
        }
        int n = f.data[0] - 0x50;
        if (n % 2) {
            kept = f;
        }
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
        CHECK(a->ram.s[0].role == TERN_INITIATOR && b->ram.s[0].role == TERN_RESPONDER);
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
    /* The board that began the handshake answers nothing twice: its own frame goes again. */
    f = m2;
    CHECK(deliver(a, 2, &f, &r) == DEMO_HEARD_OTHER && f.len == 0);
    /* message_1 again, saying it came from somewhere else, is answered there with the same
     * message_2. */
    f = m1;
    memset(f.data + 15, 0x77, 4);
    CHECK(deliver(b, 2, &f, &r) == DEMO_HEARD_CONTACT && f.len == m2.len);
    CHECK(tern_contact_destination(f.data) == 0x77777777u);
    CHECK(tern_contact_destination(m2.data) == tern_route_id(a->ram.id.address));
    CHECK(memcmp(f.data + 11, m2.data + 11, f.len - 11) == 0);
    m4 = m3;
    CHECK(deliver(b, 2, &m4, &r) == DEMO_HEARD_PAIRED);
    f = m3;
    CHECK(deliver(b, 3, &f, &r) == DEMO_HEARD_CONTACT);
    CHECK(f.len == m4.len && memcmp(f.data, m4.data, f.len) == 0);
    CHECK(b->ram.s[0].heard == 0 &&
          b->ram.s[0].session.tx.next == 0); /* the session is untouched */
    /* Once the responder has forgotten the handshake, the repeat is nothing to it. */
    /* Kept from when the last repeat came, not from when the handshake was made. */
    CHECK(demo_tick(&b->ram, 2 + HOLD) == DEMO_TICK_NONE);
    f = m3;
    CHECK(deliver(b, 2 + HOLD, &f, &r) == DEMO_HEARD_CONTACT && f.len == m4.len);
    CHECK(demo_tick(&b->ram, 2 + 2 * HOLD) == DEMO_TICK_NONE);
    f = m3;
    CHECK(deliver(b, 2 + 2 * HOLD, &f, &r) == DEMO_HEARD_OTHER && f.len == 0);
}

static void initiator_gives_up_and_keeps_what_it_had(void) {
    struct board *a = new_board(), *b = new_board(), *c = new_board();
    struct frame f;
    uint32_t n;
    tern_time now = 0;
    CHECK(contact(a, b, now) == DEMO_HEARD_PAIRED);
    CHECK(demo_contact(&a->ram, c->ram.id.address, now, f.data, &f.len) == DEMO_OK);
    /* The forwarder sent its frame as often as it may, and says so. */
    CHECK(a->ram.h.phase == DEMO_INITIATING);
    CHECK(demo_tick(&a->ram, now + 100 * HOLD) == DEMO_TICK_NONE);
    CHECK(a->ram.h.phase == DEMO_INITIATING);
    CHECK(demo_abandon(&a->ram));
    CHECK(a->ram.h.phase == DEMO_IDLE);
    CHECK(!demo_abandon(&a->ram));
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
    CHECK(demo_tick(&b->ram, HOLD - 1) == DEMO_TICK_NONE);
    CHECK(demo_tick(&b->ram, HOLD) == DEMO_TICK_LAPSED);
    CHECK(deliver(b, HOLD, &g, &r) == DEMO_HEARD_CONTACT && g.len == 60);
}

/* A board that begins again is answered again: it is not made to wait for the handshake it
 * left, and the new one is the one that completes. */
static void the_board_that_began_may_begin_again(void) {
    struct board *a = new_board(), *b = new_board();
    struct frame f, old;
    struct demo_received r;
    uint32_t n;
    CHECK(demo_contact(&a->ram, b->ram.id.address, 0, old.data, &old.len) == DEMO_OK);
    f = old;
    CHECK(deliver(b, 0, &f, &r) == DEMO_HEARD_CONTACT);
    CHECK(demo_abandon(&a->ram));
    CHECK(contact(a, b, 5) == DEMO_HEARD_PAIRED);
    CHECK(say(a, b, "second time lucky", &n) == DEMO_HEARD_MESSAGE && n == 0);
    /* The handshake it left is nothing to the other board now. */
    f = old;
    CHECK(deliver(b, 6, &f, &r) == DEMO_HEARD_CONTACT && f.len == 60);
}

/* A board with a session is not taken over by whoever asks. */
static void stranger_is_refused_until_accepted(void) {
    struct board *a = new_board(), *b = new_board(), *c = new_board(), *d = new_board();
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
    CHECK(say(a, b, "and still ours", &n) == DEMO_HEARD_MESSAGE && n == 1);
    /* Accepting one is not accepting the next. */
    CHECK(contact(d, b, 199) == DEMO_HEARD_REFUSED);
}

static bool is_carol(void *ctx, const uint8_t address[TERN_ADDRESS_LEN]) {
    return memcmp(address, ((struct board *)ctx)->ram.id.address, TERN_ADDRESS_LEN) == 0;
}

/* One the user has saved as a contact is let in whenever it comes, and nobody else with it. */
static void a_trusted_stranger_is_accepted(void) {
    struct board *a = new_board(), *b = new_board(), *c = new_board(), *d = new_board();
    uint32_t n;
    CHECK(contact(a, b, 0) == DEMO_HEARD_PAIRED);
    CHECK(contact(c, b, 10) == DEMO_HEARD_REFUSED);

    demo_trust(&b->ram, is_carol, c);
    CHECK(contact(d, b, 20) == DEMO_HEARD_REFUSED);
    CHECK(contact(c, b, 30) == DEMO_HEARD_PAIRED);
    CHECK(say(c, b, "hello", &n) == DEMO_HEARD_MESSAGE && n == 0);
    CHECK(contact(d, b, 40) == DEMO_HEARD_REFUSED);

    /* A restart forgets how to ask, until the board says again. */
    CHECK(boot(b));
    CHECK(b->ram.trusted == NULL);
}

/* Its own peer may always come again: that is how two boards recover when only one of them took
 * up the session. And a new handshake is a new session, with new keys. */
static void peer_may_make_contact_again(void) {
    struct board *a = new_board(), *b = new_board();
    uint8_t old[TERN_UNICAST_MAX_FRAME], msg[TERN_UNICAST_MAX_PLAINTEXT];
    struct demo_received r;
    uint32_t n;
    CHECK(contact(a, b, 0) == DEMO_HEARD_PAIRED);
    CHECK(demo_seal(&a->ram, 0, (const uint8_t *)"old", 3, old) == DEMO_OK);
    CHECK(contact(a, b, 0) == DEMO_HEARD_PAIRED);
    CHECK(contact(b, a, 0) == DEMO_HEARD_PAIRED);
    CHECK(b->ram.s[0].role == TERN_INITIATOR && a->ram.s[0].role == TERN_RESPONDER);
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
    CHECK(demo_seal(&a->ram, 0, (const uint8_t *)"once", 4, frame) == DEMO_OK);
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
    CHECK(demo_seal(&a->ram, 0, (const uint8_t *)"once", 4, frame) == DEMO_OK);
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
    CHECK(demo_seal(&a->ram, 0, (const uint8_t *)"once", 4, frame) == DEMO_OK);
    b->flash.broken = true;
    CHECK(demo_receive(&b->ram, 0, frame, 27, msg, &r) == DEMO_HEARD_UNSAVED);
    CHECK_EQ_U64(b->ram.s[0].heard, 0);
    b->flash.broken = false;
    CHECK(demo_receive(&b->ram, 0, frame, 27, msg, &r) == DEMO_HEARD_MESSAGE);
    CHECK(r.msg_len == 4 && memcmp(msg, "once", 4) == 0 && r.acks == 1);
    CHECK_EQ_U64(b->ram.s[0].heard, 1);
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
    CHECK(demo_seal(&a->ram, 0, (const uint8_t *)"hi", 2, frame) == DEMO_OK);
    CHECK(demo_receive(&b->ram, 0, frame, 25, msg, &r) == DEMO_HEARD_MESSAGE);
    CHECK(r.acks == 1 && r.counter == 0);
    CHECK(memcmp(r.peer, a->ram.id.address, TERN_ADDRESS_LEN) == 0);
    memcpy(ack, r.ack[0], sizeof ack);
    CHECK(ack[0] == TERN_UNICAST_ACK_HDR && memcmp(&ack[11], &frame[11], 4) == 0);

    CHECK(demo_acked(&a->ram, 0, 0, ack, sizeof ack));
    CHECK(!demo_acked(&a->ram, 0, 1, ack, sizeof ack)); /* a message a has not sent */
    CHECK(!demo_acked(&a->ram, 1, 0, ack, sizeof ack)); /* a session a does not have */
    CHECK(!demo_acked(&a->ram, -1, 0, ack, sizeof ack) &&
          !demo_acked(&a->ram, DEMO_PEERS, 0, ack, sizeof ack));
    CHECK(!demo_acked(&b->ram, 0, 0, ack, sizeof ack)); /* nor is it b's to take */
    CHECK(!demo_acked(&c->ram, 0, 0, ack, sizeof ack)); /* nor a board's with no session */
    ack[18] ^= 0x01;
    CHECK(!demo_acked(&a->ram, 0, 0, ack, sizeof ack));
    ack[18] ^= 0x01;

    for (int i = 0; i < 2; i++) {
        memset(msg, 0x55, sizeof msg);
        CHECK(demo_receive(&b->ram, 0, frame, 25, msg, &r) == DEMO_HEARD_COPY);
        CHECK(r.msg_len == 0 && msg[0] == 0x55 && r.counter == 0);
        CHECK(r.acks == 1 && memcmp(r.ack[0], ack, sizeof ack) == 0);
        CHECK_EQ_U64(b->ram.s[0].heard, 1);
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
    CHECK(demo_seal(&a->ram, 0, (const uint8_t *)"lost", 4, frame) == DEMO_STORE_FAILED);
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
    CHECK(a->ram.s[0].role == 0);
    CHECK(memcmp(address, a->ram.id.address, sizeof address) == 0);
    CHECK(contact(a, b, 0) == DEMO_HEARD_PAIRED);
}

/* First contact that the board accepts, whoever it is with. */
static enum demo_heard let_in(struct board *from, struct board *to) {
    demo_accept(&to->ram, 1);
    return contact(from, to, 0);
}

static void a_board_holds_a_session_with_each_peer(void) {
    struct board *hub = new_board(), *a = new_board(), *b = new_board(), *c = new_board();
    uint8_t frame[TERN_UNICAST_MAX_FRAME], msg[TERN_UNICAST_MAX_PLAINTEXT];
    struct demo_received r;
    uint32_t n;
    CHECK(contact(a, hub, 0) == DEMO_HEARD_PAIRED);
    CHECK(let_in(b, hub) == DEMO_HEARD_PAIRED);
    CHECK(let_in(hub, c) == DEMO_HEARD_PAIRED); /* and one the hub began itself */
    CHECK_EQ_U64(demo_peers(&hub->ram), 3);
    struct board *peers[] = {a, b, c};
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < 3; i++) {
            int slot = demo_peer(&hub->ram, peers[i]->ram.id.address);
            CHECK(slot == i);
            /* From each, to the hub: taken in that peer's session, and said to be from it. */
            CHECK(demo_seal(&peers[i]->ram, 0, (const uint8_t *)"up", 2, frame) == DEMO_OK);
            CHECK(demo_receive(&hub->ram, 0, frame, 25, msg, &r) == DEMO_HEARD_MESSAGE);
            CHECK(r.slot == slot && r.counter == (uint32_t)round && r.acks == 1 &&
                  r.ack_slot[0] == slot);
            CHECK(memcmp(r.peer, peers[i]->ram.id.address, 32) == 0);
            CHECK(hub->ram.last == slot);
            /* Its acknowledgement is that peer's to take, and no other's. */
            CHECK(demo_acked(&peers[i]->ram, 0, (uint32_t)round, r.ack[0], sizeof r.ack[0]));
            CHECK(!demo_acked(&peers[(i + 1) % 3]->ram, 0, (uint32_t)round, r.ack[0],
                              sizeof r.ack[0]));
            /* And from the hub to each: only that one opens it. */
            CHECK(say(hub, peers[i], "down", &n) == DEMO_HEARD_MESSAGE &&
                  n == 2u * (uint32_t)round);
            CHECK(demo_seal(&hub->ram, slot, (const uint8_t *)"down", 4, frame) == DEMO_OK);
            CHECK(demo_receive(&peers[(i + 1) % 3]->ram, 0, frame, 27, msg, &r) ==
                  DEMO_HEARD_OTHER);
            CHECK(demo_receive(&peers[i]->ram, 0, frame, 27, msg, &r) == DEMO_HEARD_MESSAGE);
        }
        CHECK(boot(hub)); /* and all of it again after a restart, counting on */
        CHECK_EQ_U64(demo_peers(&hub->ram), 3);
        for (int i = 0; i < 3; i++) {
            CHECK(hub->ram.s[i].heard == (uint32_t)round + 1 &&
                  hub->ram.s[i].sent == 2u * ((uint32_t)round + 1));
        }
    }
}

/* Contact again with one peer is a new session with that one, and leaves the others'. */
static void contact_again_replaces_only_that_peers_session(void) {
    struct board *hub = new_board(), *a = new_board(), *b = new_board();
    uint8_t old[TERN_UNICAST_MAX_FRAME], msg[TERN_UNICAST_MAX_PLAINTEXT];
    struct demo_received r;
    uint32_t n;
    CHECK(contact(a, hub, 0) == DEMO_HEARD_PAIRED);
    CHECK(let_in(b, hub) == DEMO_HEARD_PAIRED);
    CHECK(say(b, hub, "one", &n) == DEMO_HEARD_MESSAGE && n == 0);
    CHECK(demo_seal(&a->ram, 0, (const uint8_t *)"old", 3, old) == DEMO_OK);
    CHECK(contact(a, hub, 0) == DEMO_HEARD_PAIRED); /* a peer it has: not refused */
    CHECK_EQ_U64(demo_peers(&hub->ram), 2);
    CHECK(demo_peer(&hub->ram, a->ram.id.address) == 0 &&
          demo_peer(&hub->ram, b->ram.id.address) == 1);
    CHECK(demo_receive(&hub->ram, 0, old, 26, msg, &r) == DEMO_HEARD_OTHER);
    CHECK(say(a, hub, "new", &n) == DEMO_HEARD_MESSAGE && n == 0);
    CHECK(say(b, hub, "two", &n) == DEMO_HEARD_MESSAGE && n == 1);
}

static void a_full_board_takes_no_new_peer_until_one_is_forgotten(void) {
    struct board *hub = new_board(), *peers[DEMO_PEERS], *late = new_board();
    struct frame f;
    uint32_t n;
    for (int i = 0; i < DEMO_PEERS; i++) {
        peers[i] = new_board();
        CHECK(let_in(peers[i], hub) == DEMO_HEARD_PAIRED);
    }
    CHECK_EQ_U64(demo_peers(&hub->ram), DEMO_PEERS);
    /* Asked, and willing: but there is nowhere to keep it, and nobody is dropped for it. */
    CHECK(let_in(late, hub) == DEMO_HEARD_FULL);
    CHECK(demo_contact(&hub->ram, late->ram.id.address, 0, f.data, &f.len) == DEMO_FULL);
    for (int i = 0; i < DEMO_PEERS; i++) {
        CHECK(say(peers[i], hub, "still here", &n) == DEMO_HEARD_MESSAGE && n == 0);
    }
    CHECK(hub->ram.h.phase == DEMO_IDLE); /* and the refused handshake is not kept */
    /* A peer it has may still come again. */
    CHECK(contact(peers[3], hub, 0) == DEMO_HEARD_PAIRED);
    CHECK(contact(hub, peers[5], 0) == DEMO_HEARD_PAIRED);

    CHECK(!demo_forget(&hub->ram, -1) && !demo_forget(&hub->ram, DEMO_PEERS));
    CHECK(demo_forget(&hub->ram, 2));
    CHECK(!demo_forget(&hub->ram, 2));
    CHECK_EQ_U64(demo_peers(&hub->ram), DEMO_PEERS - 1);
    CHECK(say(peers[2], hub, "forgotten", &n) == DEMO_HEARD_OTHER);
    CHECK(let_in(late, hub) == DEMO_HEARD_PAIRED);
    CHECK(demo_peer(&hub->ram, late->ram.id.address) == 2);
    CHECK(say(late, hub, "in", &n) == DEMO_HEARD_MESSAGE && n == 0);
    CHECK(boot(hub));
    CHECK_EQ_U64(demo_peers(&hub->ram), DEMO_PEERS);
    CHECK(say(peers[2], hub, "forgotten", &n) == DEMO_HEARD_OTHER);
    CHECK(say(late, hub, "in", &n) == DEMO_HEARD_MESSAGE && n == 1);
}

/* Forgotten in RAM and not in flash, a session would come back at the next restart. */
static void a_session_is_forgotten_only_if_the_store_forgets_it(void) {
    struct board *a = new_board(), *b = new_board();
    uint32_t n;
    CHECK(contact(a, b, 0) == DEMO_HEARD_PAIRED);
    b->flash.broken = true;
    CHECK(!demo_forget(&b->ram, 0));
    b->flash.broken = false;
    CHECK_EQ_U64(demo_peers(&b->ram), 1);
    CHECK(say(a, b, "kept", &n) == DEMO_HEARD_MESSAGE && n == 0);
    CHECK(demo_forget(&b->ram, 0));
    CHECK(b->ram.last == -1);
    CHECK(boot(b));
    CHECK_EQ_U64(demo_peers(&b->ram), 0);
    CHECK(say(a, b, "gone", &n) == DEMO_HEARD_OTHER);
}

/* A board updated from a build that held one session finds it, in the first slot. */
static void the_one_session_of_an_earlier_build_is_kept(void) {
    struct board *a = new_board(), *b = new_board();
    CHECK(contact(a, b, 0) == DEMO_HEARD_PAIRED);
    CHECK(find(&a->flash, "session") >= 0 && find(&a->flash, "session1") < 0);
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

/* A message for the node itself, such as a group's invite, is not this board's to take yet. It
 * is not opened, so nothing is shown as words, nothing answers it, and its counter is not spent:
 * the board that sent it finds out, and a board that learns to take such messages still can. */
static void a_message_for_the_node_is_not_taken(void) {
    struct board *a = new_board(), *b = new_board();
    uint8_t frame[TERN_UNICAST_MAX_FRAME], msg[TERN_UNICAST_MAX_PLAINTEXT];
    struct demo_received r;
    CHECK(contact(a, b, 0) == DEMO_HEARD_PAIRED);
    CHECK(demo_seal(&a->ram, 0, (const uint8_t *)"hi", 2, frame) == DEMO_OK);
    frame[0] = TERN_UNICAST_HDR_NODE;
    CHECK(demo_receive(&b->ram, 0, frame, 25, msg, &r) == DEMO_HEARD_MALFORMED);
    CHECK(r.acks == 0 && r.msg_len == 0);
    frame[0] = TERN_UNICAST_HDR;
    CHECK(demo_receive(&b->ram, 0, frame, 25, msg, &r) == DEMO_HEARD_MESSAGE);
    CHECK(r.counter == 0);
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
    RUN(the_board_that_began_may_begin_again);
    RUN(stranger_is_refused_until_accepted);
    RUN(a_trusted_stranger_is_accepted);
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
    RUN(a_board_holds_a_session_with_each_peer);
    RUN(contact_again_replaces_only_that_peers_session);
    RUN(a_full_board_takes_no_new_peer_until_one_is_forgotten);
    RUN(a_session_is_forgotten_only_if_the_store_forgets_it);
    RUN(the_one_session_of_an_earlier_build_is_kept);
    RUN(frames_that_are_not_terns_are_malformed);
    RUN(a_message_for_the_node_is_not_taken);
    return CHECK_DONE();
}
