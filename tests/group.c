#include <string.h>

#include "check.h"
#include "tern/err.h"
#include "tern/group.h"

/* Groups (draft/groups.md), against the specification's vectors. */

struct accepted_case {
    const char *name;
    uint8_t hdr;
    bool node;
    uint8_t secret[16];
    uint8_t nonce[8];
    uint32_t from;
    uint32_t count;
    const uint8_t *content;
    size_t content_len;
    uint8_t hops;
    int8_t power;
    uint32_t self;
    uint8_t key[16];
    uint8_t gtag[4];
    const uint8_t *frame;
    size_t frame_len;
};
struct rejected_case {
    const char *name;
    uint8_t secret[16];
    uint32_t self;
    const uint8_t *frame;
    size_t frame_len;
};
struct delivery {
    const uint8_t *frame;
    size_t frame_len;
    bool accept;
    size_t group;
    uint32_t from;
    uint32_t count;
    const uint8_t *content;
    size_t content_len;
};
struct member_case {
    const char *name;
    uint32_t self;
    const uint8_t (*secrets)[16];
    size_t n_groups;
    const struct delivery *deliveries;
    size_t n;
};
struct counted {
    uint32_t from, count;
    const uint8_t *frame;
    size_t frame_len;
    bool accept;
};
struct count_case {
    const char *name;
    uint8_t secret[16];
    uint32_t self;
    const uint8_t *content;
    size_t content_len;
    const struct counted *deliveries;
    size_t n;
};
struct invite_case {
    uint8_t secret[16];
    const uint8_t *name;
    size_t name_len;
    const uint8_t *plaintext;
    size_t len;
};
struct bad_invite_case {
    const char *name;
    const uint8_t *plaintext;
    size_t len;
};

struct join_case {
    uint8_t secret[16];
    const uint8_t *name;
    size_t name_len;
    const uint8_t *payload;
    size_t payload_len;
    const char *link;
    const char *reads[3];
    size_t n_reads;
};
struct bad_join_case {
    const char *why;
    const char *link;
};

#include "groups.h"

#define COUNT(a) (sizeof(a) / sizeof(a)[0])
#define GROUPS_MAX 4

static bool bytes_eq(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len) {
    return a_len == b_len && (a_len == 0 || memcmp(a, b, a_len) == 0);
}

static struct tern_group_received receive(struct tern_group *g, uint32_t self, const uint8_t *frame,
                                          size_t len, uint8_t *content) {
    struct tern_group *const one[] = {g};
    struct tern_group_received r;
    CHECK(tern_group_open(one, 1, self, frame, len, content, TERN_GROUP_MAX_CONTENT, &r) ==
          TERN_OK);
    return r;
}

static void test_accepted(void) {
    for (size_t i = 0; i < COUNT(accepted); i++) {
        const struct accepted_case *c = &accepted[i];
        struct tern_group g;
        uint8_t frame[TERN_GROUP_MAX_FRAME], content[TERN_GROUP_MAX_CONTENT];
        tern_group_init(&g, c->secret);
        CHECK(memcmp(g.key, c->key, 16) == 0);
        int sealed = c->node ? tern_group_seal_node(&g, c->nonce, c->from, c->count, c->content,
                                                    c->content_len, frame, sizeof frame)
                             : tern_group_seal(&g, c->nonce, c->from, c->count, c->content,
                                               c->content_len, frame, sizeof frame);
        CHECK(sealed == TERN_OK);
        CHECK_EQ_U64(frame[0], c->hdr);
        /* Sealed, the flood's bytes are zero. */
        CHECK(frame[1] == 0 && frame[2] == 0 && memcmp(frame + 11, c->gtag, 4) == 0);
        frame[1] = c->hops;
        frame[2] = (uint8_t)c->power;
        if (!bytes_eq(frame, c->content_len + TERN_GROUP_OVERHEAD, c->frame, c->frame_len)) {
            fprintf(stderr, "accepted %s: frame differs\n", c->name);
            check_failures++;
        }
        struct tern_group_received r = receive(&g, c->self, c->frame, c->frame_len, content);
        if (r.verdict != TERN_GROUP_ACCEPTED || r.from != c->from || r.group != 0 ||
            r.node != c->node || r.count != c->count ||
            !bytes_eq(content, r.len, c->content, c->content_len)) {
            fprintf(stderr, "accepted %s: not received as sent\n", c->name);
            check_failures++;
        }
        /* And once: the same frame again is not a second message. */
        CHECK(receive(&g, c->self, c->frame, c->frame_len, content).verdict == TERN_GROUP_AGAIN);
        /* Its writer does not take it as another's. */
        tern_group_init(&g, c->secret);
        CHECK(receive(&g, c->from, c->frame, c->frame_len, content).verdict == TERN_GROUP_REFUSED);
    }
}

static void test_rejected(void) {
    for (size_t i = 0; i < COUNT(rejected); i++) {
        const struct rejected_case *c = &rejected[i];
        struct tern_group g;
        uint8_t content[TERN_GROUP_MAX_CONTENT];
        tern_group_init(&g, c->secret);
        if (receive(&g, c->self, c->frame, c->frame_len, content).verdict == TERN_GROUP_ACCEPTED) {
            fprintf(stderr, "rejected %s: accepted\n", c->name);
            check_failures++;
        }
        CHECK(g.heard == 0 && g.writers[0].from == 0);
    }
}

static void test_members(void) {
    for (size_t i = 0; i < COUNT(member_cases); i++) {
        const struct member_case *c = &member_cases[i];
        struct tern_group g[GROUPS_MAX];
        struct tern_group *held[GROUPS_MAX + 1] = {NULL}; /* a place with no group, first */
        CHECK(c->n_groups <= GROUPS_MAX);
        for (size_t k = 0; k < c->n_groups; k++) {
            tern_group_init(&g[k], c->secrets[k]);
            held[k + 1] = &g[k];
        }
        for (size_t k = 0; k < c->n; k++) {
            const struct delivery *d = &c->deliveries[k];
            uint8_t content[TERN_GROUP_MAX_CONTENT];
            struct tern_group_received r;
            CHECK(tern_group_open(held, c->n_groups + 1, c->self, d->frame, d->frame_len, content,
                                  sizeof content, &r) == TERN_OK);
            bool ok = (r.verdict == TERN_GROUP_ACCEPTED) == d->accept;
            if (ok && d->accept) {
                ok = r.group == d->group + 1 && r.from == d->from && r.count == d->count &&
                     bytes_eq(content, r.len, d->content, d->content_len);
            }
            if (!ok) {
                fprintf(stderr, "members %s: delivery %zu\n", c->name, k);
                check_failures++;
            }
        }
    }
}

static void test_counts(void) {
    for (size_t i = 0; i < COUNT(count_cases); i++) {
        const struct count_case *c = &count_cases[i];
        struct tern_group g;
        uint8_t frame[TERN_GROUP_MAX_FRAME], content[TERN_GROUP_MAX_CONTENT];
        tern_group_init(&g, c->secret);
        for (size_t k = 0; k < c->n; k++) {
            const struct counted *d = &c->deliveries[k];
            /* The frame is the one this place in the list, as a nonce, seals. */
            uint8_t nonce[8] = {0};
            nonce[6] = (uint8_t)(k >> 8);
            nonce[7] = (uint8_t)k;
            CHECK(tern_group_seal(&g, nonce, d->from, d->count, c->content, c->content_len, frame,
                                  sizeof frame) == TERN_OK);
            CHECK(d->frame_len == c->content_len + TERN_GROUP_OVERHEAD &&
                  memcmp(frame + 3, d->frame + 3, d->frame_len - 3) == 0);
            struct tern_group_received r = receive(&g, c->self, d->frame, d->frame_len, content);
            if ((r.verdict == TERN_GROUP_ACCEPTED) != d->accept ||
                (!d->accept && r.verdict != TERN_GROUP_AGAIN)) {
                fprintf(stderr, "counts %s: delivery %zu\n", c->name, k);
                check_failures++;
            }
        }
    }
}

/* A member that restarts keeps each writer and its highest count, and takes nothing at or below
 * it again: not what it had accepted, nor the few behind that it had not. */
static void test_kept(void) {
    static const uint8_t secret[16] = {9};
    struct tern_group g, again;
    uint8_t frame[TERN_GROUP_MAX_FRAME], content[TERN_GROUP_MAX_CONTENT], kept[TERN_GROUP_KEPT];
    uint8_t nonce[8] = {0};
    tern_group_init(&g, secret);
    CHECK(tern_group_keep(&g, kept) == 0);
    /* Writers 1 to 16 at count 40, then writer 1 again, so that 2 is the one heard longest ago. */
    for (uint32_t k = 0; k <= TERN_GROUP_WRITERS; k++) {
        uint32_t from = 1 + k % TERN_GROUP_WRITERS, count = k < TERN_GROUP_WRITERS ? 40 : 41;
        nonce[7] = (uint8_t)k;
        CHECK(tern_group_seal(&g, nonce, from, count, NULL, 0, frame, sizeof frame) == TERN_OK);
        CHECK(receive(&g, 99, frame, TERN_GROUP_OVERHEAD, content).verdict == TERN_GROUP_ACCEPTED);
    }
    CHECK(tern_group_keep(&g, kept) == TERN_GROUP_KEPT);
    CHECK(kept[3] == 2 && kept[7] == 40 && kept[TERN_GROUP_KEPT - 5] == 1 &&
          kept[TERN_GROUP_KEPT - 1] == 41);

    tern_group_init(&again, secret);
    tern_group_restore(&again, kept, sizeof kept);
    static const struct {
        uint32_t from, count;
        bool accept;
    } after[] = {
        {1, 41, false}, {1, 40, false}, {1, 39, false}, /* 39 was never accepted: not kept */
        {3, 40, false}, {3, 9, false},  {3, 41, true},
        {3, 40, false}, {17, 5, true}, /* one more writer: 2 goes, as it would have */
        {2, 40, true},
    };
    for (size_t k = 0; k < COUNT(after); k++) {
        nonce[6] = 1;
        nonce[7] = (uint8_t)k;
        CHECK(tern_group_seal(&again, nonce, after[k].from, after[k].count, NULL, 0, frame,
                              sizeof frame) == TERN_OK);
        if ((receive(&again, 99, frame, TERN_GROUP_OVERHEAD, content).verdict ==
             TERN_GROUP_ACCEPTED) != after[k].accept) {
            fprintf(stderr, "kept: delivery %zu\n", k);
            check_failures++;
        }
    }
    /* What is not a whole writer, a reserved id and a writer named twice are passed over. */
    static const uint8_t odd[] = {0,    0,    0, 7, 0, 0, 0, 50, 0, 0, 0,  0,    0,
                                  0,    0,    1, 0, 0, 0, 7, 0,  0, 0, 90, 0xFF, 0xFF,
                                  0xFF, 0xFF, 0, 0, 0, 1, 0, 0,  0, 8, 0};
    tern_group_restore(&again, odd, sizeof odd);
    CHECK(again.heard == 1 && again.writers[0].from == 7 && again.writers[0].top == 50 &&
          again.writers[1].from == 0);
    CHECK(tern_group_keep(&again, kept) == 8 && memcmp(kept, odd, 8) == 0);
    tern_group_restore(&again, NULL, 0);
    CHECK(again.heard == 0 && again.writers[0].from == 0);
}

static void test_invites(void) {
    for (size_t i = 0; i < COUNT(invites); i++) {
        const struct invite_case *c = &invites[i];
        uint8_t out[TERN_GROUP_INVITE_MAX], secret[16], name[TERN_GROUP_NAME_MAX];
        size_t n = 99;
        size_t len = tern_group_invite_write(c->secret, c->name, c->name_len, out);
        CHECK(bytes_eq(out, len, c->plaintext, c->len));
        CHECK(tern_group_invite_read(c->plaintext, c->len, secret, name, &n));
        CHECK(memcmp(secret, c->secret, 16) == 0 && bytes_eq(name, n, c->name, c->name_len));
    }
    for (size_t i = 0; i < COUNT(bad_invites); i++) {
        uint8_t secret[16], name[TERN_GROUP_NAME_MAX];
        size_t n;
        if (tern_group_invite_read(bad_invites[i].plaintext, bad_invites[i].len, secret, name,
                                   &n)) {
            fprintf(stderr, "bad invite %s: read\n", bad_invites[i].name);
            check_failures++;
        }
    }
    uint8_t out[TERN_GROUP_INVITE_MAX], long_name[TERN_GROUP_NAME_MAX + 1] = {0};
    static const uint8_t g[16] = {1}, bad[] = {0xff};
    memset(long_name, 'x', sizeof long_name);
    CHECK(tern_group_invite_write(g, long_name, sizeof long_name, out) == 0);
    CHECK(tern_group_invite_write(g, bad, sizeof bad, out) == 0);
}

static size_t text_len(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

/* A join code is made as the specification's, and read from it however it is written. */
static void test_join_codes(void) {
    for (size_t i = 0; i < COUNT(join_codes); i++) {
        const struct join_case *c = &join_codes[i];
        char link[TERN_GROUP_LINK_MAX + 1];
        size_t len = tern_group_link(c->secret, c->name, c->name_len, link);
        CHECK(len == text_len(c->link) && memcmp(link, c->link, len + 1) == 0);
        CHECK(len >= TERN_GROUP_LINK_MIN && len <= TERN_GROUP_LINK_MAX);
        /* The payload is what is after the #. */
        uint8_t payload[TERN_GROUP_CODE_MAX];
        size_t n = 0;
        CHECK(tern_base32_read(link + 23, len - 23, payload, sizeof payload, &n));
        CHECK(bytes_eq(payload, n, c->payload, c->payload_len));
        for (size_t r = 0; r <= c->n_reads; r++) {
            const char *text = r == 0 ? c->link : c->reads[r - 1];
            uint8_t secret[16], name[TERN_GROUP_NAME_MAX];
            size_t name_len = 99;
            CHECK(tern_group_link_read(text, text_len(text), secret, name, &name_len));
            CHECK(memcmp(secret, c->secret, 16) == 0 &&
                  bytes_eq(name, name_len, c->name, c->name_len));
        }
    }
    for (size_t i = 0; i < COUNT(bad_join_codes); i++) {
        const struct bad_join_case *c = &bad_join_codes[i];
        uint8_t secret[16] = {0}, name[TERN_GROUP_NAME_MAX];
        size_t name_len = 99;
        if (tern_group_link_read(c->link, text_len(c->link), secret, name, &name_len)) {
            fprintf(stderr, "bad join code %s: read\n", c->why);
            check_failures++;
        }
        /* Nothing is written for one refused. */
        static const uint8_t none[16];
        CHECK(memcmp(secret, none, 16) == 0 && name_len == 99);
    }
    /* A name too long, or not UTF-8, makes no code. */
    char link[TERN_GROUP_LINK_MAX + 1];
    uint8_t long_name[TERN_GROUP_NAME_MAX + 1];
    static const uint8_t g[16] = {1}, bad[] = {0xff};
    memset(long_name, 'x', sizeof long_name);
    CHECK(tern_group_link(g, long_name, sizeof long_name, link) == 0);
    CHECK(tern_group_link(g, bad, sizeof bad, link) == 0);
    CHECK(tern_group_link(g, long_name, TERN_GROUP_NAME_MAX, link) == TERN_GROUP_LINK_MAX);
    /* The text read is its length, not to a NUL: a link with more after it is not the link. */
    size_t len = tern_group_link(g, NULL, 0, link);
    uint8_t secret[16], name[TERN_GROUP_NAME_MAX];
    size_t name_len;
    CHECK(len == TERN_GROUP_LINK_MIN);
    CHECK(tern_group_link_read(link, len, secret, name, &name_len) && name_len == 0);
    CHECK(!tern_group_link_read(link, len - 1, secret, name, &name_len));
    CHECK(!tern_group_link_read(link, 10, secret, name, &name_len));
}

static void test_arguments(void) {
    struct tern_group g;
    static const uint8_t secret[16] = {7}, nonce[8] = {1};
    uint8_t frame[TERN_GROUP_MAX_FRAME], big[TERN_GROUP_MAX_CONTENT + 1] = {0};
    tern_group_init(&g, secret);
    CHECK(tern_group_seal(&g, nonce, 5, 0, big, sizeof big, frame, sizeof frame) == TERN_EINVAL);
    CHECK(tern_group_seal(&g, nonce, 5, 0, big, 10, frame, 40) == TERN_EINVAL);
    CHECK(tern_group_seal(&g, nonce, 0, 0, big, 10, frame, sizeof frame) == TERN_EINVAL);
    CHECK(tern_group_seal(&g, nonce, 0xFFFFFFFFu, 0, big, 10, frame, sizeof frame) == TERN_EINVAL);
    CHECK(tern_group_seal(&g, nonce, 5, 0, big, 10, frame, 41) == TERN_OK);
    /* A buffer too small for what the frame holds is the caller's mistake, said so. */
    struct tern_group *const one[] = {&g};
    struct tern_group_received r;
    uint8_t content[9];
    CHECK(tern_group_open(one, 1, 6, frame, 41, content, sizeof content, &r) == TERN_EINVAL);
    /* Left, nothing of the group remains. */
    static const struct tern_group none;
    tern_group_wipe(&g);
    CHECK(memcmp(&g, &none, sizeof g) == 0);
}

int main(void) {
    RUN(test_accepted);
    RUN(test_rejected);
    RUN(test_members);
    RUN(test_counts);
    RUN(test_kept);
    RUN(test_invites);
    RUN(test_join_codes);
    RUN(test_arguments);
    return CHECK_DONE();
}
