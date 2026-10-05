#include "demo.h"

#include "tern/crypto.h"
#include "tern/err.h"

#define MAGIC 0x5445524Eu /* "TERN" */
#define STATE_KEY "session"
#define USED_KEY "used"

static void secret_from(const char *passphrase, uint8_t secret[TERN_UNICAST_SECRET]) {
    static const char label[] = "tern demo pairing v0";
    struct tern_sha256 h;
    size_t len = 0;
    while (passphrase[len] != '\0') {
        len++;
    }
    tern_sha256_init(&h);
    tern_sha256_update(&h, (const uint8_t *)label, sizeof label - 1);
    tern_sha256_update(&h, (const uint8_t *)passphrase, len);
    tern_sha256_final(&h, secret);
}

static void fingerprint_of(const uint8_t secret[TERN_UNICAST_SECRET], uint8_t fp[8]) {
    static const char label[] = "tern demo used";
    struct tern_sha256 h;
    uint8_t digest[TERN_SHA256_LEN];
    tern_sha256_init(&h);
    tern_sha256_update(&h, (const uint8_t *)label, sizeof label - 1);
    tern_sha256_update(&h, secret, TERN_UNICAST_SECRET);
    tern_sha256_final(&h, digest);
    for (int i = 0; i < 8; i++) {
        fp[i] = digest[i];
    }
}

static bool save_state(struct demo *d) {
    return d->store.save(d->store.ctx, STATE_KEY, &d->s, sizeof d->s);
}

void demo_start(struct demo *d, const struct demo_store *store) {
    d->store = *store;
    if (!store->load(store->ctx, STATE_KEY, &d->s, sizeof d->s) || d->s.magic != MAGIC ||
        d->s.size != sizeof d->s) {
        tern_wipe(&d->s, sizeof d->s);
    }
    if (!store->load(store->ctx, USED_KEY, &d->used, sizeof d->used) ||
        d->used.count > DEMO_MAX_PASSPHRASES) {
        tern_wipe(&d->used, sizeof d->used);
    }
}

enum demo_result demo_pair(struct demo *d, enum tern_role role, const char *passphrase) {
    uint8_t secret[TERN_UNICAST_SECRET], copy[TERN_UNICAST_SECRET], fp[8];
    secret_from(passphrase, secret);
    fingerprint_of(secret, fp);

    for (uint32_t i = 0; i < d->used.count; i++) {
        if (tern_equal_ct(d->used.fingerprint[i], fp, sizeof fp)) {
            tern_wipe(secret, sizeof secret);
            return DEMO_REUSED;
        }
    }
    if (d->used.count == DEMO_MAX_PASSPHRASES) {
        tern_wipe(secret, sizeof secret);
        return DEMO_FULL;
    }

    /* The passphrase is marked used before the session exists, so no failure after this point
     * can leave a session that could be started again from counter 0. */
    for (int i = 0; i < 8; i++) {
        d->used.fingerprint[d->used.count][i] = fp[i];
    }
    d->used.count++;
    if (!d->store.save(d->store.ctx, USED_KEY, &d->used, sizeof d->used)) {
        d->used.count--;
        tern_wipe(secret, sizeof secret);
        return DEMO_STORE_FAILED;
    }

    tern_wipe(&d->s, sizeof d->s);
    d->s.magic = MAGIC;
    d->s.size = sizeof d->s;
    d->s.role = (uint8_t)role;

    /* The mirror is the receiver the other role would have: it hears this board's direction. */
    struct tern_session other;
    for (size_t i = 0; i < sizeof secret; i++) {
        copy[i] = secret[i];
    }
    tern_session_init(&other, copy, role == TERN_INITIATOR ? TERN_RESPONDER : TERN_INITIATOR);
    d->s.mirror = other.rx;
    tern_session_wipe(&other);
    tern_session_init(&d->s.session, secret, role); /* erases secret */

    if (!save_state(d)) {
        tern_wipe(&d->s, sizeof d->s);
        return DEMO_STORE_FAILED;
    }
    return DEMO_OK;
}

enum demo_result demo_seal(struct demo *d, const uint8_t *msg, size_t len, uint8_t *frame) {
    if (d->s.role == 0) {
        return DEMO_UNPAIRED;
    }
    if (d->s.conflict) {
        return DEMO_CONFLICT;
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

enum demo_heard demo_open(struct demo *d, const uint8_t *frame, size_t len, uint8_t *msg,
                          size_t *msg_len, uint32_t *counter) {
    if (d->s.role == 0) {
        return len >= TERN_UNICAST_OVERHEAD && frame[0] == TERN_UNICAST_HDR ? DEMO_HEARD_OTHER
                                                                            : DEMO_HEARD_MALFORMED;
    }
    struct tern_unicast_rx *rx[] = {&d->s.session.rx, &d->s.mirror};
    struct tern_unicast_received r;
    if (tern_unicast_open(rx, 2, frame, len, msg, TERN_UNICAST_MAX_PLAINTEXT, &r) != TERN_OK) {
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

    if (r.session == 1) {
        d->s.conflict = true;
        (void)save_state(d);
        tern_wipe(msg, r.len);
        return DEMO_HEARD_CLASH;
    }
    d->s.heard++;
    (void)save_state(d); /* so a restart does not accept this frame again */
    *msg_len = r.len;
    *counter = r.counter;
    return DEMO_HEARD_MESSAGE;
}
