#include "tern/duty.h"

void tern_duty_init(struct tern_duty *d, uint32_t ppm, uint32_t window_s) {
    tern_time window = (tern_time)window_s * 1000000000LL;
    /* Rounded up: sixty slices must not be shorter than the window. */
    d->slice = (window + TERN_DUTY_SLICES - 2) / (TERN_DUTY_SLICES - 1);
    d->limit = ppm >= 1000000u || d->slice == 0 ? -1 : window / 1000000 * (tern_time)ppm;
    d->newest = 0;
    for (int i = 0; i < TERN_DUTY_SLICES; i++) {
        d->used[i] = 0;
    }
}

/* Brings the account up to now, emptying the slices that have come round again. A clock that
 * went backwards is taken to have stood still. */
static tern_time *slice_at(struct tern_duty *d, tern_time now) {
    int64_t n = now > 0 ? now / d->slice : 0;
    if (n > d->newest) {
        int64_t gone = n - d->newest;
        if (gone > TERN_DUTY_SLICES) {
            gone = TERN_DUTY_SLICES;
        }
        for (int64_t i = 0; i < gone; i++) {
            d->used[(n - i) % TERN_DUTY_SLICES] = 0;
        }
        d->newest = n;
    }
    return &d->used[d->newest % TERN_DUTY_SLICES];
}

tern_time tern_duty_used(struct tern_duty *d, tern_time now) {
    tern_time sum = 0;
    if (d->limit < 0) {
        return 0;
    }
    (void)slice_at(d, now);
    for (int i = 0; i < TERN_DUTY_SLICES; i++) {
        sum += d->used[i];
    }
    return sum;
}

bool tern_duty_allows(struct tern_duty *d, tern_time now, tern_time airtime) {
    return d->limit < 0 || (airtime >= 0 && tern_duty_used(d, now) <= d->limit - airtime);
}

void tern_duty_charge(struct tern_duty *d, tern_time now, tern_time airtime) {
    if (d->limit >= 0 && airtime > 0) {
        *slice_at(d, now) += airtime;
    }
}
