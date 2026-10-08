#include "tern/share.h"

#include <string.h>

#include "check.h"

/* Sharing an address off the air (src/share.c), against the specification's vectors
 * (tests/vectors/sharing.json, from draft/sharing.md in ternmesh/spec): the text form, the link,
 * the short code, and what a reader takes and refuses. */

struct share_case {
    uint8_t address[TERN_ADDRESS_LEN];
    const char *text, *link, *short_code;
    size_t n_reads;
    const char *reads[8];
};

struct form_case {
    uint64_t value;
    const char *text;
};

#include "sharing.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static void each_address_is_written_as_the_specification_writes_it(void) {
    for (size_t i = 0; i < COUNT(cases); i++) {
        char text[TERN_ADDRESS_TEXT_LEN + 1], link[TERN_ADDRESS_LINK_LEN + 1];
        char code[TERN_SHORT_CODE_LEN + 1];
        tern_address_text(cases[i].address, text);
        tern_address_link(cases[i].address, link);
        tern_short_code(cases[i].address, code);
        CHECK(strcmp(text, cases[i].text) == 0);
        CHECK(strcmp(link, cases[i].link) == 0);
        if (strcmp(code, cases[i].short_code) != 0) {
            fprintf(stderr, "case %zu: short code \"%s\", expected \"%s\"\n", i, code,
                    cases[i].short_code);
            check_failures++;
        }
    }
}

static void every_spelling_reads_back(void) {
    for (size_t i = 0; i < COUNT(cases); i++) {
        for (size_t k = 0; k < cases[i].n_reads; k++) {
            uint8_t got[TERN_ADDRESS_LEN] = {0};
            CHECK(tern_address_read(cases[i].reads[k], got));
            CHECK(memcmp(got, cases[i].address, TERN_ADDRESS_LEN) == 0);
        }
    }
}

static void anything_else_is_refused(void) {
    for (size_t i = 0; i < COUNT(refused); i++) {
        uint8_t got[TERN_ADDRESS_LEN];
        memset(got, 0xA5, sizeof got);
        if (tern_address_read(refused[i], got)) {
            fprintf(stderr, "refused[%zu] \"%s\" was read\n", i, refused[i]);
            check_failures++;
        }
        for (size_t k = 0; k < sizeof got; k++) {
            CHECK_EQ_I64(got[k], 0xA5); /* nothing written */
        }
    }
}

/* Each reads, as text, and is not an address first contact takes: never kept as a contact. */
static void rejected_addresses_read_but_are_not_contacts(void) {
    for (size_t i = 0; i < COUNT(not_contacts); i++) {
        uint8_t got[TERN_ADDRESS_LEN];
        CHECK(tern_address_read(not_contacts[i], got));
        CHECK(!tern_address_valid(got));
    }
}

static void a_code_keeps_its_leading_zeros(void) {
    for (size_t i = 0; i < COUNT(forms); i++) {
        char code[TERN_SHORT_CODE_LEN + 1];
        tern_short_code_text(forms[i].value, code);
        CHECK(strcmp(code, forms[i].text) == 0);
    }
}

int main(void) {
    RUN(each_address_is_written_as_the_specification_writes_it);
    RUN(every_spelling_reads_back);
    RUN(anything_else_is_refused);
    RUN(rejected_addresses_read_but_are_not_contacts);
    RUN(a_code_keeps_its_leading_zeros);
    return CHECK_DONE();
}
