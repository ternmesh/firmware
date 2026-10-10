#include "tern/route.h"

#include <string.h>

#include "tern/crypto.h"

#define INF TERN_ROUTE_INF
#define REF_LEN 32      /* bytes of the frame the metric is reckoned in */
#define NAMED_ROUNDS 8  /* rounds a margin outlasts */
#define AGE_MAX 0x7FFFu /* the most announces a margin may outlast, whatever the round */
#define LINK_MARGIN 0   /* sixteenths of a decibel */
#define LINK_BAND (3 * 16)
#define REPLACE_BAND (6 * 16)
#define MARGIN_ZERO 128
#define RETRACTS 3
#define ASK_FRAME 62 /* a request frame of eight, which a request's jitter is reckoned in */
#define FLAG_RELAY 0x01
#define FLAG_STARTING 0x02
#define FLAG_ADDRESS 0x04
#define NS_PER_S 1000000000LL
#define MILLION 1000000LL

/* --- Frames --- */

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, (uint16_t)(v >> 16));
    put16(p + 2, (uint16_t)v);
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) << 16 | get16(p + 2); }

static bool reserved(uint32_t id) { return id == 0 || id == TERN_ROUTE_EVERYONE; }

size_t tern_announce_write(const struct tern_announce *a, uint8_t frame[TERN_ROUTE_FRAME_MAX]) {
    size_t carried = a->carries_address ? TERN_ADDRESS_LEN : 0;
    size_t len = TERN_ANNOUNCE_LEAST + carried + 5u * a->named_count + 8u * a->route_count;
    uint8_t *p = frame + TERN_ANNOUNCE_HEAD + carried;
    if (len > TERN_ROUTE_FRAME_MAX || a->named_count > TERN_ANNOUNCE_NAMED_MAX ||
        a->route_count > TERN_ANNOUNCE_ROUTES_MAX) {
        return 0;
    }
    frame[0] = TERN_HDR_ANNOUNCE;
    put32(frame + 1, a->sender);
    put16(frame + 5, a->number);
    put16(frame + 7, a->seq);
    frame[9] = (uint8_t)((a->relay ? FLAG_RELAY : 0) | (a->starting ? FLAG_STARTING : 0) |
                         (a->carries_address ? FLAG_ADDRESS : 0));
    put16(frame + 10, a->promise);
    put16(frame + 12, a->round);
    frame[14] = (uint8_t)a->power;
    frame[15] = a->named_count;
    frame[16] = a->route_count;
    if (carried) {
        memcpy(frame + TERN_ANNOUNCE_HEAD, a->address, TERN_ADDRESS_LEN);
    }
    for (int i = 0; i < a->named_count; i++, p += 5) {
        put32(p, a->named[i].id);
        p[4] = a->named[i].margin;
    }
    for (int i = 0; i < a->route_count; i++, p += 8) {
        put32(p, a->routes[i].destination);
        put16(p + 4, a->routes[i].seq);
        put16(p + 6, a->routes[i].metric);
    }
    memcpy(p, a->sig, TERN_ANNOUNCE_SIG);
    return len;
}

size_t tern_announce_signed(const uint8_t *frame, size_t len,
                            uint8_t m[TERN_ANNOUNCE_LABEL_LEN + TERN_ROUTE_FRAME_MAX]) {
    size_t body = len > TERN_ANNOUNCE_SIG ? len - TERN_ANNOUNCE_SIG : 0;
    body = body > TERN_ROUTE_FRAME_MAX ? TERN_ROUTE_FRAME_MAX : body;
    memcpy(m, TERN_ANNOUNCE_LABEL, TERN_ANNOUNCE_LABEL_LEN);
    memcpy(m + TERN_ANNOUNCE_LABEL_LEN, frame, body);
    return TERN_ANNOUNCE_LABEL_LEN + body;
}

bool tern_announce_read(struct tern_announce *a, const uint8_t *frame, size_t len) {
    size_t carried = len > 9 && (frame[9] & FLAG_ADDRESS) ? TERN_ADDRESS_LEN : 0;
    const uint8_t *p = frame + TERN_ANNOUNCE_HEAD + carried;
    if (len < TERN_ANNOUNCE_LEAST || len > TERN_ROUTE_FRAME_MAX || frame[0] != TERN_HDR_ANNOUNCE ||
        len != TERN_ANNOUNCE_LEAST + carried + 5u * frame[15] + 8u * frame[16] ||
        (frame[9] & ~(FLAG_RELAY | FLAG_STARTING | FLAG_ADDRESS)) != 0 ||
        reserved(get32(frame + 1))) {
        return false;
    }
    a->sender = get32(frame + 1);
    a->number = get16(frame + 5);
    a->seq = get16(frame + 7);
    a->relay = (frame[9] & FLAG_RELAY) != 0;
    a->starting = (frame[9] & FLAG_STARTING) != 0;
    a->promise = get16(frame + 10);
    a->round = get16(frame + 12);
    a->power = (int8_t)frame[14];
    a->named_count = frame[15];
    a->route_count = frame[16];
    a->carries_address = carried != 0;
    if (carried) {
        memcpy(a->address, frame + TERN_ANNOUNCE_HEAD, TERN_ADDRESS_LEN);
    }
    for (int i = 0; i < a->named_count; i++, p += 5) {
        a->named[i].id = get32(p);
        a->named[i].margin = p[4];
    }
    for (int i = 0; i < a->route_count; i++, p += 8) {
        a->routes[i].destination = get32(p);
        a->routes[i].seq = get16(p + 4);
        a->routes[i].metric = get16(p + 6);
    }
    memcpy(a->sig, p, TERN_ANNOUNCE_SIG);
    return true;
}

size_t tern_request_write(const struct tern_request *q, uint8_t frame[TERN_ROUTE_FRAME_MAX]) {
    uint8_t *p = frame + TERN_REQUEST_HEAD;
    if (q->count == 0 || q->count > TERN_REQUEST_MAX) {
        return 0;
    }
    frame[0] = TERN_HDR_REQUEST;
    put32(frame + 1, q->next);
    frame[5] = q->count;
    for (int i = 0; i < q->count; i++, p += 7) {
        put32(p, q->asks[i].destination);
        put16(p + 4, q->asks[i].seq);
        p[6] = q->asks[i].hops;
    }
    return TERN_REQUEST_HEAD + 7u * q->count;
}

bool tern_request_read(struct tern_request *q, const uint8_t *frame, size_t len) {
    const uint8_t *p = frame + TERN_REQUEST_HEAD;
    if (len < TERN_REQUEST_HEAD || len > TERN_ROUTE_FRAME_MAX || frame[0] != TERN_HDR_REQUEST ||
        frame[5] == 0 || len != TERN_REQUEST_HEAD + 7u * frame[5]) {
        return false;
    }
    q->next = get32(frame + 1);
    q->count = frame[5];
    for (int i = 0; i < q->count; i++, p += 7) {
        q->asks[i].destination = get32(p);
        q->asks[i].seq = get16(p + 4);
        q->asks[i].hops = p[6];
    }
    return true;
}

/* --- Rules --- */

uint32_t tern_route_id(const uint8_t address[TERN_ADDRESS_LEN]) {
    static const uint8_t label[] = "tern routing id";
    struct tern_sha256 c;
    uint8_t h[TERN_SHA256_LEN];
    tern_sha256_init(&c);
    tern_sha256_update(&c, label, sizeof label - 1);
    tern_sha256_update(&c, address, TERN_ADDRESS_LEN);
    tern_sha256_final(&c, h);
    for (int i = 0; i < TERN_SHA256_LEN; i += 4) {
        if (!reserved(get32(h + i))) {
            return get32(h + i);
        }
    }
    return 1; /* one chance in 2^248 */
}

bool tern_route_newer(uint16_t a, uint16_t b) {
    uint16_t d = (uint16_t)(a - b);
    return d != 0 && d < 0x8000;
}

uint16_t tern_route_promise_code(uint64_t seconds) {
    uint64_t minutes = (seconds + 59) / 60;
    if (seconds <= 0x7FFF) {
        return (uint16_t)seconds;
    }
    return minutes <= 32766 ? (uint16_t)(0x8000u | minutes) : 0xFFFF;
}

tern_time tern_route_promise_time(uint16_t code) {
    return code == 0xFFFF  ? TERN_ROUTE_NO_PROMISE
           : code & 0x8000 ? TERN_S(60) * (code & 0x7FFF)
                           : TERN_S(1) * code;
}

/* a / b, rounded down, for b above 0. */
static int32_t div_down(int32_t a, int32_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

int32_t tern_route_floor(bool first, int32_t floor, int8_t power, int16_t snr_q, uint8_t sf) {
    /* Semtech's demodulation floors: -7.5 dB at SF7, and 2.5 dB lower for each SF above. */
    int32_t snr_floor_q = -30 - 10 * ((int32_t)sf - 7);
    int32_t sample = 16 * (int32_t)power - 4 * ((int32_t)snr_q - snr_floor_q);
    return first ? sample : div_down(3 * floor + sample, 4);
}

uint8_t tern_route_margin(int8_t full, int32_t floor) {
    int32_t m = div_down(16 * (int32_t)full - floor, 16) + MARGIN_ZERO;
    return (uint8_t)(m < 1 ? 1 : m > 255 ? 255 : m);
}

bool tern_route_link_up(bool up, int32_t own, uint8_t theirs) {
    int32_t their = 16 * ((int32_t)theirs - MARGIN_ZERO);
    return theirs != 0 &&
           (own < their ? own : their) >= (up ? LINK_MARGIN - LINK_BAND : LINK_MARGIN);
}

int tern_route_place(const struct tern_route_neighbour *full, size_t n, int32_t floor) {
    int worst = -1;
    for (size_t i = 0; i < n; i++) {
        if (!full[i].up && (worst < 0 || full[i].floor > full[worst].floor)) {
            worst = (int)i;
        }
    }
    return worst >= 0 && floor + REPLACE_BAND <= full[worst].floor ? worst : -1;
}

bool tern_route_withdrawn(uint16_t named, uint16_t number, uint16_t round) {
    uint32_t allowed = NAMED_ROUNDS * (round > 1 ? (uint32_t)round : 1u) + 1u;
    return (uint16_t)(number - named) >= (allowed > AGE_MAX ? AGE_MAX : allowed);
}

enum tern_route_numbering tern_route_numbering(uint16_t last, uint16_t number, bool promise_passed,
                                               bool starting, bool was_starting) {
    uint16_t gap = (uint16_t)(number - last);
    if (starting) {
        return was_starting ? TERN_ROUTE_TAKE : TERN_ROUTE_AGAIN;
    }
    if (gap == 0 || gap >= 0x8000) {
        /* A copy, or late; or, after a promise of silence, it started again unheard. */
        return promise_passed ? TERN_ROUTE_AGAIN : TERN_ROUTE_DISCARD;
    }
    return TERN_ROUTE_TAKE;
}

uint16_t tern_route_link_cost(const struct tern_lora *lora) {
    tern_time ms = (tern_lora_airtime(lora, REF_LEN) + MILLION - 1) / MILLION;
    return (uint16_t)(ms < 1 ? 1 : ms >= INF ? INF - 1 : ms);
}

bool tern_route_feasible(const struct tern_route_fd *fd, uint16_t seq, uint16_t metric) {
    return !fd->has || tern_route_newer(seq, fd->seq) || (seq == fd->seq && metric < fd->metric);
}

static uint16_t total(uint16_t metric, uint16_t cost) {
    uint32_t t = (uint32_t)metric + cost;
    return metric == INF || cost == INF ? INF : t >= INF ? INF - 1 : (uint16_t)t;
}

int tern_route_select(const struct tern_route_fd *fd, const struct tern_route_choice *routes, int n,
                      int selected) {
    int best = -1;
    uint32_t best_total = INF, cur_total = INF;
    for (int i = 0; i < n; i++) {
        uint16_t t = total(routes[i].metric, routes[i].cost);
        if (t == INF || !tern_route_feasible(fd, routes[i].seq, routes[i].metric)) {
            continue;
        }
        if (i == selected) {
            cur_total = t;
        }
        if (t < best_total) {
            best = i;
            best_total = t;
        }
    }
    /* The route selected is kept unless another is lower by more than a tenth of it. */
    if (cur_total != INF && best != selected && !(10 * best_total < 9 * cur_total)) {
        return selected;
    }
    return best;
}

/* What a route is worth keeping. */
struct rank {
    bool selectable;
    uint16_t seq;
    uint16_t total;
};

static struct rank rank_of(const struct tern_route_fd *fd, const struct tern_route_choice *c) {
    uint16_t t = total(c->metric, c->cost);
    return (struct rank){t != INF && tern_route_feasible(fd, c->seq, c->metric), c->seq, t};
}

static bool below(struct rank a, struct rank b) {
    if (a.selectable != b.selectable) {
        return b.selectable;
    }
    if (a.seq != b.seq) {
        return tern_route_newer(b.seq, a.seq);
    }
    return a.total > b.total;
}

int tern_route_replace(const struct tern_route_fd *fd, const struct tern_route_choice *routes,
                       int selected, const struct tern_route_choice *offered) {
    int place = -1;
    struct rank worst = {0};
    for (int i = 0; i < TERN_ROUTE_KEPT; i++) {
        struct rank k = rank_of(fd, &routes[i]);
        if (i != selected && (place < 0 || !below(worst, k))) {
            worst = k;
            place = i;
        }
    }
    return place >= 0 && below(worst, rank_of(fd, offered)) ? place : -1;
}

/* --- The router --- */

struct tern_route_config tern_route_defaults(const struct tern_lora *lora, int8_t full_dbm,
                                             int8_t min_dbm, bool relay) {
    return (struct tern_route_config){
        .lora = *lora,
        .full_dbm = full_dbm,
        .min_dbm = min_dbm < full_dbm ? min_dbm : full_dbm,
        .relay = relay,
        .i_min = TERN_S(8),
        .doublings = 6,
        .redundancy = 3,
        .quiet_max = 2,
        .announce_ppm = 3750, /* a cap of 0.5%, a quarter of it for requests */
        .request_ppm = 1250,
        .cap_window = TERN_S(60),
        .burst = 4,
        .named_max = 8,
        .silent_max = TERN_S(24 * 3600),
        .power_k = 8,
        .power_margin_db = 10,
        .request_interval = TERN_S(10),
        .request_tries = 5,
        .hop_max = 32,
        .step_db = 3,
        .dead_hops = 24,
        .jitter = 2,
        .start_announces = 4,
        .default_hops = 6,
        .default_busy_ppm = 500000,
    };
}

static uint64_t rand_below(struct tern_route *r, uint64_t n) {
    r->rng ^= r->rng >> 12;
    r->rng ^= r->rng << 25;
    r->rng ^= r->rng >> 27;
    return n ? ((r->rng * 0x2545F4914F6CDD1DULL) >> 16) % n : 0;
}

static tern_time airtime(const struct tern_route *r, size_t len) {
    return tern_lora_airtime(&r->config.lora, (uint32_t)len);
}

static tern_time i_max(const struct tern_route *r) {
    return r->config.i_min << r->config.doublings;
}

static struct tern_route_neighbour *slot(const struct tern_route *r, uint8_t s) {
    return &r->nb[s - 1];
}

static uint8_t slot_of(const struct tern_route *r, uint32_t id) {
    for (size_t i = 0; i < r->nb_cap; i++) {
        if (r->nb[i].used && r->nb[i].id == id) {
            return (uint8_t)(i + 1);
        }
    }
    return 0;
}

static struct tern_route_dest *dest_of(const struct tern_route *r, uint32_t id) {
    for (size_t i = 0; i < r->dest_cap; i++) {
        if (r->dest[i].used && r->dest[i].id == id) {
            return &r->dest[i];
        }
    }
    return NULL;
}

/* A place for a destination new to the node. With the table full it takes one the node holds
 * nothing for but a feasibility distance: as Babel does when it collects its source table, and at
 * the same price, that a route the node once announced may be selected again. */
static struct tern_route_dest *dest_make(struct tern_route *r, uint32_t id) {
    struct tern_route_dest *d = dest_of(r, id), *idle = NULL;
    for (size_t i = 0; !d && i < r->dest_cap; i++) {
        struct tern_route_dest *x = &r->dest[i];
        bool held = false;
        for (int k = 0; k < TERN_ROUTE_KEPT; k++) {
            held |= x->e[k].slot != 0;
        }
        if (!x->used) {
            idle = x;
            break;
        }
        if (!idle && !held && !x->advertised && !x->urgent && !x->retracts && !x->tries) {
            idle = x;
        }
    }
    if (!d && idle) {
        d = idle;
        *d = (struct tern_route_dest){.id = id, .used = true, .asked = -1};
    }
    return d;
}

static struct tern_route_entry *entry_by(struct tern_route_dest *d, uint8_t s) {
    for (int i = 0; s && i < TERN_ROUTE_KEPT; i++) {
        if (d->e[i].slot == s) {
            return &d->e[i];
        }
    }
    return NULL;
}

/* Whether a route to `id` through slot `s` may be used: the link is up, and the neighbour is the
 * destination itself, or a relay once this node is done starting. */
static bool usable(const struct tern_route *r, uint8_t s, uint32_t id) {
    const struct tern_route_neighbour *n = slot(r, s);
    return n->used && n->up && (n->id == id || (n->relay && !r->starting));
}

static void choices(const struct tern_route *r, const struct tern_route_dest *d,
                    struct tern_route_choice c[TERN_ROUTE_KEPT]) {
    for (int i = 0; i < TERN_ROUTE_KEPT; i++) {
        const struct tern_route_entry *e = &d->e[i];
        c[i] = !e->slot ? (struct tern_route_choice){0, INF, INF}
                        : (struct tern_route_choice){e->seq, e->metric,
                                                     usable(r, e->slot, d->id) ? r->cost : INF};
    }
}

static void push_urgent(struct tern_route *r, struct tern_route_dest *d) {
    if (!d->urgent && r->config.relay) { /* a leaf announces no routes */
        d->urgent = true;
        r->urgent_count++;
    }
}

/* --- Trickle --- */

static void trickle_begin(struct tern_route *r) {
    tern_time half = r->interval / 2;
    r->heard = 0;
    r->fired = false;
    r->fire_at = r->now + half + (tern_time)rand_below(r, (uint64_t)(r->interval - half));
    r->interval_end = r->now + r->interval;
}

static void trickle_reset(struct tern_route *r) {
    if (r->interval > r->config.i_min) {
        r->interval = r->config.i_min;
        trickle_begin(r);
    }
}

/* --- Asking --- */

/* Adds a request to those waiting to go to `next`, or folds it into one already there. They go
 * after a jitter, so the requests one change sets off share frames, and the neighbours that heard
 * the same request do not all pass it on at once. */
static void request(struct tern_route *r, uint32_t id, uint16_t seq, uint32_t next, uint8_t hops) {
    for (size_t k = 0; k < r->ask_count; k++) {
        struct tern_route_request_waiting *a = &r->asks[k];
        if (a->next == next && a->destination == id) {
            if (tern_route_newer(seq, a->seq)) {
                a->seq = seq;
            }
            a->hops = hops > a->hops ? hops : a->hops;
            return;
        }
    }
    if (r->ask_count == TERN_ROUTE_ASKS) {
        return; /* no room: a starved node asks again */
    }
    r->asks[r->ask_count++] = (struct tern_route_request_waiting){
        .next = next, .destination = id, .seq = seq, .hops = hops};
    if (r->ask_at < 0) {
        uint64_t window = (uint64_t)r->config.jitter * (uint64_t)airtime(r, ASK_FRAME);
        r->ask_at = r->now + (tern_time)rand_below(r, window + 1);
    }
}

/* Asks about a destination: for a seq newer than the feasibility distance, of the neighbour with
 * the best of the infeasible routes, as RFC 8966 allows, so that the request goes up one path and
 * not every neighbour's; or, having never announced a route to it, for whatever route a neighbour
 * has. */
static void ask(struct tern_route *r, struct tern_route_dest *d) {
    uint16_t seq = (uint16_t)(d->fd.seq + 1), best = INF;
    uint32_t next = TERN_ROUTE_EVERYONE;
    d->asked = r->now;
    if (!d->fd.has) {
        d->asked_seq = 0;
        request(r, d->id, 0, TERN_ROUTE_EVERYONE, 0);
        return;
    }
    for (int i = 0; i < TERN_ROUTE_KEPT; i++) {
        const struct tern_route_entry *e = &d->e[i];
        uint16_t t;
        if (!e->slot) {
            continue;
        }
        if (tern_route_newer(e->seq, seq)) {
            seq = e->seq; /* ask for no less than a neighbour already has */
        }
        t = usable(r, e->slot, d->id) ? total(e->metric, r->cost) : INF;
        if (t < best) {
            best = t;
            next = slot(r, e->slot)->id;
        }
    }
    d->asked_seq = seq;
    request(r, d->id, seq, next, r->config.hop_max);
}

/* A node with routes to a destination and none feasible asks at once, unless it asked within
 * request_interval, and then again every request_interval until a route comes: a request is one
 * frame, and as easily lost as any. */
static void starved(struct tern_route *r, struct tern_route_dest *d) {
    if (d->tries == 0) {
        d->tries = 1;
        r->starving++;
        if (r->request_at < 0) {
            r->request_at = r->now + r->config.request_interval;
        }
    }
    if (d->asked < 0 || r->now - d->asked >= r->config.request_interval) {
        ask(r, d);
    }
}

static void ask_again(struct tern_route *r) {
    r->request_at = -1;
    for (size_t i = 0; i < r->dest_cap; i++) {
        struct tern_route_dest *d = &r->dest[i];
        if (!d->used || d->tries == 0) {
            continue;
        }
        if (d->sel || d->tries >= r->config.request_tries) {
            d->tries = 0;
            r->starving--;
        } else {
            d->tries++;
            ask(r, d);
        }
    }
    if (r->starving) {
        r->request_at = r->now + r->config.request_interval;
    }
}

/* --- Routes --- */

/* Chooses the route to a destination afresh, and notes whether what this node announces about it
 * changed. */
static void reselect(struct tern_route *r, struct tern_route_dest *d) {
    struct tern_route_choice c[TERN_ROUTE_KEPT];
    bool infeasible = false, was = d->sel != 0;
    int cur = -1, chosen;
    uint16_t t;
    choices(r, d, c);
    for (int i = 0; i < TERN_ROUTE_KEPT; i++) {
        if (d->sel && d->e[i].slot == d->sel) {
            cur = i;
        }
        infeasible |= total(c[i].metric, c[i].cost) != INF &&
                      !tern_route_feasible(&d->fd, c[i].seq, c[i].metric);
    }
    chosen = tern_route_select(&d->fd, c, TERN_ROUTE_KEPT, cur);
    d->sel = chosen >= 0 ? d->e[chosen].slot : 0;
    if (chosen < 0 && infeasible) {
        starved(r, d);
    }
    if (!r->config.relay) {
        return;
    }
    r->selected = r->selected - was + (chosen >= 0);
    if (chosen < 0) {
        if (d->advertised || was) {
            push_urgent(r, d); /* to retract it */
            r->changed = true;
        }
        return;
    }
    t = total(c[chosen].metric, c[chosen].cost);
    if (!d->advertised || tern_route_newer(c[chosen].seq, d->adv_seq) ||
        4 * (t > d->adv_metric ? t - d->adv_metric : d->adv_metric - t) > d->adv_metric) {
        push_urgent(r, d);
        r->changed = true;
    }
}

/* What the neighbour in slot `s` announces about a destination. */
static void update(struct tern_route *r, uint32_t id, uint16_t seq, uint16_t metric, uint8_t s) {
    struct tern_route_dest *d;
    struct tern_route_entry *e;
    if (id == r->id || reserved(id)) {
        return;
    }
    d = metric == INF ? dest_of(r, id) : dest_make(r, id);
    if (!d) {
        return;
    }
    e = entry_by(d, s);
    if (metric == INF) {
        if (!e) {
            return;
        }
        *e = (struct tern_route_entry){0};
    } else if (e) {
        e->seq = seq;
        e->metric = metric;
    } else {
        /* A free place, or else that of the route least worth keeping, if this is worth more. */
        struct tern_route_choice c[TERN_ROUTE_KEPT], mine = {seq, metric, INF};
        int place = -1, cur = -1;
        choices(r, d, c);
        for (int i = 0; i < TERN_ROUTE_KEPT; i++) {
            if (!d->e[i].slot && place < 0) {
                place = i;
            }
            if (d->sel && d->e[i].slot == d->sel) {
                cur = i;
            }
        }
        if (place < 0) {
            mine.cost = usable(r, s, id) ? r->cost : INF;
            place = tern_route_replace(&d->fd, c, cur, &mine);
        }
        if (place < 0) {
            return;
        }
        d->e[place] = (struct tern_route_entry){.slot = s, .seq = seq, .metric = metric};
    }
    reselect(r, d);
}

static void reselect_through(struct tern_route *r, uint8_t s) {
    for (size_t i = 0; i < r->dest_cap; i++) {
        if (r->dest[i].used && entry_by(&r->dest[i], s)) {
            reselect(r, &r->dest[i]);
        }
    }
}

static void forget(struct tern_route *r, uint8_t s) {
    slot(r, s)->used = false;
    slot(r, s)->up = false;
    for (size_t i = 0; i < r->dest_cap; i++) {
        struct tern_route_entry *e = r->dest[i].used ? entry_by(&r->dest[i], s) : NULL;
        if (e) {
            *e = (struct tern_route_entry){0};
            reselect(r, &r->dest[i]);
        }
    }
    r->changed = true;
    trickle_reset(r);
    r->changed = false;
}

/* A place for a neighbour just heard, `floor` being what its one frame says: a free one, or with
 * the table full the one tern_route_place gives. */
static uint8_t neighbour_make(struct tern_route *r, uint32_t id, int32_t floor) {
    size_t at = 0;
    for (; at < r->nb_cap && r->nb[at].used; at++) {
    }
    if (at == r->nb_cap) {
        int worst = tern_route_place(r->nb, r->nb_cap, floor);
        if (worst < 0) {
            return 0;
        }
        at = (size_t)worst;
        forget(r, (uint8_t)(at + 1));
    }
    r->nb[at] = (struct tern_route_neighbour){.id = id, .used = true, .owed = true};
    return (uint8_t)(at + 1);
}

/* --- Power --- */

static int8_t clamp_dbm(const struct tern_route *r, int32_t sixteenths) {
    int32_t p = -div_down(-sixteenths, 16); /* rounded up */
    return (int8_t)(p < r->config.min_dbm    ? r->config.min_dbm
                    : p > r->config.full_dbm ? r->config.full_dbm
                                             : p);
}

/* What a frame for every neighbour goes at: loud enough for the power_k with the lowest floors,
 * once it knows that many. */
static int8_t power_all(const struct tern_route *r) {
    int32_t low[32]; /* the k lowest floors, in order */
    unsigned have = 0, k = r->config.power_k > 32 ? 32 : r->config.power_k;
    for (size_t i = 0; k && i < r->nb_cap; i++) {
        const struct tern_route_neighbour *n = &r->nb[i];
        unsigned at;
        if (!n->used || (have == k && n->floor >= low[k - 1])) {
            continue;
        }
        at = have < k ? have++ : k - 1;
        for (; at > 0 && low[at - 1] > n->floor; at--) {
            low[at] = low[at - 1];
        }
        low[at] = n->floor;
    }
    return k && have == k ? clamp_dbm(r, low[k - 1] + 16 * r->config.power_margin_db)
                          : r->config.full_dbm;
}

static int8_t power_to(const struct tern_route *r, uint32_t id) {
    uint8_t s = id == TERN_ROUTE_EVERYONE ? 0 : slot_of(r, id);
    return s ? clamp_dbm(r, slot(r, s)->floor +
                                16 * (r->config.power_margin_db + (int32_t)slot(r, s)->boost))
             : power_all(r);
}

/* --- The cap --- */

static void bucket_init(struct tern_route_bucket *b, uint32_t ppm, tern_time window,
                        tern_time frame, tern_time now) {
    b->ppm = ppm ? ppm : 1;
    b->most = window * b->ppm > frame * MILLION ? window * b->ppm : frame * MILLION;
    b->have = b->most;
    b->at = now;
}

static void refill(struct tern_route_bucket *b, tern_time now) {
    tern_time dt = now - b->at;
    if (dt > 0) {
        b->have = dt >= b->most / b->ppm + 1 ? b->most : b->have + dt * b->ppm;
        b->have = b->have > b->most ? b->most : b->have;
        b->at = now;
    }
}

/* --- Announcing --- */

/* Whether the next announce carries this node's address: while it is starting, and while a
 * neighbour it keeps gives it no margin, and so may not hold it. */
static bool carries_address(const struct tern_route *r) {
    for (size_t i = 0; !r->starting && i < r->nb_cap; i++) {
        if (r->nb[i].used && r->nb[i].theirs == 0) {
            return true;
        }
    }
    return r->starting != 0;
}

/* An announce's length before its neighbours and routes. */
static size_t announce_fixed(const struct tern_route *r) {
    return TERN_ANNOUNCE_LEAST + (carries_address(r) ? TERN_ADDRESS_LEN : 0);
}

static uint8_t named_room(const struct tern_route *r) {
    size_t room = (TERN_ROUTE_FRAME_MAX - announce_fixed(r) - (r->config.relay ? 8u : 0u)) / 5;
    return (uint8_t)(r->config.named_max < room ? r->config.named_max : room);
}

static size_t neighbours(const struct tern_route *r) {
    size_t n = 0;
    for (size_t i = 0; i < r->nb_cap; i++) {
        n += r->nb[i].used;
    }
    return n;
}

/* How long the next announce will be, at most. */
static size_t planned(const struct tern_route *r) {
    size_t named = neighbours(r), room = named_room(r);
    size_t len = announce_fixed(r) + 5 * (named < room ? named : room);
    if (r->config.relay && !r->starting) {
        size_t routes = (size_t)r->urgent_count + r->selected + r->retracting;
        room = (TERN_ROUTE_FRAME_MAX - len) / 8;
        len += 8 * (routes < room ? routes : room);
    }
    return len;
}

/* One route as this node announces it, keeping the feasibility distance. False if there is nothing
 * to say about it. */
static bool advertise(struct tern_route *r, struct tern_route_dest *d,
                      struct tern_announce_route *out) {
    uint16_t seq, metric;
    if (d->sel) {
        const struct tern_route_entry *e = entry_by(d, d->sel);
        seq = e->seq;
        metric = total(e->metric, usable(r, d->sel, d->id) ? r->cost : INF);
        if (metric == INF) {
            return false;
        }
        /* Sticky: within `change` of what was last announced at this seq, that again. It is still
         * above the next hop's own metric, so the routes stay loop-free. */
        if (d->advertised && seq == d->adv_seq && d->adv_metric > e->metric &&
            4 * (metric > d->adv_metric ? metric - d->adv_metric : d->adv_metric - metric) <=
                d->adv_metric) {
            metric = d->adv_metric;
        }
        if (tern_route_feasible(&d->fd, seq, metric)) {
            d->fd = (struct tern_route_fd){true, seq, metric};
        }
        d->advertised = true;
        d->adv_seq = seq;
        d->adv_metric = metric;
        r->retracting -= d->retracts > 0;
        d->retracts = 0;
    } else if (d->advertised || d->retracts) {
        /* A retraction lost on the air would leave a neighbour with the route for good, so it goes
         * RETRACTS times, the repeats in turn with the routes. */
        uint8_t left = (uint8_t)(d->advertised ? RETRACTS - 1 : d->retracts - 1);
        seq = d->adv_seq;
        metric = INF;
        r->retracting += (left > 0 && d->retracts == 0);
        r->retracting -= (left == 0 && d->retracts > 0);
        d->retracts = left;
        d->advertised = false;
    } else {
        return false;
    }
    *out = (struct tern_announce_route){d->id, seq, metric};
    return true;
}

/* The longest this node may go before it announces again, announcing now: what is left of its
 * interval, the intervals it may keep quiet for and the one it must then announce in, the time its
 * cap takes to pay for a full frame, and the longest its announces have lately waited to go. */
static uint64_t promise(const struct tern_route *r) {
    tern_time ns = r->interval_end > r->now ? r->interval_end - r->now : 0, i = r->interval;
    tern_time waiting = r->out ? r->now - r->out_at : 0;
    tern_time mac = waiting > r->mac_wait ? waiting : r->mac_wait;
    uint64_t s;
    for (int k = 0; k <= r->config.quiet_max; k++) {
        i = i * 2 > i_max(r) ? i_max(r) : i * 2;
        ns += i;
    }
    ns += airtime(r, TERN_ROUTE_FRAME_MAX) * MILLION / r->announces.ppm;
    s = (uint64_t)((ns + NS_PER_S - 1) / NS_PER_S + (mac + NS_PER_S - 1) / NS_PER_S);
    return s < 1 ? 1 : s;
}

static size_t build(struct tern_route *r, uint8_t *frame, int8_t *power) {
    struct tern_announce a = {
        .sender = r->id,
        .number = r->number,
        .seq = r->seq,
        .relay = r->config.relay,
        .starting = r->starting != 0,
        .promise = tern_route_promise_code(promise(r)),
        .power = power_all(r),
        .carries_address = carries_address(r),
    };
    size_t room = named_room(r), start = r->nb_cap ? r->named_cursor % r->nb_cap : 0;
    size_t rotation = room ? (neighbours(r) + room - 1) / room : 0, len, owed;
    a.round = (uint16_t)(rotation > AGE_MAX ? AGE_MAX : rotation);

    /* Neighbours found since the last frame first, then the rest in turn from where it stopped. */
    for (int pass = 0; pass < 2; pass++) {
        for (size_t k = 0; k < r->nb_cap && a.named_count < room; k++) {
            size_t at = pass ? (start + k) % r->nb_cap : k;
            const struct tern_route_neighbour *n = &r->nb[at];
            if (!n->used || n->owed != (pass == 0)) {
                continue;
            }
            a.named[a.named_count++] = (struct tern_announce_named){
                n->id, tern_route_margin(r->config.full_dbm, n->floor)};
            if (pass) {
                r->named_cursor = (at + 1) % r->nb_cap;
            }
        }
        owed = pass ? owed : a.named_count;
    }
    for (size_t k = 0; k < owed; k++) {
        slot(r, slot_of(r, a.named[k].id))->owed = false;
    }

    if (r->config.relay && !r->starting) {
        size_t most = (TERN_ROUTE_FRAME_MAX - announce_fixed(r) - 5u * a.named_count) / 8;
        /* Changed routes first. */
        for (size_t k = 0; k < r->dest_cap && r->urgent_count && a.route_count < most; k++) {
            struct tern_route_dest *d = &r->dest[r->urgent_cursor];
            r->urgent_cursor = (r->urgent_cursor + 1) % r->dest_cap;
            if (!d->used || !d->urgent) {
                continue;
            }
            d->urgent = false;
            r->urgent_count--;
            if (advertise(r, d, &a.routes[a.route_count])) {
                d->listed = true;
                a.route_count++;
            }
        }
        /* Then the rest in turn, none twice: a retraction repeated in the frame it first went in
         * would count as two of its RETRACTS and be lost with the one frame. */
        for (size_t k = 0; k < r->dest_cap && a.route_count < most; k++) {
            struct tern_route_dest *d = &r->dest[r->route_cursor];
            r->route_cursor = (r->route_cursor + 1) % r->dest_cap;
            if (d->used && !d->listed && (d->sel || d->retracts) &&
                advertise(r, d, &a.routes[a.route_count])) {
                a.route_count++;
            }
        }
        for (size_t k = 0; k < r->dest_cap; k++) {
            r->dest[k].listed = false;
        }
    }
    if (a.carries_address && r->auth.address) {
        memcpy(a.address, r->auth.address, TERN_ADDRESS_LEN);
    }
    len = tern_announce_write(&a, frame);
    if (len && r->auth.sign) {
        uint8_t m[TERN_ANNOUNCE_LABEL_LEN + TERN_ROUTE_FRAME_MAX];
        r->auth.sign(r->auth.ctx, m, tern_announce_signed(frame, len, m),
                     frame + len - TERN_ANNOUNCE_SIG);
    }
    *power = a.power;
    r->promised = tern_route_promise_time(a.promise);
    return len;
}

/* Sends an announce if the cap allows, or sets the time it will. */
static size_t announce(struct tern_route *r, uint8_t *frame, int8_t *power) {
    struct tern_route_bucket *b = &r->announces;
    tern_time cost = airtime(r, planned(r)) * MILLION;
    size_t len;
    if (r->burst > 0 && (r->urgent_count == 0 || r->starting)) {
        r->announcing = false; /* a burst lasts only while changed routes remain, and can go */
        return 0;
    }
    refill(b, r->now);
    if (b->have < cost) {
        r->announce_at = r->now + (cost - b->have) / b->ppm + 1;
        return 0;
    }
    len = build(r, frame, power);
    b->have -= airtime(r, len) * MILLION;
    r->number++;
    r->asked = false;
    r->out = true;
    r->out_at = r->now;
    if (++r->burst >= r->config.burst) {
        r->announcing = false;
    }
    if (r->starting && --r->starting == 0) {
        /* It has said it started as often as it must: routes through its neighbours may be
         * selected now. */
        for (size_t i = 0; i < r->dest_cap; i++) {
            if (r->dest[i].used) {
                reselect(r, &r->dest[i]);
            }
        }
        r->changed = false;
        trickle_reset(r);
    }
    return len;
}

/* Sends the requests waiting for one next hop, out of the requests' share of the cap. Those it
 * cannot pay for are dropped: a starved node asks again. */
static size_t send_asks(struct tern_route *r, uint8_t *frame, int8_t *power) {
    struct tern_route_bucket *b = &r->requests;
    struct tern_request q = {.next = r->asks[0].next};
    size_t kept = 0, len;
    for (size_t k = 0; k < r->ask_count; k++) {
        const struct tern_route_request_waiting *a = &r->asks[k];
        if (a->next == q.next && q.count < TERN_REQUEST_MAX) {
            q.asks[q.count++] = (struct tern_request_ask){a->destination, a->seq, a->hops};
        } else {
            r->asks[kept++] = *a;
        }
    }
    len = tern_request_write(&q, frame);
    refill(b, r->now);
    if (b->have < airtime(r, len) * MILLION) {
        r->ask_count = 0;
        r->ask_at = -1;
        return 0;
    }
    b->have -= airtime(r, len) * MILLION;
    r->ask_count = kept;
    r->ask_at = kept ? r->now : -1;
    r->out = false;
    *power = power_to(r, q.next);
    return len;
}

/* --- Receiving --- */

static void on_announce(struct tern_route *r, const struct tern_announce *a, int16_t snr_q) {
    uint8_t s = slot_of(r, a->sender);
    struct tern_route_neighbour *n;
    bool fresh = s == 0, was_relay, was_up, news, named = false;
    if (a->sender == r->id) {
        return;
    }
    if (!fresh) {
        const struct tern_route_neighbour *was = slot(r, s);
        switch (tern_route_numbering(was->number, a->number, r->now - was->heard > was->promise,
                                     a->starting, was->starting)) {
        case TERN_ROUTE_DISCARD:
            return;
        case TERN_ROUTE_AGAIN:
            forget(r, s); /* what it announced before, it has lost */
            fresh = true;
            break;
        case TERN_ROUTE_TAKE:
            break;
        }
    }
    if (fresh) {
        s = neighbour_make(r, a->sender,
                           tern_route_floor(true, 0, a->power, snr_q, r->config.lora.sf));
        if (!s) {
            return; /* no room for another neighbour */
        }
        slot(r, s)->named = a->number;
    }
    n = slot(r, s);
    n->number = a->number;
    n->starting = a->starting;
    n->heard = r->now;
    n->lost = 0;
    was_relay = n->relay;
    n->relay = a->relay;
    /* A neighbour that has taken up or given up the relay's role, or a relay new to this node. */
    news = fresh ? n->relay : n->relay != was_relay;
    n->promise = tern_route_promise_time(a->promise);
    n->floor = tern_route_floor(fresh, n->floor, a->power, snr_q, r->config.lora.sf);
    for (int k = 0; k < a->named_count; k++) {
        if (a->named[k].id == r->id) {
            n->theirs = a->named[k].margin;
            n->named = a->number;
            named = true;
        }
    }
    if (!named && tern_route_withdrawn(n->named, a->number, a->round)) {
        n->theirs = 0;
    }
    r->changed = fresh;
    was_up = n->up;
    n->up = tern_route_link_up(was_up, 16 * (int32_t)r->config.full_dbm - n->floor, n->theirs);
    if (n->up != was_up || (news && !fresh)) {
        r->changed = true;
        reselect_through(r, s); /* routes through it now usable, or not */
    }
    update(r, a->sender, a->seq, 0, s);
    for (int k = 0; n->relay && !a->starting && k < a->route_count; k++) {
        update(r, a->routes[k].destination, a->routes[k].seq, a->routes[k].metric, s);
    }
    if (r->changed) {
        trickle_reset(r);
    } else if (r->heard < UINT8_MAX) {
        r->heard++;
    }
    r->changed = false;
}

static void on_ask(struct tern_route *r, const struct tern_request_ask *q) {
    struct tern_route_dest *d = q->destination == r->id ? NULL : dest_of(r, q->destination);
    bool have = r->config.relay && d && d->sel;
    if (q->hops == 0) {
        /* For any route: answered by whoever has one, and never passed on. */
        if (q->destination == r->id || have) {
            if (have) {
                push_urgent(r, d);
            }
            trickle_reset(r);
        }
        return;
    }
    if (q->destination == r->id) {
        /* Answered by the next announce, whatever Trickle would suppress. A new seq starts an
         * interval afresh; a request for one already reached only brings the interval down, so a
         * stream of them cannot keep putting the answer off. */
        r->asked = true;
        if (tern_route_newer(q->seq, r->seq)) {
            r->seq = q->seq;
            r->interval = r->config.i_min;
            trickle_begin(r);
        } else {
            trickle_reset(r);
        }
        return;
    }
    if (!have) {
        return;
    }
    if (!tern_route_newer(q->seq, entry_by(d, d->sel)->seq)) {
        push_urgent(r, d);
        trickle_reset(r);
        return;
    }
    if (q->hops > 1 && !(d->asked >= 0 && d->asked_seq == q->seq &&
                         r->now - d->asked < r->config.request_interval)) {
        d->asked = r->now;
        d->asked_seq = q->seq;
        request(r, d->id, q->seq, slot(r, d->sel)->id, (uint8_t)(q->hops - 1));
    }
}

/* The address an announce is checked with (the specification's "Signed"): the one it carries,
 * if that is its sender's and the one held, or the one held. NULL if it is to be discarded. With
 * no means to check, every announce is taken. */
static const uint8_t *check(const struct tern_route *r, const struct tern_announce *a,
                            const uint8_t *frame, size_t len) {
    uint8_t s = slot_of(r, a->sender), m[TERN_ANNOUNCE_LABEL_LEN + TERN_ROUTE_FRAME_MAX];
    const struct tern_route_neighbour *n = s ? slot(r, s) : NULL;
    const uint8_t *key = n && n->has_address ? n->address : NULL;
    if (!r->auth.verify) {
        return frame; /* anything but NULL: nothing is checked, and nothing held */
    }
    if (a->carries_address) {
        if (tern_route_id(a->address) != a->sender ||
            (key && memcmp(key, a->address, TERN_ADDRESS_LEN) != 0)) {
            return NULL;
        }
        key = a->address;
    }
    if (!key || !r->auth.verify(r->auth.ctx, key, m, tern_announce_signed(frame, len, m), a->sig)) {
        return NULL;
    }
    return key;
}

void tern_route_auth(struct tern_route *r, const struct tern_route_auth *auth) { r->auth = *auth; }

void tern_route_heard(struct tern_route *r, tern_time now, const uint8_t *frame, size_t len,
                      int16_t snr_q) {
    r->now = now;
    if (len > 0 && frame[0] == TERN_HDR_ANNOUNCE) {
        struct tern_announce a;
        const uint8_t *key;
        if (tern_announce_read(&a, frame, len) && (key = check(r, &a, frame, len)) != NULL) {
            uint8_t address[TERN_ADDRESS_LEN];
            bool known = r->auth.verify != NULL;
            if (known) {
                memcpy(address, key, TERN_ADDRESS_LEN); /* the slot may be forgotten */
            }
            on_announce(r, &a, snr_q);
            uint8_t s = slot_of(r, a.sender);
            if (s && known && !slot(r, s)->has_address) {
                slot(r, s)->has_address = true;
                memcpy(slot(r, s)->address, address, TERN_ADDRESS_LEN);
            }
        }
    } else if (len > 0 && frame[0] == TERN_HDR_REQUEST) {
        struct tern_request q;
        if (tern_request_read(&q, frame, len) &&
            (q.next == r->id || q.next == TERN_ROUTE_EVERYONE)) {
            for (int k = 0; k < q.count; k++) {
                on_ask(r, &q.asks[k]);
            }
        }
    }
}

/* --- Life --- */

static tern_time house_period(const struct tern_route *r) {
    tern_time p = r->config.silent_max / 4;
    return (p < i_max(r) ? p : i_max(r)) + 1;
}

void tern_route_init(struct tern_route *r, const struct tern_route_config *config, uint32_t id,
                     struct tern_route_neighbour *neighbours_, size_t neighbour_cap,
                     struct tern_route_dest *dests, size_t dest_cap, uint16_t seq, uint64_t seed,
                     tern_time now) {
    *r = (struct tern_route){
        .config = *config,
        .id = id,
        .seq = seq,
        .cost = tern_route_link_cost(&config->lora),
        .starting = config->start_announces,
        .nb = neighbours_,
        .nb_cap = neighbour_cap > 255 ? 255 : neighbour_cap,
        .dest = dests,
        .dest_cap = dest_cap,
        .interval = config->i_min,
        .ask_at = -1,
        .request_at = -1,
        .rng = seed ? seed : 1,
        .now = now,
    };
    for (size_t i = 0; i < r->nb_cap; i++) {
        r->nb[i].used = false;
    }
    for (size_t i = 0; i < r->dest_cap; i++) {
        r->dest[i].used = false;
    }
    r->number = (uint16_t)rand_below(r, 0x10000);
    bucket_init(&r->announces, config->announce_ppm, config->cap_window,
                airtime(r, TERN_ROUTE_FRAME_MAX), now);
    bucket_init(&r->requests, config->request_ppm, config->cap_window,
                airtime(r, TERN_ROUTE_FRAME_MAX), now);
    trickle_begin(r);
    r->house_at = now + house_period(r);
}

tern_time tern_route_due(const struct tern_route *r) {
    tern_time t = r->fired ? r->interval_end : r->fire_at;
    t = r->house_at < t ? r->house_at : t;
    t = r->announcing && r->announce_at < t ? r->announce_at : t;
    t = r->ask_at >= 0 && r->ask_at < t ? r->ask_at : t;
    t = r->request_at >= 0 && r->request_at < t ? r->request_at : t;
    return t;
}

size_t tern_route_poll(struct tern_route *r, tern_time now, uint8_t frame[TERN_ROUTE_FRAME_MAX],
                       int8_t *power_dbm) {
    size_t len = 0;
    r->now = now;
    if (now >= r->house_at) {
        /* Forgets the neighbours unheard for silent_max, and for two of their promises: a node
         * may go quiet that long. */
        for (size_t i = 0; i < r->nb_cap; i++) {
            const struct tern_route_neighbour *n = &r->nb[i];
            if (n->used && now - n->heard > r->config.silent_max &&
                now - n->heard > 2 * n->promise) {
                forget(r, (uint8_t)(i + 1));
            }
        }
        r->house_at = now + house_period(r);
    }
    if (r->request_at >= 0 && now >= r->request_at) {
        ask_again(r);
    }
    if (!r->fired && now >= r->fire_at) {
        r->fired = true;
        /* Changed routes waiting are an inconsistency of this node's own: never suppressed. */
        if (r->config.redundancy == 0 || r->heard < r->config.redundancy ||
            r->quiet >= r->config.quiet_max || r->urgent_count > 0 || r->asked || r->starting) {
            r->quiet = 0;
            if (!r->announcing) {
                r->announcing = true;
                r->burst = 0;
                r->announce_at = now;
            }
        } else {
            r->quiet++;
        }
    }
    if (r->fired && now >= r->interval_end) {
        /* With changes of its own still waiting it stays where it is rather than doubling, so
         * they go within the next interval. */
        if (r->urgent_count == 0 && !r->asked && !r->starting) {
            r->interval = r->interval * 2 > i_max(r) ? i_max(r) : r->interval * 2;
        }
        trickle_begin(r);
    }
    if (r->ask_at >= 0 && now >= r->ask_at) {
        len = send_asks(r, frame, power_dbm);
    }
    if (len == 0 && r->announcing && now >= r->announce_at) {
        len = announce(r, frame, power_dbm);
    }
    return len;
}

void tern_route_sent(struct tern_route *r, tern_time now) {
    if (r->out) {
        tern_time waited = now - r->out_at, kept = r->mac_wait - r->mac_wait / 8;
        r->mac_wait = waited > kept ? waited : kept;
        r->out = false;
    }
}

static bool tried_already(uint32_t id, const uint32_t *tried, int n) {
    for (int i = 0; i < n; i++) {
        if (tried[i] == id) {
            return true;
        }
    }
    return false;
}

int tern_route_default(const struct tern_route_neighbour *n, size_t count, bool leaf, bool starting,
                       uint32_t busy_ppm, uint32_t busy_max, const uint32_t *tried,
                       int tried_count) {
    int best = -1;
    if (!leaf || starting || (busy_max < 1000000 && busy_ppm >= busy_max)) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        if (n[i].used && n[i].up && n[i].relay && !tried_already(n[i].id, tried, tried_count) &&
            (best < 0 || n[i].floor < n[best].floor)) {
            best = (int)i;
        }
    }
    return best;
}

uint16_t tern_route_default_metric(uint8_t hops, uint16_t cost) {
    uint32_t t = (uint32_t)hops * cost;
    return t >= INF ? INF - 1 : (uint16_t)t;
}

/* A leaf's default: the slot of its nearest relay not in `tried`, or 0. */
static uint8_t nearest_relay(const struct tern_route *r, const uint32_t *tried, int tried_count) {
    if (!r->config.default_hops) {
        return 0;
    }
    int i = tern_route_default(r->nb, r->nb_cap, !r->config.relay, r->starting != 0, r->busy,
                               r->config.default_busy_ppm, tried, tried_count);
    return i < 0 ? 0 : (uint8_t)(i + 1);
}

void tern_route_busy(struct tern_route *r, uint32_t busy_ppm) { r->busy = busy_ppm; }

bool tern_route_next(const struct tern_route *r, uint32_t destination, uint32_t *next,
                     uint16_t *metric) {
    struct tern_route_dest *d = dest_of(r, destination);
    const struct tern_route_entry *e = d ? entry_by(d, d->sel) : NULL;
    if (!e || !usable(r, d->sel, d->id)) {
        uint8_t s = nearest_relay(r, NULL, 0);
        if (!s) {
            return false;
        }
        *next = slot(r, s)->id;
        *metric = tern_route_default_metric(r->config.default_hops, r->cost);
        return true;
    }
    *next = slot(r, d->sel)->id;
    *metric = total(e->metric, r->cost);
    return true;
}

/* --- For the frames that follow routes --- */

int8_t tern_route_power(const struct tern_route *r, uint32_t neighbour) {
    return power_to(r, neighbour);
}

int8_t tern_route_power_back(const struct tern_route *r, int8_t power, int16_t snr_q) {
    return clamp_dbm(r, tern_route_floor(true, 0, power, snr_q, r->config.lora.sf) +
                            16 * r->config.power_margin_db);
}

bool tern_route_other(const struct tern_route *r, uint32_t destination, const uint32_t *tried,
                      int tried_count, uint32_t *next) {
    struct tern_route_dest *d = dest_of(r, destination);
    uint16_t best = INF;
    bool found = false;
    for (int i = 0; d && i < TERN_ROUTE_KEPT; i++) {
        const struct tern_route_entry *e = &d->e[i];
        uint16_t t;
        /* The selected route is feasible by what it was when selected, though its metric may now
         * equal the feasibility distance. */
        if (!e->slot || !usable(r, e->slot, d->id) ||
            tried_already(slot(r, e->slot)->id, tried, tried_count) ||
            (e->slot != d->sel && !tern_route_feasible(&d->fd, e->seq, e->metric))) {
            continue;
        }
        t = total(e->metric, r->cost);
        if (t < best) {
            best = t;
            *next = slot(r, e->slot)->id;
            found = true;
        }
    }
    if (!found) {
        uint8_t s = nearest_relay(r, tried, tried_count);
        if (s) {
            *next = slot(r, s)->id;
            found = true;
        }
    }
    return found;
}

void tern_route_lost(struct tern_route *r, tern_time now, uint32_t neighbour) {
    uint8_t s = slot_of(r, neighbour);
    struct tern_route_neighbour *n;
    int room = r->config.full_dbm - r->config.min_dbm;
    r->now = now;
    if (!s) {
        return;
    }
    n = slot(r, s);
    n->boost = (uint8_t)(n->boost + r->config.step_db > room ? room : n->boost + r->config.step_db);
    if (n->lost < UINT8_MAX) {
        n->lost++;
    }
    if (r->config.dead_hops && n->lost >= r->config.dead_hops) {
        forget(r, s);
    }
}

void tern_route_passed(struct tern_route *r, tern_time now, uint32_t neighbour, int8_t power,
                       int16_t snr_q) {
    uint8_t s = slot_of(r, neighbour);
    struct tern_route_neighbour *n;
    bool was_up;
    r->now = now;
    if (!s) {
        return;
    }
    n = slot(r, s);
    n->floor = tern_route_floor(false, n->floor, power, snr_q, r->config.lora.sf);
    n->heard = now;
    n->lost = 0;
    n->boost = n->boost > 0 ? (uint8_t)(n->boost - 1) : 0;
    was_up = n->up;
    n->up = tern_route_link_up(was_up, 16 * (int32_t)r->config.full_dbm - n->floor, n->theirs);
    if (n->up != was_up) {
        r->changed = true;
        reselect_through(r, s);
        trickle_reset(r);
        r->changed = false;
    }
}

void tern_route_want(struct tern_route *r, tern_time now, uint32_t destination) {
    struct tern_route_dest *d;
    r->now = now;
    if (destination == r->id || !(d = dest_make(r, destination)) || d->sel) {
        return;
    }
    if (d->asked < 0 || now - d->asked >= r->config.request_interval) {
        ask(r, d);
    }
}
