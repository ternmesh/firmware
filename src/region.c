#include "tern/region.h"

#include <stddef.h>

#include "tern/err.h"

/* The specification's profiles (draft/phy.md), where the reasons for each value are. The tests
 * check this table against the specification's own copy, vectors/phy.json. */
static const struct tern_region regions[] = {
    [TERN_REGION_US915] = {.name = "US915",
                           .freq_hz = 921250000,
                           .bw_hz = 500000,
                           .sf = 9,
                           .max_eirp_dbm = 36,
                           .max_conducted_dbm = 30,
                           .duty_ppm = TERN_DUTY_UNLIMITED,
                           .duty_window_s = 3600},
    [TERN_REGION_EU868] = {.name = "EU868",
                           .freq_hz = 869475000,
                           .bw_hz = 125000,
                           .sf = 7,
                           .max_eirp_dbm = 29,
                           .max_conducted_dbm = TERN_POWER_UNLIMITED,
                           .duty_ppm = 100000,
                           .duty_window_s = 3600},
};

const struct tern_region *tern_region(enum tern_region_id id) {
    if (id < TERN_REGION_US915 || id >= TERN_REGION_END) {
        return NULL;
    }
    return &regions[id];
}

struct tern_lora tern_region_lora(const struct tern_region *r) {
    struct tern_lora m = tern_lora_default(r->sf, r->bw_hz);
    m.preamble = TERN_PREAMBLE;
    return m;
}

int tern_region_radio(const struct tern_region *r, int8_t tx_power_dbm, int8_t antenna_dbi,
                      struct tern_radio_config *cfg) {
    if (tx_power_dbm > r->max_conducted_dbm || (int)tx_power_dbm + antenna_dbi > r->max_eirp_dbm) {
        return TERN_EINVAL;
    }
    cfg->mod = tern_region_lora(r);
    cfg->freq_hz = r->freq_hz;
    cfg->tx_power_dbm = tx_power_dbm;
    cfg->sync_word = TERN_SYNC_WORD;
    return TERN_OK;
}
