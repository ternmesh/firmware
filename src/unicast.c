#include "tern/unicast.h"

#include "tern/err.h"
#include "tern/sleep.h"

/* Secured unicast frames, as draft/unicast-security.md in ternmesh/spec defines them. Section
 * names in comments are that document's. Acknowledgements, and the copies they answer, are
 * draft/forwarding.md's. */

#define AT_DTAG TERN_UNICAST_HEAD
#define AT_BODY                                                                                    \
    (AT_DTAG + TERN_UNICAST_DTAG) /* a message's ciphertext, or an acknowledgement's proof */

#define EPOCH_SHIFT 5 /* 32 messages to an epoch */
#define LAST_EPOCH (UINT32_MAX >> EPOCH_SHIFT)
#define BEHIND 31 /* how far below H the window reaches */
#define AHEAD 32  /* and how far above */
#define FIRST_WINDOW 31
#define CHECK 4 /* the bytes of a frame's AEAD tag that a copy must share with its message */

#define LABEL(s) (const uint8_t *)(s), (sizeof(s) - 1)

static void put_u32be(uint8_t out[4], uint32_t n) {
    out[0] = (uint8_t)(n >> 24);
    out[1] = (uint8_t)(n >> 16);
    out[2] = (uint8_t)(n >> 8);
    out[3] = (uint8_t)n;
}

static void copy(uint8_t *dst, const uint8_t *src, size_t len) {
    for (size_t i = 0; i < len; i++) {
        dst[i] = src[i];
    }
}

static bool same(const uint8_t *a, const uint8_t *b, size_t len) {
    /* What is compared here travels in the clear, so it need not take constant time. */
    for (size_t i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

/* Expand(key, label || suffix, out_len), for the specification's labels: no label and suffix
 * together are longer than 17 bytes, nor any output longer than 32. */
static void expand(uint8_t *out, size_t out_len, const uint8_t *key, size_t key_len,
                   const uint8_t *label, size_t label_len, const uint8_t *suffix,
                   size_t suffix_len) {
    uint8_t info[32];
    copy(info, label, label_len);
    copy(info + label_len, suffix, suffix_len);
    (void)tern_hkdf_expand(out, out_len, key, key_len, info, label_len + suffix_len);
}

static uint32_t seconds(tern_time t) { return (uint32_t)(t / TERN_S(1)); }

/* --- Keys ------------------------------------------------------------------------------------- */

/* IV_d and EK_d(0). */
static void derive_direction(const uint8_t secret[TERN_UNICAST_SECRET], uint8_t dir,
                             uint8_t iv[TERN_CCM_NONCE], uint8_t epoch_key[32]) {
    expand(iv, TERN_CCM_NONCE, secret, TERN_UNICAST_SECRET, LABEL("tern v0 iv"), &dir, 1);
    expand(epoch_key, 32, secret, TERN_UNICAST_SECRET, LABEL("tern v0 epoch"), &dir, 1);
}

/* TK_d(e) from EK_d(e), expanded. */
static void tag_key(const uint8_t epoch_key[32], struct tern_aes128 *tk) {
    uint8_t k[TERN_AES128_KEY];
    expand(k, sizeof k, epoch_key, 32, LABEL("tern v0 tag"), NULL, 0);
    tern_aes128_init(tk, k);
    tern_wipe(k, sizeof k);
}

/* EK_d(e+1) from EK_d(e). next may be cur. */
static void next_epoch_key(const uint8_t cur[32], uint8_t next[32]) {
    uint8_t k[32];
    expand(k, sizeof k, cur, 32, LABEL("tern v0 next"), NULL, 0);
    copy(next, k, sizeof k);
    tern_wipe(k, sizeof k);
}

static void message_key(const uint8_t epoch_key[32], uint32_t n, uint8_t mk[TERN_AES128_KEY]) {
    uint8_t be[4];
    put_u32be(be, n);
    expand(mk, TERN_AES128_KEY, epoch_key, 32, LABEL("tern v0 msg"), be, sizeof be);
}

static void dtag(const struct tern_aes128 *tag_key, uint32_t n, uint8_t out[TERN_UNICAST_DTAG]) {
    uint8_t block[TERN_AES_BLOCK] = {0};
    put_u32be(&block[12], n);
    tern_aes128_encrypt(tag_key, block, block);
    copy(out, block, TERN_UNICAST_DTAG);
}

/* The tag's block with 0x01 at byte 11, so that neither gives the other. */
static void proof(const struct tern_aes128 *tag_key, uint32_t n, uint8_t out[TERN_UNICAST_PROOF]) {
    uint8_t block[TERN_AES_BLOCK] = {0};
    block[11] = 0x01;
    put_u32be(&block[12], n);
    tern_aes128_encrypt(tag_key, block, block);
    copy(out, block, TERN_UNICAST_PROOF);
}

static void nonce(const uint8_t iv[TERN_CCM_NONCE], uint32_t n, uint8_t out[TERN_CCM_NONCE]) {
    uint8_t be[4];
    put_u32be(be, n);
    copy(out, iv, TERN_CCM_NONCE);
    for (int i = 0; i < 4; i++) {
        out[9 + i] ^= be[i];
    }
}

/* --- The session ------------------------------------------------------------------------------ */

static void tx_init(struct tern_unicast_tx *tx, const uint8_t secret[TERN_UNICAST_SECRET],
                    uint8_t dir) {
    tern_wipe(tx, sizeof *tx);
    derive_direction(secret, dir, tx->iv, tx->epoch_key);
    tag_key(tx->epoch_key, &tx->tag_key);
    tx->dir = dir;
}

/* The destination tags of counters from to to, each from its own epoch's tag key: every one is
 * in the window, so its epoch key is held. */
static void window_tags(struct tern_unicast_rx *rx, uint64_t from, uint64_t to) {
    struct tern_aes128 tk;
    uint32_t have = 0;
    bool any = false;
    for (uint64_t m = from; m <= to; m++) {
        uint32_t e = (uint32_t)(m >> EPOCH_SHIFT);
        if (!any || e != have) {
            tag_key(rx->epoch_keys[e - rx->first_epoch], &tk);
            have = e;
            any = true;
        }
        dtag(&tk, (uint32_t)m, rx->dtags[m % 64]);
    }
    tern_wipe(&tk, sizeof tk);
}

static void rx_init(struct tern_unicast_rx *rx, const uint8_t secret[TERN_UNICAST_SECRET],
                    uint8_t dir) {
    tern_wipe(rx, sizeof *rx);
    derive_direction(secret, dir, rx->iv, rx->epoch_keys[0]);
    rx->dir = dir;
    rx->first_epoch = 0;
    rx->epochs = 1;
    window_tags(rx, 0, FIRST_WINDOW);
}

void tern_session_init(struct tern_session *s, uint8_t secret[TERN_UNICAST_SECRET],
                       enum tern_role role) {
    uint8_t send = (uint8_t)role;
    uint8_t receive = (uint8_t)(role == TERN_INITIATOR ? TERN_RESPONDER : TERN_INITIATOR);
    tx_init(&s->tx, secret, send);
    rx_init(&s->rx, secret, receive);
    s->age = 0;
    /* The Keys section: S regenerates every key in the session, so it goes first. */
    tern_wipe(secret, TERN_UNICAST_SECRET);
}

void tern_session_wipe(struct tern_session *s) { tern_wipe(s, sizeof *s); }

/* A message is tried RETRIES + 1 times (draft/forwarding.md), each a wake later where it sleeps. */
#define TRIES 4

void tern_session_sleep(struct tern_session *s, uint16_t own_wake, uint16_t peer_wake) {
    uint32_t own = tern_sleep_wake_ok(own_wake) ? tern_sleep_wake_s(own_wake) : 0;
    uint32_t peer = tern_sleep_wake_ok(peer_wake) ? tern_sleep_wake_s(peer_wake) : 0;
    s->rx.copy_extra = (uint16_t)(TRIES * own);
    s->tx.wait_extra = peer ? TRIES * (peer + TERN_SLEEP_LISTEN_MIN_S) : 0;
}

/* --- Sending ---------------------------------------------------------------------------------- */

bool tern_unicast_restart(const struct tern_unicast_tx *tx) {
    if (tx->given_up >= TERN_UNICAST_RESTART_AFTER) {
        return true;
    }
    uint64_t reach = tx->acked_any ? (uint64_t)tx->acked + AHEAD : FIRST_WINDOW;
    return (uint64_t)tx->next > reach;
}

void tern_unicast_given_up(struct tern_unicast_tx *tx) {
    if (tx->given_up < UINT8_MAX) {
        tx->given_up++;
    }
}

static int seal(struct tern_unicast_tx *tx, uint8_t hdr, const uint8_t *plaintext, size_t len,
                uint8_t *frame, size_t frame_cap) {
    if (tx->spent || tern_unicast_restart(tx)) {
        return TERN_ESPENT;
    }
    if (len > TERN_UNICAST_MAX_PLAINTEXT || frame == NULL ||
        frame_cap < len + TERN_UNICAST_OVERHEAD || (plaintext == NULL && len > 0)) {
        return TERN_EINVAL;
    }

    uint32_t n = tx->next;
    uint8_t mk[TERN_AES128_KEY], nc[TERN_CCM_NONCE], aad[1 + TERN_UNICAST_DTAG];

    frame[0] = hdr;
    for (size_t i = 1; i < TERN_UNICAST_HEAD; i++) {
        frame[i] = 0;
    }
    dtag(&tx->tag_key, n, &frame[AT_DTAG]);

    aad[0] = hdr;
    copy(&aad[1], &frame[AT_DTAG], TERN_UNICAST_DTAG);
    message_key(tx->epoch_key, n, mk);
    nonce(tx->iv, n, nc);
    (void)tern_ccm_seal(mk, nc, aad, sizeof aad, plaintext, len, &frame[AT_BODY],
                        &frame[AT_BODY + len]);
    tern_wipe(mk, sizeof mk);

    /* Sending, steps 1, 2 and 6: one more each time, never past 2^32 - 1, and once the last
     * message of an epoch has gone, its keys give way to the next. Its tag key is kept a while
     * as the previous one, for the acknowledgements still to come; the one before is erased. */
    if (n == UINT32_MAX || tx->next % 32 == 31) {
        uint8_t k[TERN_AES128_KEY];
        expand(k, sizeof k, tx->epoch_key, 32, LABEL("tern v0 tag"), NULL, 0);
        copy(tx->prev_tag_key, k, sizeof k);
        tern_wipe(k, sizeof k);
        tx->prev_held = true;
        tx->prev_dated = false;
    }
    if (n == UINT32_MAX) {
        tx->spent = true;
        tern_wipe(tx->epoch_key, sizeof tx->epoch_key);
        tern_wipe(&tx->tag_key, sizeof tx->tag_key);
    } else {
        tx->next = n + 1;
        if (tx->next % 32 == 0) {
            next_epoch_key(tx->epoch_key, tx->epoch_key);
            tag_key(tx->epoch_key, &tx->tag_key);
        }
    }
    return TERN_OK;
}

int tern_unicast_seal(struct tern_unicast_tx *tx, const uint8_t *plaintext, size_t len,
                      uint8_t *frame, size_t frame_cap) {
    return seal(tx, TERN_UNICAST_HDR, plaintext, len, frame, frame_cap);
}

int tern_unicast_seal_node(struct tern_unicast_tx *tx, const uint8_t *plaintext, size_t len,
                           uint8_t *frame, size_t frame_cap) {
    return seal(tx, TERN_UNICAST_HDR_NODE, plaintext, len, frame, frame_cap);
}

/* --- Receiving -------------------------------------------------------------------------------- */

static uint32_t window_lo(const struct tern_unicast_rx *rx) {
    return rx->started && rx->high > BEHIND ? rx->high - BEHIND : 0;
}

static uint32_t window_hi(const struct tern_unicast_rx *rx) {
    if (!rx->started) {
        return FIRST_WINDOW;
    }
    return rx->high > UINT32_MAX - AHEAD ? UINT32_MAX : rx->high + AHEAD;
}

static bool accepted(const struct tern_unicast_rx *rx, uint32_t n) {
    return rx->started && n <= rx->high && rx->high - n <= BEHIND &&
           ((rx->seen >> (rx->high - n)) & 1u);
}

/* The tag key of epoch e, expanded, if the receiver holds it: from its epoch key, or one of the two
 * kept below the epochs it holds. */
static bool rx_tag_key(const struct tern_unicast_rx *rx, uint32_t e, struct tern_aes128 *tk) {
    if (e >= rx->first_epoch && e - rx->first_epoch < rx->epochs) {
        tag_key(rx->epoch_keys[e - rx->first_epoch], tk);
        return true;
    }
    for (unsigned i = 0; i < 2; i++) {
        if (((rx->old_held >> i) & 1u) && (uint64_t)e + 2 - i == rx->first_epoch) {
            tern_aes128_init(tk, rx->old_tag_keys[i]);
            return true;
        }
    }
    return false;
}

/* Erases the tag keys kept below the epochs held that answer no copy any more. */
static void drop_old_tag_keys(struct tern_unicast_rx *rx) {
    for (unsigned i = 0; i < 2; i++) {
        if (!((rx->old_held >> i) & 1u)) {
            continue;
        }
        uint64_t e = (uint64_t)rx->first_epoch - 2 + i;
        bool needed = false;
        for (unsigned k = 0; k < 32 && !needed; k++) {
            /* Counter n is kept at n % 32 if it is accepted and within 31 of high; a bit left
             * from a counter 32 before it is no one's. */
            uint32_t n = rx->high - ((rx->high - k) % 32);
            needed = ((rx->kept >> k) & 1u) && accepted(rx, n) && (n >> EPOCH_SHIFT) == e;
        }
        if (!needed) {
            tern_wipe(rx->old_tag_keys[i], TERN_AES128_KEY);
            rx->old_held &= (uint8_t) ~(1u << i);
        }
    }
}

/* Keeps exactly the epoch keys for the counters still in the window (Receiving, step 7). An epoch
 * whose counters have all left the window, by falling below it or by being accepted, is erased,
 * but only once the key after it is held; its tag key is kept below the rest, for the copies of
 * its messages still answered. */
static void keep_epochs(struct tern_unicast_rx *rx) {
    uint32_t lo = window_lo(rx), hi = window_hi(rx);

    /* The lowest counter the window still holds. Every counter above H is still in it. */
    uint64_t lowest = lo;
    while (lowest <= hi && accepted(rx, (uint32_t)lowest)) {
        lowest++;
    }

    if (lowest > hi) {
        /* Only possible once H is 2^32 - 1 and the 31 below it are in: nothing is left to
         * receive, and the last epoch has no next. */
        tern_wipe(rx->epoch_keys, sizeof rx->epoch_keys);
        rx->epochs = 0;
        return;
    }

    uint32_t first = (uint32_t)(lowest >> EPOCH_SHIFT), last = hi >> EPOCH_SHIFT;
    while (rx->first_epoch < first) {
        if (rx->epochs == 1) {
            next_epoch_key(rx->epoch_keys[0], rx->epoch_keys[1]);
            rx->epochs = 2;
        }
        /* The tag key of the epoch about to go moves below, and the oldest below goes. */
        copy(rx->old_tag_keys[0], rx->old_tag_keys[1], TERN_AES128_KEY);
        expand(rx->old_tag_keys[1], TERN_AES128_KEY, rx->epoch_keys[0], 32, LABEL("tern v0 tag"),
               NULL, 0);
        rx->old_held = (uint8_t)(((rx->old_held >> 1) & 1u) | 2u);
        for (uint8_t i = 0; i + 1 < rx->epochs; i++) {
            copy(rx->epoch_keys[i], rx->epoch_keys[i + 1], 32);
        }
        tern_wipe(rx->epoch_keys[rx->epochs - 1], 32);
        rx->epochs--;
        rx->first_epoch++;
    }
    while (rx->first_epoch + rx->epochs - 1 < last && rx->epochs < 3) {
        next_epoch_key(rx->epoch_keys[rx->epochs - 1], rx->epoch_keys[rx->epochs]);
        rx->epochs++;
    }
    drop_old_tag_keys(rx);
}

/* Receiving, step 5, and what forwarding.md has a destination keep to answer copies. */
static void accept(struct tern_unicast_rx *rx, uint32_t n, const uint8_t *frame, size_t len,
                   tern_time now) {
    uint32_t old_hi = window_hi(rx);
    unsigned k = n % 32;

    copy(rx->check[k], &frame[len - TERN_CCM_TAG], CHECK);
    rx->len[k] = (uint8_t)(len - TERN_UNICAST_OVERHEAD);
    rx->node = (rx->node & ~(1u << k)) | ((frame[0] == TERN_UNICAST_HDR_NODE ? 1u : 0u) << k);
    rx->acks &= ~((uint64_t)3 << (2 * k));
    rx->at[k] = (uint16_t)seconds(now);
    rx->kept |= 1u << k;

    if (!rx->started) {
        rx->started = true;
        rx->high = n;
        rx->seen = 1;
    } else if (n > rx->high) {
        uint32_t step = n - rx->high;
        rx->seen = step > BEHIND ? 1u : (rx->seen << step) | 1u;
        rx->high = n;
    } else {
        rx->seen |= 1u << (rx->high - n);
    }

    /* The epoch keys first, so that every counter entering the window has its own. The counters
     * that have entered the window get their tags. Those that have left it need nothing: their
     * slots are the entering counters' (the window is 64 wide, so the counters sharing a slot are
     * never in it together), and until overwritten they are never read. */
    keep_epochs(rx);
    uint32_t lo = window_lo(rx), hi = window_hi(rx);
    uint64_t from = (uint64_t)old_hi + 1 > lo ? (uint64_t)old_hi + 1 : lo;
    if (from <= hi) {
        window_tags(rx, from, hi);
    }
}

/* Whether a frame is a copy of accepted message n that is still answered: n is no more than 31
 * below H, what answers its copies is held (counter n's at n % 32 is n's own: of the counters
 * within 31 of H, no two share it), and the frame has its length, its node flag, its tag and the
 * first four bytes of its AEAD tag; fewer than COPY_ACKS acknowledgements of it have gone, and
 * no more than COPY_TIME has passed since it was accepted. */
static unsigned acks_of(const struct tern_unicast_rx *rx, unsigned k) {
    return (unsigned)((rx->acks >> (2 * k)) & 3u);
}

/* Whether what was kept of counter k's message is past COPY_TIME. The time is kept to 16 bits,
 * which is far longer than COPY_TIME, and tern_unicast_expire() lets go of it long before it
 * wraps. */
static bool late(const struct tern_unicast_rx *rx, unsigned k, tern_time now) {
    return (uint16_t)((uint16_t)seconds(now) - rx->at[k]) >
           (uint16_t)(TERN_UNICAST_COPY_TIME / TERN_S(1) + rx->copy_extra);
}

static bool copy_of(const struct tern_unicast_rx *rx, uint32_t n, const uint8_t *frame, size_t len,
                    tern_time now) {
    unsigned k = n % 32;
    return accepted(rx, n) && ((rx->kept >> k) & 1u) &&
           rx->len[k] == (uint8_t)(len - TERN_UNICAST_OVERHEAD) &&
           (((rx->node >> k) & 1u) != 0) == (frame[0] == TERN_UNICAST_HDR_NODE) &&
           same(rx->dtags[n % 64], &frame[AT_DTAG], TERN_UNICAST_DTAG) &&
           same(rx->check[k], &frame[len - TERN_CCM_TAG], CHECK) &&
           acks_of(rx, k) < TERN_UNICAST_COPY_ACKS && !late(rx, k, now);
}

static void rx_expire(struct tern_unicast_rx *rx, tern_time now) {
    for (unsigned k = 0; k < 32; k++) {
        if (((rx->kept >> k) & 1u) && late(rx, k, now)) {
            rx->kept &= ~(1u << k);
        }
    }
    drop_old_tag_keys(rx);
}

void tern_unicast_expire(struct tern_session *s, tern_time now) {
    struct tern_unicast_tx *tx = &s->tx;
    if (tx->prev_held && !tx->prev_dated) {
        tx->prev_at = seconds(now);
        tx->prev_dated = true;
    } else if (tx->prev_held &&
               seconds(now) - tx->prev_at >
                   (uint32_t)(TERN_UNICAST_COPY_TIME / TERN_S(1)) + tx->wait_extra) {
        /* No source waits longer than COPY_TIME for an acknowledgement. */
        tern_wipe(tx->prev_tag_key, sizeof tx->prev_tag_key);
        tx->prev_held = false;
    }
    rx_expire(&s->rx, now);
}

int tern_unicast_open(struct tern_unicast_rx *const *rx, size_t count, const uint8_t *frame,
                      size_t len, uint8_t *plaintext, size_t plaintext_cap, tern_time now,
                      struct tern_unicast_received *out) {
    if ((rx == NULL && count > 0) || frame == NULL || out == NULL) {
        return TERN_EINVAL;
    }
    out->session = 0;
    out->counter = 0;
    out->len = 0;

    /* Receiving, step 1. */
    if (len < TERN_UNICAST_OVERHEAD || len > TERN_UNICAST_MAX_FRAME ||
        (frame[0] != TERN_UNICAST_HDR && frame[0] != TERN_UNICAST_HDR_NODE)) {
        out->verdict = TERN_UNICAST_MALFORMED;
        return TERN_OK;
    }
    size_t pt_len = len - TERN_UNICAST_OVERHEAD;
    if (pt_len > plaintext_cap || (plaintext == NULL && pt_len > 0)) {
        return TERN_EINVAL;
    }

    const uint8_t *tag = &frame[AT_DTAG];
    const uint8_t *ct = &frame[AT_BODY];
    uint8_t aad[1 + TERN_UNICAST_DTAG];
    aad[0] = frame[0];
    copy(&aad[1], tag, TERN_UNICAST_DTAG);

    /* Steps 2 to 4: every window entry with this tag, in every session, until one authenticates.
     * A linear scan stands in for the specification's table; it gives the same answer, and a
     * faster structure is a later change. */
    bool matched = false, copied = false;
    for (size_t i = 0; i < count; i++) {
        rx_expire(rx[i], now);
    }
    for (size_t i = 0; i < count; i++) {
        struct tern_unicast_rx *r = rx[i];
        uint32_t lo = window_lo(r), hi = window_hi(r);
        for (uint64_t m = lo; m <= hi; m++) {
            uint32_t n = (uint32_t)m;
            if (!same(r->dtags[m % 64], tag, TERN_UNICAST_DTAG)) {
                continue;
            }
            if (accepted(r, n)) {
                /* Perhaps a copy of a message this node has: remembered, in case no entry still
                 * in a window takes the frame. An accepted counter's tag is still where it was
                 * put: the counter that shares its slot is 64 on, and enters only once this one
                 * is more than 31 below H. */
                if (!copied && copy_of(r, n, frame, len, now)) {
                    copied = true;
                    out->session = i;
                    out->counter = n;
                }
                continue;
            }
            matched = true;

            uint8_t mk[TERN_AES128_KEY], nc[TERN_CCM_NONCE];
            message_key(r->epoch_keys[(n >> EPOCH_SHIFT) - r->first_epoch], n, mk);
            nonce(r->iv, n, nc);
            bool ok = tern_ccm_open(mk, nc, aad, sizeof aad, ct, pt_len, &ct[pt_len], plaintext);
            tern_wipe(mk, sizeof mk);
            if (ok) {
                accept(r, n, frame, len, now);
                out->verdict = TERN_UNICAST_ACCEPTED;
                out->session = i;
                out->counter = n;
                out->len = pt_len;
                return TERN_OK;
            }
        }
    }

    if (copied) {
        out->verdict = TERN_UNICAST_COPY;
        return TERN_OK;
    }
    out->verdict = matched ? TERN_UNICAST_FORGED : TERN_UNICAST_NOT_OURS;
    return TERN_OK;
}

bool tern_unicast_peek(struct tern_unicast_rx *const *rx, size_t count, const uint8_t *frame,
                       size_t len, uint8_t *plaintext, size_t plaintext_cap, size_t *session) {
    if ((rx == NULL && count > 0) || frame == NULL || session == NULL ||
        len < TERN_UNICAST_OVERHEAD || len > TERN_UNICAST_MAX_FRAME ||
        (frame[0] != TERN_UNICAST_HDR && frame[0] != TERN_UNICAST_HDR_NODE)) {
        return false;
    }
    size_t pt_len = len - TERN_UNICAST_OVERHEAD;
    if (pt_len > plaintext_cap || (plaintext == NULL && pt_len > 0)) {
        return false;
    }
    const uint8_t *tag = &frame[AT_DTAG];
    const uint8_t *ct = &frame[AT_BODY];
    uint8_t aad[1 + TERN_UNICAST_DTAG];
    aad[0] = frame[0];
    copy(&aad[1], tag, TERN_UNICAST_DTAG);
    /* As tern_unicast_open() looks, and stopping where it would accept. */
    for (size_t i = 0; i < count; i++) {
        const struct tern_unicast_rx *r = rx[i];
        uint32_t lo = window_lo(r), hi = window_hi(r);
        for (uint64_t m = lo; m <= hi; m++) {
            uint32_t n = (uint32_t)m;
            if (!same(r->dtags[m % 64], tag, TERN_UNICAST_DTAG) || accepted(r, n)) {
                continue;
            }
            uint8_t mk[TERN_AES128_KEY], nc[TERN_CCM_NONCE];
            message_key(r->epoch_keys[(n >> EPOCH_SHIFT) - r->first_epoch], n, mk);
            nonce(r->iv, n, nc);
            bool ok = tern_ccm_open(mk, nc, aad, sizeof aad, ct, pt_len, &ct[pt_len], plaintext);
            tern_wipe(mk, sizeof mk);
            if (ok) {
                tern_wipe(plaintext, pt_len);
                *session = i;
                return true;
            }
        }
    }
    return false;
}

/* --- Acknowledgements ------------------------------------------------------------------------- */

bool tern_unicast_copy_next(struct tern_unicast_rx *const *rx, size_t count, const uint8_t *frame,
                            size_t len, tern_time now, struct tern_unicast_received *got) {
    if (rx == NULL || frame == NULL || got == NULL || len < TERN_UNICAST_OVERHEAD ||
        len > TERN_UNICAST_MAX_FRAME || got->verdict != TERN_UNICAST_COPY) {
        return false;
    }
    /* In the order tern_unicast_open() looks: by session, and in each by counter. */
    for (size_t i = got->session; i < count; i++) {
        const struct tern_unicast_rx *r = rx[i];
        uint64_t from = i == got->session ? (uint64_t)got->counter + 1 : window_lo(r);
        for (uint64_t m = from; r->started && m <= r->high; m++) {
            if (copy_of(r, (uint32_t)m, frame, len, now)) {
                got->session = i;
                got->counter = (uint32_t)m;
                return true;
            }
        }
    }
    return false;
}

bool tern_unicast_ack(struct tern_unicast_rx *rx, uint32_t counter,
                      uint8_t frame[TERN_UNICAST_ACK_LEN]) {
    unsigned k = counter % 32;
    struct tern_aes128 tk;
    if (!accepted(rx, counter) || !((rx->kept >> k) & 1u) ||
        !rx_tag_key(rx, counter >> EPOCH_SHIFT, &tk)) {
        return false;
    }
    frame[0] = TERN_UNICAST_ACK_HDR;
    for (size_t i = 1; i < TERN_UNICAST_HEAD; i++) {
        frame[i] = 0;
    }
    copy(&frame[AT_DTAG], rx->dtags[counter % 64], TERN_UNICAST_DTAG);
    proof(&tk, counter, &frame[AT_BODY]);
    tern_wipe(&tk, sizeof tk);
    if (acks_of(rx, k) < 3) {
        rx->acks += (uint64_t)1 << (2 * k);
    } else {
        /* The fourth: no copy is answered after it. Kept no longer. */
        rx->kept &= ~(1u << k);
    }
    return true;
}

bool tern_unicast_acked(struct tern_unicast_tx *tx, uint32_t counter, const uint8_t *frame,
                        size_t len) {
    if (frame == NULL || len != TERN_UNICAST_ACK_LEN || frame[0] != TERN_UNICAST_ACK_HDR) {
        return false;
    }
    /* A counter not yet used has no acknowledgement, and one sealed before a restart, or in an
     * epoch whose tag key has gone, has nothing left to check it against. */
    uint64_t end = tx->spent ? (uint64_t)UINT32_MAX + 1 : tx->next;
    if (counter >= end || counter < tx->sent_from) {
        return false;
    }
    struct tern_aes128 tk;
    uint64_t now_epoch = end >> EPOCH_SHIFT, e = counter >> EPOCH_SHIFT;
    if (e == now_epoch && !tx->spent) {
        tk = tx->tag_key;
    } else if (e + 1 == now_epoch && tx->prev_held) {
        tern_aes128_init(&tk, tx->prev_tag_key);
    } else {
        return false;
    }
    uint8_t want[TERN_UNICAST_DTAG + TERN_UNICAST_PROOF], diff = 0;
    dtag(&tk, counter, want);
    proof(&tk, counter, &want[TERN_UNICAST_DTAG]);
    tern_wipe(&tk, sizeof tk);
    for (size_t i = 0; i < sizeof want; i++) {
        diff |= (uint8_t)(want[i] ^ frame[AT_DTAG + i]);
    }
    if (diff != 0) {
        return false;
    }
    /* Starting again: K and G. */
    if (!tx->acked_any || counter > tx->acked) {
        tx->acked = counter;
        tx->acked_any = true;
    }
    tx->given_up = 0;
    return true;
}

/* --- Packing ---------------------------------------------------------------------------------- */

#define PACKED_FORM 0x02
#define PACKED_USED (1 + 57 + 124 + 4)
_Static_assert(PACKED_USED <= TERN_SESSION_PACKED, "a packed session fits");

static uint8_t *put_bytes(uint8_t *p, const uint8_t *from, size_t len) {
    copy(p, from, len);
    return p + len;
}

static uint8_t *put_u32le(uint8_t *p, uint32_t n) {
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(n >> (8 * i));
    }
    return p + 4;
}

static const uint8_t *get_u32le(const uint8_t *p, uint32_t *n) {
    *n = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    return p + 4;
}

static const uint8_t *get_bytes(const uint8_t *p, uint8_t *to, size_t len) {
    copy(to, p, len);
    return p + len;
}

void tern_session_pack(const struct tern_session *s, uint8_t out[TERN_SESSION_PACKED]) {
    const struct tern_unicast_tx *tx = &s->tx;
    const struct tern_unicast_rx *rx = &s->rx;
    uint8_t *p = out;
    *p++ = PACKED_FORM;
    p = put_bytes(p, tx->iv, TERN_CCM_NONCE);
    p = put_bytes(p, tx->epoch_key, 32);
    p = put_u32le(p, tx->next);
    *p++ = tx->dir;
    *p++ = tx->spent ? 1 : 0;
    *p++ = tx->acked_any ? 1 : 0;
    p = put_u32le(p, tx->acked);
    *p++ = tx->given_up;
    p = put_bytes(p, rx->iv, TERN_CCM_NONCE);
    *p++ = rx->dir;
    *p++ = rx->started ? 1 : 0;
    p = put_u32le(p, rx->high);
    p = put_u32le(p, rx->seen);
    p = put_u32le(p, rx->first_epoch);
    *p++ = rx->epochs;
    p = put_bytes(p, (const uint8_t *)rx->epoch_keys, sizeof rx->epoch_keys);
    p = put_u32le(p, s->age);
    while (p < out + TERN_SESSION_PACKED) {
        *p++ = 0;
    }
}

bool tern_session_unpack(struct tern_session *s, const uint8_t in[TERN_SESSION_PACKED]) {
    struct tern_unicast_tx *tx = &s->tx;
    struct tern_unicast_rx *rx = &s->rx;
    tern_session_wipe(s);
    const uint8_t *p = in;
    if (*p++ != PACKED_FORM) {
        return false;
    }
    p = get_bytes(p, tx->iv, TERN_CCM_NONCE);
    p = get_bytes(p, tx->epoch_key, 32);
    p = get_u32le(p, &tx->next);
    tx->dir = *p++;
    uint8_t spent = *p++, acked_any = *p++;
    tx->spent = spent == 1;
    tx->acked_any = acked_any == 1;
    p = get_u32le(p, &tx->acked);
    tx->given_up = *p++;
    /* Nothing sealed before can be checked: its tags were not kept. */
    tx->sent_from = tx->next;
    p = get_bytes(p, rx->iv, TERN_CCM_NONCE);
    rx->dir = *p++;
    uint8_t started = *p++;
    rx->started = started == 1;
    p = get_u32le(p, &rx->high);
    p = get_u32le(p, &rx->seen);
    p = get_u32le(p, &rx->first_epoch);
    rx->epochs = *p++;
    p = get_bytes(p, (uint8_t *)rx->epoch_keys, sizeof rx->epoch_keys);
    (void)get_u32le(p, &s->age);

    bool directions = (tx->dir == TERN_INITIATOR && rx->dir == TERN_RESPONDER) ||
                      (tx->dir == TERN_RESPONDER && rx->dir == TERN_INITIATOR);
    /* A receiver that has accepted something has accepted its highest; one that has not holds
     * no highest at all (accept()). */
    bool window = started == 1 ? (rx->seen & 1u) != 0 : rx->high == 0 && rx->seen == 0;
    if (!directions || spent > 1 || acked_any > 1 || started > 1 || !window || rx->epochs > 3 ||
        (acked_any == 1 && !spent && tx->acked >= tx->next)) {
        tern_session_wipe(s);
        return false;
    }
    /* The epoch keys held are exactly those keep_epochs() leaves: from the epoch of the lowest
     * counter the window still waits for to that of its highest (Receiving, step 7), or none when
     * it waits for nothing. Fewer would have tern_unicast_open() take a key from outside the
     * array; more, and a later epoch's key would never be worked out. */
    uint32_t lo = window_lo(rx), hi = window_hi(rx);
    uint64_t lowest = lo;
    while (lowest <= hi && accepted(rx, (uint32_t)lowest)) {
        lowest++;
    }
    bool keys = lowest > hi ? rx->epochs == 0
                            : rx->first_epoch == (uint32_t)(lowest >> EPOCH_SHIFT) &&
                                  rx->epochs == (hi >> EPOCH_SHIFT) - rx->first_epoch + 1;
    if (!keys) {
        tern_session_wipe(s);
        return false;
    }
    /* Every tag tern_unicast_open() may read: those of the counters in the window still to come.
     * An accepted counter's is not, nor anything to answer its copies with. */
    if (lowest <= hi) {
        window_tags(rx, lowest, hi);
    }
    if (!tx->spent) {
        tag_key(tx->epoch_key, &tx->tag_key);
    }
    return true;
}
