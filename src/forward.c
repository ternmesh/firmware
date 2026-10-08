#include "tern/forward.h"

#include <string.h>

#include "tern/lora.h"

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
    return len >= TERN_FORWARD_HEAD &&
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
        s->state = TERN_FORWARD_WAITING;
        s->at = now + (back == INT8_MIN ? 0 : jitter(f, len));
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
        /* What passed it on was its next hop; so was an acknowledgement's first copy, if its next
         * hop was where it was going. */
        if (passed || (s->next == dest_of(s) && h.hops == f->config.hop_max)) {
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
        return;
    }
    if (h.next != f->route->id) {
        return;
    }
    out->back = tern_route_power_back(f->route, h.power, snr_q);
    if (h.destination == f->route->id) {
        out->got = tern_forward_message(h.hdr) ? TERN_FORWARD_MESSAGE : TERN_FORWARD_CONTACT;
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
    if (!tern_route_next(f->route, h.destination, &next, &metric)) {
        f->counts.no_route++;
        tern_route_want(f->route, now, h.destination);
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

tern_time tern_forward_due(const struct tern_forward *f) {
    tern_time t = INT64_MAX;
    for (size_t i = 0; i < f->cap; i++) {
        const struct tern_forward_slot *s = &f->slot[i];
        bool timed = s->state == TERN_FORWARD_LISTENING || s->state == TERN_FORWARD_HELD ||
                     s->state == TERN_FORWARD_WAITING;
        if (timed && s->at < t) {
            t = s->at;
        }
        if (timed && s->tracked && s->deadline >= 0 && s->deadline < t) {
            t = s->deadline;
        }
    }
    return t;
}

/* A hop was sent as often as it may be and never heard passed on. */
static void give_up(struct tern_forward *f, struct tern_forward_slot *s, tern_time now) {
    uint32_t other;
    f->counts.given_up++;
    tern_route_lost(f->route, now, s->next);
    s->unheard = 0;
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

static enum tern_forward_kind kind_of(const struct tern_forward_slot *s) {
    return s->frame[0] == TERN_HDR_ACK ? TERN_FORWARD_REPLY
           : s->own                    ? TERN_FORWARD_OWN
                                       : TERN_FORWARD_RELAY;
}

size_t tern_forward_poll(struct tern_forward *f, tern_time now,
                         uint8_t frame[TERN_FORWARD_FRAME_MAX], int8_t *power_dbm,
                         enum tern_forward_kind *kind, uint8_t *handle) {
    struct tern_forward_slot *go = NULL;
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
    for (size_t i = 0; i < f->cap; i++) {
        struct tern_forward_slot *s = &f->slot[i];
        if (s->state == TERN_FORWARD_WAITING && s->at <= now &&
            (!go || kind_of(s) > kind_of(go) || (kind_of(s) == kind_of(go) && s->at < go->at))) {
            go = s;
        }
    }
    if (!go) {
        return 0;
    }
    /* Loud enough for the next hop, and for the node the frame came from, which listens for it;
     * and louder each time it is sent again. */
    go->power = tern_forward_power(tern_route_power(f->route, go->next), go->back, go->tries,
                                   f->config.step_db, f->route->config.full_dbm);
    tern_forward_head_write(
        &(struct tern_forward_head){go->frame[0], go->hops, go->power, go->next, dest_of(go)},
        go->frame);
    memcpy(frame, go->frame, go->len);
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
        (void)tern_route_next(f->route, dest_of(s), &next, &metric);
        s->deadline = now + f->config.ack_wait +
                      (tern_time)f->config.ack_factor * (tern_time)metric * TERN_MS(1);
    }
}

void tern_forward_withdrawn(struct tern_forward *f, tern_time now, uint8_t handle) {
    struct tern_forward_slot *s = out_slot(f, handle);
    if (!s) {
        return;
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
