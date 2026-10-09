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

void power_watch(struct power_watch *w, uint16_t mv) {
    if (mv < POWER_NONE_MV) {
        *w = (struct power_watch){0};
        return;
    }
    if (w->last != 0) {
        if (mv >= w->last + POWER_STEP_MV) {
            w->charging = true;
            w->peak = mv;
        } else if (mv + POWER_STEP_MV <= w->last) {
            w->charging = false;
        }
    }
    if (w->charging) {
        if (mv > w->peak) {
            w->peak = mv;
        } else if (mv + POWER_SAG_MV <= w->peak) {
            w->charging = false; /* a slow fall: running down, not charging */
        }
    }
    w->last = mv;
    if (mv < POWER_EMPTY_MV && !w->charging) {
        w->low = w->low < UINT8_MAX ? (uint8_t)(w->low + 1) : w->low;
    } else {
        w->low = 0;
    }
}

bool power_empty(const struct power_watch *w) { return w->low >= 2 && !w->charging; }
