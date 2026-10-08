#include "link.h"

#include <string.h>

#include "check.h"

/* The Heltec V3 port's half of the companion link (ports/heltec-v3/main/link.c), on a host with a
 * fake board: what it answers, the news it sends, and the specification's exchange, a whole
 * connection, frame for frame. */

#define BYTES 192

struct step {
    bool from_client;
    uint8_t frame[BYTES];
    size_t len;
};

/* The specification's exchange, alone: the build writes it from the same JSON as tests/companion.c
 * reads. */
#include "companion_exchange.h"

#define COUNT(a) (sizeof(a) / sizeof(a)[0])

/* The public keys of RFC 8032's first two Ed25519 test vectors, as the vectors use them. */
static const uint8_t alice[TERN_ADDRESS_LEN] = {
    0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe, 0xd3, 0xc9, 0x64, 0x07, 0x3a,
    0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a};
static const uint8_t bob[TERN_ADDRESS_LEN] = {
    0x3d, 0x40, 0x17, 0xc3, 0xe8, 0x43, 0x89, 0x5a, 0x92, 0xb7, 0x0a, 0xa7, 0x4d, 0x1b, 0x7e, 0xbc,
    0x9c, 0x98, 0x2c, 0xcf, 0x2e, 0xc4, 0x96, 0x8c, 0xc0, 0xcd, 0x55, 0xf1, 0x2a, 0xf4, 0x66, 0x0c};

/* The fake board: what it shows the link, and every frame the link sent. */
struct board {
    struct link_view view;
    uint32_t clock;
    bool session_with_bob;
    uint8_t why;
    uint8_t set_answer;
    bool save_fails;
    uint8_t saved[sizeof(struct link_contact) * LINK_CONTACTS];
    bool have_saved;
    uint32_t ids; /* the message ids set aside, 0 if none were */
    unsigned id_saves;
    unsigned id_saves_at_queued; /* how many there had been when QUEUED was last sent */
    uint8_t out[64][TERN_COMPANION_MAX_FRAME];
    size_t out_len[64];
    unsigned out_conn[64];
    size_t n_out;
};

static void board_out(void *ctx, unsigned conn, const uint8_t *frame, size_t len) {
    struct board *b = ctx;
    if (len > 0 && frame[0] == TERN_C_QUEUED) {
        b->id_saves_at_queued = b->id_saves;
    }
    if (b->n_out < COUNT(b->out)) {
        memcpy(b->out[b->n_out], frame, len);
        b->out_conn[b->n_out] = conn;
        b->out_len[b->n_out++] = len;
    }
}

static void board_view(void *ctx, struct link_view *v) {
    struct board *b = ctx;
    *v = b->view;
    v->time = b->clock;
}

static uint8_t board_set(void *ctx, const struct tern_companion_msg *m) {
    struct board *b = ctx;
    if (b->set_answer == 0 && m->setting == TERN_C_SET_POWER) {
        b->view.power = m->power;
    }
    return b->set_answer;
}

static void board_set_time(void *ctx, uint32_t time) { ((struct board *)ctx)->clock = time; }

static bool board_session(void *ctx, const uint8_t address[TERN_ADDRESS_LEN]) {
    struct board *b = ctx;
    return b->session_with_bob && memcmp(address, bob, TERN_ADDRESS_LEN) == 0;
}

static uint8_t board_why(void *ctx, const uint8_t address[TERN_ADDRESS_LEN]) {
    (void)address;
    return ((struct board *)ctx)->why;
}

static bool board_load(void *ctx, void *buf, size_t len) {
    struct board *b = ctx;
    if (!b->have_saved || len != sizeof b->saved) {
        return false;
    }
    memcpy(buf, b->saved, len);
    return true;
}

static bool board_save(void *ctx, const void *buf, size_t len) {
    struct board *b = ctx;
    if (b->save_fails || len != sizeof b->saved) {
        return false;
    }
    memcpy(b->saved, buf, len);
    b->have_saved = true;
    return true;
}

static bool board_load_ids(void *ctx, uint32_t *next) {
    struct board *b = ctx;
    *next = b->ids;
    return b->ids != 0;
}

static bool board_save_ids(void *ctx, uint32_t next) {
    struct board *b = ctx;
    if (b->save_fails) {
        return false;
    }
    b->ids = next;
    b->id_saves++;
    return true;
}

static struct board board;
static struct link companion;

/* The node the exchange shows: Alice, a relay in EU868 at 14 dBm, who knows Bob, has a message
 * from him, and hears one neighbour. */
static void start(void) {
    memset(&board, 0, sizeof board);
    memcpy(board.view.address, alice, TERN_ADDRESS_LEN);
    board.view.role = 1;
    board.view.region = "EU868";
    board.view.power = 14;
    board.view.n_neighbours = 1;
    board.view.neighbours[0] = (struct link_neighbour){0x1D2E3F40, 1, -38, 42};
    board.view.period_s = 3600;
    board.view.allowed_ms = 360000;
    board.view.used_ms = 12345;
    board.view.millivolts = 3987;
    board.view.percent = 81;
    board.view.power_flags = 1;
    board.session_with_bob = true;
    board.why = TERN_C_WAIT_ROUTE;
    struct link_host host = {
        .ctx = &board,
        .firmware = "tern 0.1.0 heltec-v3",
        .out = board_out,
        .view = board_view,
        .set = board_set,
        .set_time = board_set_time,
        .session = board_session,
        .why = board_why,
        .load = board_load,
        .save = board_save,
        .load_ids = board_load_ids,
        .save_ids = board_save_ids,
    };
    link_init(&companion, &host);
    link_open(&companion, LINK_SERIAL, 0, 0);
}

/* The board restarts: the link begins again with what the board kept. */
static void restart(void) {
    struct link_host host = companion.host;
    link_init(&companion, &host);
    link_open(&companion, LINK_SERIAL, 0, 0);
}

static uint32_t add(void) {
    return link_add(&companion, bob, 1789999000, TERN_C_RECEIVED, 0, (const uint8_t *)"x", 1);
}

static void request_at(tern_time now, const struct tern_companion_msg *q) {
    uint8_t frame[TERN_COMPANION_MAX_FRAME];
    size_t len = tern_companion_write(q, frame);
    board.n_out = 0;
    link_receive(&companion, LINK_SERIAL, now, frame, len);
}

static void request(const struct tern_companion_msg *q) { request_at(TERN_S(100), q); }

/* The i-th frame the link sent, read. */
static struct tern_companion_msg sent(size_t i) {
    struct tern_companion_msg m = {0};
    if (i >= board.n_out || tern_companion_read(&m, board.out[i], board.out_len[i]) != 0) {
        m.type = 0;
    }
    return m;
}

static void hello(void) { request(&(struct tern_companion_msg){.type = TERN_C_HELLO, .seq = 1}); }

static void the_exchange_is_followed_frame_for_frame(void) {
    start();
    companion.contacts[0] = (struct link_contact){.used = true, .name_len = 3, .name = "Bob"};
    memcpy(companion.contacts[0].address, bob, TERN_ADDRESS_LEN);
    companion.next_id = 17;
    static const char where[] = "Where are you?";
    CHECK_EQ_U64(link_add(&companion, bob, 1789999000, TERN_C_RECEIVED, 0, (const uint8_t *)where,
                          sizeof where - 1),
                 17);

    size_t expected = 0;
    board.n_out = 0;
    for (size_t i = 0; i < COUNT(exchange); i++) {
        const struct step *s = &exchange[i];
        if (s->from_client) {
            /* Every node frame before this client's must already have been sent. */
            CHECK_EQ_U64(board.n_out, expected);
            if (s->frame[0] == TERN_C_SEND) {
                board.clock += 60;
            }
            link_receive(&companion, LINK_SERIAL, TERN_S(100), s->frame, s->len);
            continue;
        }
        if (s->frame[0] == TERN_C_STATE && expected == board.n_out) {
            /* What became of the message is the board's to say, and the exchange says it. */
            struct tern_companion_msg m = {0};
            CHECK_EQ_I64(tern_companion_read(&m, s->frame, s->len), 0);
            link_state(&companion, m.id, m.state, m.reason, (uint16_t)m.wait);
        }
        bool ok = expected < board.n_out && board.out_len[expected] == s->len &&
                  memcmp(board.out[expected], s->frame, s->len) == 0;
        if (!ok) {
            fprintf(stderr, "exchange: step %zu differs\n", i);
        }
        CHECK(ok);
        expected++;
    }
    CHECK_EQ_U64(board.n_out, expected);
}

static void nothing_but_hello_before_hello(void) {
    start();
    request(&(struct tern_companion_msg){.type = TERN_C_PING, .seq = 9});
    CHECK_EQ_U64(board.n_out, 1);
    CHECK_EQ_I64(sent(0).type, TERN_C_ERROR);
    CHECK_EQ_I64(sent(0).code, TERN_C_ERR_HELLO_FIRST);
    CHECK_EQ_I64(sent(0).seq, 9);
    /* No news either: a message arriving is kept, and not sent to a terminal that never asked. */
    board.n_out = 0;
    CHECK(link_add(&companion, bob, 5, TERN_C_RECEIVED, 0, (const uint8_t *)"hi", 2) != 0);
    CHECK_EQ_U64(board.n_out, 0);
    hello();
    CHECK_EQ_I64(sent(0).type, TERN_C_INFO);
    CHECK_EQ_I64(sent(0).version, TERN_COMPANION_VERSION);
}

static void requests_it_cannot_read_are_answered(void) {
    start();
    static const uint8_t cut[] = {TERN_C_SYNC, 4, 0, 0};
    static const uint8_t unknown[] = {0x20, 5};
    static const uint8_t news_type[] = {TERN_C_SELF, 6};
    static const uint8_t short_frame[] = {TERN_C_PING};
    board.n_out = 0;
    link_receive(&companion, LINK_SERIAL, 0, cut, sizeof cut);
    link_receive(&companion, LINK_SERIAL, 0, unknown, sizeof unknown);
    link_receive(&companion, LINK_SERIAL, 0, news_type, sizeof news_type);
    link_receive(&companion, LINK_SERIAL, 0, short_frame, sizeof short_frame);
    CHECK_EQ_U64(board.n_out, 2);
    CHECK_EQ_I64(sent(0).code, TERN_C_ERR_MALFORMED);
    CHECK_EQ_I64(sent(0).seq, 4);
    CHECK_EQ_I64(sent(1).code, TERN_C_ERR_UNKNOWN);
    CHECK_EQ_I64(sent(1).seq, 5);
}

static struct tern_companion_msg send_to(const uint8_t to[TERN_ADDRESS_LEN], uint32_t ref,
                                         const char *text) {
    struct tern_companion_msg q = {.type = TERN_C_SEND, .seq = 7, .ref = ref};
    memcpy(q.address, to, TERN_ADDRESS_LEN);
    q.text_len = (uint8_t)strlen(text);
    memcpy(q.text, text, q.text_len);
    return q;
}

static void a_send_sent_again_is_one_message(void) {
    start();
    hello();
    struct tern_companion_msg q = send_to(bob, 0xC0FFEE01, "On the ridge by six");
    request(&q);
    CHECK_EQ_I64(sent(0).type, TERN_C_QUEUED);
    uint32_t id = sent(0).id;
    CHECK_EQ_I64(sent(1).type, TERN_C_MESSAGE);
    CHECK_EQ_I64(sent(1).id, id);
    CHECK_EQ_I64(sent(1).reason, TERN_C_WAIT_ROUTE);

    /* The answer was lost, and the client sends it again: the same message. */
    request(&q);
    CHECK_EQ_U64(board.n_out, 1);
    CHECK_EQ_I64(sent(0).type, TERN_C_QUEUED);
    CHECK_EQ_I64(sent(0).id, id);

    /* Another client with the same ref, and other words: another message. */
    struct tern_companion_msg other = send_to(bob, 0xC0FFEE01, "Running late");
    request(&other);
    CHECK_EQ_I64(sent(0).type, TERN_C_QUEUED);
    CHECK(sent(0).id != id);
    CHECK_EQ_I64(sent(1).type, TERN_C_MESSAGE);

    CHECK(link_outgoing(&companion) != NULL && link_outgoing(&companion)->id == id);
}

static void a_send_it_cannot_take_is_refused(void) {
    start();
    hello();
    struct tern_companion_msg q = send_to(alice, 1, "to myself");
    request(&q);
    CHECK_EQ_I64(sent(0).code, TERN_C_ERR_ADDRESS);
    uint8_t not_a_point[TERN_ADDRESS_LEN];
    memset(not_a_point, 0xFF, sizeof not_a_point);
    q = send_to(not_a_point, 1, "anyone?");
    request(&q);
    CHECK_EQ_I64(sent(0).code, TERN_C_ERR_ADDRESS);
    q = send_to(bob, 1, "");
    request(&q);
    CHECK_EQ_I64(sent(0).code, TERN_C_ERR_REFUSED);

    /* Room for LINK_MESSAGES waiting to go, and then no more. */
    for (uint32_t i = 0; i < LINK_MESSAGES; i++) {
        q = send_to(bob, 100 + i, "queued");
        request(&q);
        CHECK_EQ_I64(sent(0).type, TERN_C_QUEUED);
    }
    q = send_to(bob, 999, "one too many");
    request(&q);
    CHECK_EQ_I64(sent(0).code, TERN_C_ERR_FULL);
}

/* A message the forwarder has keeps its place, however many come after, until its end is known:
 * otherwise its delivery could not be told. */
static void a_message_on_its_way_is_not_overwritten(void) {
    start();
    hello();
    struct tern_companion_msg q = send_to(bob, 1, "on its way");
    request(&q);
    uint32_t id = sent(0).id;
    link_taken(&companion, id);
    for (int i = 0; i < 2 * LINK_MESSAGES; i++) {
        (void)link_add(&companion, bob, 0, TERN_C_RECEIVED, 0, (const uint8_t *)"in", 2);
    }
    board.n_out = 0;
    link_state(&companion, id, TERN_C_DELIVERED, 0, 0);
    CHECK_EQ_I64(sent(0).type, TERN_C_STATE);
    CHECK_EQ_I64(sent(0).id, id);
    CHECK_EQ_I64(sent(0).state, TERN_C_DELIVERED);
    /* Its end known, it gives way like any other. */
    for (int i = 0; i < LINK_MESSAGES; i++) {
        (void)link_add(&companion, bob, 0, TERN_C_RECEIVED, 0, (const uint8_t *)"in", 2);
    }
    board.n_out = 0;
    link_state(&companion, id, TERN_C_NOT_DELIVERED, 0, 0);
    CHECK_EQ_U64(board.n_out, 0);
}

static void a_message_handed_over_is_not_claimed_sent(void) {
    start();
    hello();
    struct tern_companion_msg q = send_to(bob, 1, "hello");
    request(&q);
    uint32_t id = sent(0).id;
    board.n_out = 0;
    link_taken(&companion, id);
    CHECK_EQ_I64(sent(0).type, TERN_C_STATE);
    CHECK_EQ_I64(sent(0).state, TERN_C_WAITING);
    CHECK_EQ_I64(sent(0).reason, TERN_C_WAIT_UNNAMED);
    CHECK(link_outgoing(&companion) == NULL);

    /* A state unchanged is not news. */
    board.n_out = 0;
    link_state(&companion, id, TERN_C_WAITING, TERN_C_WAIT_UNNAMED, 3);
    CHECK_EQ_U64(board.n_out, 0);
}

static void first_contact_failing_gives_up_what_waited_for_it(void) {
    start();
    hello();
    board.why = TERN_C_WAIT_SESSION;
    struct tern_companion_msg q = send_to(bob, 1, "hello");
    request(&q);
    uint32_t id = sent(0).id;
    board.n_out = 0;
    link_unreachable(&companion, bob);
    CHECK_EQ_I64(sent(0).type, TERN_C_STATE);
    CHECK_EQ_I64(sent(0).id, id);
    CHECK_EQ_I64(sent(0).state, TERN_C_NOT_DELIVERED);
    CHECK(link_outgoing(&companion) == NULL);
}

static void reading_marks_received_messages_read(void) {
    start();
    hello();
    uint32_t a = link_add(&companion, bob, 1, TERN_C_RECEIVED, 0, (const uint8_t *)"one", 3);
    uint32_t b = link_add(&companion, bob, 2, TERN_C_RECEIVED, 0, (const uint8_t *)"two", 3);
    request(&(struct tern_companion_msg){.type = TERN_C_READ, .seq = 3, .through = a});
    CHECK_EQ_U64(board.n_out, 2);
    CHECK_EQ_I64(sent(0).type, TERN_C_OK);
    CHECK_EQ_I64(sent(1).id, a);
    CHECK_EQ_I64(sent(1).flags, TERN_C_READ_FLAG);
    request(&(struct tern_companion_msg){.type = TERN_C_READ, .seq = 4, .through = b});
    CHECK_EQ_U64(board.n_out, 2); /* the first is read already: no news of it again */
    CHECK_EQ_I64(sent(1).id, b);
}

static void received_text_is_cut_to_what_a_message_carries(void) {
    start();
    hello();
    uint8_t text[200];
    memset(text, 'x', sizeof text);
    text[127] = 0xE2; /* a three-byte character that the cut would split */
    text[128] = 0x82;
    text[129] = 0xAC;
    board.n_out = 0;
    CHECK(link_add(&companion, bob, 1, TERN_C_RECEIVED, 0, text, sizeof text) != 0);
    CHECK_EQ_I64(sent(0).text_len, 127);
    static const uint8_t junk[] = {0xFF, 0xFE};
    board.n_out = 0;
    CHECK_EQ_U64(link_add(&companion, bob, 1, TERN_C_RECEIVED, 0, junk, sizeof junk), 0);
    CHECK_EQ_U64(board.n_out, 0);
}

static void contacts_are_saved_renamed_and_removed(void) {
    start();
    hello();
    struct tern_companion_msg q = {.type = TERN_C_SAVE_CONTACT, .seq = 2, .text_len = 3};
    memcpy(q.address, bob, TERN_ADDRESS_LEN);
    memcpy(q.text, "Bob", 3);
    request(&q);
    CHECK_EQ_I64(sent(0).type, TERN_C_OK);
    CHECK_EQ_I64(sent(1).type, TERN_C_CONTACT);
    CHECK_EQ_I64(sent(1).session, 1);
    CHECK(board.have_saved);

    /* Renamed: still one contact. */
    q.text_len = 6;
    memcpy(q.text, "Robert", 6);
    request(&q);
    CHECK_EQ_I64(sent(1).text_len, 6);
    size_t n = 0;
    for (size_t i = 0; i < LINK_CONTACTS; i++) {
        n += companion.contacts[i].used;
    }
    CHECK_EQ_U64(n, 1);

    /* After a restart, it is still there. */
    struct link_host host = companion.host;
    link_init(&companion, &host);
    link_open(&companion, LINK_SERIAL, 0, 0);
    CHECK(companion.contacts[0].used && companion.contacts[0].name_len == 6);

    /* Saved nowhere, removed nowhere. */
    hello();
    board.save_fails = true;
    q.type = TERN_C_REMOVE_CONTACT;
    request(&q);
    CHECK_EQ_I64(sent(0).code, TERN_C_ERR_NOT_NOW);
    CHECK(companion.contacts[0].used);
    board.save_fails = false;
    request(&q);
    CHECK_EQ_I64(sent(0).type, TERN_C_OK);
    CHECK_EQ_I64(sent(1).type, TERN_C_CONTACT_GONE);
    /* Removing what is not a contact is not an error, and not news. */
    request(&q);
    CHECK_EQ_U64(board.n_out, 1);
    CHECK_EQ_I64(sent(0).type, TERN_C_OK);

    /* The node's own address is not a contact. */
    q.type = TERN_C_SAVE_CONTACT;
    memcpy(q.address, alice, TERN_ADDRESS_LEN);
    request(&q);
    CHECK_EQ_I64(sent(0).code, TERN_C_ERR_ADDRESS);
}

static void settings_are_the_boards_to_refuse(void) {
    start();
    hello();
    request(&(struct tern_companion_msg){.type = TERN_C_SYNC, .seq = 2});
    board.set_answer = TERN_C_ERR_REFUSED;
    request(&(struct tern_companion_msg){
        .type = TERN_C_SET, .seq = 5, .setting = TERN_C_SET_POWER, .power = 30});
    CHECK_EQ_I64(sent(0).code, TERN_C_ERR_REFUSED);
    board.set_answer = 0;
    request(&(struct tern_companion_msg){
        .type = TERN_C_SET, .seq = 6, .setting = TERN_C_SET_POWER, .power = 10});
    CHECK_EQ_I64(sent(0).type, TERN_C_OK);
    /* The node changed: SELF says so at the next look. */
    board.n_out = 0;
    link_tick(&companion, TERN_S(200));
    CHECK_EQ_I64(sent(0).type, TERN_C_SELF);
    CHECK_EQ_I64(sent(0).power, 10);
}

static void neighbours_are_news_when_they_change(void) {
    start();
    hello();
    request(&(struct tern_companion_msg){.type = TERN_C_SYNC, .seq = 2});
    tern_time t = TERN_S(100);

    /* A little change, or none, is not news. */
    board.view.neighbours[0].snr = -36;
    board.view.neighbours[0].heard = 3;
    board.n_out = 0;
    link_tick(&companion, t += TERN_S(20));
    CHECK_EQ_U64(board.n_out, 0);

    /* A new neighbour is news at once; one changed enough, once LINK_QUIET has passed. */
    board.view.n_neighbours = 2;
    board.view.neighbours[1] = (struct link_neighbour){0x01020304, 0, 10, 1};
    board.view.neighbours[0].snr = -20;
    link_tick(&companion, t += TERN_S(1));
    CHECK_EQ_U64(board.n_out, 2);
    CHECK_EQ_I64(sent(0).routing_id, 0x1D2E3F40);
    CHECK_EQ_I64(sent(1).routing_id, 0x01020304);
    board.view.neighbours[1].snr = 40;
    board.n_out = 0;
    link_tick(&companion, t += TERN_S(1));
    CHECK_EQ_U64(board.n_out, 0);
    link_tick(&companion, t += LINK_QUIET);
    CHECK_EQ_U64(board.n_out, 1);
    CHECK_EQ_I64(sent(0).snr, 40);

    /* Forgotten: gone. */
    board.view.n_neighbours = 1;
    board.n_out = 0;
    link_tick(&companion, t += TERN_S(1));
    CHECK_EQ_U64(board.n_out, 1);
    CHECK_EQ_I64(sent(0).type, TERN_C_NEIGHBOUR_GONE);
    CHECK_EQ_I64(sent(0).routing_id, 0x01020304);
}

static void the_air_is_news_no_more_than_every_quiet(void) {
    start();
    hello();
    request(&(struct tern_companion_msg){.type = TERN_C_SYNC, .seq = 2});
    board.view.used_ms += 70;
    board.n_out = 0;
    link_tick(&companion, TERN_S(100) + TERN_S(2));
    CHECK_EQ_U64(board.n_out, 0);
    link_tick(&companion, TERN_S(100) + LINK_QUIET);
    CHECK_EQ_U64(board.n_out, 1);
    CHECK_EQ_I64(sent(0).type, TERN_C_AIRTIME);
    CHECK_EQ_I64(sent(0).used, 12415);
}

/* Over USB serial the board cannot see a client close the port, so one that asks nothing for
 * LINK_LAPSE, counted from the answer to its last request, is gone (draft/companion.md, "Going
 * quiet"). */
static void a_silent_serial_client_is_taken_for_gone(void) {
    start();
    link_open(&companion, LINK_SERIAL, LINK_LAPSE, 0);
    tern_time t = TERN_S(100);
    request_at(t, &(struct tern_companion_msg){.type = TERN_C_HELLO, .seq = 1});
    request_at(t += TERN_S(10), &(struct tern_companion_msg){.type = TERN_C_SYNC, .seq = 2});

    /* A PING inside the lapse keeps the connection, and the lapse counts from its answer. */
    request_at(t += LINK_LAPSE - TERN_S(1),
               &(struct tern_companion_msg){.type = TERN_C_PING, .seq = 3});
    CHECK_EQ_I64(sent(0).type, TERN_C_OK);
    link_tick(&companion, t + LINK_LAPSE - TERN_S(1));
    CHECK(companion.conns[LINK_SERIAL].hello);

    /* Silent for the lapse: no more news, and nothing answered but HELLO. */
    link_tick(&companion, t += LINK_LAPSE);
    CHECK(!companion.conns[LINK_SERIAL].hello);
    board.n_out = 0;
    link_add(&companion, bob, 1, TERN_C_RECEIVED, 0, (const uint8_t *)"hi", 2);
    CHECK_EQ_U64(board.n_out, 0);
    request_at(t += TERN_S(1), &(struct tern_companion_msg){.type = TERN_C_PING, .seq = 4});
    CHECK_EQ_I64(sent(0).type, TERN_C_ERROR);
    CHECK_EQ_I64(sent(0).code, TERN_C_ERR_HELLO_FIRST);

    /* A client that starts again is a client again, its news counted from 0. Until it syncs,
     * nothing is told twice: the sync tells it all. */
    request_at(t += TERN_S(1), &(struct tern_companion_msg){.type = TERN_C_HELLO, .seq = 5});
    CHECK_EQ_I64(sent(0).type, TERN_C_INFO);
    board.n_out = 0;
    link_tick(&companion, t += LINK_LOOK);
    CHECK_EQ_U64(board.n_out, 0);
    request_at(t += TERN_S(1), &(struct tern_companion_msg){.type = TERN_C_SYNC, .seq = 6});
    CHECK_EQ_I64(sent(0).type, TERN_C_SELF);
    CHECK_EQ_I64(sent(0).seq, 0);

    /* A request late enough is refused even before link_tick has looked. */
    request_at(t += LINK_LAPSE, &(struct tern_companion_msg){.type = TERN_C_PING, .seq = 7});
    CHECK_EQ_I64(sent(0).code, TERN_C_ERR_HELLO_FIRST);
}

static void request_on(unsigned conn, const struct tern_companion_msg *q) {
    uint8_t frame[TERN_COMPANION_MAX_FRAME];
    size_t len = tern_companion_write(q, frame);
    board.n_out = 0;
    link_receive(&companion, conn, TERN_S(100), frame, len);
}

/* Each frame sent since the last request went on `conn`. */
static bool all_on(unsigned conn) {
    for (size_t i = 0; i < board.n_out; i++) {
        if (board.out_conn[i] != conn) {
            return false;
        }
    }
    return board.n_out > 0;
}

/* A client on USB and one over Bluetooth: each is answered alone and counts its own news, a sync
 * is told to the one that asked, and news of a change goes to both. */
static void two_clients_drive_one_node(void) {
    start();
    link_open(&companion, LINK_BLE, 0, 23);

    /* Bluetooth's default MTU is too small for a frame: HELLO is refused until it grows. */
    request_on(LINK_BLE, &(struct tern_companion_msg){.type = TERN_C_HELLO, .seq = 1});
    CHECK(all_on(LINK_BLE));
    CHECK_EQ_I64(sent(0).code, TERN_C_ERR_MTU);
    link_mtu(&companion, LINK_BLE, 185);
    request_on(LINK_BLE, &(struct tern_companion_msg){.type = TERN_C_HELLO, .seq = 2});
    CHECK(all_on(LINK_BLE));
    CHECK_EQ_I64(sent(0).type, TERN_C_INFO);

    request_on(LINK_SERIAL, &(struct tern_companion_msg){.type = TERN_C_HELLO, .seq = 1});
    request_on(LINK_SERIAL, &(struct tern_companion_msg){.type = TERN_C_SYNC, .seq = 2});
    CHECK(all_on(LINK_SERIAL));
    size_t told = board.n_out - 1; /* the news before SYNCED */

    /* A message received is news to both, each numbered by its own count. */
    board.n_out = 0;
    link_add(&companion, bob, 1, TERN_C_RECEIVED, 0, (const uint8_t *)"hi", 2);
    CHECK_EQ_U64(board.n_out, 2);
    CHECK_EQ_U64(board.out_conn[0], LINK_SERIAL);
    CHECK_EQ_I64(sent(0).seq, (int64_t)told);
    CHECK_EQ_U64(board.out_conn[1], LINK_BLE);
    CHECK_EQ_I64(sent(1).seq, 0);

    /* Only a synced client hears what changed on the board. */
    board.view.used_ms += 1000;
    board.n_out = 0;
    link_tick(&companion, TERN_S(100) + LINK_QUIET);
    CHECK(all_on(LINK_SERIAL));

    /* Gone: nothing more goes to it. */
    link_close(&companion, LINK_BLE);
    board.n_out = 0;
    link_add(&companion, bob, 2, TERN_C_RECEIVED, 0, (const uint8_t *)"yo", 2);
    CHECK(all_on(LINK_SERIAL));
    CHECK_EQ_U64(board.n_out, 1);
    request_on(LINK_BLE, &(struct tern_companion_msg){.type = TERN_C_PING, .seq = 3});
    CHECK_EQ_U64(board.n_out, 0);
}

static void the_oldest_finished_message_makes_room(void) {
    start();
    hello();
    uint32_t first = 0;
    for (int i = 0; i < LINK_MESSAGES; i++) {
        uint32_t id = link_add(&companion, bob, 1, TERN_C_RECEIVED, 0, (const uint8_t *)"x", 1);
        first = first == 0 ? id : first;
    }
    uint32_t next = link_add(&companion, bob, 1, TERN_C_RECEIVED, 0, (const uint8_t *)"y", 1);
    CHECK(next != 0);
    board.n_out = 0;
    request(&(struct tern_companion_msg){.type = TERN_C_SYNC, .seq = 2, .after = 0});
    /* SELF, then the messages: the first is gone, and the newest is there. */
    CHECK_EQ_I64(sent(1).type, TERN_C_MESSAGE);
    CHECK_EQ_I64(sent(1).id, first + 1);
    CHECK_EQ_I64(sent(LINK_MESSAGES).id, next);
}

static void a_restart_gives_no_id_again(void) {
    start();
    CHECK_EQ_U64(add(), 1);
    CHECK_EQ_U64(add(), 2);
    CHECK_EQ_U64(board.id_saves, 1);

    /* The ids set aside and not used are skipped. */
    restart();
    CHECK_EQ_U64(add(), 1 + LINK_ID_STEP);
    CHECK_EQ_U64(board.id_saves, 2);

    /* One write sets aside LINK_ID_STEP of them, and the next is when they run out. */
    uint32_t last = 0;
    for (unsigned i = 1; i < LINK_ID_STEP; i++) {
        last = add();
    }
    CHECK_EQ_U64(last, 2 * LINK_ID_STEP);
    CHECK_EQ_U64(board.id_saves, 2);
    CHECK_EQ_U64(add(), 2 * LINK_ID_STEP + 1);
    CHECK_EQ_U64(board.id_saves, 3);

    /* A restart with no message since costs no write, and none of the ids. */
    restart();
    restart();
    CHECK_EQ_U64(board.id_saves, 3);
    CHECK_EQ_U64(add(), 3 * LINK_ID_STEP + 1);

    /* A write that fails does not stop the message, and is tried again with the next. */
    restart();
    board.save_fails = true;
    uint32_t a = add();
    CHECK_EQ_U64(a, 4 * LINK_ID_STEP + 1);
    board.save_fails = false;
    CHECK_EQ_U64(add(), a + 1);
    restart();
    CHECK_EQ_U64(add(), a + 1 + LINK_ID_STEP);

    /* An id a client is told in QUEUED is set aside before it is told: a restart just after the
     * answer does not give it to another message. */
    start();
    hello();
    struct tern_companion_msg q = send_to(bob, 0xC0FFEE02, "On the ridge by six");
    request(&q);
    CHECK_EQ_I64(sent(0).type, TERN_C_QUEUED);
    CHECK_EQ_U64(board.id_saves_at_queued, 1);
    CHECK_EQ_U64(board.id_saves, 1);
    restart();
    CHECK(add() > sent(0).id);
}

int main(void) {
    RUN(the_exchange_is_followed_frame_for_frame);
    RUN(nothing_but_hello_before_hello);
    RUN(requests_it_cannot_read_are_answered);
    RUN(a_send_sent_again_is_one_message);
    RUN(a_send_it_cannot_take_is_refused);
    RUN(a_message_on_its_way_is_not_overwritten);
    RUN(a_message_handed_over_is_not_claimed_sent);
    RUN(first_contact_failing_gives_up_what_waited_for_it);
    RUN(reading_marks_received_messages_read);
    RUN(received_text_is_cut_to_what_a_message_carries);
    RUN(contacts_are_saved_renamed_and_removed);
    RUN(settings_are_the_boards_to_refuse);
    RUN(neighbours_are_news_when_they_change);
    RUN(the_air_is_news_no_more_than_every_quiet);
    RUN(the_oldest_finished_message_makes_room);
    RUN(a_silent_serial_client_is_taken_for_gone);
    RUN(two_clients_drive_one_node);
    RUN(a_restart_gives_no_id_again);
    return CHECK_DONE();
}
