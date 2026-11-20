#include <string.h>

#include "check.h"
#include "tern/manage.h"

/* Managing a node over the mesh (draft/managing.md): the specification's vectors for what goes in
 * its messages, and what a managed node acts on. */

struct content_case {
    const char *why;
    uint8_t content[160];
    size_t len;
};
struct allowed_case {
    uint8_t type, setting;
    bool allowed;
};

struct notice_case {
    uint8_t contact[32];
    uint8_t owner, by;
    uint8_t content[TERN_MANAGE_NOTICE_LEN];
};
struct locked_case {
    uint8_t type;
    bool allowed;
};

#include "managing.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static void the_limits_are_the_specifications(void) {
    CHECK_EQ_U64(TERN_MANAGE_REQUEST_MAX, V_REQUEST_MAX);
    CHECK_EQ_U64(TERN_MANAGE_ANSWER_MAX, V_ANSWER_MAX);
    CHECK_EQ_U64(TERN_C_OWNERS, V_OWNERS);
    CHECK_EQ_U64(TERN_MANAGE_TRIAL, TERN_S(V_TRIAL_S));
    CHECK_EQ_U64(TERN_MANAGE_POLL_S, V_MANAGED_POLL_S);
    CHECK_EQ_U64(TERN_MANAGE_AWAKE_S, V_MANAGED_AWAKE_S);
}

static void requests_and_answers_are_read(void) {
    for (size_t i = 0; i < COUNT(requests); i++) {
        CHECK_EQ_I64(requests[i].content[0], TERN_MANAGE_REQUEST);
        CHECK(tern_manage_request_ok(requests[i].content + 1, requests[i].len - 1));
    }
    for (size_t i = 0; i < COUNT(answers); i++) {
        CHECK_EQ_I64(answers[i].content[0], TERN_MANAGE_ANSWER);
        CHECK(tern_manage_answer_ok(answers[i].content + 1, answers[i].len - 1));
    }
}

static void what_is_not_one_is_ignored(void) {
    for (size_t i = 0; i < COUNT(rejected_answers); i++) {
        const struct content_case *c = &rejected_answers[i];
        uint8_t contact[TERN_ADDRESS_LEN], owner, by;
        bool ok = c->content[0] == TERN_MANAGE_REQUEST
                      ? tern_manage_request_ok(c->content + 1, c->len - 1)
                  : c->content[0] == TERN_MANAGE_ANSWER
                      ? tern_manage_answer_ok(c->content + 1, c->len - 1)
                      : tern_manage_notice_read(c->content, c->len, contact, &owner, &by);
        if (ok) {
            fprintf(stderr, "taken: %s\n", c->why);
        }
        CHECK(!ok);
    }
}

static void what_may_be_asked_over_the_mesh(void) {
    for (size_t i = 0; i < COUNT(allowed); i++) {
        CHECK_EQ_I64(tern_manage_allowed(allowed[i].type, allowed[i].setting), allowed[i].allowed);
    }
}

static void notices_are_written_and_read(void) {
    for (size_t i = 0; i < COUNT(notices); i++) {
        uint8_t out[TERN_MANAGE_NOTICE_LEN], contact[TERN_ADDRESS_LEN], owner, by;
        tern_manage_notice_write(notices[i].contact, notices[i].owner, notices[i].by, out);
        CHECK(memcmp(out, notices[i].content, sizeof out) == 0);
        CHECK(tern_manage_notice_read(out, sizeof out, contact, &owner, &by));
        CHECK(memcmp(contact, notices[i].contact, sizeof contact) == 0);
        CHECK_EQ_I64(owner, notices[i].owner);
        CHECK_EQ_I64(by, notices[i].by);
    }
}

static void what_a_locked_node_says_over_bluetooth(void) {
    for (size_t i = 0; i < COUNT(locked); i++) {
        CHECK_EQ_I64(tern_manage_unlocked(locked[i].type), locked[i].allowed);
    }
}

int main(void) {
    RUN(the_limits_are_the_specifications);
    RUN(requests_and_answers_are_read);
    RUN(what_is_not_one_is_ignored);
    RUN(what_may_be_asked_over_the_mesh);
    RUN(notices_are_written_and_read);
    RUN(what_a_locked_node_says_over_bluetooth);
    return CHECK_DONE();
}
