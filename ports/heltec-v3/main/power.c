#include "power.h"

uint16_t power_pick(struct power_sense *s, uint16_t at_low_mv, uint16_t at_high_mv) {
    if (!s->known) {
        uint16_t most = at_low_mv > at_high_mv ? at_low_mv : at_high_mv;
        if (most < POWER_NONE_MV) {
            return 0; /* no battery: nothing to learn from */
        }
        s->known = true;
        s->high_enables = at_high_mv > at_low_mv;
    }
    uint16_t mv = s->high_enables ? at_high_mv : at_low_mv;
    return mv < POWER_NONE_MV ? 0 : mv;
}

/* The voltage at each charge, from full to empty, for a single cell under the light load a node
 * puts on it. Between two points the charge is taken to change evenly. */
static const struct {
    uint16_t mv;
    uint8_t percent;
} curve[] = {
    {4200, 100}, {4100, 90}, {4000, 80}, {3920, 70}, {3850, 60}, {3800, 50},
    {3760, 40},  {3720, 30}, {3680, 20}, {3600, 10}, {3450, 5},  {3300, 0},
};

uint8_t power_percent(uint16_t mv) {
    if (mv < POWER_NONE_MV) {
        return POWER_UNKNOWN;
    }
    if (mv >= curve[0].mv) {
        return 100;
    }
    for (unsigned i = 1; i < sizeof curve / sizeof curve[0]; i++) {
        if (mv >= curve[i].mv) {
            unsigned span = curve[i - 1].mv - curve[i].mv;
            unsigned rise = curve[i - 1].percent - curve[i].percent;
            return (uint8_t)(curve[i].percent + (mv - curve[i].mv) * rise / span);
        }
    }
    return 0;
}
