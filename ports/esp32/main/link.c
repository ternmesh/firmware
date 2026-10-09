#include "link.h"

#include <string.h>

#include "tern/crypto.h"

/* --- Frames out ----------------------------------------------------------------------------- */

/* Each client gets a frame as its version has it: a SYNCED to one of version 2 is two bytes. */
static void send_msg(struct link *l, struct link_conn *c, const struct tern_companion_msg *m) {
    uint8_t frame[TERN_COMPANION_MAX_FRAME];
    size_t len = tern_companion_write_as(m, frame, c->version);
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
    if (!c->hello || c->version < tern_companion_since(m->type)) {
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

/* The frame a message is told of in, by its kind. */
static uint8_t type_of(const struct link_message *x) {
    return x->kind == LINK_KIND_GROUP    ? TERN_C_GROUP_MESSAGE
           : x->kind == LINK_KIND_INVITE ? TERN_C_INVITE
                                         : TERN_C_MESSAGE;
}

/* A message as news, of whichever kind it is. */
static void news_message(struct link *l, struct link_conn *to, const struct link_message *x) {
    struct tern_companion_msg m = {
        .type = type_of(x),
        .id = x->id,
        .from = x->from,
        .time = x->time,
        .flags = x->flags,
        .state = x->state,
        .reason = x->reason,
        .wait = x->wait,
    };
    memcpy(m.address, x->address, TERN_ADDRESS_LEN);
    memcpy(m.group, x->group, sizeof m.group);
    put_text(&m, x->text, x->text_len);
    news(l, to, &m);
}

static void news_group(struct link *l, struct link_conn *to, const struct link_group *g) {
    struct tern_companion_msg m = {.type = TERN_C_GROUP};
    memcpy(m.group, g->id, sizeof m.group);
    put_text(&m, g->name, g->name_len);
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

/* A position held, as a POSITION or GROUP_POSITION record: the centre of its cell, and its age
 * as of now. Precision 0 when nothing is held. */
static void put_position(struct tern_companion_msg *m, const struct tern_position *p, bool holds,
                         tern_time at, tern_time now) {
    m->altitude = 0;
    if (!holds || p->precision == 0) {
        return;
    }
    m->precision = p->precision;
    tern_position_centre(p, &m->lat, &m->lon);
    m->altitude = p->fields & TERN_POSITION_ALTITUDE ? p->altitude : TERN_C_NO_ALTITUDE;
    m->accuracy = p->fields & TERN_POSITION_ACCURACY ? p->accuracy : 0;
    tern_time since = now > at ? (now - at) / TERN_S(1) : 0;
    uint64_t age = (uint64_t)(p->fields & TERN_POSITION_AGE ? p->age : 0) + (uint64_t)since;
    m->age = age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
}

static void news_position(struct link *l, struct link_conn *to, size_t place, tern_time now) {
    const struct tern_position_held *h = &l->contact_positions[place];
    struct tern_companion_msg m = {.type = TERN_C_POSITION};
    memcpy(m.address, l->contacts[place].address, TERN_ADDRESS_LEN);
    put_position(&m, &h->position, h->holds, h->at, now);
    news(l, to, &m);
}

static void news_group_position(struct link *l, struct link_conn *to, size_t place,
                                const struct link_member_position *x, tern_time now) {
    struct tern_companion_msg m = {.type = TERN_C_GROUP_POSITION, .from = x->from};
    memcpy(m.group, l->groups[place].id, sizeof m.group);
    put_position(&m, &x->position, x->used, x->at, now);
    news(l, to, &m);
}

/* SHARING or GROUP_SHARING: minutes left rounded up, as of now. */
static void put_share(struct tern_companion_msg *m, const struct link_share *s, tern_time now) {
    if (s->precision == 0) {
        return;
    }
    m->precision = s->precision;
    m->fields = s->fields;
    m->interval = s->interval;
    if (s->until != 0) {
        tern_time left = s->until > now ? s->until - now : 0;
        tern_time minutes = (left + TERN_S(60) - 1) / TERN_S(60);
        m->minutes = (uint16_t)(minutes > UINT16_MAX ? UINT16_MAX : minutes);
    }
}

static void news_sharing(struct link *l, struct link_conn *to, size_t place, tern_time now) {
    struct tern_companion_msg m = {.type = TERN_C_SHARING};
    memcpy(m.address, l->contact_shares[place].address, TERN_ADDRESS_LEN);
    put_share(&m, &l->contact_shares[place], now);
    news(l, to, &m);
}

static void news_group_sharing(struct link *l, struct link_conn *to, size_t place, tern_time now) {
    struct tern_companion_msg m = {.type = TERN_C_GROUP_SHARING};
    memcpy(m.group, l->groups[place].id, sizeof m.group);
    put_share(&m, &l->group_shares[place], now);
    news(l, to, &m);
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

/* --- Groups ------------------------------------------------------------------------------------
 */

static void hold_group(struct link_group *g, const uint8_t secret[TERN_GROUP_SECRET],
                       const uint8_t *name, size_t name_len) {
    g->used = true;
    tern_group_init(&g->g, secret);
    tern_companion_group_id(secret, g->id);
    g->name_len = (uint8_t)name_len;
    memcpy(g->name, name, name_len);
}

static struct link_group *find_group(struct link *l, const uint8_t id[TERN_COMPANION_GROUP]) {
    for (size_t i = 0; i < LINK_GROUPS; i++) {
        if (l->groups[i].used && memcmp(l->groups[i].id, id, TERN_COMPANION_GROUP) == 0) {
            return &l->groups[i];
        }
    }
    return NULL;
}

static struct link_group *free_group(struct link *l) {
    for (size_t i = 0; i < LINK_GROUPS; i++) {
        if (!l->groups[i].used) {
            return &l->groups[i];
        }
    }
    return NULL;
}

static bool save_groups(struct link *l) {
    struct link_group_saved kept[LINK_GROUPS] = {0};
    for (size_t i = 0; i < LINK_GROUPS; i++) {
        const struct link_group *g = &l->groups[i];
        if (g->used) {
            kept[i].used = true;
            kept[i].name_len = g->name_len;
            memcpy(kept[i].secret, g->g.secret, TERN_GROUP_SECRET);
            memcpy(kept[i].name, g->name, g->name_len);
        }
    }
    bool ok = l->host.save_groups(l->host.ctx, kept, sizeof kept);
    tern_wipe(kept, sizeof kept);
    return ok;
}

bool link_group_count(struct link *l, uint32_t *count) {
    if (l->next_count == UINT32_MAX) {
        return false;
    }
    if (l->next_count >= l->counts_saved) {
        uint32_t to = l->next_count > UINT32_MAX - LINK_COUNT_STEP
                          ? UINT32_MAX
                          : l->next_count + LINK_COUNT_STEP;
        if (!l->host.save_count(l->host.ctx, to)) {
            return false;
        }
        l->counts_saved = to;
    }
    *count = l->next_count++;
    return true;
}

void link_group_heard(struct link *l) { l->writers_changed = true; }

bool link_keep_writers(struct link *l) {
    if (!l->writers_changed) {
        return false;
    }
    uint8_t kept[LINK_WRITERS_MAX];
    size_t len = 0;
    for (size_t i = 0; i < LINK_GROUPS; i++) {
        const struct link_group *g = &l->groups[i];
        size_t n = g->used ? tern_group_keep(&g->g, kept + len + TERN_COMPANION_GROUP + 1) : 0;
        if (n != 0) {
            memcpy(kept + len, g->id, TERN_COMPANION_GROUP);
            kept[len + TERN_COMPANION_GROUP] = (uint8_t)(n / 8);
            len += TERN_COMPANION_GROUP + 1 + n;
        }
    }
    if (l->host.save_writers(l->host.ctx, kept, len)) {
        l->writers_changed = false;
    }
    return true;
}

/* Gives each group held the writers flash kept for it. What is not as link_keep_writers() wrote
 * it is passed over from there on. */
static void load_writers(struct link *l) {
    uint8_t kept[LINK_WRITERS_MAX];
    size_t len = l->host.load_writers(l->host.ctx, kept, sizeof kept), at = 0;
    while (len <= sizeof kept && at + TERN_COMPANION_GROUP + 1 <= len) {
        size_t n = (size_t)kept[at + TERN_COMPANION_GROUP] * 8;
        const uint8_t *writers = kept + at + TERN_COMPANION_GROUP + 1;
        if (n == 0 || n > TERN_GROUP_KEPT || n > len - (size_t)(writers - kept)) {
            return;
        }
        struct link_group *g = find_group(l, kept + at);
        if (g != NULL) {
            tern_group_restore(&g->g, writers, n);
        }
        at += TERN_COMPANION_GROUP + 1 + n;
    }
}

void link_groups(struct link *l, struct tern_group *out[LINK_GROUPS]) {
    for (size_t i = 0; i < LINK_GROUPS; i++) {
        out[i] = l->groups[i].used ? &l->groups[i].g : NULL;
    }
}

const struct tern_group *link_group_of(struct link *l, const struct link_message *x) {
    const struct link_group *g = find_group(l, x->group);
    return g != NULL ? &g->g : NULL;
}

size_t link_invite_write(struct link *l, const struct link_message *x,
                         uint8_t out[TERN_GROUP_INVITE_MAX]) {
    const struct link_group *g = find_group(l, x->group);
    return g == NULL ? 0 : tern_group_invite_write(g->g.secret, x->text, x->text_len, out);
}

/* --- Messages in flash -------------------------------------------------------------------------
 *
 * What is written once: a version, the kind, the state and flags it began with, then the id, the
 * time, the address, the group, the writer, an invite's secret and the text. What changes after is
 * a word: the id it is of, so that a word left by the place's last message is not taken for this
 * one's, then the state, the flags and whether it was handed over. */

#define SAVED_VERSION 1
#define SAVED_ADDRESS 12
#define SAVED_GROUP (SAVED_ADDRESS + TERN_ADDRESS_LEN)
#define SAVED_FROM (SAVED_GROUP + TERN_COMPANION_GROUP)
#define SAVED_SECRET (SAVED_FROM + 4)
#define SAVED_LEN (SAVED_SECRET + TERN_GROUP_SECRET)
_Static_assert(LINK_SAVED_HEAD == SAVED_LEN + 1, "a saved message's head");

static void put32(uint8_t *p, uint32_t v) {
    for (size_t i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void save_state(struct link *l, const struct link_message *x) {
    if (x->saved) {
        uint64_t word = (uint64_t)x->id | (uint64_t)x->state << 32 | (uint64_t)x->flags << 40 |
                        (uint64_t)(x->taken ? 1 : 0) << 48;
        (void)l->host.save_state(l->host.ctx, (size_t)(x - l->messages), word);
    }
}

/* Takes the oldest saved message whose end is known out of flash, to make room there: the node
 * holds it still, until a restart. False if there is none but `but`. */
static bool unsave_oldest(struct link *l, const struct link_message *but) {
    struct link_message *oldest = NULL;
    for (size_t i = 0; i < LINK_MESSAGES; i++) {
        struct link_message *x = &l->messages[i];
        if (x->used && x->saved && x != but && x->state != TERN_C_WAITING &&
            (oldest == NULL || x->id < oldest->id)) {
            oldest = x;
        }
    }
    if (oldest == NULL || l->host.save_message(l->host.ctx, (size_t)(oldest - l->messages),
                                               (const uint8_t *)"", 0) != LINK_SAVED) {
        return false;
    }
    oldest->saved = false;
    return true;
}

/* Saves a message newly kept. Where flash has no room, the newest are the ones to have there, so
 * the oldest make way: LINK_UNSAVE of them at most, which is room for any message. A write that
 * failed for another reason takes nothing out. If it cannot be saved even so, what the place held
 * before is forgotten, so that a restart does not bring back a message the node has let go of. */
static void save_message(struct link *l, struct link_message *x) {
    uint8_t b[LINK_SAVED_MAX];
    size_t place = (size_t)(x - l->messages);
    b[0] = SAVED_VERSION;
    b[1] = x->kind;
    b[2] = x->state;
    b[3] = x->flags;
    put32(b + 4, x->id);
    put32(b + 8, x->time);
    memcpy(b + SAVED_ADDRESS, x->address, TERN_ADDRESS_LEN);
    memcpy(b + SAVED_GROUP, x->group, TERN_COMPANION_GROUP);
    put32(b + SAVED_FROM, x->from);
    memcpy(b + SAVED_SECRET, x->secret, TERN_GROUP_SECRET);
    b[SAVED_LEN] = x->text_len;
    memcpy(b + LINK_SAVED_HEAD, x->text, x->text_len);
    size_t len = LINK_SAVED_HEAD + x->text_len;
    enum link_saved how = l->host.save_message(l->host.ctx, place, b, len);
    for (unsigned i = 0; i < LINK_UNSAVE && how == LINK_NO_ROOM && unsave_oldest(l, x); i++) {
        how = l->host.save_message(l->host.ctx, place, b, len);
    }
    x->saved = how == LINK_SAVED;
    if (!x->saved) {
        (void)l->host.save_message(l->host.ctx, place, b, 0);
    }
    tern_wipe(b, sizeof b);
}

/* What the place held when the node last ran, as it stands now that it has restarted. */
static void load_message(struct link *l, size_t place) {
    uint8_t b[LINK_SAVED_MAX];
    struct link_message *x = &l->messages[place];
    size_t len = l->host.load_message(l->host.ctx, place, b, sizeof b);
    bool invite = len >= LINK_SAVED_HEAD && b[1] == LINK_KIND_INVITE;
    if (len < LINK_SAVED_HEAD || len > sizeof b || b[0] != SAVED_VERSION ||
        b[1] > LINK_KIND_INVITE || b[2] > TERN_C_RECEIVED || get32(b + 4) == 0 ||
        len != LINK_SAVED_HEAD + (size_t)b[SAVED_LEN] ||
        b[SAVED_LEN] > (invite ? TERN_COMPANION_NAME_MAX : TERN_COMPANION_TEXT_MAX) ||
        (b[SAVED_LEN] == 0 && !invite) || find_message(l, get32(b + 4)) != NULL) {
        tern_wipe(b, sizeof b);
        return;
    }
    x->kind = b[1];
    x->state = b[2];
    x->flags = b[3];
    x->id = get32(b + 4);
    x->time = get32(b + 8);
    memcpy(x->address, b + SAVED_ADDRESS, TERN_ADDRESS_LEN);
    memcpy(x->group, b + SAVED_GROUP, TERN_COMPANION_GROUP);
    x->from = get32(b + SAVED_FROM);
    memcpy(x->secret, b + SAVED_SECRET, TERN_GROUP_SECRET);
    x->text_len = b[SAVED_LEN];
    memcpy(x->text, b + LINK_SAVED_HEAD, x->text_len);
    tern_wipe(b, sizeof b);
    x->used = true;
    x->saved = true;

    uint64_t word = 0;
    bool received = x->state == TERN_C_RECEIVED;
    uint8_t state = 0;
    if (l->host.load_state(l->host.ctx, place, &word) && (uint32_t)word == x->id) {
        state = (uint8_t)(word >> 32);
        /* A received message stays one, and no other becomes one. */
        if (state <= TERN_C_RECEIVED && (state == TERN_C_RECEIVED) == received) {
            x->state = state;
            x->flags = (uint8_t)(word >> 40);
            x->taken = (word >> 48 & 1) != 0;
        }
    }
    if (x->id >= l->next_id) {
        l->next_id = x->id + 1;
    }
    if (x->state != TERN_C_WAITING && x->state != TERN_C_SENT) {
        return;
    }
    /* Whatever held its frame is gone. */
    bool taken = x->taken;
    x->taken = false;
    state = x->state;
    if (x->kind == LINK_KIND_GROUP) {
        if (state == TERN_C_WAITING && find_group(l, x->group) == NULL) {
            state = TERN_C_NOT_DELIVERED;
        }
    } else if (taken || state == TERN_C_SENT ||
               (x->kind == LINK_KIND_INVITE && find_group(l, x->group) == NULL)) {
        state = TERN_C_NOT_DELIVERED;
    }
    if (state != x->state || taken) {
        x->state = state;
        save_state(l, x);
    }
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
    struct link_group_saved kept[LINK_GROUPS];
    if (l->host.load_groups(l->host.ctx, kept, sizeof kept)) {
        for (size_t i = 0; i < LINK_GROUPS; i++) {
            if (kept[i].used && kept[i].name_len <= TERN_COMPANION_NAME_MAX) {
                hold_group(&l->groups[i], kept[i].secret, kept[i].name, kept[i].name_len);
            }
        }
    }
    tern_wipe(kept, sizeof kept);
    load_writers(l);
    if (l->host.load_count(l->host.ctx, &saved)) {
        l->next_count = saved;
        l->counts_saved = saved;
    }
    for (size_t i = 0; i < LINK_MESSAGES; i++) {
        load_message(l, i);
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

/* Keeps a message of any kind, and tells of it: `like` gives everything but its id and its text.
 * An invite's text is its group's name, which may be empty; any other's may not. */
static uint32_t keep_message(struct link *l, const struct link_message *like, const uint8_t *text,
                             size_t len) {
    bool invite = like->kind == LINK_KIND_INVITE;
    size_t keep = tern_companion_utf8_prefix(
        text, len, invite ? TERN_COMPANION_NAME_MAX : TERN_COMPANION_TEXT_MAX);
    struct link_message *x = keep == 0 && !invite ? NULL : room(l);
    if (x == NULL) {
        return 0;
    }
    reserve_id(l);
    tern_wipe(x->secret, sizeof x->secret);
    *x = *like;
    x->used = true;
    x->taken = false;
    x->id = l->next_id++;
    memcpy(x->text, text, keep);
    x->text_len = (uint8_t)keep;
    save_message(l, x);
    news_message(l, NULL, x);
    return x->id;
}

uint32_t link_add(struct link *l, const uint8_t address[TERN_ADDRESS_LEN], uint32_t time,
                  uint8_t state, uint8_t reason, const uint8_t *text, size_t len) {
    struct link_message x = {.time = time, .state = state, .reason = reason};
    memcpy(x.address, address, TERN_ADDRESS_LEN);
    return keep_message(l, &x, text, len);
}

uint32_t link_add_group(struct link *l, size_t place, uint32_t from, uint32_t time,
                        const uint8_t *text, size_t len) {
    if (place >= LINK_GROUPS || !l->groups[place].used) {
        return 0;
    }
    struct link_message x = {
        .kind = LINK_KIND_GROUP, .time = time, .state = TERN_C_RECEIVED, .from = from};
    memcpy(x.group, l->groups[place].id, sizeof x.group);
    return keep_message(l, &x, text, len);
}

uint32_t link_add_invite(struct link *l, const uint8_t from[TERN_ADDRESS_LEN], uint32_t time,
                         const uint8_t secret[TERN_GROUP_SECRET], const uint8_t *name,
                         size_t name_len) {
    struct link_message x = {.kind = LINK_KIND_INVITE, .time = time, .state = TERN_C_RECEIVED};
    memcpy(x.address, from, TERN_ADDRESS_LEN);
    memcpy(x.secret, secret, sizeof x.secret);
    tern_companion_group_id(secret, x.group);
    uint32_t id = keep_message(l, &x, name, name_len);
    tern_wipe(&x, sizeof x);
    return id;
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
    bool moved = x->state != state;
    x->state = state;
    x->reason = reason;
    x->wait = wait;
    if (moved) {
        save_state(l, x);
    }
    if (changed) {
        /* Only to a client that was told of the message: one from before groups was not sent a
         * group message or an invite, and news of what became of one would be of nothing. */
        uint8_t since = tern_companion_since(type_of(x));
        for (size_t i = 0; i < LINK_CONNS; i++) {
            struct tern_companion_msg m = {
                .type = TERN_C_STATE, .id = id, .state = state, .reason = reason, .wait = wait};
            if (l->conns[i].version >= since) {
                tell(l, &l->conns[i], &m);
            }
        }
    }
}

bool link_wanted(struct link *l, uint32_t id) {
    const struct link_message *x = find_message(l, id);
    return x != NULL && (x->state == TERN_C_WAITING || x->state == TERN_C_SENT);
}

void link_taken(struct link *l, uint32_t id) {
    struct link_message *x = find_message(l, id);
    if (x != NULL) {
        x->taken = true;
        save_state(l, x);
        link_state(l, id, TERN_C_WAITING, TERN_C_WAIT_UNNAMED, 0);
    }
}

void link_unreachable(struct link *l, const uint8_t address[TERN_ADDRESS_LEN]) {
    for (size_t i = 0; i < LINK_MESSAGES; i++) {
        struct link_message *x = &l->messages[i];
        if (x->used && x->kind != LINK_KIND_GROUP && waiting(x) &&
            x->reason == TERN_C_WAIT_SESSION &&
            memcmp(x->address, address, TERN_ADDRESS_LEN) == 0) {
            link_state(l, x->id, TERN_C_NOT_DELIVERED, 0, 0);
        }
    }
}

void link_session_changed(struct link *l, const uint8_t address[TERN_ADDRESS_LEN]) {
    const struct link_contact *c = find_contact(l, address);
    if (c != NULL) {
        /* The old session's messages can no longer be opened, so a new one starts with no
         * counter to hold a position to (draft/positions.md, "Older positions"). */
        l->contact_positions[c - l->contacts].counted = false;
        l->contact_positions[c - l->contacts].counter = 0;
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
    for (size_t i = 0; i < LINK_GROUPS; i++) {
        if (l->groups[i].used) {
            news_group(l, c, &l->groups[i]);
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
    for (size_t i = 0; i < LINK_CONTACTS; i++) {
        if (l->contacts[i].used && l->contact_positions[i].holds) {
            news_position(l, c, i, now);
        }
    }
    for (size_t i = 0; i < LINK_GROUPS; i++) {
        for (size_t k = 0; l->groups[i].used && k < LINK_GROUP_POSITIONS; k++) {
            if (l->group_positions[i][k].used) {
                news_group_position(l, c, i, &l->group_positions[i][k], now);
            }
        }
    }
    for (size_t i = 0; i < LINK_CONTACTS; i++) {
        if (l->contacts[i].used && l->contact_shares[i].precision != 0) {
            news_sharing(l, c, i, now);
        }
    }
    for (size_t i = 0; i < LINK_GROUPS; i++) {
        if (l->groups[i].used && l->group_shares[i].precision != 0) {
            news_group_sharing(l, c, i, now);
        }
    }
    c->air = airtime_of(&l->view);
    tell(l, c, &c->air);
    c->power = power_of(&l->view);
    tell(l, c, &c->power);
    c->look_at = c->air_at = c->power_at = now;
    c->synced = true;
    /* With the count as it stands, so that a client that missed the last of the news knows. */
    struct tern_companion_msg a = {.type = TERN_C_SYNCED, .seq = q->seq, .news = c->news};
    send_msg(l, c, &a);
}

/* SEND and SEND_GROUP: a message to an address or to a group, once for each ref. */
static void send_request(struct link *l, const struct tern_companion_msg *q) {
    bool group = q->type == TERN_C_SEND_GROUP;
    uint8_t to[TERN_ADDRESS_LEN] = {0};
    l->host.view(l->host.ctx, &l->view);
    if (group) {
        if (find_group(l, q->group) == NULL) {
            error(l, q->seq, TERN_C_ERR_NOT_HELD);
            return;
        }
        memcpy(to, q->group, TERN_COMPANION_GROUP);
    } else {
        if (!usable_address(l, q->address)) {
            error(l, q->seq, TERN_C_ERR_ADDRESS);
            return;
        }
        memcpy(to, q->address, TERN_ADDRESS_LEN);
    }
    if (q->text_len == 0) {
        error(l, q->seq, TERN_C_ERR_REFUSED);
        return;
    }
    uint8_t d[8];
    digest(q->text, q->text_len, d);
    for (size_t i = 0; i < LINK_REFS; i++) {
        const struct link_ref *r = &l->refs[i];
        if (r->used && r->group == group && r->ref == q->ref &&
            memcmp(r->to, to, TERN_ADDRESS_LEN) == 0 && memcmp(r->digest, d, sizeof d) == 0) {
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
    struct link_message like = {.time = l->view.time, .state = TERN_C_WAITING};
    if (group) {
        like.kind = LINK_KIND_GROUP;
        memcpy(like.group, q->group, sizeof like.group);
    } else {
        like.reason = l->host.why(l->host.ctx, q->address);
        memcpy(like.address, q->address, TERN_ADDRESS_LEN);
    }
    uint32_t id = keep_message(l, &like, q->text, q->text_len);
    struct link_ref *r = &l->refs[l->next_ref];
    *r = (struct link_ref){.used = true, .group = group, .ref = q->ref, .id = id};
    memcpy(r->to, to, TERN_ADDRESS_LEN);
    memcpy(r->digest, d, sizeof d);
    l->next_ref = (l->next_ref + 1) % LINK_REFS;
}

/* Marks received messages read, up to an id: those of the kinds a client of `version` is sent. */
static void read_through(struct link *l, uint32_t through, uint8_t version) {
    for (size_t i = 0; i < LINK_MESSAGES; i++) {
        struct link_message *x = &l->messages[i];
        if (x->used && x->state == TERN_C_RECEIVED && x->id <= through &&
            !(x->flags & TERN_C_READ_FLAG) &&
            (x->kind == LINK_KIND_MESSAGE || version >= tern_companion_since(TERN_C_INVITE))) {
            x->flags |= TERN_C_READ_FLAG;
            save_state(l, x);
            news_message(l, NULL, x);
        }
    }
}

void link_read(struct link *l, uint32_t through) {
    read_through(l, through, TERN_COMPANION_VERSION);
}

/* A client has not shown its user what it was never sent: its READ leaves those unread. */
static void read_request(struct link *l, const struct tern_companion_msg *q) {
    answer(l, TERN_C_OK, q->seq);
    read_through(l, q->through, l->asker->version);
}

static void save_contact(struct link *l, const struct tern_companion_msg *q) {
    l->host.view(l->host.ctx, &l->view);
    if (!usable_address(l, q->address)) {
        error(l, q->seq, TERN_C_ERR_ADDRESS);
        return;
    }
    struct link_contact *c = find_contact(l, q->address);
    /* A free place, one that owes no stopped position to whoever was there before if there is
     * one: the stop is given up if its place is taken. */
    for (size_t i = 0; c == NULL && i < LINK_CONTACTS; i++) {
        if (!l->contacts[i].used && !l->contact_shares[i].stop) {
            c = &l->contacts[i];
        }
    }
    for (size_t i = 0; c == NULL && i < LINK_CONTACTS; i++) {
        if (!l->contacts[i].used) {
            c = &l->contacts[i];
        }
    }
    bool fresh = c != NULL && !c->used;
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
    if (fresh) {
        size_t place = (size_t)(c - l->contacts);
        l->contact_shares[place] = (struct link_share){0};
        tern_position_held_init(&l->contact_positions[place]);
    }
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
        /* Sharing with it ends, and the position held from it is forgotten. */
        size_t place = (size_t)(c - l->contacts);
        struct link_share *s = &l->contact_shares[place];
        struct tern_position_held *h = &l->contact_positions[place];
        bool shared = s->precision != 0, held = h->holds;
        *s = (struct link_share){.stop = s->gone || s->stop, .in_flight = s->in_flight};
        memcpy(s->address, q->address, TERN_ADDRESS_LEN);
        tern_position_held_init(h);
        struct tern_companion_msg r = {.type = TERN_C_POSITION};
        memcpy(r.address, q->address, TERN_ADDRESS_LEN);
        if (held) {
            news(l, NULL, &r);
        }
        if (shared) {
            r.type = TERN_C_SHARING;
            news(l, NULL, &r);
        }
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
        if (x->used && x->kind != LINK_KIND_GROUP &&
            (x->state == TERN_C_WAITING || x->state == TERN_C_SENT) &&
            memcmp(x->address, q->address, TERN_ADDRESS_LEN) == 0) {
            link_state(l, x->id, TERN_C_NOT_DELIVERED, 0, 0);
        }
    }
    link_session_changed(l, q->address);
}

static void made(struct link *l, uint8_t seq, const struct link_group *g) {
    struct tern_companion_msg a = {.type = TERN_C_MADE, .seq = seq};
    memcpy(a.group, g->id, sizeof a.group);
    send_msg(l, l->asker, &a);
}

/* Takes a group into a free place and saves it. 0, or the ERROR code to answer with. */
static uint8_t take_group(struct link *l, const uint8_t secret[TERN_GROUP_SECRET],
                          const uint8_t *name, size_t name_len, struct link_group **out) {
    struct link_group *g = free_group(l);
    if (g == NULL) {
        return TERN_C_ERR_FULL;
    }
    hold_group(g, secret, name, name_len);
    if (!save_groups(l)) {
        tern_wipe(g, sizeof *g);
        return TERN_C_ERR_NOT_NOW;
    }
    *out = g;
    return 0;
}

static void make_group(struct link *l, const struct tern_companion_msg *q) {
    uint8_t secret[TERN_GROUP_SECRET];
    struct link_group *g = NULL;
    uint8_t code = l->host.random(l->host.ctx, secret, sizeof secret)
                       ? take_group(l, secret, q->text, q->text_len, &g)
                       : TERN_C_ERR_NOT_NOW;
    tern_wipe(secret, sizeof secret);
    if (code != 0) {
        error(l, q->seq, code);
        return;
    }
    made(l, q->seq, g);
    news_group(l, NULL, g);
}

static void join(struct link *l, const struct tern_companion_msg *q) {
    struct link_message *x = find_message(l, q->id);
    if (x == NULL || x->kind != LINK_KIND_INVITE || x->state != TERN_C_RECEIVED) {
        error(l, q->seq, TERN_C_ERR_NOT_HELD);
        return;
    }
    struct link_group *g = find_group(l, x->group);
    if (g != NULL) {
        answer(l, TERN_C_OK, q->seq); /* held already: nothing changes */
        return;
    }
    uint8_t code = take_group(l, x->secret, x->text, x->text_len, &g);
    if (code != 0) {
        error(l, q->seq, code);
        return;
    }
    answer(l, TERN_C_OK, q->seq);
    news_group(l, NULL, g);
}

static void leave_group(struct link *l, const struct tern_companion_msg *q) {
    struct link_group *g = find_group(l, q->group);
    if (g != NULL) {
        struct link_group was = *g;
        g->used = false;
        if (!save_groups(l)) {
            *g = was;
            tern_wipe(&was, sizeof was);
            error(l, q->seq, TERN_C_ERR_NOT_NOW);
            return;
        }
        tern_wipe(&was, sizeof was);
        tern_wipe(g, sizeof *g);
        /* Its writers go with it, from flash too. */
        l->writers_changed = true;
        (void)link_keep_writers(l);
    }
    answer(l, TERN_C_OK, q->seq);
    if (g == NULL) {
        return;
    }
    struct tern_companion_msg m = {.type = TERN_C_GROUP_GONE};
    memcpy(m.group, q->group, sizeof m.group);
    news(l, NULL, &m);
    /* Sharing with it ends, and the positions held from it are forgotten. Its keys are gone, so
     * no stopped position can follow. */
    size_t place = (size_t)(g - l->groups);
    for (size_t k = 0; k < LINK_GROUP_POSITIONS; k++) {
        struct link_member_position *x = &l->group_positions[place][k];
        if (x->used) {
            struct tern_companion_msg r = {.type = TERN_C_GROUP_POSITION, .from = x->from};
            memcpy(r.group, q->group, sizeof r.group);
            news(l, NULL, &r);
        }
        *x = (struct link_member_position){0};
    }
    if (l->group_shares[place].precision != 0) {
        struct tern_companion_msg r = {.type = TERN_C_GROUP_SHARING};
        memcpy(r.group, q->group, sizeof r.group);
        news(l, NULL, &r);
    }
    l->group_shares[place] = (struct link_share){0};
    /* What was still to go to the group, or to invite someone to it, never will: with the board
     * already or not, since it asks whether each is still wanted (link_wanted()) and lets go of
     * the frame of one that is not. */
    for (size_t i = 0; i < LINK_MESSAGES; i++) {
        struct link_message *x = &l->messages[i];
        if (x->used && x->kind != LINK_KIND_MESSAGE && x->state == TERN_C_WAITING &&
            memcmp(x->group, q->group, sizeof x->group) == 0) {
            link_state(l, x->id, TERN_C_NOT_DELIVERED, 0, 0);
        }
    }
}

static void name_group(struct link *l, const struct tern_companion_msg *q) {
    struct link_group *g = find_group(l, q->group);
    if (g == NULL) {
        error(l, q->seq, TERN_C_ERR_NOT_HELD);
        return;
    }
    uint8_t was_len = g->name_len, was[TERN_COMPANION_NAME_MAX];
    memcpy(was, g->name, sizeof was);
    g->name_len = q->text_len;
    memcpy(g->name, q->text, q->text_len);
    if (!save_groups(l)) {
        g->name_len = was_len;
        memcpy(g->name, was, sizeof was);
        error(l, q->seq, TERN_C_ERR_NOT_NOW);
        return;
    }
    answer(l, TERN_C_OK, q->seq);
    news_group(l, NULL, g);
}

static void send_invite(struct link *l, const struct tern_companion_msg *q) {
    l->host.view(l->host.ctx, &l->view);
    const struct link_group *g = find_group(l, q->group);
    if (g == NULL) {
        error(l, q->seq, TERN_C_ERR_NOT_HELD);
        return;
    }
    if (!usable_address(l, q->address)) {
        error(l, q->seq, TERN_C_ERR_ADDRESS);
        return;
    }
    if (room(l) == NULL) {
        error(l, q->seq, TERN_C_ERR_FULL);
        return;
    }
    reserve_id(l);
    struct tern_companion_msg a = {.type = TERN_C_QUEUED, .seq = q->seq, .id = l->next_id};
    send_msg(l, l->asker, &a);
    struct link_message like = {.kind = LINK_KIND_INVITE,
                                .time = l->view.time,
                                .state = TERN_C_WAITING,
                                .reason = l->host.why(l->host.ctx, q->address)};
    memcpy(like.address, q->address, TERN_ADDRESS_LEN);
    memcpy(like.group, q->group, sizeof like.group);
    keep_message(l, &like, g->name, g->name_len);
}

/* A serial client that has asked nothing for the lapse is taken for gone. */
/* --- Updates -------------------------------------------------------------------------------- */

/* The same image as the update under way goes on from where the node's bytes end, so a client
 * whose link dropped need not send it all again; any other begins afresh. */
/* --- Positions ------------------------------------------------------------------------------ */

static void set_position(struct link *l, const struct tern_companion_msg *q, tern_time now) {
    if (q->lat < -TERN_POSITION_LAT_MAX || q->lat > TERN_POSITION_LAT_MAX ||
        q->lon < -TERN_POSITION_LON_MAX || q->lon > TERN_POSITION_LON_MAX) {
        error(l, q->seq, TERN_C_ERR_REFUSED);
        return;
    }
    l->fix = (struct link_fix){.have = true,
                               .lat = q->lat,
                               .lon = q->lon,
                               .altitude = q->altitude,
                               .accuracy = q->accuracy,
                               .at = now - TERN_S(q->age)};
    answer(l, TERN_C_OK, q->seq);
}

/* What SHARE and SHARE_GROUP set: 0, or the ERROR code to answer with. Turning off sharing that
 * is off changes nothing; turning off sharing that sent a position owes a stopped one. */
static uint8_t share(struct link_share *s, const struct tern_companion_msg *q, tern_time least,
                     tern_time now, bool *changed) {
    *changed = false;
    if (q->precision == 0) {
        if (s->precision != 0) {
            s->stop = s->stop || s->gone;
            s->precision = 0;
            s->sent = s->gone = false; /* one on its way still is: the stopped one waits for it */
            *changed = true;
        }
        return 0;
    }
    if (q->precision > TERN_POSITION_PRECISION_MAX ||
        (q->fields & ~(TERN_C_SHARE_ALTITUDE | TERN_C_SHARE_ACCURACY)) != 0 ||
        TERN_S(q->interval) < least) {
        return TERN_C_ERR_REFUSED;
    }
    bool on = s->precision != 0;
    s->precision = q->precision;
    s->fields = q->fields;
    s->interval = q->interval;
    s->until = q->minutes == 0 ? 0 : now + TERN_S((tern_time)q->minutes * 60);
    s->sent = false; /* changed: the next goes as the first does */
    s->go_at = 0;
    s->stop = false; /* a position on its way says more than a stopped one would */
    if (!on) {
        s->gone = false;
    }
    *changed = true;
    return 0;
}

static void share_request(struct link *l, const struct tern_companion_msg *q, tern_time now) {
    l->host.view(l->host.ctx, &l->view);
    if (!usable_address(l, q->address)) {
        error(l, q->seq, TERN_C_ERR_ADDRESS);
        return;
    }
    struct link_contact *c = find_contact(l, q->address);
    if (c == NULL) {
        error(l, q->seq, TERN_C_ERR_NOT_CONTACT);
        return;
    }
    size_t place = (size_t)(c - l->contacts);
    struct link_share *s = &l->contact_shares[place];
    memcpy(s->address, q->address, TERN_ADDRESS_LEN);
    bool changed;
    uint8_t code = share(s, q, TERN_POSITION_MIN, now, &changed);
    if (code != 0) {
        error(l, q->seq, code);
        return;
    }
    answer(l, TERN_C_OK, q->seq);
    if (changed) {
        news_sharing(l, NULL, place, now);
    }
}

static void share_group(struct link *l, const struct tern_companion_msg *q, tern_time now) {
    struct link_group *g = find_group(l, q->group);
    if (g == NULL) {
        error(l, q->seq, TERN_C_ERR_NOT_HELD);
        return;
    }
    size_t place = (size_t)(g - l->groups);
    bool changed;
    uint8_t code = share(&l->group_shares[place], q, TERN_POSITION_GROUP_MIN, now, &changed);
    if (code != 0) {
        error(l, q->seq, code);
        return;
    }
    answer(l, TERN_C_OK, q->seq);
    if (changed) {
        news_group_sharing(l, NULL, place, now);
    }
}

/* Whether a position is due to one destination: the cell the fix is in at its precision (the
 * last sent, while the fix is near it), and whether it is the last's. */
static bool due(struct link *l, struct link_share *s, tern_time now, struct tern_position *cell) {
    if (!l->fix.have) {
        return false;
    }
    bool near = s->sent && tern_position_near(&s->cell, l->fix.lat, l->fix.lon) &&
                s->cell.precision == s->precision;
    if (near) {
        *cell = s->cell;
    } else {
        tern_position_locate(cell, l->fix.lat, l->fix.lon, s->precision);
    }
    tern_time age = now - l->fix.at;
    if (!tern_position_due(TERN_S(s->interval), s->sent, s->last, !near, age, now)) {
        s->go_at = 0;
        return false;
    }
    /* A random wait, of up to an eighth of the interval, drawn once it is due. */
    if (s->go_at == 0) {
        uint64_t r = 0; /* 64 bits: an eighth of the interval is more nanoseconds than 32 hold */
        if (!l->host.random(l->host.ctx, (uint8_t *)&r, sizeof r)) {
            r = 0;
        }
        tern_time spread = TERN_S(s->interval) / 8;
        s->go_at = now + (spread > 0 ? (tern_time)(r % (uint64_t)spread) : 0);
        if (s->go_at == 0) {
            s->go_at = 1;
        }
    }
    return now >= s->go_at;
}

/* The plaintext of a position to one destination, from the fix and what the user chose. */
static void write_position(struct link *l, const struct link_share *s, tern_time now,
                           struct link_position_out *out) {
    struct tern_position p = out->cell;
    p.fields = 0;
    if ((s->fields & TERN_C_SHARE_ALTITUDE) && l->fix.altitude != TERN_C_NO_ALTITUDE) {
        p.fields |= TERN_POSITION_ALTITUDE;
        p.altitude = l->fix.altitude;
    }
    if ((s->fields & TERN_C_SHARE_ACCURACY) && l->fix.accuracy != 0) {
        p.fields |= TERN_POSITION_ACCURACY;
        p.accuracy = tern_position_accuracy_byte(l->fix.accuracy);
    }
    tern_time age = (now - l->fix.at) / TERN_S(1);
    if (age >= 60) {
        p.fields |= TERN_POSITION_AGE;
        p.age = age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
    }
    out->len = tern_position_write(&p, out->plaintext);
}

static void stopped(struct link_position_out *out) {
    out->cell = (struct tern_position){0};
    out->len = tern_position_write(&out->cell, out->plaintext);
}

bool link_position_next(struct link *l, tern_time now, bool groups, struct link_position_out *out) {
    for (size_t i = 0; i < LINK_CONTACTS; i++) {
        struct link_share *s = &l->contact_shares[i];
        if ((s->precision == 0 && !s->stop) || s->in_flight ||
            !l->host.session(l->host.ctx, s->address)) {
            continue; /* no first contact for a position */
        }
        *out = (struct link_position_out){.place = i};
        memcpy(out->address, s->address, TERN_ADDRESS_LEN);
        if (s->precision == 0) {
            stopped(out);
            return true;
        }
        if (due(l, s, now, &out->cell)) {
            write_position(l, s, now, out);
            return true;
        }
    }
    for (size_t i = 0; groups && i < LINK_GROUPS; i++) {
        struct link_share *s = &l->group_shares[i];
        if (!l->groups[i].used || (s->precision == 0 && !s->stop)) {
            continue;
        }
        *out = (struct link_position_out){.group = true, .place = i};
        if (s->precision == 0) {
            stopped(out);
            return true;
        }
        if (due(l, s, now, &out->cell)) {
            write_position(l, s, now, out);
            return true;
        }
    }
    return false;
}

void link_position_sent(struct link *l, const struct link_position_out *out, tern_time now) {
    struct link_share *s =
        out->group ? &l->group_shares[out->place] : &l->contact_shares[out->place];
    if (out->cell.precision == 0) {
        s->stop = false;
    } else {
        s->sent = s->gone = true;
        s->last = now;
        s->cell = out->cell;
        s->go_at = 0;
    }
    s->in_flight = !out->group;
}

bool link_group_position_wanted(const struct link *l, size_t place,
                                const uint8_t id[TERN_COMPANION_GROUP], bool stopped) {
    if (place >= LINK_GROUPS || !l->groups[place].used ||
        memcmp(l->groups[place].id, id, TERN_COMPANION_GROUP) != 0) {
        return false;
    }
    return stopped || l->group_shares[place].precision != 0;
}

void link_position_done(struct link *l, const uint8_t address[TERN_ADDRESS_LEN]) {
    for (size_t i = 0; i < LINK_CONTACTS; i++) {
        if (memcmp(l->contact_shares[i].address, address, TERN_ADDRESS_LEN) == 0) {
            l->contact_shares[i].in_flight = false;
        }
    }
}

void link_position_received(struct link *l, tern_time now, const uint8_t from[TERN_ADDRESS_LEN],
                            uint32_t counter, const uint8_t *plaintext, size_t len) {
    struct link_contact *c = find_contact(l, from);
    if (c == NULL) {
        return; /* whose position a user is shown is theirs to choose */
    }
    size_t place = (size_t)(c - l->contacts);
    if (tern_position_receive(&l->contact_positions[place], true, counter, plaintext, len, now)) {
        news_position(l, NULL, place, now);
    }
}

void link_group_position_received(struct link *l, tern_time now, size_t place, uint32_t from,
                                  const uint8_t *plaintext, size_t len) {
    struct tern_position p;
    if (place >= LINK_GROUPS || !l->groups[place].used || !tern_position_read(&p, plaintext, len)) {
        return;
    }
    struct link_member_position *at = NULL, *oldest = NULL;
    for (size_t k = 0; k < LINK_GROUP_POSITIONS; k++) {
        struct link_member_position *x = &l->group_positions[place][k];
        if (x->used && x->from == from) {
            at = x;
            break;
        }
        if (oldest == NULL || (oldest->used && (!x->used || x->at < oldest->at))) {
            oldest = x;
        }
    }
    if (p.precision == 0) {
        if (at != NULL) {
            at->used = false;
            news_group_position(l, NULL, place, at, now);
            *at = (struct link_member_position){0};
        }
        return;
    }
    if (at == NULL) {
        at = oldest; /* the one heard from longest ago makes room, and is forgotten as news */
        if (at->used) {
            at->used = false;
            news_group_position(l, NULL, place, at, now);
        }
    }
    *at = (struct link_member_position){.used = true, .from = from, .position = p, .at = now};
    news_group_position(l, NULL, place, at, now);
}

/* Sharing that has run out, and positions received a day ago, as time goes on. */
static void positions_tick(struct link *l, tern_time now) {
    for (size_t i = 0; i < LINK_CONTACTS; i++) {
        struct link_share *s = &l->contact_shares[i];
        if (l->contacts[i].used && s->precision != 0 && s->until != 0 && now >= s->until) {
            s->stop = s->stop || s->gone;
            s->precision = 0;
            s->sent = s->gone = false;
            news_sharing(l, NULL, i, now);
        }
        if (l->contacts[i].used && tern_position_expire(&l->contact_positions[i], now)) {
            news_position(l, NULL, i, now);
        }
    }
    for (size_t i = 0; i < LINK_GROUPS; i++) {
        struct link_share *s = &l->group_shares[i];
        if (l->groups[i].used && s->precision != 0 && s->until != 0 && now >= s->until) {
            s->stop = s->stop || s->gone;
            s->precision = 0;
            s->sent = s->gone = false;
            news_group_sharing(l, NULL, i, now);
        }
        for (size_t k = 0; l->groups[i].used && k < LINK_GROUP_POSITIONS; k++) {
            struct link_member_position *x = &l->group_positions[i][k];
            if (x->used && now - x->at >= TERN_POSITION_KEEP) {
                x->used = false;
                news_group_position(l, NULL, i, x, now);
                *x = (struct link_member_position){0};
            }
        }
    }
}

static void update_begin(struct link *l, const struct tern_companion_msg *q) {
    struct link_update *u = &l->update;
    if (q->size == 0) {
        error(l, q->seq, TERN_C_ERR_REFUSED);
        return;
    }
    bool board = l->host.board != NULL && l->host.board[0] != '\0';
    if (!board || q->size > l->host.update_room) {
        error(l, q->seq, TERN_C_ERR_FULL);
        return;
    }
    if (!(u->on && u->size == q->size && memcmp(u->digest, q->digest, sizeof u->digest) == 0)) {
        u->on = false;
        if (!l->host.update_begin(l->host.ctx, q->size)) {
            error(l, q->seq, TERN_C_ERR_NOT_NOW);
            return;
        }
        *u = (struct link_update){.on = true, .size = q->size};
        memcpy(u->digest, q->digest, sizeof u->digest);
        tern_sha256_init(&u->sha);
    }
    struct tern_companion_msg a = {.type = TERN_C_UPDATING, .seq = q->seq, .offset = u->held};
    send_msg(l, l->asker, &a);
}

static void update_data(struct link *l, const struct tern_companion_msg *q) {
    struct link_update *u = &l->update;
    uint32_t end = q->offset + q->data_len;
    if (q->data_len == 0 || (u->on && (end < q->offset || end > u->size))) {
        error(l, q->seq, TERN_C_ERR_REFUSED);
        return;
    }
    if (u->on && q->offset == u->held) {
        if (!l->host.update_write(l->host.ctx, q->offset, q->data, q->data_len)) {
            u->on = false;
            error(l, q->seq, TERN_C_ERR_NOT_THERE);
            return;
        }
        tern_sha256_update(&u->sha, q->data, q->data_len);
        u->held = end;
        answer(l, TERN_C_OK, q->seq);
    } else if (u->on && end == u->held) {
        answer(l, TERN_C_OK, q->seq); /* the last again, its answer lost: held once already */
    } else {
        error(l, q->seq, TERN_C_ERR_NOT_THERE);
    }
}

static void update_end(struct link *l, const struct tern_companion_msg *q) {
    struct link_update *u = &l->update;
    if (!u->on || u->held < u->size) {
        error(l, q->seq, TERN_C_ERR_NOT_THERE);
        return;
    }
    uint8_t got[TERN_SHA256_LEN];
    struct tern_sha256 sha = u->sha;
    tern_sha256_final(&sha, got);
    u->on = false;
    uint8_t code = memcmp(got, u->digest, sizeof got) == 0 ? l->host.update_run(l->host.ctx)
                                                           : TERN_C_ERR_NOT_AN_IMAGE;
    if (code != 0) {
        error(l, q->seq, code);
    } else {
        answer(l, TERN_C_OK, q->seq);
    }
}

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
    /* A request the client's version does not have is one this node does not know from it: it
     * could not be told what the request changed. */
    if (c->hello && q.type != TERN_C_HELLO && c->version < tern_companion_since(q.type)) {
        error(l, q.seq, TERN_C_ERR_UNKNOWN);
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
        a.board_len = (uint8_t)cstr_len(l->host.board, TERN_COMPANION_BOARD_MAX);
        memcpy(a.board, l->host.board == NULL ? "" : l->host.board, a.board_len);
        a.release_len = (uint8_t)cstr_len(l->host.release, TERN_COMPANION_RELEASE_MAX);
        memcpy(a.release, l->host.release == NULL ? "" : l->host.release, a.release_len);
        c->version = q.version; /* INFO too is as the client's version has it */
        send_msg(l, c, &a);
        c->hello = true;
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
    case TERN_C_SEND_GROUP:
        send_request(l, &q);
        break;
    case TERN_C_MAKE_GROUP:
        make_group(l, &q);
        break;
    case TERN_C_LEAVE_GROUP:
        leave_group(l, &q);
        break;
    case TERN_C_NAME_GROUP:
        name_group(l, &q);
        break;
    case TERN_C_SEND_INVITE:
        send_invite(l, &q);
        break;
    case TERN_C_JOIN:
        join(l, &q);
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
    case TERN_C_UPDATE_BEGIN:
        update_begin(l, &q);
        break;
    case TERN_C_UPDATE_DATA:
        update_data(l, &q);
        break;
    case TERN_C_UPDATE_END:
        update_end(l, &q);
        break;
    case TERN_C_SET_POSITION:
        set_position(l, &q, now);
        break;
    case TERN_C_SHARE:
        share_request(l, &q, now);
        break;
    case TERN_C_SHARE_GROUP:
        share_group(l, &q, now);
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
    positions_tick(l, now);
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
