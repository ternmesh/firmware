#include "tern/companion.h"

#include <string.h>

#include "check.h"

/* The companion protocol's frames and byte streams (draft/companion.md), against the
 * specification's vectors. What a node answers is the port's, and is tested with it
 * (tests/link.c). */

#define BYTES 192

struct frame_case {
    struct tern_companion_msg msg;
    uint8_t frame[BYTES];
    size_t frame_len;
    uint8_t stream[BYTES];
    size_t stream_len;
};
struct extended_case {
    struct tern_companion_msg msg;
    uint8_t frame[BYTES];
    size_t frame_len;
};
struct rejected_case {
    const char *why;
    uint8_t frame[BYTES];
    size_t len;
    int answer; /* the ERROR code a node answers with, or -1 for none */
};
struct group_id_case {
    uint8_t secret[16];
    uint8_t id[TERN_COMPANION_GROUP];
};
struct item {
    bool frame; /* else a run of text */
    uint8_t bytes[BYTES];
    size_t len;
};
struct stream_case {
    const char *why;
    uint8_t stream[BYTES];
    size_t len;
    const struct item *items;
    size_t n_items;
    uint8_t pending[BYTES];
    size_t pending_len;
};
struct step {
    bool from_client;
    uint8_t frame[BYTES];
    size_t len;
};

#include "companion.h"

#define COUNT(a) (sizeof(a) / sizeof(a)[0])

static void crc_matches_the_published_check_value(void) {
    CHECK_EQ_U64(tern_companion_crc(crc_input, sizeof crc_input), crc_expected);
}

static void frames_are_written_and_read(void) {
    for (size_t i = 0; i < COUNT(frames); i++) {
        const struct frame_case *c = &frames[i];
        uint8_t out[TERN_COMPANION_MAX_FRAME], again[TERN_COMPANION_MAX_FRAME];
        uint8_t wrapped[TERN_COMPANION_STREAM_MAX];
        CHECK_EQ_U64(tern_companion_write(&c->msg, out), c->frame_len);
        CHECK(memcmp(out, c->frame, c->frame_len) == 0);

        struct tern_companion_msg m = {0};
        CHECK_EQ_I64(tern_companion_read(&m, c->frame, c->frame_len), TERN_C_READ_OK);
        CHECK_EQ_U64(tern_companion_write(&m, again), c->frame_len);
        CHECK(memcmp(again, c->frame, c->frame_len) == 0);

        CHECK_EQ_U64(tern_companion_wrap(c->frame, c->frame_len, wrapped), c->stream_len);
        CHECK(memcmp(wrapped, c->stream, c->stream_len) == 0);
    }
}

static void bytes_past_the_fields_are_ignored(void) {
    for (size_t i = 0; i < COUNT(extended); i++) {
        const struct extended_case *c = &extended[i];
        struct tern_companion_msg m = {0};
        uint8_t got[TERN_COMPANION_MAX_FRAME], want[TERN_COMPANION_MAX_FRAME];
        CHECK_EQ_I64(tern_companion_read(&m, c->frame, c->frame_len), TERN_C_READ_OK);
        size_t n = tern_companion_write(&m, got);
        CHECK(n != 0 && n < c->frame_len);
        CHECK_EQ_U64(tern_companion_write(&c->msg, want), n);
        CHECK(memcmp(got, want, n) == 0);
    }
}

static void a_groups_id_is_worked_out_from_its_secret(void) {
    for (size_t i = 0; i < COUNT(group_ids); i++) {
        uint8_t id[TERN_COMPANION_GROUP];
        tern_companion_group_id(group_ids[i].secret, id);
        CHECK(memcmp(id, group_ids[i].id, sizeof id) == 0);
    }
}

/* Which version first has each type: what a node goes by in what it sends an older client. */
static void each_type_has_the_version_it_came_in(void) {
    CHECK(tern_companion_since(TERN_C_SEND) == 0 && tern_companion_since(TERN_C_MESSAGE) == 0);
    CHECK(tern_companion_since(TERN_C_END_SESSION) == 1 && tern_companion_since(TERN_C_ASKED) == 1);
    for (unsigned t = TERN_C_MAKE_GROUP; t <= TERN_C_JOIN; t++) {
        CHECK(tern_companion_since((uint8_t)t) == 2);
    }
    for (unsigned t = TERN_C_GROUP; t <= TERN_C_INVITE; t++) {
        CHECK(tern_companion_since((uint8_t)t) == 2);
    }
    CHECK(tern_companion_since(TERN_C_MADE) == 2 && tern_companion_since(TERN_C_QUEUED) == 0);
}

static void rejected_frames_are_answered_as_the_draft_says(void) {
    for (size_t i = 0; i < COUNT(rejected); i++) {
        const struct rejected_case *c = &rejected[i];
        struct tern_companion_msg m = {0};
        enum tern_companion_read r = tern_companion_read(&m, c->frame, c->len);
        int answer = -1;
        if (r != TERN_C_READ_SHORT && tern_companion_request(c->frame[0])) {
            answer = (int)r;
        }
        if (r == TERN_C_READ_OK || answer != c->answer) {
            fprintf(stderr, "rejected: %s\n", c->why);
        }
        CHECK(r != TERN_C_READ_OK);
        CHECK_EQ_I64(answer, c->answer);
    }
}

/* What a parser hands on, kept in order, with text gathered into runs as the vectors give it. */
struct seen {
    struct item items[16];
    size_t n;
};

static void seen_frame(void *ctx, const uint8_t *frame, size_t len) {
    struct seen *s = ctx;
    if (s->n < COUNT(s->items)) {
        s->items[s->n].frame = true;
        memcpy(s->items[s->n].bytes, frame, len);
        s->items[s->n++].len = len;
    }
}

static void seen_text(void *ctx, uint8_t byte) {
    struct seen *s = ctx;
    if (s->n > 0 && !s->items[s->n - 1].frame) {
        struct item *it = &s->items[s->n - 1];
        it->bytes[it->len++] = byte;
    } else if (s->n < COUNT(s->items)) {
        s->items[s->n] = (struct item){.frame = false, .bytes = {byte}, .len = 1};
        s->n++;
    }
}

static void streams_are_split_into_frames_and_text(void) {
    for (size_t i = 0; i < COUNT(streams); i++) {
        const struct stream_case *c = &streams[i];
        struct seen s = {.n = 0};
        struct tern_companion_sink sink = {&s, seen_frame, seen_text};
        struct tern_companion_parser p;
        tern_companion_parser_init(&p);
        for (size_t k = 0; k < c->len; k++) {
            tern_companion_push(&p, 0, c->stream[k], &sink);
        }
        bool ok = s.n == c->n_items && p.len == c->pending_len &&
                  memcmp(p.buf, c->pending, c->pending_len) == 0;
        for (size_t k = 0; ok && k < s.n; k++) {
            ok = s.items[k].frame == c->items[k].frame && s.items[k].len == c->items[k].len &&
                 memcmp(s.items[k].bytes, c->items[k].bytes, c->items[k].len) == 0;
        }
        if (!ok) {
            fprintf(stderr, "stream: %s\n", c->why);
        }
        CHECK(ok);
    }
}

/* A client that leaves mid-frame: the node waits GAP for the rest, then reads the start as text
 * and finds the next client's frame. */
static void a_partial_frame_lapses(void) {
    static const uint8_t ping[] = {0x03, 0x07};
    uint8_t wrapped[TERN_COMPANION_STREAM_MAX];
    size_t n = tern_companion_wrap(ping, sizeof ping, wrapped);
    struct seen s = {.n = 0};
    struct tern_companion_sink sink = {&s, seen_frame, seen_text};
    struct tern_companion_parser p;
    tern_companion_parser_init(&p);
    for (size_t k = 0; k < 5; k++) {
        tern_companion_push(&p, TERN_MS(10), wrapped[k], &sink);
    }
    tern_companion_idle(&p, TERN_MS(400), &sink);
    CHECK_EQ_U64(s.n, 0);
    tern_companion_idle(&p, TERN_MS(10) + TERN_COMPANION_GAP, &sink);
    /* The magic's first byte was text; the rest, 54 00 02 03, starts nothing and is text too. */
    CHECK_EQ_U64(s.n, 1);
    CHECK_EQ_U64(p.len, 0);
    for (size_t k = 0; k < n; k++) {
        tern_companion_push(&p, TERN_S(1), wrapped[k], &sink);
    }
    CHECK_EQ_U64(s.n, 2);
    CHECK(s.items[1].frame && s.items[1].len == 2 && s.items[1].bytes[0] == 0x03);
}

static void utf8_is_checked_and_cut_on_a_character(void) {
    static const uint8_t bird[] = {'a', 0xF0, 0x9F, 0x90, 0xA6, 'b'};
    CHECK(tern_companion_utf8(bird, sizeof bird));
    CHECK_EQ_U64(tern_companion_utf8_prefix(bird, sizeof bird, 4), 1);
    CHECK_EQ_U64(tern_companion_utf8_prefix(bird, sizeof bird, 5), 5);
    static const uint8_t overlong[] = {0xC0, 0xAF}, surrogate[] = {0xED, 0xA0, 0x80},
                         past[] = {0xF4, 0x90, 0x80, 0x80}, cut[] = {0xE2, 0x82};
    CHECK(!tern_companion_utf8(overlong, sizeof overlong));
    CHECK(!tern_companion_utf8(surrogate, sizeof surrogate));
    CHECK(!tern_companion_utf8(past, sizeof past));
    CHECK(!tern_companion_utf8(cut, sizeof cut));
    CHECK(tern_companion_utf8(NULL, 0));
}

/* Each connection's frames read as the version its client speaks has them, and written back the
 * same: an older one's SYNCED is two bytes. A request its version lacks (version 1's last, which
 * a client must not send) is unknown to it. */
static void each_connection_is_read_by_its_version(void) {
    const struct {
        const struct step *steps;
        size_t n;
        uint8_t version;
    } runs[] = {
        {exchange, COUNT(exchange), TERN_COMPANION_VERSION},
        {older_0, COUNT(older_0), 0},
        {older_1, COUNT(older_1), 1},
        {older_2, COUNT(older_2), 2},
    };
    for (size_t r = 0; r < COUNT(runs); r++) {
        for (size_t i = 0; i < runs[r].n; i++) {
            const struct step *s = &runs[r].steps[i];
            struct tern_companion_msg m = {0};
            uint8_t out[TERN_COMPANION_MAX_FRAME];
            enum tern_companion_read got =
                tern_companion_read_as(&m, s->frame, s->len, runs[r].version);
            if (tern_companion_since(s->frame[0]) > runs[r].version) {
                CHECK_EQ_I64(got, TERN_C_READ_UNKNOWN);
                continue;
            }
            CHECK_EQ_I64(got, TERN_C_READ_OK);
            CHECK_EQ_U64(tern_companion_write_as(&m, out, runs[r].version), s->len);
            CHECK(memcmp(out, s->frame, s->len) == 0);
        }
    }
}

/* SYNCED's count came with version 3: a node writes it to no earlier client, and a client reads
 * it from no earlier node. */
static void synced_carries_the_count_from_version_3(void) {
    struct tern_companion_msg m = {.type = TERN_C_SYNCED, .seq = 2, .news = 6};
    uint8_t out[TERN_COMPANION_MAX_FRAME];
    CHECK_EQ_U64(tern_companion_write_as(&m, out, 3), 3);
    CHECK(out[2] == 6);
    CHECK_EQ_U64(tern_companion_write_as(&m, out, 2), 2);
    CHECK_EQ_U64(tern_companion_write_as(&m, out, 0), 2);

    const uint8_t short_synced[] = {TERN_C_SYNCED, 2};
    m = (struct tern_companion_msg){.news = 9};
    CHECK_EQ_I64(tern_companion_read_as(&m, short_synced, sizeof short_synced, 2), TERN_C_READ_OK);
    CHECK(m.type == TERN_C_SYNCED && m.news == 9);
    CHECK_EQ_I64(tern_companion_read(&m, short_synced, sizeof short_synced), TERN_C_READ_MALFORMED);
}

/* A type a later version added is unknown to an earlier one before its fields are looked at, so
 * one cut short is still unknown, not malformed; and it is written to none. */
static void a_later_type_is_unknown_to_an_earlier_version(void) {
    const uint8_t end_session[] = {TERN_C_END_SESSION, 1}; /* cut short */
    const uint8_t group[] = {TERN_C_GROUP, 1};
    struct tern_companion_msg m = {0};
    CHECK_EQ_I64(tern_companion_read_as(&m, end_session, sizeof end_session, 0),
                 TERN_C_READ_UNKNOWN);
    CHECK_EQ_I64(tern_companion_read_as(&m, end_session, sizeof end_session, 1),
                 TERN_C_READ_MALFORMED);
    CHECK_EQ_I64(tern_companion_read_as(&m, group, sizeof group, 1), TERN_C_READ_UNKNOWN);
    uint8_t out[TERN_COMPANION_MAX_FRAME];
    m = (struct tern_companion_msg){.type = TERN_C_GROUP_GONE, .seq = 1};
    CHECK_EQ_U64(tern_companion_write_as(&m, out, 1), 0);
    CHECK_EQ_U64(tern_companion_write_as(&m, out, 2), 2 + TERN_COMPANION_GROUP);
}

static void nothing_is_written_that_cannot_be_read(void) {
    struct tern_companion_msg m = {.type = TERN_C_SET, .seq = 1, .setting = 9};
    uint8_t out[TERN_COMPANION_MAX_FRAME];
    CHECK_EQ_U64(tern_companion_write(&m, out), 0);
    m = (struct tern_companion_msg){.type = 0x2f};
    CHECK_EQ_U64(tern_companion_write(&m, out), 0);
    m = (struct tern_companion_msg){.type = TERN_C_SAVE_CONTACT, .text_len = 32};
    CHECK_EQ_U64(tern_companion_write(&m, out), 0);
}

int main(void) {
    RUN(crc_matches_the_published_check_value);
    RUN(frames_are_written_and_read);
    RUN(bytes_past_the_fields_are_ignored);
    RUN(rejected_frames_are_answered_as_the_draft_says);
    RUN(a_groups_id_is_worked_out_from_its_secret);
    RUN(each_type_has_the_version_it_came_in);
    RUN(streams_are_split_into_frames_and_text);
    RUN(a_partial_frame_lapses);
    RUN(utf8_is_checked_and_cut_on_a_character);
    RUN(each_connection_is_read_by_its_version);
    RUN(synced_carries_the_count_from_version_3);
    RUN(a_later_type_is_unknown_to_an_earlier_version);
    RUN(nothing_is_written_that_cannot_be_read);
    return CHECK_DONE();
}
