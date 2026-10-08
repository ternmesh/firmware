#include "tern/flood.h"

#include <string.h>

#include "tern/crypto.h"
#include "tern/lora.h"

#define MILLION 1000000

void tern_flood_id(const uint8_t *frame, size_t len, uint8_t id[TERN_FLOOD_ID]) {
    struct tern_sha256 c;
    uint8_t h[TERN_SHA256_LEN];
    tern_sha256_init(&c);
    if (len > TERN_FLOOD_HEAD) {
        tern_sha256_update(&c, frame + TERN_FLOOD_HEAD, len - TERN_FLOOD_HEAD);
    }
    tern_sha256_final(&c, h);
    memcpy(id, h, TERN_FLOOD_ID);
}

int tern_flood_passes(bool relay, unsigned neighbours, uint8_t hops, uint8_t most, uint8_t sparse) {
    if (hops > most) {
        hops = most;
    }
    if (!relay || hops == 0) {
        return -1;
    }
    if (neighbours <= sparse) {
        return hops; /* a bridge spends no hop */
    }
    return hops > 1 ? hops - 1 : -1;
}

int8_t tern_flood_power(int8_t every, const int32_t *floors, size_t n, uint8_t margin_db,
                        int8_t lowest, int8_t full) {
    int32_t need = every;
    for (size_t i = 0; i < n; i++) {
        if (floors[i] == TERN_FLOOD_FLOOR_UNKNOWN) {
            return full;
        }
        int32_t sixteenths = floors[i] + 16 * (int32_t)margin_db;
        /* Rounded up to a whole dBm, whichever side of zero it is. */
        int32_t p = sixteenths >= 0 ? (sixteenths + 15) / 16 : -(-sixteenths / 16);
        need = p > need ? p : need;
    }
    return (int8_t)(need < lowest ? lowest : need > full ? full : need);
}

void tern_flood_bucket_init(struct tern_flood_bucket *b, uint32_t ppm, tern_time window,
                            tern_time frame, tern_time now) {
    b->ppm = ppm ? ppm : 1;
    b->most = window * b->ppm > frame * MILLION ? window * b->ppm : frame * MILLION;
    b->have = b->most;
    b->at = now;
}

static void refill(struct tern_flood_bucket *b, tern_time now) {
    tern_time dt = now - b->at;
    if (dt > 0) {
        b->have = dt >= b->most / b->ppm + 1 ? b->most : b->have + dt * b->ppm;
        b->have = b->have > b->most ? b->most : b->have;
        b->at = now;
    }
}

bool tern_flood_bucket_pays(struct tern_flood_bucket *b, tern_time now, tern_time airtime) {
    refill(b, now);
    if (b->have < airtime * MILLION) {
        return false;
    }
    b->have -= airtime * MILLION;
    return true;
}

/* When a bucket that cannot pay for `airtime` now will be able to. */
static tern_time pays_at(const struct tern_flood_bucket *b, tern_time airtime) {
    int64_t short_by = airtime * MILLION - b->have;
    return short_by <= 0 ? b->at : b->at + (short_by + b->ppm - 1) / b->ppm;
}

struct tern_flood_config tern_flood_defaults(void) {
    return (struct tern_flood_config){
        .hops = 5,
        .sparse = 8,
        .wait = 8,
        .copies = 2,
        .own_ppm = 5000,
        .own_window = TERN_S(600),
        .relay_ppm = 30000,
        .relay_window = TERN_S(60),
    };
}

static tern_time airtime(const struct tern_flood *f, size_t len) {
    return tern_lora_airtime(&f->route->config.lora, (uint32_t)len);
}

void tern_flood_init(struct tern_flood *f, const struct tern_flood_config *config,
                     struct tern_route *route, struct tern_flood_slot *slots, size_t cap,
                     uint8_t (*seen)[TERN_FLOOD_ID], size_t seen_cap, uint64_t seed,
                     tern_time now) {
    *f = (struct tern_flood){
        .config = *config,
        .route = route,
        .slot = slots,
        .cap = cap > TERN_FLOOD_SLOTS_MAX ? TERN_FLOOD_SLOTS_MAX : cap,
        .seen = seen,
        .seen_cap = seen_cap,
        .rng = seed ? seed : 0x9E3779B97F4A7C15ULL,
    };
    for (size_t i = 0; i < f->cap; i++) {
        f->slot[i].state = TERN_FLOOD_FREE;
    }
    tern_time longest = airtime(f, TERN_FLOOD_FRAME_MAX);
    tern_flood_bucket_init(&f->own, config->own_ppm, config->own_window, longest, now);
    tern_flood_bucket_init(&f->relay, config->relay_ppm, config->relay_window, longest, now);
}

static uint64_t rand_below(struct tern_flood *f, uint64_t n) {
    f->rng ^= f->rng >> 12;
    f->rng ^= f->rng << 25;
    f->rng ^= f->rng >> 27;
    return n ? ((f->rng * 0x2545F4914F6CDD1DULL) >> 16) % n : 0;
}

static bool seen(const struct tern_flood *f, const uint8_t id[TERN_FLOOD_ID]) {
    for (size_t i = 0; i < f->seen_count; i++) {
        if (memcmp(f->seen[i], id, TERN_FLOOD_ID) == 0) {
            return true;
        }
    }
    return false;
}

static void take(struct tern_flood *f, const uint8_t id[TERN_FLOOD_ID]) {
    if (f->seen_cap == 0) {
        return;
    }
    memcpy(f->seen[f->seen_next], id, TERN_FLOOD_ID);
    f->seen_next = (f->seen_next + 1) % f->seen_cap;
    f->seen_count += f->seen_count < f->seen_cap;
}

static struct tern_flood_slot *free_slot(struct tern_flood *f) {
    for (size_t i = 0; i < f->cap; i++) {
        if (f->slot[i].state == TERN_FLOOD_FREE) {
            return &f->slot[i];
        }
    }
    return NULL;
}

/* The relay neighbours: those the node may use that are relays. */
static unsigned relay_neighbours(const struct tern_route *r) {
    unsigned count = 0;
    for (size_t i = 0; i < r->nb_cap; i++) {
        count += r->nb[i].used && r->nb[i].relay && r->nb[i].up;
    }
    return count;
}

/* What a flooded frame goes at now: as a frame for every neighbour, and loud enough for every
 * relay neighbour a selected route goes through. */
static int8_t power(const struct tern_flood *f) {
    const struct tern_route *r = f->route;
    int32_t need = tern_route_power(r, TERN_ROUTE_EVERYONE);
    for (size_t i = 0; i < r->dest_cap; i++) {
        const struct tern_route_dest *d = &r->dest[i];
        if (!d->used || d->sel == 0 || d->sel > r->nb_cap) {
            continue;
        }
        const struct tern_route_neighbour *n = &r->nb[d->sel - 1];
        if (!n->used || !n->relay) {
            continue;
        }
        int8_t p = tern_flood_power((int8_t)need, &n->floor, 1, r->config.power_margin_db,
                                    r->config.min_dbm, r->config.full_dbm);
        need = p > need ? p : need;
    }
    return (int8_t)need;
}

bool tern_flood_send(struct tern_flood *f, tern_time now, const uint8_t *frame, size_t len) {
    struct tern_flood_slot *s;
    if (!tern_flood_frame(frame, len) || (s = free_slot(f)) == NULL) {
        return false;
    }
    *s = (struct tern_flood_slot){
        .state = TERN_FLOOD_WAITING, .own = true, .len = (uint8_t)len, .at = now};
    memcpy(s->frame, frame, len);
    s->frame[1] = f->config.hops;
    tern_flood_id(frame, len, s->id);
    take(f, s->id);
    return true;
}

bool tern_flood_heard(struct tern_flood *f, tern_time now, const uint8_t *frame, size_t len) {
    uint8_t id[TERN_FLOOD_ID];
    if (!tern_flood_frame(frame, len)) {
        return false;
    }
    tern_flood_id(frame, len, id);
    if (seen(f, id)) {
        f->counts.copies++;
        for (size_t i = 0; i < f->cap; i++) {
            struct tern_flood_slot *s = &f->slot[i];
            if (s->state == TERN_FLOOD_FREE || s->own || memcmp(s->id, id, TERN_FLOOD_ID) != 0) {
                continue;
            }
            if (s->copies < UINT8_MAX) {
                s->copies++;
            }
            if (f->config.copies && s->copies >= f->config.copies) {
                f->counts.cancelled++;
                if (s->state == TERN_FLOOD_OUT) {
                    s->dropped = true; /* freed once the caller lets go of it */
                } else {
                    s->state = TERN_FLOOD_FREE;
                }
            }
        }
        return false;
    }
    take(f, id);
    f->counts.heard++;

    int hops = tern_flood_passes(f->route->config.relay, relay_neighbours(f->route), frame[1],
                                 f->config.hops, f->config.sparse);
    if (hops < 0 || (f->config.copies && f->config.copies <= 1)) {
        f->counts.hop_limit += f->route->config.relay && hops < 0;
        return true;
    }
    struct tern_flood_slot *s = free_slot(f);
    if (s == NULL) {
        f->counts.no_room++;
        return true;
    }
    *s = (struct tern_flood_slot){.state = TERN_FLOOD_WAITING, .copies = 1, .len = (uint8_t)len};
    memcpy(s->frame, frame, len);
    memcpy(s->id, id, TERN_FLOOD_ID);
    s->frame[1] = (uint8_t)hops;
    s->at =
        now + (tern_time)rand_below(f, (uint64_t)f->config.wait * (uint64_t)airtime(f, len) + 1);
    f->counts.passed_on++;
    return true;
}

tern_time tern_flood_due(const struct tern_flood *f) {
    tern_time due = INT64_MAX;
    for (size_t i = 0; i < f->cap; i++) {
        const struct tern_flood_slot *s = &f->slot[i];
        if (s->state == TERN_FLOOD_WAITING && s->at < due) {
            due = s->at;
        }
    }
    return due;
}

size_t tern_flood_poll(struct tern_flood *f, tern_time now, uint8_t frame[TERN_FLOOD_FRAME_MAX],
                       int8_t *power_dbm, enum tern_flood_kind *kind, uint8_t *handle) {
    /* Frames passed on go before the node's own, as for frames that follow routes; of each, the
     * one that has waited longest. */
    for (int own = 0; own <= 1; own++) {
        for (;;) {
            struct tern_flood_slot *s = NULL;
            for (size_t i = 0; i < f->cap; i++) {
                struct tern_flood_slot *c = &f->slot[i];
                if (c->state == TERN_FLOOD_WAITING && c->own == (bool)own && c->at <= now &&
                    (s == NULL || c->at < s->at)) {
                    s = c;
                }
            }
            if (s == NULL) {
                break;
            }
            struct tern_flood_bucket *b = own ? &f->own : &f->relay;
            if (!tern_flood_bucket_pays(b, now, airtime(f, s->len))) {
                if (own) {
                    s->at = pays_at(b, airtime(f, s->len)); /* it waits for its allowance */
                    s->at = s->at > now ? s->at : now + 1;
                    break; /* and those behind it wait their turn */
                }
                s->state = TERN_FLOOD_FREE;
                f->counts.unpaid++;
                continue;
            }
            s->power = power(f);
            s->frame[2] = (uint8_t)s->power;
            s->state = TERN_FLOOD_OUT;
            s->dropped = false;
            memcpy(frame, s->frame, s->len);
            *power_dbm = s->power;
            *kind = own ? TERN_FLOOD_OWN : TERN_FLOOD_RELAY;
            *handle = (uint8_t)(s - f->slot);
            return s->len;
        }
    }
    return 0;
}

bool tern_flood_wanted(const struct tern_flood *f, uint8_t handle) {
    return handle < f->cap && f->slot[handle].state == TERN_FLOOD_OUT && !f->slot[handle].dropped;
}

void tern_flood_withdrawn(struct tern_flood *f, tern_time now, uint8_t handle) {
    if (handle >= f->cap || f->slot[handle].state != TERN_FLOOD_OUT) {
        return;
    }
    struct tern_flood_slot *s = &f->slot[handle];
    struct tern_flood_bucket *b = s->own ? &f->own : &f->relay;
    refill(b, now);
    b->have += airtime(f, s->len) * MILLION;
    b->have = b->have > b->most ? b->most : b->have;
    if (s->dropped || !s->own) {
        s->state = TERN_FLOOD_FREE; /* a frame passed on is offered once */
    } else {
        s->state = TERN_FLOOD_WAITING;
        s->at = now;
    }
}

void tern_flood_sent(struct tern_flood *f, tern_time now, uint8_t handle) {
    (void)now;
    if (handle < f->cap && f->slot[handle].state == TERN_FLOOD_OUT) {
        f->slot[handle].state = TERN_FLOOD_FREE;
    }
}
