#include "link.h"

#include <string.h>

#include "tern/crypto.h"

/* --- Frames out ----------------------------------------------------------------------------- */

static void send_msg(struct link *l, const struct tern_companion_msg *m) {
    uint8_t frame[TERN_COMPANION_MAX_FRAME];
    size_t len = tern_companion_write(m, frame);
    if (len != 0) {
        l->host.out(l->host.ctx, frame, len);
    }
}

static void answer(struct link *l, uint8_t type, uint8_t seq) {
    struct tern_companion_msg m = {.type = type, .seq = seq};
    send_msg(l, &m);
}

static void error(struct link *l, uint8_t seq, uint8_t code) {
    struct tern_companion_msg m = {.type = TERN_C_ERROR, .seq = seq, .code = code};
    send_msg(l, &m);
}

/* News goes only to a client that has said HELLO, numbered by the count. */
static void news(struct link *l, struct tern_companion_msg *m) {
    if (!l->hello) {
        return;
    }
    m->seq = l->news++;
    send_msg(l, m);
}

static void put_text(struct tern_companion_msg *m, const uint8_t *text, size_t len) {
    memcpy(m->text, text, len);
    m->text_len = (uint8_t)len;
}

static size_t cstr_len(const char *s, size_t max) {
    size_t n = 0;
    while (s != NULL && n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

/* --- What the node holds, as news ------------------------------------------------------------- */

static void news_self(struct link *l) {
    const struct link_view *v = &l->view;
    struct tern_companion_msg m = {
        .type = TERN_C_SELF, .role = v->role, .power = v->power, .time = v->time};
    memcpy(m.address, v->address, TERN_ADDRESS_LEN);
    put_text(&m, (const uint8_t *)v->region, cstr_len(v->region, TERN_COMPANION_REGION_MAX));
    l->self_role = v->role;
    l->self_power = v->power;
    l->self_region = v->region;
    news(l, &m);
}

static void news_contact(struct link *l, const struct link_contact *c) {
    struct tern_companion_msg m = {.type = TERN_C_CONTACT};
    memcpy(m.address, c->address, TERN_ADDRESS_LEN);
    m.session = l->host.session(l->host.ctx, c->address) ? 1 : 0;
    put_text(&m, c->name, c->name_len);
    news(l, &m);
}

static void news_message(struct link *l, const struct link_message *x) {
    struct tern_companion_msg m = {
        .type = TERN_C_MESSAGE,
        .id = x->id,
        .time = x->time,
        .flags = x->flags,
        .state = x->state,
        .reason = x->reason,
        .wait = x->wait,
    };
    memcpy(m.address, x->address, TERN_ADDRESS_LEN);
    put_text(&m, x->text, x->text_len);
    news(l, &m);
}

static void news_neighbour(struct link *l, const struct link_neighbour *n) {
    struct tern_companion_msg m = {.type = TERN_C_NEIGHBOUR,
                                   .routing_id = n->id,
                                   .role = n->role,
                                   .snr = n->snr,
                                   .heard = n->heard};
    news(l, &m);
}

static struct tern_companion_msg airtime_of(const struct link_view *v) {
    return (struct tern_companion_msg){.type = TERN_C_AIRTIME,
                                       .period = v->period_s,
                                       .allowed = v->allowed_ms,
                                       .used = v->used_ms,
                                       .wait = v->wait_ms};
}

static struct tern_companion_msg power_of(const struct link_view *v) {
    return (struct tern_companion_msg){.type = TERN_C_POWER,
                                       .millivolts = v->millivolts,
                                       .percent = v->percent,
                                       .flags = v->power_flags};
}

/* --- Contacts and messages ------------------------------------------------------------------- */

static struct link_contact *find_contact(struct link *l, const uint8_t address[TERN_ADDRESS_LEN]) {
    for (size_t i = 0; i < LINK_CONTACTS; i++) {
        if (l->contacts[i].used && memcmp(l->contacts[i].address, address, TERN_ADDRESS_LEN) == 0) {
            return &l->contacts[i];
        }
    }
    return NULL;
}

static struct link_message *find_message(struct link *l, uint32_t id) {
    for (size_t i = 0; i < LINK_MESSAGES; i++) {
        if (l->messages[i].used && l->messages[i].id == id) {
            return &l->messages[i];
        }
    }
    return NULL;
}

static bool waiting(const struct link_message *x) {
    return x->state == TERN_C_WAITING && !x->aired;
}

/* A free place for a message: an empty one, or else the oldest that is not waiting to go. */
static struct link_message *room(struct link *l) {
    struct link_message *oldest = NULL;
    for (size_t i = 0; i < LINK_MESSAGES; i++) {
        struct link_message *x = &l->messages[i];
        if (!x->used) {
            return x;
        }
        if (!waiting(x) && (oldest == NULL || x->id < oldest->id)) {
            oldest = x;
        }
    }
    return oldest;
}

static void digest(const uint8_t *text, size_t len, uint8_t out[8]) {
    struct tern_sha256 c;
    uint8_t d[TERN_SHA256_LEN];
    tern_sha256_init(&c);
    tern_sha256_update(&c, text, len);
    tern_sha256_final(&c, d);
    memcpy(out, d, 8);
}

void link_init(struct link *l, const struct link_host *host) {
    memset(l, 0, sizeof *l);
    l->host = *host;
    l->next_id = 1;
    if (!l->host.load(l->host.ctx, l->contacts, sizeof l->contacts)) {
        memset(l->contacts, 0, sizeof l->contacts);
    }
}

uint32_t link_add(struct link *l, const uint8_t address[TERN_ADDRESS_LEN], uint32_t time,
                  uint8_t state, uint8_t reason, const uint8_t *text, size_t len) {
    size_t keep = tern_companion_utf8_prefix(text, len, TERN_COMPANION_TEXT_MAX);
    struct link_message *x = keep == 0 ? NULL : room(l);
    if (x == NULL) {
        return 0;
    }
    *x = (struct link_message){
        .used = true, .id = l->next_id++, .time = time, .state = state, .reason = reason};
    memcpy(x->address, address, TERN_ADDRESS_LEN);
    memcpy(x->text, text, keep);
    x->text_len = (uint8_t)keep;
    news_message(l, x);
    return x->id;
}

struct link_message *link_outgoing(struct link *l) {
    struct link_message *first = NULL;
    for (size_t i = 0; i < LINK_MESSAGES; i++) {
        struct link_message *x = &l->messages[i];
        if (x->used && waiting(x) && (first == NULL || x->id < first->id)) {
            first = x;
        }
    }
    return first;
}

void link_state(struct link *l, uint32_t id, uint8_t state, uint8_t reason, uint16_t wait) {
    struct link_message *x = find_message(l, id);
    if (x == NULL) {
        return;
    }
    if (state != TERN_C_WAITING) {
        reason = TERN_C_WAIT_UNNAMED;
        wait = 0;
    }
    bool changed = x->state != state || x->reason != reason;
    x->state = state;
    x->reason = reason;
    x->wait = wait;
    if (changed) {
        struct tern_companion_msg m = {
            .type = TERN_C_STATE, .id = id, .state = state, .reason = reason, .wait = wait};
        news(l, &m);
    }
}

void link_aired(struct link *l, uint32_t id) {
    struct link_message *x = find_message(l, id);
    if (x != NULL) {
        x->aired = true;
        link_state(l, id, TERN_C_WAITING, TERN_C_WAIT_UNNAMED, 0);
    }
}

void link_unreachable(struct link *l, const uint8_t address[TERN_ADDRESS_LEN]) {
    for (size_t i = 0; i < LINK_MESSAGES; i++) {
        struct link_message *x = &l->messages[i];
        if (x->used && waiting(x) && x->reason == TERN_C_WAIT_SESSION &&
            memcmp(x->address, address, TERN_ADDRESS_LEN) == 0) {
            link_state(l, x->id, TERN_C_NOT_DELIVERED, 0, 0);
        }
    }
}

void link_session_changed(struct link *l, const uint8_t address[TERN_ADDRESS_LEN]) {
    const struct link_contact *c = find_contact(l, address);
    if (c != NULL) {
        news_contact(l, c);
    }
}

/* --- Requests -------------------------------------------------------------------------------- */

static bool usable_address(struct link *l, const uint8_t address[TERN_ADDRESS_LEN]) {
    return memcmp(address, l->view.address, TERN_ADDRESS_LEN) != 0 && tern_address_valid(address);
}

/* Tells a client everything, and remembers what it was told. */
static void sync(struct link *l, const struct tern_companion_msg *q, tern_time now) {
    l->host.view(l->host.ctx, &l->view);
    news_self(l);
    for (size_t i = 0; i < LINK_CONTACTS; i++) {
        if (l->contacts[i].used) {
            news_contact(l, &l->contacts[i]);
        }
    }
    /* Oldest first, as a client would have heard them. */
    uint32_t after = q->after;
    for (;;) {
        const struct link_message *next = NULL;
        for (size_t i = 0; i < LINK_MESSAGES; i++) {
            const struct link_message *x = &l->messages[i];
            if (x->used && x->id > after && (next == NULL || x->id < next->id)) {
                next = x;
            }
        }
        if (next == NULL) {
            break;
        }
        news_message(l, next);
        after = next->id;
    }
    l->n_told = 0;
    for (size_t i = 0; i < l->view.n_neighbours && i < LINK_NEIGHBOURS; i++) {
        news_neighbour(l, &l->view.neighbours[i]);
        l->told[l->n_told++] = (struct link_told){l->view.neighbours[i], now};
    }
    l->air = airtime_of(&l->view);
    news(l, &l->air);
    l->power = power_of(&l->view);
    news(l, &l->power);
    l->look_at = l->air_at = l->power_at = now;
    l->synced = true;
    answer(l, TERN_C_SYNCED, q->seq);
}

static void send_request(struct link *l, const struct tern_companion_msg *q) {
    l->host.view(l->host.ctx, &l->view);
    if (!usable_address(l, q->address)) {
        error(l, q->seq, TERN_C_ERR_ADDRESS);
        return;
    }
    if (q->text_len == 0) {
        error(l, q->seq, TERN_C_ERR_REFUSED);
        return;
    }
    uint8_t d[8];
    digest(q->text, q->text_len, d);
    for (size_t i = 0; i < LINK_REFS; i++) {
        const struct link_ref *r = &l->refs[i];
        if (r->used && r->ref == q->ref && memcmp(r->to, q->address, TERN_ADDRESS_LEN) == 0 &&
            memcmp(r->digest, d, sizeof d) == 0) {
            struct tern_companion_msg a = {.type = TERN_C_QUEUED, .seq = q->seq, .id = r->id};
            send_msg(l, &a);
            return;
        }
    }
    struct link_message *x = room(l);
    if (x == NULL) {
        error(l, q->seq, TERN_C_ERR_FULL);
        return;
    }
    /* Answered before the news of it, as the draft's exchange has it. */
    struct tern_companion_msg a = {.type = TERN_C_QUEUED, .seq = q->seq, .id = l->next_id};
    send_msg(l, &a);
    uint32_t id = link_add(l, q->address, l->view.time, TERN_C_WAITING,
                           l->host.why(l->host.ctx, q->address), q->text, q->text_len);
    struct link_ref *r = &l->refs[l->next_ref];
    *r = (struct link_ref){.used = true, .ref = q->ref, .id = id};
    memcpy(r->to, q->address, TERN_ADDRESS_LEN);
    memcpy(r->digest, d, sizeof d);
    l->next_ref = (l->next_ref + 1) % LINK_REFS;
}

static void read_request(struct link *l, const struct tern_companion_msg *q) {
    answer(l, TERN_C_OK, q->seq);
    for (size_t i = 0; i < LINK_MESSAGES; i++) {
        struct link_message *x = &l->messages[i];
        if (x->used && x->state == TERN_C_RECEIVED && x->id <= q->through &&
            !(x->flags & TERN_C_READ_FLAG)) {
            x->flags |= TERN_C_READ_FLAG;
            news_message(l, x);
        }
    }
}

static void save_contact(struct link *l, const struct tern_companion_msg *q) {
    l->host.view(l->host.ctx, &l->view);
    if (!usable_address(l, q->address)) {
        error(l, q->seq, TERN_C_ERR_ADDRESS);
        return;
    }
    struct link_contact *c = find_contact(l, q->address);
    for (size_t i = 0; c == NULL && i < LINK_CONTACTS; i++) {
        if (!l->contacts[i].used) {
            c = &l->contacts[i];
        }
    }
    if (c == NULL) {
        error(l, q->seq, TERN_C_ERR_FULL);
        return;
    }
    struct link_contact was = *c;
    *c = (struct link_contact){.used = true, .name_len = q->text_len};
    memcpy(c->address, q->address, TERN_ADDRESS_LEN);
    memcpy(c->name, q->text, q->text_len);
    if (!l->host.save(l->host.ctx, l->contacts, sizeof l->contacts)) {
        *c = was;
        error(l, q->seq, TERN_C_ERR_NOT_NOW);
        return;
    }
    answer(l, TERN_C_OK, q->seq);
    news_contact(l, c);
}

static void remove_contact(struct link *l, const struct tern_companion_msg *q) {
    struct link_contact *c = find_contact(l, q->address);
    if (c != NULL) {
        struct link_contact was = *c;
        c->used = false;
        if (!l->host.save(l->host.ctx, l->contacts, sizeof l->contacts)) {
            *c = was;
            error(l, q->seq, TERN_C_ERR_NOT_NOW);
            return;
        }
    }
    answer(l, TERN_C_OK, q->seq);
    if (c != NULL) {
        struct tern_companion_msg m = {.type = TERN_C_CONTACT_GONE};
        memcpy(m.address, q->address, TERN_ADDRESS_LEN);
        news(l, &m);
    }
}

/* A serial client that has asked nothing for the lapse is taken for gone. */
static void lapse(struct link *l, tern_time now) {
    if (l->hello && l->host.lapse != 0 && now - l->answered_at >= l->host.lapse) {
        l->hello = l->synced = false; /* whoever opens the port next says HELLO */
    }
}

void link_receive(struct link *l, tern_time now, const uint8_t *frame, size_t len) {
    static struct tern_companion_msg q; /* a request is answered before the next is read */
    memset(&q, 0, sizeof q);
    enum tern_companion_read r = tern_companion_read(&q, frame, len);
    if (r == TERN_C_READ_SHORT || !tern_companion_request(frame[0])) {
        return; /* nothing to answer: too short, or not a request */
    }
    /* Late is gone, even if link_tick has not yet looked. Every request is answered here, before
     * the next is read, so now is when this one was. */
    lapse(l, now);
    l->answered_at = now;
    if (r != TERN_C_READ_OK) {
        error(l, frame[1], (uint8_t)r);
        return;
    }
    if (!l->hello && q.type != TERN_C_HELLO) {
        error(l, q.seq, TERN_C_ERR_HELLO_FIRST);
        return;
    }
    switch (q.type) {
    case TERN_C_HELLO: {
        struct tern_companion_msg a = {
            .type = TERN_C_INFO, .seq = q.seq, .version = TERN_COMPANION_VERSION};
        put_text(&a, (const uint8_t *)l->host.firmware,
                 cstr_len(l->host.firmware, TERN_COMPANION_FIRMWARE_MAX));
        send_msg(l, &a);
        l->hello = true;
        l->synced = false;
        l->news = 0;
        break;
    }
    case TERN_C_SYNC:
        sync(l, &q, now);
        break;
    case TERN_C_PING:
        answer(l, TERN_C_OK, q.seq);
        break;
    case TERN_C_SET_TIME:
        l->host.set_time(l->host.ctx, q.time);
        answer(l, TERN_C_OK, q.seq);
        break;
    case TERN_C_SET: {
        uint8_t code = l->host.set(l->host.ctx, &q);
        if (code != 0) {
            error(l, q.seq, code);
        } else {
            answer(l, TERN_C_OK, q.seq);
        }
        break;
    }
    case TERN_C_SEND:
        send_request(l, &q);
        break;
    case TERN_C_READ:
        read_request(l, &q);
        break;
    case TERN_C_SAVE_CONTACT:
        save_contact(l, &q);
        break;
    case TERN_C_REMOVE_CONTACT:
        remove_contact(l, &q);
        break;
    default:
        error(l, q.seq, TERN_C_ERR_UNKNOWN);
        break;
    }
}

/* --- Telling what changed -------------------------------------------------------------------- */

static bool same_air(const struct tern_companion_msg *a, const struct tern_companion_msg *b) {
    return a->period == b->period && a->allowed == b->allowed && a->used == b->used &&
           a->wait == b->wait;
}

static bool same_power(const struct tern_companion_msg *a, const struct tern_companion_msg *b) {
    return a->millivolts == b->millivolts && a->percent == b->percent && a->flags == b->flags;
}

static void look_at_neighbours(struct link *l, tern_time now) {
    const struct link_view *v = &l->view;
    /* Gone: told of, and no longer there. */
    for (size_t i = 0; i < l->n_told;) {
        bool there = false;
        for (size_t k = 0; !there && k < v->n_neighbours; k++) {
            there = v->neighbours[k].id == l->told[i].n.id;
        }
        if (there) {
            i++;
            continue;
        }
        struct tern_companion_msg m = {.type = TERN_C_NEIGHBOUR_GONE,
                                       .routing_id = l->told[i].n.id};
        news(l, &m);
        l->told[i] = l->told[--l->n_told];
    }
    /* New, or changed enough to say, and not said of lately. */
    for (size_t k = 0; k < v->n_neighbours; k++) {
        const struct link_neighbour *n = &v->neighbours[k];
        struct link_told *t = NULL;
        for (size_t i = 0; t == NULL && i < l->n_told; i++) {
            t = l->told[i].n.id == n->id ? &l->told[i] : NULL;
        }
        if (t == NULL) {
            if (l->n_told == LINK_NEIGHBOURS) {
                continue;
            }
            t = &l->told[l->n_told++];
        } else {
            int moved = t->n.snr - n->snr;
            bool changed =
                t->n.role != n->role || moved >= LINK_SNR_STEP || -moved >= LINK_SNR_STEP;
            if (!changed || now - t->at < LINK_QUIET) {
                continue;
            }
        }
        news_neighbour(l, n);
        *t = (struct link_told){*n, now};
    }
}

void link_tick(struct link *l, tern_time now) {
    lapse(l, now);
    /* Before a sync there is nothing told to say what changed from: the sync tells it all. */
    if (!l->synced || now - l->look_at < LINK_LOOK) {
        return;
    }
    l->look_at = now;
    l->host.view(l->host.ctx, &l->view);
    const struct link_view *v = &l->view;
    if (v->role != l->self_role || v->power != l->self_power || v->region != l->self_region) {
        news_self(l);
    }
    look_at_neighbours(l, now);
    struct tern_companion_msg air = airtime_of(v), power = power_of(v);
    if (!same_air(&air, &l->air) && now - l->air_at >= LINK_QUIET) {
        l->air = air;
        l->air_at = now;
        news(l, &l->air);
    }
    if (!same_power(&power, &l->power) && now - l->power_at >= LINK_QUIET) {
        l->power = power;
        l->power_at = now;
        news(l, &l->power);
    }
}
