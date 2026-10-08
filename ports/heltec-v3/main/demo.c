#include "demo.h"

#include "tern/crypto.h"
#include "tern/err.h"

#define MAGIC 0x5445524Eu /* "TERN" */
#define STATE_KEY "session"
#define IDENTITY_KEY "identity"

struct identity_record {
    uint32_t magic;
    uint8_t seed[TERN_SEED_LEN];
};

static void copy(uint8_t *to, const uint8_t *from, size_t len) {
    for (size_t i = 0; i < len; i++) {
        to[i] = from[i];
    }
}

static bool same(const uint8_t *a, const uint8_t *b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

/* A slot's record: the first has the name the one session of earlier builds had, so a board that
 * is updated keeps its peer. */
static void state_key(int slot, char key[sizeof STATE_KEY + 1]) {
    copy((uint8_t *)key, (const uint8_t *)STATE_KEY, sizeof STATE_KEY);
    if (slot != 0) {
        key[sizeof STATE_KEY - 1] = (char)('0' + slot);
        key[sizeof STATE_KEY] = '\0';
    }
}

static bool save_state(struct demo *d, int slot) {
    char key[sizeof STATE_KEY + 1];
    state_key(slot, key);
    return d->store.save(d->store.ctx, key, &d->s[slot], sizeof d->s[slot]);
}

int demo_peer(const struct demo *d, const uint8_t address[TERN_ADDRESS_LEN]) {
    for (int i = 0; i < DEMO_PEERS; i++) {
        if (d->s[i].role != 0 && same(d->s[i].peer, address, TERN_ADDRESS_LEN)) {
            return i;
        }
    }
    return -1;
}

size_t demo_peers(const struct demo *d) {
    size_t n = 0;
    for (int i = 0; i < DEMO_PEERS; i++) {
        n += d->s[i].role != 0;
    }
    return n;
}

static int free_slot(const struct demo *d) {
    for (int i = 0; i < DEMO_PEERS; i++) {
        if (d->s[i].role == 0) {
            return i;
        }
    }
    return -1;
}

static bool held(const struct demo *d, int slot) {
    return slot >= 0 && slot < DEMO_PEERS && d->s[slot].role != 0;
}

/* Some session, for a board that has lost the one it last used. */
static int any_slot(const struct demo *d) {
    for (int i = 0; i < DEMO_PEERS; i++) {
        if (d->s[i].role != 0) {
            return i;
        }
    }
    return -1;
}

bool demo_forget(struct demo *d, int slot) {
    if (!held(d, slot)) {
        return false;
    }
    static struct demo_state old;
    old = d->s[slot];
    tern_wipe(&d->s[slot], sizeof d->s[slot]);
    /* The record is written over with nothing, which is not loaded as a session. */
    if (!save_state(d, slot)) {
        d->s[slot] = old;
        tern_wipe(&old, sizeof old);
        return false;
    }
    tern_wipe(&old, sizeof old);
    if (d->last == slot) {
        d->last = any_slot(d);
    }
    return true;
}

static void forget_handshake(struct demo *d) { tern_wipe(&d->h, sizeof d->h); }

bool demo_start(struct demo *d, const struct demo_store *store, tern_time retry) {
    struct identity_record rec;
    d->store = *store;
    d->retry = retry;
    d->accept_until = 0;
    d->trusted = NULL;
    d->trusted_ctx = NULL;
    forget_handshake(d);

    bool ok = store->load(store->ctx, IDENTITY_KEY, &rec, sizeof rec) && rec.magic == MAGIC;
    if (!ok) {
        /* Saved before it is used: an address this board gave out is one it still has. */
        rec.magic = MAGIC;
        ok = store->random(store->ctx, rec.seed, sizeof rec.seed) &&
             store->save(store->ctx, IDENTITY_KEY, &rec, sizeof rec);
    }
    if (ok) {
        tern_identity_init(&d->id, rec.seed);
    } else {
        tern_identity_wipe(&d->id);
    }
    tern_wipe(&rec, sizeof rec);

    for (int i = 0; i < DEMO_PEERS; i++) {
        char key[sizeof STATE_KEY + 1];
        state_key(i, key);
        struct demo_state *s = &d->s[i];
        if (!store->load(store->ctx, key, s, sizeof *s) || s->magic != MAGIC ||
            s->size != sizeof *s || s->role == 0) {
            tern_wipe(s, sizeof *s);
        }
    }
    d->last = any_slot(d);
    return ok;
}

/* A connection identifier: one of the 48 values EDHOC sends as one byte. */
static uint8_t connection_id(uint8_t r) {
    r %= 48;
    return r < 24 ? r : (uint8_t)(0x20 + (r - 24));
}

/* A handshake's ephemeral key and connection identifier, drawn together. */
static bool draw(struct demo *d, uint8_t ephemeral[TERN_CONTACT_EPHEMERAL], uint8_t *id) {
    uint8_t r[TERN_CONTACT_EPHEMERAL + 1];
    bool ok = d->store.random(d->store.ctx, r, sizeof r);
    if (ok) {
        copy(ephemeral, r, TERN_CONTACT_EPHEMERAL);
        *id = connection_id(r[TERN_CONTACT_EPHEMERAL]);
    }
    tern_wipe(r, sizeof r);
    return ok;
}

enum demo_result demo_contact(struct demo *d, const uint8_t peer[TERN_ADDRESS_LEN], tern_time now,
                              uint8_t *frame, size_t *len) {
    uint8_t ephemeral[TERN_CONTACT_EPHEMERAL], c_i;
    struct demo_handshake *h = &d->h;
    if (same(peer, d->id.address, TERN_ADDRESS_LEN)) {
        return DEMO_OWN_ADDRESS;
    }
    if (!tern_address_valid(peer)) {
        return DEMO_BAD_ADDRESS;
    }
    if (demo_peer(d, peer) < 0 && free_slot(d) < 0) {
        return DEMO_FULL;
    }
    if (!draw(d, ephemeral, &c_i)) {
        return DEMO_NO_RANDOM;
    }
    forget_handshake(d);
    int err = tern_contact_start(&h->c, peer, ephemeral, c_i, h->out, sizeof h->out, &h->out_len);
    tern_wipe(ephemeral, sizeof ephemeral);
    if (err != TERN_OK) {
        forget_handshake(d);
        return DEMO_BAD_ADDRESS;
    }
    h->phase = DEMO_INITIATING;
    h->tries = 1;
    h->next = now + d->retry;
    copy(frame, h->out, h->out_len);
    *len = h->out_len;
    return DEMO_OK;
}

void demo_accept(struct demo *d, tern_time until) { d->accept_until = until; }

void demo_trust(struct demo *d, bool (*trusted)(void *ctx, const uint8_t address[TERN_ADDRESS_LEN]),
                void *ctx) {
    d->trusted = trusted;
    d->trusted_ctx = ctx;
}

enum demo_result demo_seal(struct demo *d, int slot, const uint8_t *msg, size_t len,
                           uint8_t *frame) {
    if (!held(d, slot)) {
        return DEMO_UNPAIRED;
    }
    if (len > TERN_UNICAST_MAX_PLAINTEXT) {
        return DEMO_TOO_LONG;
    }
    struct demo_state *s = &d->s[slot];
    int err = tern_unicast_seal(&s->session.tx, msg, len, frame, len + TERN_UNICAST_OVERHEAD);
    if (err == TERN_ESPENT) {
        return DEMO_SPENT;
    }
    s->sent++;
    d->last = slot;
    /* Saved before it is sent: if the board stops between the two, the counter is still used. */
    return save_state(d, slot) ? DEMO_OK : DEMO_STORE_FAILED;
}

bool demo_acked(const struct demo *d, int slot, uint32_t counter, const uint8_t *frame,
                size_t len) {
    return held(d, slot) && tern_unicast_acked(&d->s[slot].session.tx, counter, frame, len);
}

static enum demo_heard unicast_frame(struct demo *d, const uint8_t *frame, size_t len, uint8_t *msg,
                                     struct demo_received *out) {
    /* Every session's receiver, and the slot each is in. */
    struct tern_unicast_rx *rx[DEMO_PEERS];
    int slot[DEMO_PEERS];
    size_t n = 0;
    /* The windows as they were, to go back to if what the frame does to one cannot be saved. */
    static struct tern_unicast_rx before[DEMO_PEERS];
    for (int i = 0; i < DEMO_PEERS; i++) {
        if (d->s[i].role != 0) {
            rx[n] = &d->s[i].session.rx;
            before[n] = d->s[i].session.rx;
            slot[n++] = i;
        }
    }
    if (n == 0) {
        return len >= TERN_UNICAST_OVERHEAD ? DEMO_HEARD_OTHER : DEMO_HEARD_MALFORMED;
    }
    struct tern_unicast_received r;
    enum demo_heard what = DEMO_HEARD_MALFORMED;
    if (tern_unicast_open(rx, n, frame, len, msg, TERN_UNICAST_MAX_PLAINTEXT, &r) == TERN_OK) {
        switch (r.verdict) {
        case TERN_UNICAST_ACCEPTED: {
            struct demo_state *s = &d->s[slot[r.session]];
            s->heard++;
            /* Shown only once saved: otherwise a restart would reload the old window, and the
             * same frame would be accepted, and shown, a second time. Unsaved, it is as if it
             * had not come: the window goes back, so that the frame is taken when its sender
             * tries again, and is not acknowledged as a copy of a message nobody was shown. */
            if (!save_state(d, slot[r.session])) {
                tern_wipe(msg, r.len);
                s->session.rx = before[r.session];
                s->heard--;
                what = DEMO_HEARD_UNSAVED;
                break;
            }
            copy(out->peer, s->peer, TERN_ADDRESS_LEN);
            out->slot = slot[r.session];
            out->counter = r.counter;
            out->msg_len = r.len;
            out->ack_slot[0] = out->slot;
            out->acks = tern_unicast_ack(rx[r.session], r.counter, out->ack[0]);
            d->last = out->slot;
            what = DEMO_HEARD_MESSAGE;
            break;
        }
        case TERN_UNICAST_COPY:
            /* Its acknowledgement did not get back: owed again, as often as the copy comes, and
             * for every message it is a copy of, whichever session each is in. */
            copy(out->peer, d->s[slot[r.session]].peer, TERN_ADDRESS_LEN);
            out->slot = slot[r.session];
            out->counter = r.counter;
            do {
                out->ack_slot[out->acks] = slot[r.session];
                out->acks += tern_unicast_ack(rx[r.session], r.counter, out->ack[out->acks]);
            } while (out->acks < DEMO_ACKS && tern_unicast_copy_next(rx, n, frame, len, &r));
            what = DEMO_HEARD_COPY;
            break;
        case TERN_UNICAST_NOT_OURS:
            what = DEMO_HEARD_OTHER;
            break;
        case TERN_UNICAST_FORGED:
            what = DEMO_HEARD_FORGED;
            break;
        default:
            break;
        }
    }
    tern_wipe(before, sizeof before);
    return what;
}

/* Remembers the frame just processed and the answer to it. */
static void remember(struct demo_handshake *h, const uint8_t *frame, size_t len,
                     const struct demo_received *out) {
    copy(h->in, frame, len);
    h->in_len = len;
    copy(h->out, out->reply, out->reply_len);
    h->out_len = out->reply_len;
}

/* Whether this board takes a session with the node that has just proved it is `peer`. */
static bool accepts(const struct demo *d, const uint8_t peer[TERN_ADDRESS_LEN], tern_time now) {
    return demo_peers(d) == 0 || now < d->accept_until || demo_peer(d, peer) >= 0 ||
           (d->trusted != NULL && d->trusted(d->trusted_ctx, peer));
}

/* Turns a complete handshake into a session: in place of the one the board had with that peer,
 * or in a slot of its own. */
static enum demo_heard start_session(struct demo *d, tern_time now, const uint8_t *frame,
                                     size_t len, struct demo_received *out) {
    struct demo_handshake *h = &d->h;
    bool responder = h->c.role == TERN_RESPONDER;
    int slot = demo_peer(d, h->c.peer);
    if (slot < 0) {
        slot = free_slot(d);
    }

    if ((responder && !accepts(d, h->c.peer, now)) || slot < 0) {
        bool refused = responder && !accepts(d, h->c.peer, now);
        copy(out->peer, h->c.peer, TERN_ADDRESS_LEN);
        out->reply_len = 0;
        forget_handshake(d);
        return refused ? DEMO_HEARD_REFUSED : DEMO_HEARD_FULL;
    }

    static struct demo_state old;
    struct demo_state *s = &d->s[slot];
    old = *s;
    tern_wipe(s, sizeof *s);
    s->magic = MAGIC;
    s->size = sizeof *s;
    s->role = h->c.role;
    (void)tern_contact_finish(&h->c, &s->session, s->peer); /* erases the handshake */
    copy(out->peer, s->peer, TERN_ADDRESS_LEN);

    if (!save_state(d, slot)) {
        /* message_4 is not sent, so the other board does not take up a session this one would
         * lose at its next restart. */
        *s = old;
        tern_wipe(&old, sizeof old);
        out->reply_len = 0;
        forget_handshake(d);
        return DEMO_HEARD_UNPAIRED;
    }
    tern_wipe(&old, sizeof old);
    d->accept_until = 0;
    d->last = slot;
    out->slot = slot;

    if (responder) {
        /* Kept for a while, in case message_4 is lost and message_3 comes again. */
        remember(h, frame, len, out);
        h->phase = DEMO_ANSWERED;
        h->until = now + DEMO_HOLD(d->retry);
    } else {
        forget_handshake(d);
    }
    return DEMO_HEARD_PAIRED;
}

static enum demo_heard contact_frame(struct demo *d, tern_time now, const uint8_t *frame,
                                     size_t len, struct demo_received *out) {
    struct demo_handshake *h = &d->h;

    /* A frame already answered gets the same answer, which is not computed again. */
    if (h->in_len != 0 && len == h->in_len && same(frame, h->in, len)) {
        copy(out->reply, h->out, h->out_len);
        out->reply_len = h->out_len;
        return DEMO_HEARD_CONTACT;
    }

    if (frame[0] == 0x51) {
        uint8_t ephemeral[TERN_CONTACT_EPHEMERAL], c_r;
        if (h->phase == DEMO_INITIATING || h->phase == DEMO_RESPONDING) {
            return DEMO_HEARD_OTHER; /* one handshake at a time */
        }
        if (!draw(d, ephemeral, &c_r)) {
            return DEMO_HEARD_OTHER;
        }
        /* Not in h until it is known to be for this board: someone else's message_1 must not
         * cost it the answer it is keeping. */
        struct tern_contact c;
        int v = tern_contact_respond(&c, &d->id, ephemeral, c_r, frame, len, out->reply,
                                     sizeof out->reply, &out->reply_len);
        tern_wipe(ephemeral, sizeof ephemeral);
        if (v != TERN_CONTACT_PROCESSED) {
            out->reply_len = 0;
            tern_contact_wipe(&c);
            return DEMO_HEARD_OTHER;
        }
        forget_handshake(d);
        h->c = c;
        tern_contact_wipe(&c);
        remember(h, frame, len, out);
        h->phase = DEMO_RESPONDING;
        h->until = now + DEMO_HOLD(d->retry);
        return DEMO_HEARD_CONTACT;
    }

    if (h->phase != DEMO_INITIATING && h->phase != DEMO_RESPONDING) {
        return DEMO_HEARD_OTHER;
    }
    int v = tern_contact_receive(&h->c, &d->id, frame, len, out->reply, sizeof out->reply,
                                 &out->reply_len);
    if (v == TERN_CONTACT_NOT_OURS) {
        return DEMO_HEARD_OTHER;
    }
    if (v != TERN_CONTACT_PROCESSED) {
        out->reply_len = 0;
        forget_handshake(d);
        return DEMO_HEARD_FAILED;
    }
    if (tern_contact_complete(&h->c)) {
        return start_session(d, now, frame, len, out);
    }
    /* The initiator, with message_3 to send, and to send again until message_4 comes. */
    remember(h, frame, len, out);
    h->tries = 1;
    h->next = now + d->retry;
    return DEMO_HEARD_CONTACT;
}

enum demo_heard demo_receive(struct demo *d, tern_time now, const uint8_t *frame, size_t len,
                             uint8_t *msg, struct demo_received *out) {
    out->msg_len = 0;
    out->counter = 0;
    out->reply_len = 0;
    out->acks = 0;
    out->slot = -1;
    if (len == 0) {
        return DEMO_HEARD_MALFORMED;
    }
    if (frame[0] == TERN_UNICAST_HDR) {
        return unicast_frame(d, frame, len, msg, out);
    }
    if (frame[0] >= 0x51 && frame[0] <= 0x54) {
        return contact_frame(d, now, frame, len, out);
    }
    return DEMO_HEARD_MALFORMED;
}

enum demo_tick demo_tick(struct demo *d, tern_time now, uint8_t *frame, size_t *len) {
    struct demo_handshake *h = &d->h;
    *len = 0;
    if (h->phase == DEMO_INITIATING && now >= h->next) {
        if (h->tries >= DEMO_TRIES) {
            forget_handshake(d);
            return DEMO_TICK_GAVE_UP;
        }
        h->tries++;
        h->next = now + d->retry;
        copy(frame, h->out, h->out_len);
        *len = h->out_len;
        return DEMO_TICK_RESEND;
    }
    if ((h->phase == DEMO_RESPONDING || h->phase == DEMO_ANSWERED) && now >= h->until) {
        bool unfinished = h->phase == DEMO_RESPONDING;
        forget_handshake(d);
        return unfinished ? DEMO_TICK_LAPSED : DEMO_TICK_NONE;
    }
    return DEMO_TICK_NONE;
}
