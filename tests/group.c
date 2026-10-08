#include <string.h>

#include "check.h"
#include "tern/err.h"
#include "tern/group.h"

/* Groups (draft/groups.md), against the specification's vectors. */

struct accepted_case {
    const char *name;
    uint8_t secret[16];
    uint8_t nonce[8];
    uint32_t from;
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
struct recent_again {
    uint8_t nonce[8];
    const uint8_t *frame;
    size_t frame_len;
    bool accept;
};
struct recent_case {
    uint8_t secret[16];
    uint32_t self, from;
    const uint8_t *content;
    size_t content_len;
    uint64_t first;
    unsigned count;
    const struct recent_again *again;
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
        CHECK(tern_group_seal(&g, c->nonce, c->from, c->content, c->content_len, frame,
                              sizeof frame) == TERN_OK);
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
        CHECK(g.count == 0);
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
                ok = r.group == d->group + 1 && r.from == d->from &&
                     bytes_eq(content, r.len, d->content, d->content_len);
            }
            if (!ok) {
                fprintf(stderr, "members %s: delivery %zu\n", c->name, k);
                check_failures++;
            }
        }
    }
}

static void test_recent(void) {
    for (size_t i = 0; i < COUNT(recent_cases); i++) {
        const struct recent_case *c = &recent_cases[i];
        struct tern_group g;
        uint8_t frame[TERN_GROUP_MAX_FRAME], content[TERN_GROUP_MAX_CONTENT];
        tern_group_init(&g, c->secret);
        for (uint64_t n = c->first; n < c->first + c->count; n++) {
            uint8_t nonce[8];
            for (int k = 0; k < 8; k++) {
                nonce[k] = (uint8_t)(n >> (56 - 8 * k));
            }
            CHECK(tern_group_seal(&g, nonce, c->from, c->content, c->content_len, frame,
                                  sizeof frame) == TERN_OK);
            CHECK(receive(&g, c->self, frame, c->content_len + TERN_GROUP_OVERHEAD, content)
                      .verdict == TERN_GROUP_ACCEPTED);
        }
        for (size_t k = 0; k < c->n; k++) {
            const struct recent_again *a = &c->again[k];
            bool got = receive(&g, c->self, a->frame, a->frame_len, content).verdict ==
                       TERN_GROUP_ACCEPTED;
            if (got != a->accept) {
                fprintf(stderr, "recent: again %zu\n", k);
                check_failures++;
            }
        }
    }
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

static void test_arguments(void) {
    struct tern_group g;
    static const uint8_t secret[16] = {7}, nonce[8] = {1};
    uint8_t frame[TERN_GROUP_MAX_FRAME], big[TERN_GROUP_MAX_CONTENT + 1] = {0};
    tern_group_init(&g, secret);
    CHECK(tern_group_seal(&g, nonce, 5, big, sizeof big, frame, sizeof frame) == TERN_EINVAL);
    CHECK(tern_group_seal(&g, nonce, 5, big, 10, frame, 36) == TERN_EINVAL);
    CHECK(tern_group_seal(&g, nonce, 0, big, 10, frame, sizeof frame) == TERN_EINVAL);
    CHECK(tern_group_seal(&g, nonce, 0xFFFFFFFFu, big, 10, frame, sizeof frame) == TERN_EINVAL);
    CHECK(tern_group_seal(&g, nonce, 5, big, 10, frame, 37) == TERN_OK);
    /* A buffer too small for what the frame holds is the caller's mistake, said so. */
    struct tern_group *const one[] = {&g};
    struct tern_group_received r;
    uint8_t content[9];
    CHECK(tern_group_open(one, 1, 6, frame, 37, content, sizeof content, &r) == TERN_EINVAL);
    /* Left, nothing of the group remains. */
    static const struct tern_group none;
    tern_group_wipe(&g);
    CHECK(memcmp(&g, &none, sizeof g) == 0);
}

int main(void) {
    RUN(test_accepted);
    RUN(test_rejected);
    RUN(test_members);
    RUN(test_recent);
    RUN(test_invites);
    RUN(test_arguments);
    return CHECK_DONE();
}
