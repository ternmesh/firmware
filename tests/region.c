#include "tern/region.h"
#include "tern/duty.h"

#include <string.h>

#include "check.h"
#include "tern/err.h"

/* The specification's radio settings (draft/phy.md), against its vectors. */

struct profile_case {
    const char *name;
    uint32_t freq_hz, bw_hz;
    uint8_t sf;
    int max_eirp_dbm;
    uint32_t duty_ppm, duty_window_s;
    struct {
        uint32_t len;
        int64_t ns;
    } airtime[8];
    bool ldro;
};

struct duty_case {
    const char *profile, *why;
    int runs;
    struct {
        int64_t first_at;
        int count;
        int64_t every;
        uint32_t len;
    } sent[4];
    int64_t at;
    uint32_t len;
    bool must_refuse;
};

#include "phy.h"

static const struct tern_region *by_name(const char *name) {
    for (int id = TERN_REGION_US915; id < TERN_REGION_END; id++) {
        const struct tern_region *r = tern_region((enum tern_region_id)id);
        if (r != NULL && strcmp(r->name, name) == 0) {
            return r;
        }
    }
    return NULL;
}

static void settings_every_frame_uses(void) {
    CHECK_EQ_U64(TERN_SYNC_WORD, VECTOR_SYNC_WORD);
    CHECK_EQ_U64(tern_sync_word_sx126x(TERN_SYNC_WORD), VECTOR_SYNC_WORD_SX126X);
    CHECK_EQ_I64(TERN_PREAMBLE, VECTOR_PREAMBLE);
    for (int id = TERN_REGION_US915; id < TERN_REGION_END; id++) {
        struct tern_lora m = tern_region_lora(tern_region((enum tern_region_id)id));
        CHECK_EQ_I64(m.preamble, VECTOR_PREAMBLE);
        CHECK_EQ_I64(m.cr + 4, VECTOR_CR_DENOMINATOR);
        CHECK(m.crc && !m.implicit_header && m.ldro == TERN_LDRO_AUTO);
    }
}

static void every_profile_matches_the_specification(void) {
    size_t count = sizeof profiles / sizeof profiles[0];
    /* Each of the specification's is here, and nothing is here that is not the specification's. */
    CHECK_EQ_I64(TERN_REGION_END - TERN_REGION_US915, count);
    for (size_t i = 0; i < count; i++) {
        const struct profile_case *p = &profiles[i];
        const struct tern_region *r = by_name(p->name);
        CHECK(r != NULL);
        if (r == NULL) {
            continue;
        }
        CHECK_EQ_I64(r->freq_hz, p->freq_hz);
        CHECK_EQ_I64(r->bw_hz, p->bw_hz);
        CHECK_EQ_I64(r->sf, p->sf);
        CHECK_EQ_I64(r->max_eirp_dbm, p->max_eirp_dbm);
        CHECK_EQ_I64(r->duty_ppm, p->duty_ppm);
        CHECK_EQ_I64(r->duty_window_s, p->duty_window_s);
        struct tern_lora m = tern_region_lora(r);
        CHECK(m.implicit_header == !VECTOR_EXPLICIT_HEADER);
        CHECK(m.crc == VECTOR_CRC);
        CHECK(tern_lora_ldro(&m) == p->ldro);
        /* The core has no setting for inverted IQ: every radio driver sends it as it comes. */
        CHECK(!VECTOR_IQ_INVERTED);
        for (int j = 0; j < VECTOR_LENGTHS; j++) {
            CHECK_EQ_I64(tern_lora_airtime(&m, p->airtime[j].len), p->airtime[j].ns);
        }
    }
}

/* The limit on transmitting: having sent what a case says, the account refuses the frame the
 * limit forbids. Where the limit does not forbid it the account may still refuse, since it rounds
 * against itself; how closely it follows the limit is tests/duty.c's. */
static void the_limit_on_transmitting_refuses_what_the_specification_says(void) {
    int refused = 0;
    for (size_t i = 0; i < sizeof duties / sizeof duties[0]; i++) {
        const struct duty_case *c = &duties[i];
        const struct tern_region *r = by_name(c->profile);
        struct tern_lora m = tern_region_lora(r);
        struct tern_duty d;
        tern_duty_init(&d, r->duty_ppm, r->duty_window_s);
        for (int k = 0; k < c->runs; k++) {
            for (int n = 0; n < c->sent[k].count; n++) {
                tern_duty_charge(&d, c->sent[k].first_at + n * c->sent[k].every,
                                 tern_lora_airtime(&m, c->sent[k].len));
            }
        }
        if (c->must_refuse) {
            refused++;
            if (tern_duty_allows(&d, c->at, tern_lora_airtime(&m, c->len))) {
                fprintf(stderr, "%s: sent, %s\n", c->profile, c->why);
                check_failures++;
            }
        }
    }
    CHECK(refused > 0);
}

static void unknown_regions_are_null(void) {
    CHECK(tern_region((enum tern_region_id)0) == NULL);
    CHECK(tern_region(TERN_REGION_END) == NULL);
    CHECK(tern_region((enum tern_region_id) - 1) == NULL);
}

static void radio_configuration_is_the_profiles(void) {
    const struct tern_region *us = tern_region(TERN_REGION_US915);
    struct tern_radio_config cfg;
    memset(&cfg, 0, sizeof cfg);
    CHECK_EQ_I64(tern_region_radio(us, 22, 3, &cfg), TERN_OK);
    CHECK_EQ_I64(cfg.freq_hz, 921250000);
    CHECK_EQ_I64(cfg.mod.bw_hz, 500000);
    CHECK_EQ_I64(cfg.mod.sf, 9);
    CHECK_EQ_I64(cfg.tx_power_dbm, 22);
    CHECK_EQ_U64(cfg.sync_word, 0x5E);
}

/* The limit is on what leaves the antenna, so the same transmitter is allowed with one antenna
 * and refused with another. */
static void more_power_than_a_region_allows_is_refused(void) {
    const struct tern_region *us = tern_region(TERN_REGION_US915);
    const struct tern_region *eu = tern_region(TERN_REGION_EU868);
    struct tern_radio_config cfg, untouched;
    memset(&cfg, 0x5A, sizeof cfg);
    untouched = cfg;

    CHECK_EQ_I64(tern_region_radio(us, 30, 6, &cfg), TERN_OK);
    CHECK_EQ_I64(tern_region_radio(eu, 22, 7, &cfg), TERN_OK);
    CHECK_EQ_I64(tern_region_radio(eu, 29, 0, &cfg), TERN_OK); /* no conducted limit there */

    cfg = untouched;
    CHECK_EQ_I64(tern_region_radio(us, 30, 7, &cfg), TERN_EINVAL); /* radiated */
    CHECK_EQ_I64(tern_region_radio(us, 31, 0, &cfg), TERN_EINVAL); /* conducted */
    CHECK_EQ_I64(tern_region_radio(eu, 22, 8, &cfg), TERN_EINVAL);
    CHECK_EQ_I64(tern_region_radio(eu, 127, 127, &cfg), TERN_EINVAL);
    CHECK(memcmp(&cfg, &untouched, sizeof cfg) == 0);
}

int main(void) {
    RUN(settings_every_frame_uses);
    RUN(every_profile_matches_the_specification);
    RUN(the_limit_on_transmitting_refuses_what_the_specification_says);
    RUN(unknown_regions_are_null);
    RUN(radio_configuration_is_the_profiles);
    RUN(more_power_than_a_region_allows_is_refused);
    return CHECK_DONE();
}
