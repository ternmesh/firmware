#ifndef TERN_AIRTIME_H
#define TERN_AIRTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tern/lora.h"
#include "tern/time.h"

/* The airtime budget: specification draft 0, draft/airtime.md in ternmesh/spec.
 *
 * How much of its time a node may spend sending, in three parts. Routing's is the router's cap
 * (tern/route.h), unchanged: nothing here adds to it or takes from it. The other two are a token
 * bucket each: one for the frames the node writes, 1% of its time held for ten minutes, and, for a
 * relay, one for the frames it passes on, 8% held for a minute. A leaf has no part for frames
 * passed on. The flooder's two allowances (tern/flood.h) stay, inside the parts: a flood is paid
 * for by its allowance and its part together, and the allowances are buckets of this kind.
 *
 * A frame of the node's own that its buckets do not hold waits until they do, behind any of its
 * own ahead of it; a frame passed on that they do not hold is dropped; an acknowledgement goes
 * whatever they hold, and is charged with the bucket going no lower than empty. A frame is charged
 * when it goes, and one that never goes, never. The region's limit on transmitting (tern/duty.h)
 * is a separate account, kept beside this one: a frame goes only once both allow it.
 *
 * Like the rest of the core it owns no clock or memory. The forwarder and the flooder are each
 * handed one (tern_forward_budget(), tern_flood_budget()) and keep to it themselves; a port asks
 * it how long the node's own frames will wait, which is what a client is shown with reason 4.
 *
 *     tern_airtime_init(&a, &config, &lora, relay, now);
 *     tern_forward_budget(&forward, &a);  tern_flood_budget(&flood, &a);
 *     ... tern_airtime_waits(&a, &flood.own, now, waiting, n, wait_s) for what a client is told
 *
 * Time is integer nanoseconds and a bucket counts millionths of them, so nothing is rounded: the
 * arithmetic is the specification's reference model's (vectors/tools/airtime.py) to the
 * nanosecond. Every parameter is provisional.
 *
 * What the forwarder and the flooder ask of a frame is defined here, inline, so that each still
 * builds from its own file and the ones it built from before: the simulator builds them so. */

#define TERN_AIRTIME_MILLION 1000000

/* A token bucket: nanoseconds of airtime, in millionths. It fills at `ppm` millionths of the
 * node's time and holds `most`. */
struct tern_airtime_bucket {
    int64_t have;
    int64_t most;
    uint32_t ppm;
    tern_time at; /* when `have` was last brought up to date */
};

/* A bucket that fills at `ppm` millionths of the node's time and holds `window` of that, or
 * `frame`, the airtime of the longest frame, if that is more. It starts full. */
static inline void tern_airtime_bucket_init(struct tern_airtime_bucket *b, uint32_t ppm,
                                            tern_time window, tern_time frame, tern_time now) {
    b->ppm = ppm ? ppm : 1;
    b->most = window * b->ppm > frame * TERN_AIRTIME_MILLION ? window * b->ppm
                                                             : frame * TERN_AIRTIME_MILLION;
    b->have = b->most;
    b->at = now;
}

/* Brings a bucket up to `now`. Past the time it takes to fill from empty it is full, which keeps a
 * long time without a frame from overflowing. */
static inline void tern_airtime_bucket_fill(struct tern_airtime_bucket *b, tern_time now) {
    tern_time dt = now - b->at;
    if (dt > 0) {
        b->have = dt >= b->most / b->ppm + 1 ? b->most : b->have + dt * b->ppm;
        b->have = b->have > b->most ? b->most : b->have;
        b->at = now;
    }
}

/* Whether the bucket holds `airtime` at `now`: has at least that much. Brings it up to date, and
 * takes nothing. */
static inline bool tern_airtime_bucket_holds(struct tern_airtime_bucket *b, tern_time now,
                                             tern_time airtime) {
    tern_airtime_bucket_fill(b, now);
    return b->have >= airtime * TERN_AIRTIME_MILLION;
}

/* Takes `airtime` at `now`, the bucket going no lower than empty. */
static inline void tern_airtime_bucket_charge(struct tern_airtime_bucket *b, tern_time now,
                                              tern_time airtime) {
    tern_airtime_bucket_fill(b, now);
    b->have =
        b->have > airtime * TERN_AIRTIME_MILLION ? b->have - airtime * TERN_AIRTIME_MILLION : 0;
}

/* Gives `airtime` back at `now`, the bucket going no higher than full: for a frame charged that
 * then did not go. */
static inline void tern_airtime_bucket_give(struct tern_airtime_bucket *b, tern_time now,
                                            tern_time airtime) {
    tern_airtime_bucket_fill(b, now);
    b->have += airtime * TERN_AIRTIME_MILLION;
    b->have = b->have > b->most ? b->most : b->have;
}

/* How long after `now` the bucket holds `airtime`, if nothing is charged to it meanwhile: 0 if it
 * does now. Changes nothing. */
static inline tern_time tern_airtime_bucket_wait(const struct tern_airtime_bucket *b, tern_time now,
                                                 tern_time airtime) {
    struct tern_airtime_bucket c = *b; /* a copy: asking spends nothing */
    tern_airtime_bucket_fill(&c, now);
    int64_t short_by = airtime * TERN_AIRTIME_MILLION - c.have;
    return short_by <= 0 ? 0 : (short_by + c.ppm - 1) / c.ppm;
}

/* --- Rules, each as the specification states it, checked against its vectors. --- */

/* The hdrs the rules tell apart, as their own headers name them (src/airtime.c checks that they
 * agree). */
#define TERN_AIRTIME_HDR_ACK 0x50      /* TERN_HDR_ACK, tern/forward.h */
#define TERN_AIRTIME_HDR_ANNOUNCE 0x59 /* TERN_HDR_ANNOUNCE and _REQUEST, tern/route.h */
#define TERN_AIRTIME_HDR_REQUEST 0x5A
#define TERN_AIRTIME_HDR_GROUP 0x60 /* TERN_HDR_GROUP, _GROUP_NODE and _CARD, tern/flood.h */
#define TERN_AIRTIME_HDR_GROUP_NODE 0x61
#define TERN_AIRTIME_HDR_CARD 0x68

enum tern_airtime_part {
    TERN_AIRTIME_ROUTING, /* announces and requests: the router's cap, not this */
    TERN_AIRTIME_OWN,     /* the node's own frames */
    TERN_AIRTIME_RELAYED, /* frames it passes on */
};

/* The part that pays for a frame with this hdr, the node's own or passed on. Every frame but an
 * announce or request is paid for here: a message, an acknowledgement, a first-contact frame or a
 * flood, and anything else a node might send. */
static inline enum tern_airtime_part tern_airtime_part(uint8_t hdr, bool own) {
    if (hdr == TERN_AIRTIME_HDR_ANNOUNCE || hdr == TERN_AIRTIME_HDR_REQUEST) {
        return TERN_AIRTIME_ROUTING;
    }
    return own ? TERN_AIRTIME_OWN : TERN_AIRTIME_RELAYED;
}

/* Whether one of the flooder's allowances pays for it too: the one for the node's own floods, or
 * the one for those it passes on, as `own` says. */
static inline bool tern_airtime_flooded(uint8_t hdr) {
    return hdr == TERN_AIRTIME_HDR_GROUP || hdr == TERN_AIRTIME_HDR_GROUP_NODE ||
           hdr == TERN_AIRTIME_HDR_CARD;
}

enum tern_airtime_unpaid {
    TERN_AIRTIME_ROUTES,  /* the router's cap decides, not the budget */
    TERN_AIRTIME_WAITS,   /* until its buckets hold it */
    TERN_AIRTIME_DROPPED, /* not sent at all */
    TERN_AIRTIME_SENT,    /* anyway, and charged, its bucket going no lower than empty */
};

/* What becomes of a frame with this hdr when the buckets that pay for it do not all hold it. An
 * acknowledgement ends a message whose every hop is paid for already, so it is never held. */
static inline enum tern_airtime_unpaid tern_airtime_unpaid(uint8_t hdr, bool own) {
    if (tern_airtime_part(hdr, own) == TERN_AIRTIME_ROUTING) {
        return TERN_AIRTIME_ROUTES;
    }
    if (hdr == TERN_AIRTIME_HDR_ACK) {
        return TERN_AIRTIME_SENT;
    }
    return own ? TERN_AIRTIME_WAITS : TERN_AIRTIME_DROPPED;
}

/* The reason and wait the companion protocol (tern/companion.h) shows a waiting message of the
 * node's own with, as far as the budget has them: 1 with no route, 2 with no session, and
 * otherwise whichever of 3, the region's limit, and 4, the budget, lets it go later, 3 if both at
 * once. The waits are in seconds; `wait` is written, 0 for 1 and 2 and at most 0xFFFF. */
uint8_t tern_airtime_reason(bool route, bool session, uint32_t region_wait_s,
                            uint32_t budget_wait_s, uint16_t *wait);

/* --- The budget --- */

struct tern_airtime_config {
    uint32_t own_ppm;
    tern_time own_window;
    uint32_t relay_ppm; /* a relay's; a leaf has no part for frames passed on */
    tern_time relay_window;
};

/* The specification's parameters. */
struct tern_airtime_config tern_airtime_defaults(void);

struct tern_airtime {
    bool relay;
    struct tern_airtime_bucket own;
    struct tern_airtime_bucket relayed; /* a relay's only */
};

/* Starts a budget for a node sending with these settings, a relay or not, every bucket full. A
 * node that starts again starts its budget full: there is nothing to keep across a restart. */
void tern_airtime_init(struct tern_airtime *a, const struct tern_airtime_config *config,
                       const struct tern_lora *lora, bool relay, tern_time now);

/* The bucket of the budget that pays for a frame, or NULL for one that none of its buckets pays
 * for: an announce or request, or on a leaf a frame passed on. */
static inline struct tern_airtime_bucket *tern_airtime_bucket(struct tern_airtime *a, uint8_t hdr,
                                                              bool own) {
    switch (tern_airtime_part(hdr, own)) {
    case TERN_AIRTIME_OWN:
        return &a->own;
    case TERN_AIRTIME_RELAYED:
        return a->relay ? &a->relayed : NULL;
    default:
        return NULL;
    }
}

/* A frame that goes though no bucket of the budget pays for it: an announce or request, which the
 * router's cap decides, or an acknowledgement, which is never held. */
static inline bool tern_airtime_free(uint8_t hdr, bool own) {
    enum tern_airtime_unpaid u = tern_airtime_unpaid(hdr, own);
    return u == TERN_AIRTIME_ROUTES || u == TERN_AIRTIME_SENT;
}

/* Whether the budget lets a frame of `airtime` go at `now`, as far as its own parts say: an
 * acknowledgement or an announce or request always, a frame its part holds, and on a leaf no
 * frame passed on but an acknowledgement. A flood's allowance is the flooder's to ask. Takes
 * nothing. */
static inline bool tern_airtime_pays(struct tern_airtime *a, tern_time now, uint8_t hdr, bool own,
                                     tern_time airtime) {
    struct tern_airtime_bucket *b = tern_airtime_bucket(a, hdr, own);
    return tern_airtime_free(hdr, own) || (b != NULL && tern_airtime_bucket_holds(b, now, airtime));
}

/* When a frame that tern_airtime_pays() refuses could go, if nothing is charged meanwhile: `now`
 * if it could now, INT64_MAX if never (a leaf's frame passed on). Changes nothing. */
static inline tern_time tern_airtime_pays_at(struct tern_airtime *a, tern_time now, uint8_t hdr,
                                             bool own, tern_time airtime) {
    const struct tern_airtime_bucket *b = tern_airtime_bucket(a, hdr, own);
    return tern_airtime_free(hdr, own) ? now
           : b == NULL                 ? INT64_MAX
                                       : now + tern_airtime_bucket_wait(b, now, airtime);
}

/* A frame went on the air at `now`: its airtime is taken from its part, going no lower than
 * empty. A flood's allowance is the flooder's to charge. */
static inline void tern_airtime_charge(struct tern_airtime *a, tern_time now, uint8_t hdr, bool own,
                                       tern_time airtime) {
    struct tern_airtime_bucket *b = tern_airtime_bucket(a, hdr, own);
    if (b != NULL) {
        tern_airtime_bucket_charge(b, now, airtime);
    }
}

/* A frame of the node's own, waiting. */
struct tern_airtime_waiting {
    uint8_t hdr;
    tern_time airtime;
};

/* For each of the node's own frames waiting, in the order they are to go, how many seconds after
 * `now` it goes if nothing else is charged meanwhile: once every bucket that pays for it holds its
 * airtime, and not before the frame ahead of it, each charged as it goes. Rounded up, and at most
 * 0xFFFF. `flood_own` is the flooder's allowance for the node's own floods, which pays for a
 * flood too; NULL counts only the budget's own part. Changes nothing. */
void tern_airtime_waits(const struct tern_airtime *a, const struct tern_airtime_bucket *flood_own,
                        tern_time now, const struct tern_airtime_waiting *waiting, size_t n,
                        uint16_t *wait_s);

#endif
