#include "tern/airtime.h"

#include "tern/companion.h"
#include "tern/flood.h"
#include "tern/forward.h"
#include "tern/route.h"

#define WAIT_MAX 0xFFFF

_Static_assert(TERN_AIRTIME_HDR_ACK == TERN_HDR_ACK, "an acknowledgement's hdr");
_Static_assert(TERN_AIRTIME_HDR_ANNOUNCE == TERN_HDR_ANNOUNCE &&
                   TERN_AIRTIME_HDR_REQUEST == TERN_HDR_REQUEST,
               "routing's hdrs");
_Static_assert(TERN_AIRTIME_HDR_GROUP == TERN_HDR_GROUP &&
                   TERN_AIRTIME_HDR_GROUP_NODE == TERN_HDR_GROUP_NODE &&
                   TERN_AIRTIME_HDR_CARD == TERN_HDR_CARD,
               "the flooded hdrs");
_Static_assert(TERN_C_WAIT_REGION == 3 && TERN_C_WAIT_BUDGET == 4, "the companion's reasons");

uint8_t tern_airtime_reason(bool route, bool session, uint32_t region_wait_s,
                            uint32_t budget_wait_s, uint16_t *wait) {
    *wait = 0;
    if (!route) {
        return TERN_C_WAIT_ROUTE;
    }
    if (!session) {
        return TERN_C_WAIT_SESSION;
    }
    /* The one that lets it go later, since that is when it goes; the region's where both let it go
     * at once, since no setting changes it. */
    if (region_wait_s >= budget_wait_s) {
        *wait = (uint16_t)(region_wait_s > WAIT_MAX ? WAIT_MAX : region_wait_s);
        return TERN_C_WAIT_REGION;
    }
    *wait = (uint16_t)(budget_wait_s > WAIT_MAX ? WAIT_MAX : budget_wait_s);
    return TERN_C_WAIT_BUDGET;
}

struct tern_airtime_config tern_airtime_defaults(void) {
    return (struct tern_airtime_config){
        .own_ppm = 10000,
        .own_window = TERN_S(600),
        .relay_ppm = 80000,
        .relay_window = TERN_S(60),
    };
}

void tern_airtime_init(struct tern_airtime *a, const struct tern_airtime_config *config,
                       const struct tern_lora *lora, bool relay, tern_time now) {
    tern_time longest = tern_lora_airtime(lora, TERN_FLOOD_FRAME_MAX);
    longest = longest > 0 ? longest : 0;
    *a = (struct tern_airtime){.relay = relay};
    tern_airtime_bucket_init(&a->own, config->own_ppm, config->own_window, longest, now);
    tern_airtime_bucket_init(&a->relayed, config->relay_ppm, config->relay_window, longest, now);
}

void tern_airtime_waits(const struct tern_airtime *a, const struct tern_airtime_bucket *flood_own,
                        tern_time now, const struct tern_airtime_waiting *waiting, size_t n,
                        uint16_t *wait_s) {
    /* Copies, charged as each frame goes in turn. */
    struct tern_airtime_bucket own = a->own;
    struct tern_airtime_bucket flood = flood_own ? *flood_own : own;
    tern_time t = now;
    for (size_t i = 0; i < n; i++) {
        tern_time air = waiting[i].airtime;
        bool flooded = flood_own != NULL && tern_airtime_flooded(waiting[i].hdr);
        tern_time w = tern_airtime_bucket_wait(&own, t, air);
        if (flooded) {
            tern_time fw = tern_airtime_bucket_wait(&flood, t, air);
            w = fw > w ? fw : w;
        }
        t += w; /* not before the frame ahead of it */
        tern_airtime_bucket_charge(&own, t, air);
        if (flooded) {
            tern_airtime_bucket_charge(&flood, t, air);
        }
        tern_time s = (t - now + TERN_S(1) - 1) / TERN_S(1);
        wait_s[i] = (uint16_t)(s > WAIT_MAX ? WAIT_MAX : s);
    }
}
