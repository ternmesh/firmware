#include "tern/forward.h"

#include <string.h>

#include "tern/lora.h"
#include "tern/sleep.h"

#define MILLION 1000000
#define AT_NEXT 3 /* where the head keeps the next hop, and the destination */
#define AT_DEST 7

static uint32_t get32(const uint8_t *b) {
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static void put32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}

void tern_forward_head_write(const struct tern_forward_head *h, uint8_t *frame) {
    frame[0] = h->hdr;
    frame[1] = h->hops;
    frame[2] = (uint8_t)h->power;
    put32(frame + AT_NEXT, h->next);
    put32(frame + AT_DEST, h->destination);
}

static bool an_id(uint32_t id) { return id != 0 && id != TERN_ROUTE_EVERYONE; }

bool tern_forward_head_read(struct tern_forward_head *h, const uint8_t *frame, size_t len) {
    if (len < TERN_FORWARD_MIN || len > TERN_FORWARD_FRAME_MAX || !tern_forward_frame(frame, len) ||
        (frame[0] == TERN_HDR_ACK && len != TERN_ACK_LEN) ||
        (frame[0] == TERN_HDR_HELD && len != TERN_HELD_LEN) ||
        (tern_forward_message(frame[0]) && len < TERN_MESSAGE_MIN) ||
        (tern_forward_contact(frame[0]) && len != TERN_CONTACT_LEN(frame[0]))) {
        return false;
    }
    *h = (struct tern_forward_head){
        .hdr = frame[0],
        .hops = frame[1],
        .power = (int8_t)frame[2],
        .next = get32(frame + AT_NEXT),
        .destination = get32(frame + AT_DEST),
    };
    return an_id(h->next) && an_id(h->destination);
}

size_t tern_forward_held_write(int8_t power, uint32_t keeper, uint32_t destination,
                               const uint8_t tag[TERN_FORWARD_TAG], uint8_t frame[TERN_HELD_LEN]) {
    tern_forward_head_write(
        &(struct tern_forward_head){TERN_HDR_HELD, 0, power, keeper, destination}, frame);
    memcpy(frame + TERN_FORWARD_HEAD, tag, TERN_FORWARD_TAG);
    return TERN_HELD_LEN;
}

bool tern_forward_same(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len) {
    return a_len == b_len && a_len >= TERN_FORWARD_MIN && a[0] == b[0] &&
           memcmp(a + AT_DEST, b + AT_DEST, a_len - AT_DEST) == 0;
}

bool tern_forward_ends(const uint8_t *sent, size_t sent_len, const uint8_t *heard,
                       size_t heard_len) {
    bool passed =
        tern_forward_same(sent, sent_len, heard, heard_len) && (int)heard[1] + 1 == (int)sent[1];
    bool answered =
        tern_forward_message(sent[0]) && heard[0] == TERN_HDR_ACK &&
        memcmp(sent + TERN_FORWARD_HEAD, heard + TERN_FORWARD_HEAD, TERN_FORWARD_TAG) == 0;
    return passed || answered;
}

bool tern_forward_listens(const uint8_t *frame, size_t len) {
    return len >= TERN_FORWARD_HEAD && frame[0] != TERN_HDR_HELD &&
           (tern_forward_message(frame[0]) || get32(frame + AT_NEXT) != get32(frame + AT_DEST));
}

int8_t tern_forward_power(int8_t neighbour, int8_t back, uint8_t tries, uint8_t step, int8_t full) {
    int power = (back > neighbour ? back : neighbour) + (int)tries * step;
    return (int8_t)(power > full ? full : power);
}

struct tern_forward_config tern_forward_defaults(void) {
    return (struct tern_forward_config){
        .hop_max = 32,
        .hop_retries = 2,
        .salvage = 1,
        .retries = 3,
        .step_db = 3,
        .jitter = 2,
        .retry_jitter = 4,
        .ack_factor = 4,
        .hop_wait = TERN_S(4),
        .ack_wait = TERN_S(5),
    };
}

void tern_forward_init(struct tern_forward *f, const struct tern_forward_config *config,
                       struct tern_route *route, struct tern_forward_slot *slots, size_t cap,
                       uint64_t seed) {
    *f = (struct tern_forward){
        .config = *config,
        .route = route,
        .slot = slots,
        .cap = cap > TERN_FORWARD_SLOTS_MAX ? TERN_FORWARD_SLOTS_MAX : cap,
        .rng = seed ? seed : 0x9E3779B97F4A7C15ULL,
    };
    if (f->config.salvage > TERN_FORWARD_SALVAGE_MAX) {
        f->config.salvage = TERN_FORWARD_SALVAGE_MAX;
    }
    memset(slots, 0, f->cap * sizeof *slots); /* every one free */
}

void tern_forward_budget(struct tern_forward *f, struct tern_airtime *budget) {
    f->budget = budget;
}

void tern_forward_keeper(struct tern_forward *f, struct tern_forward_kept *store, size_t cap) {
    f->kept = store;
    f->kept_cap = store ? cap : 0;
    for (size_t i = 0; i < f->kept_cap; i++) {
        store[i].leaf = 0;
    }
}

void tern_forward_wake(struct tern_forward *f, uint32_t destination, uint16_t wake) {
    struct tern_forward_wake *free_at = NULL;
    for (size_t i = 0; i < TERN_FORWARD_WAKES; i++) {
        struct tern_forward_wake *w = &f->wakes[i];
        if (w->destination == destination) {
            w->wake = wake;
            w->destination = wake ? destination : 0;
            return;
        }
        if (!free_at && w->destination == 0) {
            free_at = w;
        }
    }
    if (wake) {
        /* With no room, the first place goes: a source told too little waits too short a time,
         * and its message is still held for the leaf. */
        free_at = free_at ? free_at : &f->wakes[0];
        *free_at = (struct tern_forward_wake){destination, wake};
    }
}

static uint16_t wake_of(const struct tern_forward *f, uint32_t destination) {
    for (size_t i = 0; i < TERN_FORWARD_WAKES; i++) {
        if (f->wakes[i].destination == destination && destination != 0) {
            return f->wakes[i].wake;
        }
    }
    return 0;
}

static uint64_t rand_below(struct tern_forward *f, uint64_t n) {
    f->rng ^= f->rng >> 12;
    f->rng ^= f->rng << 25;
    f->rng ^= f->rng >> 27;
    return n ? ((f->rng * 0x2545F4914F6CDD1DULL) >> 16) % n : 0;
}

static tern_time airtime(const struct tern_forward *f, size_t len) {
    return tern_lora_airtime(&f->route->config.lora, (uint32_t)len);
}

/* How long a frame sent in answer to one received waits: the nodes that heard the same frame
 * would otherwise answer it at the same instant. */
static tern_time jitter(struct tern_forward *f, size_t len) {
    return (tern_time)rand_below(f, (uint64_t)f->config.jitter * (uint64_t)airtime(f, len) + 1);
}

/* How long a frame sent again waits. What lost it the first time may have been another node's
 * frame, sent at the same moment and due again at the same moment as this one. */
static tern_time again(struct tern_forward *f, size_t len) {
    return (tern_time)rand_below(f,
                                 (uint64_t)f->config.retry_jitter * (uint64_t)airtime(f, len) + 1);
}

static uint32_t dest_of(const struct tern_forward_slot *s) { return get32(s->frame + AT_DEST); }

static bool same(const struct tern_forward_slot *s, uint8_t hdr, const uint8_t *tag) {
    return s->frame[0] == hdr && memcmp(s->frame + TERN_FORWARD_HEAD, tag, TERN_FORWARD_TAG) == 0;
}

static struct tern_forward_slot *free_slot(struct tern_forward *f) {
    for (size_t i = 0; i < f->cap; i++) {
        if (f->slot[i].state == TERN_FORWARD_FREE) {
            return &f->slot[i];
        }
    }
    return NULL;
}

/* A hop is over, heard or given up on: a message of this node's waits on for its acknowledgement,
 * and anything else is done with. */
static void rest(struct tern_forward_slot *s, tern_time now) {
    if (s->tracked) {
        s->state = TERN_FORWARD_HELD;
        s->at = s->deadline > now ? s->deadline : now;
    } else {
        s->state = TERN_FORWARD_FREE;
    }
}

/* --- A keeper's (the specification's "Leaves that sleep") --- */

/* Whether this node keeps `leaf` with it asleep: a frame for it is held, not sent. */
static bool asleep(const struct tern_forward *f, uint32_t leaf, tern_time now) {
    bool answered;
    uint16_t wake;
    return f->kept_cap &&
           tern_route_keeper_of(f->route, leaf, now, &answered, &wake) == TERN_KEEP_ASLEEP;
}

/* A frame of this node's own for a sleeping leaf it keeps, not yet sent: one it holds, as it holds
 * any other for the leaf, but in its slot. All but an acknowledgement of a frame just heard from
 * the leaf, which ends the hop the leaf listens for, and so goes at once. */
static bool own_held(const struct tern_forward *f, const struct tern_forward_slot *s, tern_time now,
                     bool *answered) {
    uint16_t wake;
    enum tern_route_keep st;
    *answered = false;
    if (s->state != TERN_FORWARD_WAITING || !s->own || dest_of(s) != s->next || !f->kept_cap ||
        (s->frame[0] == TERN_HDR_ACK && !s->tracked && s->back != INT8_MIN)) {
        return false;
    }
    st = tern_route_keeper_of(f->route, s->next, now, answered, &wake);
    return st == TERN_KEEP_ASLEEP || st == TERN_KEEP_AWAKE;
}

/* Such a frame waits in its slot until the leaf has polled and been answered, as what a keeper
 * holds for it does (keep()). */
static bool deferred(const struct tern_forward *f, const struct tern_forward_slot *s,
                     tern_time now) {
    bool answered;
    return own_held(f, s, now, &answered) && !answered;
}

/* Tells the router how many frames are held for `leaf`, for its answer to the leaf's next poll:
 * those a keeper holds for it, and its own not yet sent, so that the leaf listens for them. */
static void recount(struct tern_forward *f, uint32_t leaf, tern_time now) {
    unsigned n = 0;
    bool answered;
    for (size_t i = 0; i < f->kept_cap; i++) {
        n += f->kept[i].leaf == leaf;
    }
    for (size_t i = 0; i < f->cap; i++) {
        n += f->slot[i].next == leaf && own_held(f, &f->slot[i], now, &answered);
    }
    tern_route_holding(f->route, leaf, (uint8_t)(n > UINT8_MAX ? UINT8_MAX : n));
}

static struct tern_forward_kept *kept_same(struct tern_forward *f, const uint8_t *frame,
                                           size_t len) {
    for (size_t i = 0; i < f->kept_cap; i++) {
        struct tern_forward_kept *k = &f->kept[i];
        if (k->leaf && tern_forward_same(k->frame, k->len, frame, len)) {
            return k;
        }
    }
    return NULL;
}

/* The held notice for a frame held, to the node it came from, which needs `back` to hear it. No
 * frame says which node that was, so the notice names the keeper, in the place a head has `next`,
 * and the node before matches it against the neighbour it sent the frame to. */
static void notice(struct tern_forward *f, const uint8_t *frame, int8_t back) {
    struct tern_forward_notice *n;
    if (f->notice_count == TERN_FORWARD_NOTICES) {
        return; /* the node before tries again, and hears the next */
    }
    n = &f->notices[f->notice_count++];
    n->power = back == INT8_MIN ? tern_route_power(f->route, TERN_ROUTE_EVERYONE) : back;
    tern_forward_held_write(n->power, f->route->id, get32(frame + AT_DEST),
                            frame + TERN_FORWARD_HEAD, n->frame);
}

/* Holds a frame for `leaf`, with `hops` as it goes to it, first held at `at`. False with no room.
 */
static bool hold(struct tern_forward *f, uint32_t leaf, const uint8_t *frame, size_t len,
                 uint8_t hops, tern_time at, tern_time now) {
    for (size_t i = 0; i < f->kept_cap; i++) {
        struct tern_forward_kept *k = &f->kept[i];
        if (k->leaf == 0) {
            *k = (struct tern_forward_kept){.leaf = leaf,
                                            .order = f->kept_order++,
                                            .at = at,
                                            .hops = hops,
                                            .len = (uint8_t)len};
            memcpy(k->frame, frame, len);
            f->counts.held++;
            recount(f, leaf, now);
            return true;
        }
    }
    f->counts.held_full++;
    return false;
}

/* A frame handed over that is not to go now, its leaf asleep again or its hop given up, is held
 * again; with no room it is dropped. */
static void hold_again(struct tern_forward *f, struct tern_forward_slot *s, tern_time now) {
    (void)hold(f, s->kept_for, s->frame, s->len, s->hops, TERN_S((int64_t)s->held_s), now);
    s->state = TERN_FORWARD_FREE;
}

/* How long past ACK_WAIT and the route's time a source waits for a frame's answer, in seconds, for
 * a destination that sleeps: its wake, as it said, or for a frame a keeper said it holds, with none
 * said, the longest any leaf sleeps; and the shortest listen. 0 for one not known to sleep. */
static uint32_t sleep_wait_s(const struct tern_forward *f, const struct tern_forward_slot *s) {
    uint16_t peer = wake_of(f, dest_of(s));
    uint32_t w = peer ? tern_sleep_wake_s(peer) : s->held_long ? TERN_SLEEP_WAKE_MAX_S : 0;
    return w ? w + TERN_SLEEP_LISTEN_MIN_S : 0;
}

/* A keeper holds a frame of this node's for its sleeping leaf (draft/sleeping.md, "Telling the
 * source"): its answer comes once the leaf has polled, so this wait, and each after it, is a
 * sleeping destination's. */
static void held_wait(struct tern_forward *f, struct tern_forward_slot *s, tern_time now) {
    s->held_long = true;
    tern_time until = now + f->config.ack_wait + TERN_S((int64_t)sleep_wait_s(f, s));
    if (s->deadline >= 0 && s->deadline < until) {
        s->deadline = until;
    }
}

bool tern_forward_send(struct tern_forward *f, tern_time now, uint32_t destination,
                       const uint8_t *frame, size_t len, bool tracked, int8_t back) {
    struct tern_forward_slot *s = free_slot(f);
    uint32_t next;
    uint16_t metric;
    bool routed;
    if (len < TERN_FORWARD_MIN || len > TERN_FORWARD_FRAME_MAX || !tern_forward_frame(frame, len) ||
        !an_id(destination) || destination == f->route->id || !s) {
        return false;
    }
    routed = tern_route_next(f->route, destination, &next, &metric);
    if (!routed) {
        tern_route_want(f->route, now, destination);
        if (!tracked) {
            return false;
        }
    }
    *s = (struct tern_forward_slot){
        .own = true,
        .tracked = tracked,
        .hops = f->config.hop_max,
        .back = back,
        .len = (uint8_t)len,
        .next = routed ? next : 0,
        .deadline = -1,
    };
    memcpy(s->frame, frame, len);
    put32(s->frame + AT_DEST, destination);
    if (routed) {
        bool answered;
        s->state = TERN_FORWARD_WAITING;
        s->at = now + (back == INT8_MIN ? 0 : jitter(f, len));
        if (own_held(f, s, now, &answered)) {
            recount(f, s->next, now); /* the leaf's next poll is told of it */
        }
    } else {
        /* Asking counts as a try, and is waited on as one. */
        s->state = TERN_FORWARD_HELD;
        s->at = now + f->config.ack_wait;
    }
    return true;
}

void tern_forward_heard(struct tern_forward *f, tern_time now, const uint8_t *frame, size_t len,
                        int16_t snr_q, struct tern_forward_heard *out) {
    struct tern_forward_head h;
    struct tern_forward_slot *s;
    uint32_t next;
    uint16_t metric;
    bool ack;
    *out = (struct tern_forward_heard){.got = TERN_FORWARD_NOTHING, .back = INT8_MIN};
    if (!tern_forward_head_read(&h, frame, len)) {
        return;
    }
    ack = h.hdr == TERN_HDR_ACK;
    if (h.hdr == TERN_HDR_HELD) {
        /* A keeper holds a frame this node sent it, for its sleeping leaf: the hop is over, as if
         * the frame had been passed on, and teaches nothing, as an acknowledgement does not. */
        for (size_t i = 0; i < f->cap; i++) {
            s = &f->slot[i];
            bool sent =
                s->state == TERN_FORWARD_LISTENING ||
                ((s->state == TERN_FORWARD_WAITING || s->state == TERN_FORWARD_OUT) && s->again);
            if (!sent || s->next != h.next || dest_of(s) != h.destination ||
                memcmp(s->frame + TERN_FORWARD_HEAD, frame + TERN_FORWARD_HEAD, TERN_FORWARD_TAG) !=
                    0) {
                continue;
            }
            s->unheard = 0;
            if (s->own && s->tracked) {
                held_wait(f, s, now);
            }
            if (s->state == TERN_FORWARD_OUT) {
                s->done = true;
            } else {
                rest(s, now);
            }
        }
        return;
    }
    /* A hop of this node's is over once its next hop is heard sending the frame on, one hop
     * further; or, for a message, once an acknowledgement for it is heard at all. */
    for (size_t i = 0; i < f->cap; i++) {
        bool sent, passed;
        s = &f->slot[i];
        sent = s->state == TERN_FORWARD_LISTENING ||
               ((s->state == TERN_FORWARD_WAITING || s->state == TERN_FORWARD_OUT) && s->again);
        if (!sent) {
            continue;
        }
        /* The slot's frame holds the hops it last went with. */
        if (!tern_forward_ends(s->frame, s->len, frame, len)) {
            continue;
        }
        passed = h.hdr == s->frame[0];
        /* What passed it on was its next hop. An acknowledgement ends the hop, but teaches
         * nothing: no node on the way can check it, and anyone can send one. */
        if (passed) {
            tern_route_passed(f->route, now, s->next, h.power, snr_q);
        }
        s->unheard = 0;
        if (s->state == TERN_FORWARD_OUT) {
            s->done = true;
        } else {
            rest(s, now);
        }
    }
    if (ack && h.destination == f->route->id) {
        out->got = TERN_FORWARD_ACK; /* however it was heard */
        tern_route_held_got(f->route, now);
        return;
    }
    if (h.next != f->route->id) {
        return;
    }
    out->back = tern_route_power_back(f->route, h.power, snr_q);
    if (h.destination == f->route->id) {
        out->got = tern_forward_message(h.hdr) ? TERN_FORWARD_MESSAGE : TERN_FORWARD_CONTACT;
        tern_route_held_got(f->route, now);
        return;
    }
    if (!f->route->config.relay) {
        return;
    }
    if (h.hops <= 1) {
        f->counts.hop_limit++;
        return;
    }
    for (size_t i = 0; i < f->cap; i++) {
        s = &f->slot[i];
        if (s->state != TERN_FORWARD_FREE && s->state != TERN_FORWARD_FAILED &&
            tern_forward_same(s->frame, s->len, frame, len)) {
            return; /* still passing it on: the hop before will hear that */
        }
    }
    if (kept_same(f, frame, len)) {
        notice(f, frame, out->back); /* held already: the node before missed the notice */
        return;
    }
    if (!tern_route_next(f->route, h.destination, &next, &metric)) {
        f->counts.no_route++;
        tern_route_want(f->route, now, h.destination);
        return;
    }
    if (next == h.destination && asleep(f, next, now)) {
        /* For a sleeping leaf this node keeps: held until it polls, and the node before told. */
        if (hold(f, next, frame, len, (uint8_t)(h.hops - 1), now, now)) {
            notice(f, frame, out->back);
        }
        return;
    }
    s = free_slot(f);
    if (!s) {
        f->counts.no_room++;
        return;
    }
    *s = (struct tern_forward_slot){
        .state = TERN_FORWARD_WAITING,
        .hops = (uint8_t)(h.hops - 1),
        .back = out->back,
        .len = (uint8_t)len,
        .next = next,
        .at = now + jitter(f, len),
        .deadline = -1,
    };
    if (next == h.destination && f->kept_cap) {
        bool answered;
        uint16_t wake;
        if (tern_route_keeper_of(f->route, next, now, &answered, &wake) == TERN_KEEP_AWAKE) {
            s->kept_for = next; /* its leaf awake: held if its hop is given up */
            s->held_s = (uint32_t)(now / TERN_S(1));
        }
    }
    memcpy(s->frame, frame, len);
    f->counts.passed_on++;
}

bool tern_forward_acked(struct tern_forward *f, const uint8_t tag[TERN_FORWARD_TAG]) {
    return tern_forward_done(f, TERN_HDR_MESSAGE, tag) || tern_forward_done(f, TERN_HDR_NODE, tag);
}

bool tern_forward_done(struct tern_forward *f, uint8_t hdr, const uint8_t tag[TERN_FORWARD_TAG]) {
    for (size_t i = 0; i < f->cap; i++) {
        struct tern_forward_slot *s = &f->slot[i];
        if (s->state == TERN_FORWARD_FREE || s->state == TERN_FORWARD_FAILED || !s->tracked ||
            !same(s, hdr, tag)) {
            continue;
        }
        s->tracked = false;
        if (s->state == TERN_FORWARD_OUT) {
            s->done = true; /* freed once the caller lets go of it */
        } else {
            s->state = TERN_FORWARD_FREE;
        }
        return true;
    }
    return false;
}

bool tern_forward_failed(struct tern_forward *f, uint8_t tag[TERN_FORWARD_TAG]) {
    for (size_t i = 0; i < f->cap; i++) {
        struct tern_forward_slot *s = &f->slot[i];
        if (s->state == TERN_FORWARD_FAILED && tern_forward_message(s->frame[0])) {
            memcpy(tag, s->frame + TERN_FORWARD_HEAD, TERN_FORWARD_TAG);
            s->state = TERN_FORWARD_FREE;
            return true;
        }
    }
    return false;
}

bool tern_forward_contact_failed(struct tern_forward *f, uint8_t *hdr,
                                 uint8_t tag[TERN_FORWARD_TAG]) {
    for (size_t i = 0; i < f->cap; i++) {
        struct tern_forward_slot *s = &f->slot[i];
        if (s->state == TERN_FORWARD_FAILED && tern_forward_contact(s->frame[0])) {
            *hdr = s->frame[0];
            memcpy(tag, s->frame + TERN_FORWARD_HEAD, TERN_FORWARD_TAG);
            s->state = TERN_FORWARD_FREE;
            return true;
        }
    }
    return false;
}

static enum tern_forward_kind kind_of(const struct tern_forward_slot *s) {
    return s->frame[0] == TERN_HDR_ACK || s->frame[0] == TERN_HDR_HELD ? TERN_FORWARD_REPLY
           : s->own                                                    ? TERN_FORWARD_OWN
                                                                       : TERN_FORWARD_RELAY;
}

/* The node's own frame first in line, which those of its own behind it wait for: of those waiting,
 * the one that has waited longest. Acknowledgements are not in the line. */
static const struct tern_forward_slot *own_first(const struct tern_forward *f) {
    const struct tern_forward_slot *first = NULL;
    for (size_t i = 0; i < f->cap; i++) {
        const struct tern_forward_slot *s = &f->slot[i];
        if (s->state == TERN_FORWARD_WAITING && kind_of(s) == TERN_FORWARD_OWN &&
            !deferred(f, s, f->route->now) && (first == NULL || s->at < first->at)) {
            first = s;
        }
    }
    return first;
}

/* When what a keeper holds next needs it: a frame to hand over, pass on or drop. */
static tern_time kept_due(const struct tern_forward *f) {
    tern_time t = INT64_MAX, now = f->route->now;
    bool room = false;
    for (size_t i = 0; i < f->cap && !room; i++) {
        room = f->slot[i].state == TERN_FORWARD_FREE;
    }
    /* A poll to be answered: what is held for its leaf, this node's own frames among it, is
     * counted afresh first (keep()), as the node polls the forwarder before the router. */
    for (size_t i = 0; f->kept_cap && i < TERN_ROUTE_KEPT_LEAVES; i++) {
        const struct tern_route_kept_leaf *e = &f->route->keeps[i];
        if (e->leaf && e->answer_due && e->answer_at < t) {
            t = e->answer_at;
        }
    }
    for (size_t i = 0; i < f->kept_cap; i++) {
        const struct tern_forward_kept *k = &f->kept[i];
        bool answered;
        uint16_t wake;
        tern_time at;
        if (!k->leaf) {
            continue;
        }
        switch (tern_route_keeper_of(f->route, k->leaf, now, &answered, &wake)) {
        case TERN_KEEP_ASLEEP:
            at = k->at + tern_sleep_hold_for(wake) + 1; /* held longer than HOLD_FOR */
            break;
        case TERN_KEEP_AWAKE:
            /* To hand over once a slot is free: a slot freed is due of itself. */
            at = answered && room ? now : k->at + tern_sleep_hold_for(wake) + 1;
            break;
        case TERN_KEEP_NONE:
            at = room ? now : INT64_MAX;
            break;
        default:
            at = now; /* dropped */
            break;
        }
        t = at < t ? at : t;
    }
    return t;
}

tern_time tern_forward_due(const struct tern_forward *f) {
    tern_time t = f->notice_count ? f->route->now : INT64_MAX;
    const struct tern_forward_slot *first = own_first(f);
    tern_time k = kept_due(f);
    t = k < t ? k : t;
    for (size_t i = 0; i < f->cap; i++) {
        const struct tern_forward_slot *s = &f->slot[i];
        bool timed = (s->state == TERN_FORWARD_LISTENING || s->state == TERN_FORWARD_HELD ||
                      s->state == TERN_FORWARD_WAITING) &&
                     !deferred(f, s, f->route->now);
        /* The node's own go behind the first of them, which goes once the budget lets it. */
        bool behind = s->state == TERN_FORWARD_WAITING && kind_of(s) == TERN_FORWARD_OWN;
        if (timed && !behind && s->at < t) {
            t = s->at;
        }
        if (timed && s->tracked && s->deadline >= 0 && s->deadline < t) {
            t = s->deadline;
        }
    }
    if (first != NULL) {
        tern_time at = first->at;
        if (f->budget) {
            const struct tern_airtime_bucket *b =
                tern_airtime_bucket(f->budget, first->frame[0], true);
            tern_time from = b && b->at > at ? b->at : at;
            at = tern_airtime_pays_at(f->budget, from, first->frame[0], true,
                                      airtime(f, first->len));
        }
        t = at < t ? at : t;
    }
    return t;
}

size_t tern_forward_own_waiting(const struct tern_forward *f, struct tern_airtime_waiting *waiting,
                                uint8_t *handles, size_t n) {
    size_t count = 0;
    tern_time after = INT64_MIN;
    size_t after_i = 0;
    /* In the order tern_forward_poll() takes them: the one that has waited longest first, and of
     * two that have waited as long, the one in the earlier slot. */
    while (count < n) {
        const struct tern_forward_slot *next = NULL;
        size_t next_i = 0;
        for (size_t i = 0; i < f->cap; i++) {
            const struct tern_forward_slot *s = &f->slot[i];
            if (s->state != TERN_FORWARD_WAITING || kind_of(s) != TERN_FORWARD_OWN) {
                continue;
            }
            bool later = count == 0 || s->at > after || (s->at == after && i > after_i);
            if (later && (next == NULL || s->at < next->at)) {
                next = s;
                next_i = i;
            }
        }
        if (next == NULL) {
            break;
        }
        waiting[count] = (struct tern_airtime_waiting){next->frame[0], airtime(f, next->len)};
        handles[count] = (uint8_t)next_i;
        count++;
        after = next->at;
        after_i = next_i;
    }
    return count;
}

/* A hop was sent as often as it may be and never heard passed on. */
static void give_up(struct tern_forward *f, struct tern_forward_slot *s, tern_time now) {
    uint32_t other;
    f->counts.given_up++;
    tern_route_lost(f->route, now, s->next);
    s->unheard = 0;
    if (s->kept_for && f->kept_cap) {
        hold_again(f, s, now); /* to its sleeping leaf: held, the once, not sent another way */
        return;
    }
    if (s->frame[0] != TERN_HDR_ACK && s->salvages < f->config.salvage) {
        s->gone[s->salvages] = s->next;
        if (tern_route_other(f->route, dest_of(s), s->gone, s->salvages + 1, &other)) {
            s->salvages++;
            s->next = other;
            s->tries = 0;
            s->again = false;
            s->state = TERN_FORWARD_WAITING;
            s->at = now;
            f->counts.salvaged++;
            return;
        }
    }
    rest(s, now);
}

/* A message's acknowledgement did not come in its time: the message starts again, if it may,
 * whatever had become of its first hop. A next hop that has by now been sent it as often as a hop
 * is, and never heard passing it on, is counted against as one given up on. */
static void try_again(struct tern_forward *f, struct tern_forward_slot *s, tern_time now) {
    uint16_t metric;
    if (s->unheard > f->config.hop_retries) {
        f->counts.given_up++;
        tern_route_lost(f->route, now, s->next);
        s->unheard = 0;
    }
    if (s->attempts >= f->config.retries) {
        s->state = TERN_FORWARD_FAILED;
        return;
    }
    s->attempts++;
    s->tries = 0;
    s->salvages = 0;
    s->again = false;
    s->deadline = -1;
    s->hops = f->config.hop_max;
    {
        uint32_t next;
        if (tern_route_next(f->route, dest_of(s), &next, &metric)) {
            s->unheard = next == s->next ? s->unheard : 0;
            s->next = next;
            s->state = TERN_FORWARD_WAITING;
            s->at = now + again(f, s->len);
        } else {
            tern_route_want(f->route, now, dest_of(s));
            s->state = TERN_FORWARD_HELD;
            s->at = now + f->config.ack_wait;
        }
    }
}

/* What a keeper does with what it holds at `now`: drops what has waited longer than HOLD_FOR or
 * whose leaf it no longer keeps at all; passes on what is for a leaf that names another keeper;
 * takes back what was handed over to a leaf asleep again; and hands over, to a leaf that has
 * polled and been answered, what it holds for it: acknowledgements first, then the rest, each in
 * the order it came, as far as there are slots. */
static void keep(struct tern_forward *f, tern_time now) {
    for (size_t i = 0; i < f->cap; i++) {
        struct tern_forward_slot *s = &f->slot[i];
        if (s->state == TERN_FORWARD_WAITING && s->kept_for && asleep(f, s->kept_for, now)) {
            hold_again(f, s, now);
        }
    }
    for (int acks = 1; acks >= 0; acks--) {
        for (;;) {
            struct tern_forward_kept *k = NULL;
            for (size_t i = 0; i < f->kept_cap; i++) {
                struct tern_forward_kept *x = &f->kept[i];
                bool answered;
                uint16_t wake;
                enum tern_route_keep st;
                if (!x->leaf) {
                    continue;
                }
                st = tern_route_keeper_of(f->route, x->leaf, now, &answered, &wake);
                if (st == TERN_KEEP_GONE ||
                    (st != TERN_KEEP_NONE && now - x->at > tern_sleep_hold_for(wake))) {
                    uint32_t leaf = x->leaf;
                    x->leaf = 0;
                    f->counts.held_late += st != TERN_KEEP_GONE;
                    recount(f, leaf, now);
                    continue;
                }
                bool go = st == TERN_KEEP_NONE || (st == TERN_KEEP_AWAKE && answered);
                if (go && (x->frame[0] == TERN_HDR_ACK) == (acks == 1) &&
                    (!k || x->order < k->order)) {
                    k = x;
                }
            }
            struct tern_forward_slot *s = k ? free_slot(f) : NULL;
            if (!s) {
                break;
            }
            uint32_t leaf = k->leaf, next = leaf;
            uint16_t metric;
            bool keeps, answered;
            uint16_t wake;
            keeps = tern_route_keeper_of(f->route, leaf, now, &answered, &wake) != TERN_KEEP_NONE;
            k->leaf = 0;
            if (!keeps && !tern_route_next(f->route, get32(k->frame + AT_DEST), &next, &metric)) {
                recount(f, leaf, now); /* named another keeper, and no route there: dropped */
                continue;
            }
            *s = (struct tern_forward_slot){
                .state = TERN_FORWARD_WAITING,
                .hops = k->hops,
                .back = INT8_MIN,
                .len = k->len,
                .next = next,
                .at = now,
                .deadline = -1,
                .kept_for = keeps ? leaf : 0,
                .held_s = (uint32_t)(k->at / TERN_S(1)),
            };
            memcpy(s->frame, k->frame, k->len);
            f->counts.handed += keeps;
            recount(f, leaf, now);
        }
    }
    /* This node's own frames for its leaves come and go in their slots: counted afresh for each
     * leaf it keeps, so an answer to a poll says how many are waiting. */
    for (size_t i = 0; i < TERN_ROUTE_KEPT_LEAVES; i++) {
        if (f->route->keeps[i].leaf) {
            recount(f, f->route->keeps[i].leaf, now);
        }
    }
}

size_t tern_forward_poll(struct tern_forward *f, tern_time now,
                         uint8_t frame[TERN_FORWARD_FRAME_MAX], int8_t *power_dbm,
                         enum tern_forward_kind *kind, uint8_t *handle) {
    struct tern_forward_slot *go = NULL;
    if (f->kept_cap) {
        keep(f, now);
    }
    if (f->notice_count) {
        /* A held notice first, as an acknowledgement: paid for as one, and never held. */
        struct tern_forward_notice n = f->notices[0];
        memmove(&f->notices[0], &f->notices[1], (f->notice_count - 1) * sizeof f->notices[0]);
        f->notice_count--;
        if (f->budget) {
            tern_airtime_charge(f->budget, now, TERN_HDR_HELD, true, airtime(f, TERN_HELD_LEN));
        }
        memcpy(frame, n.frame, TERN_HELD_LEN);
        *power_dbm = n.power;
        *kind = TERN_FORWARD_REPLY;
        *handle = TERN_FORWARD_NOTICE;
        return TERN_HELD_LEN;
    }
    for (size_t i = 0; i < f->cap; i++) {
        struct tern_forward_slot *s = &f->slot[i];
        bool late = s->tracked && s->deadline >= 0 && s->deadline <= now;
        if (late && (s->state == TERN_FORWARD_LISTENING || s->state == TERN_FORWARD_WAITING)) {
            try_again(f, s, now);
        } else if (s->state == TERN_FORWARD_LISTENING && s->at <= now) {
            if (s->tries < f->config.hop_retries) {
                s->tries++;
                s->again = true;
                s->state = TERN_FORWARD_WAITING;
                s->at = now + again(f, s->len);
                f->counts.sent_again++;
            } else {
                give_up(f, s, now);
            }
        } else if (s->state == TERN_FORWARD_HELD && s->at <= now) {
            try_again(f, s, now);
        }
    }
    /* With a budget, the node's own wait while the first of them is not paid for; and a frame
     * passed on that is not paid for is dropped, and the next looked for. */
    const struct tern_forward_slot *first = own_first(f);
    bool own_held =
        f->budget && first && first->at <= now &&
        !tern_airtime_pays(f->budget, now, first->frame[0], true, airtime(f, first->len));
    for (;;) {
        go = NULL;
        for (size_t i = 0; i < f->cap; i++) {
            struct tern_forward_slot *s = &f->slot[i];
            if (s->state == TERN_FORWARD_WAITING && s->at <= now && !deferred(f, s, now) &&
                !(own_held && kind_of(s) == TERN_FORWARD_OWN) &&
                (!go || kind_of(s) > kind_of(go) ||
                 (kind_of(s) == kind_of(go) && s->at < go->at))) {
                go = s;
            }
        }
        if (!go) {
            return 0;
        }
        if (f->budget && kind_of(go) == TERN_FORWARD_RELAY &&
            !tern_airtime_pays(f->budget, now, go->frame[0], false, airtime(f, go->len))) {
            f->counts.unpaid++;
            if (go->kept_for && f->kept_cap) {
                hold_again(f, go, now); /* held for a sleeping leaf: held still, not dropped */
            } else {
                go->state = TERN_FORWARD_FREE;
            }
            continue;
        }
        break;
    }
    /* Loud enough for the next hop, and for the node the frame came from, which listens for it;
     * and louder each time it is sent again. */
    go->power = tern_forward_power(tern_route_power(f->route, go->next), go->back, go->tries,
                                   f->config.step_db, f->route->config.full_dbm);
    tern_forward_head_write(
        &(struct tern_forward_head){go->frame[0], go->hops, go->power, go->next, dest_of(go)},
        go->frame);
    memcpy(frame, go->frame, go->len);
    /* Charged as it is handed over, so that a caller taking several before any has gone cannot have
     * each paid for with the same airtime; given back if it is withdrawn. */
    if (f->budget) {
        tern_airtime_charge(f->budget, now, go->frame[0], go->own, airtime(f, go->len));
    }
    go->state = TERN_FORWARD_OUT;
    go->done = false;
    *power_dbm = go->power;
    *kind = kind_of(go);
    *handle = (uint8_t)(go - f->slot);
    return go->len;
}

static struct tern_forward_slot *out_slot(const struct tern_forward *f, uint8_t handle) {
    return handle < f->cap && f->slot[handle].state == TERN_FORWARD_OUT ? &f->slot[handle] : NULL;
}

bool tern_forward_wanted(const struct tern_forward *f, uint8_t handle) {
    const struct tern_forward_slot *s = out_slot(f, handle);
    return s && !s->done;
}

/* A message's acknowledgement is waited for from when it first goes: a time to get there and
 * back, by what its route costs. */
static void start_waiting(struct tern_forward *f, struct tern_forward_slot *s, tern_time now) {
    uint32_t next;
    uint16_t metric = 0;
    if (s->tracked && s->deadline < 0) {
        const struct tern_route *r = f->route;
        /* A leaf with no route now sent it by default, or will next: its wait is the default
         * route's, though the radio grew too busy to take one since. */
        if (!tern_route_next(r, dest_of(s), &next, &metric) && !r->config.relay &&
            r->config.default_hops) {
            metric = tern_route_default_metric(r->config.default_hops, r->cost);
        }
        uint32_t own_wake = 0, peer_s = sleep_wait_s(f, s);
        if (tern_route_sleeping(r, NULL)) {
            /* Asleep, the leaf hears its acknowledgement only at its next poll. */
            own_wake = tern_sleep_wake_s(r->config.sleep_wake);
        }
        s->deadline = now + f->config.ack_wait +
                      (tern_time)f->config.ack_factor * (tern_time)metric * TERN_MS(1);
        if (peer_s || own_wake) {
            /* The shortest listen once, though both sleep. */
            s->deadline += TERN_S((int64_t)(peer_s ? peer_s : TERN_SLEEP_LISTEN_MIN_S) + own_wake);
        }
    }
}

void tern_forward_withdrawn(struct tern_forward *f, tern_time now, uint8_t handle) {
    struct tern_forward_slot *s = out_slot(f, handle);
    if (!s) {
        return;
    }
    struct tern_airtime_bucket *part =
        f->budget ? tern_airtime_bucket(f->budget, s->frame[0], s->own) : NULL;
    if (part != NULL) {
        tern_airtime_bucket_give(part, now, airtime(f, s->len));
    }
    if (s->done) {
        start_waiting(f, s, now);
        rest(s, now);
    } else {
        s->state = TERN_FORWARD_WAITING;
        s->at = now;
    }
}

void tern_forward_sent(struct tern_forward *f, tern_time now, uint8_t handle) {
    struct tern_forward_slot *s = out_slot(f, handle);
    if (!s) {
        return;
    }
    bool listens = tern_forward_listens(s->frame, s->len);
    start_waiting(f, s, now);
    /* A frame not listened for says nothing of its next hop, however often it goes unanswered. */
    if (!s->done && listens && s->unheard < UINT8_MAX) {
        s->unheard++;
    }
    if (s->done || !listens) {
        rest(s, now); /* heard already; or a last hop with nothing to hear */
        return;
    }
    s->state = TERN_FORWARD_LISTENING;
    s->at = now + f->config.hop_wait + 2 * airtime(f, s->len);
}

bool tern_forward_may_sleep(const struct tern_forward *f, tern_time now, tern_time *wake_at) {
    tern_time t;
    if (!tern_route_sleeping(f->route, NULL) || now < tern_route_listen_until(f->route) ||
        f->notice_count) {
        return false;
    }
    for (size_t i = 0; i < f->cap; i++) {
        const struct tern_forward_slot *s = &f->slot[i];
        if (s->state == TERN_FORWARD_LISTENING || s->state == TERN_FORWARD_OUT) {
            return false;
        }
    }
    t = tern_route_due(f->route);
    *wake_at = tern_forward_due(f) < t ? tern_forward_due(f) : t;
    return true;
}
