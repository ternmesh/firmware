#include "link.h"

#include <string.h>

#include "tern/crypto.h"

/* --- Frames out ----------------------------------------------------------------------------- */

static void send_msg(struct link *l, struct link_conn *c, const struct tern_companion_msg *m) {
    uint8_t frame[TERN_COMPANION_MAX_FRAME];
    size_t len = tern_companion_write(m, frame);
    if (len != 0) {
        l->host.out(l->host.ctx, (unsigned)(c - l->conns), frame, len);
    }
}

/* Answers go to the client that asked. */
static void answer(struct link *l, uint8_t type, uint8_t seq) {
    struct tern_companion_msg m = {.type = type, .seq = seq};
    send_msg(l, l->asker, &m);
}

static void error(struct link *l, uint8_t seq, uint8_t code) {
    struct tern_companion_msg m = {.type = TERN_C_ERROR, .seq = seq, .code = code};
    send_msg(l, l->asker, &m);
}

/* News goes only to a client that has said HELLO, numbered by its count, and only news its
 * version defines. */
static void tell(struct link *l, struct link_conn *c, struct tern_companion_msg *m) {
    if (!c->hello || (m->type == TERN_C_ASKED && c->version < 1)) {
        return;
    }
    m->seq = c->news++;
    send_msg(l, c, m);
}

/* News of what a request or the board changed goes to every client (draft/companion.md, "News"):
 * to one, if `to` names it, and to all if it is NULL. */
static void news(struct link *l, struct link_conn *to, struct tern_companion_msg *m) {
    for (size_t i = 0; i < LINK_CONNS; i++) {
        if (to == NULL || to == &l->conns[i]) {
            tell(l, &l->conns[i], m);
        }
    }
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

static void news_self(struct link *l, struct link_conn *c) {
    const struct link_view *v = &l->view;
    struct tern_companion_msg m = {
        .type = TERN_C_SELF, .role = v->role, .power = v->power, .time = v->time};
    memcpy(m.address, v->address, TERN_ADDRESS_LEN);
    put_text(&m, (const uint8_t *)v->region, cstr_len(v->region, TERN_COMPANION_REGION_MAX));
    c->self_role = v->role;
    c->self_power = v->power;
    c->self_region = v->region;
    tell(l, c, &m);
}

static void news_contact(struct link *l, struct link_conn *to, const struct link_contact *c) {
    struct tern_companion_msg m = {.type = TERN_C_CONTACT};
    memcpy(m.address, c->address, TERN_ADDRESS_LEN);
    m.session = l->host.session(l->host.ctx, c->address) ? 1 : 0;
    put_text(&m, c->name, c->name_len);
    news(l, to, &m);
}

static void news_message(struct link *l, struct link_conn *to, const struct link_message *x) {
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
    news(l, to, &m);
}

static void news_neighbour(struct link *l, struct link_conn *c, const struct link_neighbour *n) {
    struct tern_companion_msg m = {.type = TERN_C_NEIGHBOUR,
                                   .routing_id = n->id,
                                   .role = n->role,
                                   .snr = n->snr,
                                   .heard = n->heard};
    tell(l, c, &m);
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

bool link_contact(const struct link *l, const uint8_t address[TERN_ADDRESS_LEN]) {
    for (size_t i = 0; i < LINK_CONTACTS; i++) {
        if (l->contacts[i].used && memcmp(l->contacts[i].address, address, TERN_ADDRESS_LEN) == 0) {
            return true;
        }
    }
    return false;
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
    return x->state == TERN_C_WAITING && !x->taken;
}

/* A free place for a message: an empty one, or else the oldest whose end is known. One still
 * waiting keeps its place, with the forwarder or not: what becomes of it is yet to be told. */
static struct link_message *room(struct link *l) {
    struct link_message *oldest = NULL;
    for (size_t i = 0; i < LINK_MESSAGES; i++) {
        struct link_message *x = &l->messages[i];
        if (!x->used) {
            return x;
        }
        if (x->state != TERN_C_WAITING && (oldest == NULL || x->id < oldest->id)) {
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
    uint32_t saved = 0;
    if (l->host.load_ids(l->host.ctx, &saved) && saved > 1) {
        l->next_id = saved;
        l->ids_saved = saved;
    }
    if (!l->host.load(l->host.ctx, l->contacts, sizeof l->contacts)) {
        memset(l->contacts, 0, sizeof l->contacts);
    }
}

/* Sets the next message id aside in flash, if it is not yet. Done before the id is told to
 * anyone, so that a restart at any moment after cannot give it again. */
static void reserve_id(struct link *l) {
    if (l->next_id >= l->ids_saved) {
        uint32_t to = l->next_id + LINK_ID_STEP;
        if (l->host.save_ids(l->host.ctx, to)) {
            l->ids_saved = to;
        }
    }
}

uint32_t link_add(struct link *l, const uint8_t address[TERN_ADDRESS_LEN], uint32_t time,
                  uint8_t state, uint8_t reason, const uint8_t *text, size_t len) {
    size_t keep = tern_companion_utf8_prefix(text, len, TERN_COMPANION_TEXT_MAX);
    struct link_message *x = keep == 0 ? NULL : room(l);
    if (x == NULL) {
        return 0;
    }
    reserve_id(l);
    *x = (struct link_message){
        .used = true, .id = l->next_id++, .time = time, .state = state, .reason = reason};
    memcpy(x->address, address, TERN_ADDRESS_LEN);
    memcpy(x->text, text, keep);
    x->text_len = (uint8_t)keep;
    news_message(l, NULL, x);
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
        news(l, NULL, &m);
    }
}

void link_taken(struct link *l, uint32_t id) {
    struct link_message *x = find_message(l, id);
    if (x != NULL) {
        x->taken = true;
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
        news_contact(l, NULL, c);
    }
}

void link_asked(struct link *l, tern_time now, const uint8_t address[TERN_ADDRESS_LEN],
                uint8_t why) {
    struct link_asked *a = NULL;
    for (size_t i = 0; i < LINK_ASKED; i++) {
        struct link_asked *x = &l->asked[i];
        if (x->used && memcmp(x->address, address, TERN_ADDRESS_LEN) == 0) {
            if (now - x->at < LINK_QUIET) {
                return;
            }
            a = x;
            break;
        }
        /* Else an empty place, or the one told of longest ago. */
        if (a == NULL || (a->used && (!x->used || x->at < a->at))) {
            a = x;
        }
    }
    *a = (struct link_asked){.used = true, .at = now};
    memcpy(a->address, address, TERN_ADDRESS_LEN);
    struct tern_companion_msg m = {.type = TERN_C_ASKED, .why = why};
    memcpy(m.address, address, TERN_ADDRESS_LEN);
    news(l, NULL, &m);
}

/* --- Requests -------------------------------------------------------------------------------- */

static bool usable_address(struct link *l, const uint8_t address[TERN_ADDRESS_LEN]) {
    return memcmp(address, l->view.address, TERN_ADDRESS_LEN) != 0 && tern_address_valid(address);
}

/* Tells the client that asked everything, and remembers what it was told. */
static void sync(struct link *l, const struct tern_companion_msg *q, tern_time now) {
    struct link_conn *c = l->asker;
    l->host.view(l->host.ctx, &l->view);
    news_self(l, c);
    for (size_t i = 0; i < LINK_CONTACTS; i++) {
        if (l->contacts[i].used) {
            news_contact(l, c, &l->contacts[i]);
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
        news_message(l, c, next);
        after = next->id;
    }
    c->n_told = 0;
    for (size_t i = 0; i < l->view.n_neighbours && i < LINK_NEIGHBOURS; i++) {
        news_neighbour(l, c, &l->view.neighbours[i]);
        c->told[c->n_told++] = (struct link_told){l->view.neighbours[i], now};
    }
    c->air = airtime_of(&l->view);
    tell(l, c, &c->air);
    c->power = power_of(&l->view);
    tell(l, c, &c->power);
    c->look_at = c->air_at = c->power_at = now;
    c->synced = true;
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
            send_msg(l, l->asker, &a);
            return;
        }
    }
    struct link_message *x = room(l);
    if (x == NULL) {
        error(l, q->seq, TERN_C_ERR_FULL);
        return;
    }
    /* Answered before the news of it, as the draft's exchange has it, and the id set aside
     * before the answer names it. */
    reserve_id(l);
    struct tern_companion_msg a = {.type = TERN_C_QUEUED, .seq = q->seq, .id = l->next_id};
    send_msg(l, l->asker, &a);
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
            news_message(l, NULL, x);
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
    news_contact(l, NULL, c);
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
        news(l, NULL, &m);
    }
}

static void end_session(struct link *l, const struct tern_companion_msg *q) {
    uint8_t code = l->host.end_session(l->host.ctx, q->address);
    if (code != 0) {
        error(l, q->seq, code);
        return;
    }
    answer(l, TERN_C_OK, q->seq);
    /* Every message to it whose end is not known, with the forwarder or not: the board has let go
     * of those, and said nothing, so that the answer comes before the news. */
    for (size_t i = 0; i < LINK_MESSAGES; i++) {
        struct link_message *x = &l->messages[i];
        if (x->used && (x->state == TERN_C_WAITING || x->state == TERN_C_SENT) &&
            memcmp(x->address, q->address, TERN_ADDRESS_LEN) == 0) {
            link_state(l, x->id, TERN_C_NOT_DELIVERED, 0, 0);
        }
    }
    link_session_changed(l, q->address);
}

/* A serial client that has asked nothing for the lapse is taken for gone. */
static void lapse(struct link_conn *c, tern_time now) {
    if (c->hello && c->lapse != 0 && now - c->answered_at >= c->lapse) {
        c->hello = c->synced = false; /* whoever opens the port next says HELLO */
    }
}

void link_open(struct link *l, unsigned conn, tern_time lapse, size_t mtu) {
    struct link_conn *c = &l->conns[conn];
    *c = (struct link_conn){.open = true, .lapse = lapse, .mtu = mtu};
}

void link_mtu(struct link *l, unsigned conn, size_t mtu) { l->conns[conn].mtu = mtu; }

void link_close(struct link *l, unsigned conn) {
    l->conns[conn] = (struct link_conn){.open = false};
}

void link_receive(struct link *l, unsigned conn, tern_time now, const uint8_t *frame, size_t len) {
    static struct tern_companion_msg q; /* a request is answered before the next is read */
    struct link_conn *c = &l->conns[conn];
    if (!c->open) {
        return;
    }
    l->asker = c;
    memset(&q, 0, sizeof q);
    enum tern_companion_read r = tern_companion_read(&q, frame, len);
    if (r == TERN_C_READ_SHORT || !tern_companion_request(frame[0])) {
        return; /* nothing to answer: too short, or not a request */
    }
    /* Late is gone, even if link_tick has not yet looked. Every request is answered here, before
     * the next is read, so now is when this one was. */
    lapse(c, now);
    c->answered_at = now;
    if (r != TERN_C_READ_OK) {
        error(l, frame[1], (uint8_t)r);
        return;
    }
    if (!c->hello && q.type != TERN_C_HELLO) {
        error(l, q.seq, TERN_C_ERR_HELLO_FIRST);
        return;
    }
    switch (q.type) {
    case TERN_C_HELLO: {
        /* Over Bluetooth a frame is one notification, which the ATT MTU bounds: 3 bytes of it
         * are the ATT header. */
        if (c->mtu != 0 && c->mtu < TERN_COMPANION_MAX_FRAME + 3) {
            error(l, q.seq, TERN_C_ERR_MTU);
            break;
        }
        struct tern_companion_msg a = {
            .type = TERN_C_INFO, .seq = q.seq, .version = TERN_COMPANION_VERSION};
        put_text(&a, (const uint8_t *)l->host.firmware,
                 cstr_len(l->host.firmware, TERN_COMPANION_FIRMWARE_MAX));
        send_msg(l, c, &a);
        c->hello = true;
        c->version = q.version;
        c->synced = false;
        c->news = 0;
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
    case TERN_C_END_SESSION:
        end_session(l, &q);
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

static void look_at_neighbours(struct link *l, struct link_conn *c, tern_time now) {
    const struct link_view *v = &l->view;
    /* Gone: told of, and no longer there. */
    for (size_t i = 0; i < c->n_told;) {
        bool there = false;
        for (size_t k = 0; !there && k < v->n_neighbours; k++) {
            there = v->neighbours[k].id == c->told[i].n.id;
        }
        if (there) {
            i++;
            continue;
        }
        struct tern_companion_msg m = {.type = TERN_C_NEIGHBOUR_GONE,
                                       .routing_id = c->told[i].n.id};
        tell(l, c, &m);
        c->told[i] = c->told[--c->n_told];
    }
    /* New, or changed enough to say, and not said of lately. */
    for (size_t k = 0; k < v->n_neighbours; k++) {
        const struct link_neighbour *n = &v->neighbours[k];
        struct link_told *t = NULL;
        for (size_t i = 0; t == NULL && i < c->n_told; i++) {
            t = c->told[i].n.id == n->id ? &c->told[i] : NULL;
        }
        if (t == NULL) {
            if (c->n_told == LINK_NEIGHBOURS) {
                continue;
            }
            t = &c->told[c->n_told++];
        } else {
            int moved = t->n.snr - n->snr;
            bool changed =
                t->n.role != n->role || moved >= LINK_SNR_STEP || -moved >= LINK_SNR_STEP;
            if (!changed || now - t->at < LINK_QUIET) {
                continue;
            }
        }
        news_neighbour(l, c, n);
        *t = (struct link_told){*n, now};
    }
}

static void look(struct link *l, struct link_conn *c, tern_time now) {
    c->look_at = now;
    const struct link_view *v = &l->view;
    if (v->role != c->self_role || v->power != c->self_power || v->region != c->self_region) {
        news_self(l, c);
    }
    look_at_neighbours(l, c, now);
    struct tern_companion_msg air = airtime_of(v), power = power_of(v);
    if (!same_air(&air, &c->air) && now - c->air_at >= LINK_QUIET) {
        c->air = air;
        c->air_at = now;
        tell(l, c, &c->air);
    }
    if (!same_power(&power, &c->power) && now - c->power_at >= LINK_QUIET) {
        c->power = power;
        c->power_at = now;
        tell(l, c, &c->power);
    }
}

void link_tick(struct link *l, tern_time now) {
    bool viewed = false;
    for (size_t i = 0; i < LINK_CONNS; i++) {
        struct link_conn *c = &l->conns[i];
        lapse(c, now);
        /* Before a sync there is nothing told to say what changed from: the sync tells it all. */
        if (!c->synced || now - c->look_at < LINK_LOOK) {
            continue;
        }
        if (!viewed) {
            l->host.view(l->host.ctx, &l->view);
            viewed = true;
        }
        look(l, c, now);
    }
}
