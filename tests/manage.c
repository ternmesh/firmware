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

#include "managing.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static void the_limits_are_the_specifications(void) {
    CHECK_EQ_U64(TERN_MANAGE_REQUEST_MAX, V_REQUEST_MAX);
    CHECK_EQ_U64(TERN_MANAGE_ANSWER_MAX, V_ANSWER_MAX);
    CHECK_EQ_U64(TERN_C_OWNERS, V_OWNERS);
    CHECK_EQ_U64(TERN_MANAGE_TRIAL, TERN_S(V_TRIAL_S));
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
        bool request = c->content[0] == TERN_MANAGE_REQUEST;
        bool ok = request ? tern_manage_request_ok(c->content + 1, c->len - 1)
                          : tern_manage_answer_ok(c->content + 1, c->len - 1);
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

int main(void) {
    RUN(the_limits_are_the_specifications);
    RUN(requests_and_answers_are_read);
    RUN(what_is_not_one_is_ignored);
    RUN(what_may_be_asked_over_the_mesh);
    return CHECK_DONE();
}
