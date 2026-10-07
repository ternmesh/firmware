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

static bool save_state(struct demo *d) {
    return d->store.save(d->store.ctx, STATE_KEY, &d->s, sizeof d->s);
}

static void forget_handshake(struct demo *d) { tern_wipe(&d->h, sizeof d->h); }

bool demo_start(struct demo *d, const struct demo_store *store, tern_time retry) {
    struct identity_record rec;
    d->store = *store;
    d->retry = retry;
    d->accept_until = 0;
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

    if (!store->load(store->ctx, STATE_KEY, &d->s, sizeof d->s) || d->s.magic != MAGIC ||
        d->s.size != sizeof d->s) {
        tern_wipe(&d->s, sizeof d->s);
    }
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

enum demo_result demo_seal(struct demo *d, const uint8_t *msg, size_t len, uint8_t *frame) {
    if (d->s.role == 0) {
        return DEMO_UNPAIRED;
    }
    if (len > TERN_UNICAST_MAX_PLAINTEXT) {
        return DEMO_TOO_LONG;
    }
    int err =
        tern_unicast_seal(&d->s.session.tx, 0, 0, msg, len, frame, len + TERN_UNICAST_OVERHEAD);
    if (err == TERN_ESPENT) {
        return DEMO_SPENT;
    }
    d->s.sent++;
    /* Saved before it is sent: if the board stops between the two, the counter is still used. */
    return save_state(d) ? DEMO_OK : DEMO_STORE_FAILED;
}

static enum demo_heard unicast_frame(struct demo *d, const uint8_t *frame, size_t len, uint8_t *msg,
                                     struct demo_received *out) {
    if (d->s.role == 0) {
        return len >= TERN_UNICAST_OVERHEAD ? DEMO_HEARD_OTHER : DEMO_HEARD_MALFORMED;
    }
    struct tern_unicast_rx *rx[] = {&d->s.session.rx};
    struct tern_unicast_received r;
    if (tern_unicast_open(rx, 1, frame, len, msg, TERN_UNICAST_MAX_PLAINTEXT, &r) != TERN_OK) {
        return DEMO_HEARD_MALFORMED;
    }
    switch (r.verdict) {
    case TERN_UNICAST_ACCEPTED:
        break;
    case TERN_UNICAST_NOT_OURS:
        return DEMO_HEARD_OTHER;
    case TERN_UNICAST_FORGED:
        return DEMO_HEARD_FORGED;
    default:
        return DEMO_HEARD_MALFORMED;
    }

    d->s.heard++;
    /* Shown only once saved: otherwise a restart would reload the old window, and the same frame
     * would be accepted, and shown, a second time. */
    if (!save_state(d)) {
        tern_wipe(msg, r.len);
        return DEMO_HEARD_UNSAVED;
    }
    out->msg_len = r.len;
    out->counter = r.counter;
    return DEMO_HEARD_MESSAGE;
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
    return d->s.role == 0 || now < d->accept_until || same(peer, d->s.peer, TERN_ADDRESS_LEN);
}

/* Turns a complete handshake into the board's session, replacing the one it had. */
static enum demo_heard start_session(struct demo *d, tern_time now, const uint8_t *frame,
                                     size_t len, struct demo_received *out) {
    struct demo_handshake *h = &d->h;
    bool responder = h->c.role == TERN_RESPONDER;
    struct demo_state old = d->s;

    if (responder && !accepts(d, h->c.peer, now)) {
        copy(out->peer, h->c.peer, TERN_ADDRESS_LEN);
        out->reply_len = 0;
        forget_handshake(d);
        tern_wipe(&old, sizeof old);
        return DEMO_HEARD_REFUSED;
    }

    tern_wipe(&d->s, sizeof d->s);
    d->s.magic = MAGIC;
    d->s.size = sizeof d->s;
    d->s.role = h->c.role;
    (void)tern_contact_finish(&h->c, &d->s.session, d->s.peer); /* erases the handshake */
    copy(out->peer, d->s.peer, TERN_ADDRESS_LEN);

    if (!save_state(d)) {
        /* message_4 is not sent, so the other board does not take up a session this one would
         * lose at its next restart. */
        d->s = old;
        tern_wipe(&old, sizeof old);
        out->reply_len = 0;
        forget_handshake(d);
        return DEMO_HEARD_UNPAIRED;
    }
    tern_wipe(&old, sizeof old);
    d->accept_until = 0;

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
