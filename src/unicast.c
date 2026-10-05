#include "tern/unicast.h"

#include "tern/err.h"

/* Secured unicast frames, as draft/unicast-security.md in ternmesh/spec defines them. Section
 * names in comments are that document's. */

#define EPOCH_SHIFT 5 /* 32 messages to an epoch */
#define LAST_EPOCH (UINT32_MAX >> EPOCH_SHIFT)
#define BEHIND 31 /* how far below H the window reaches */
#define AHEAD 32  /* and how far above */
#define FIRST_WINDOW 31

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

/* --- Keys ------------------------------------------------------------------------------------- */

/* TK_d, IV_d and EK_d(0). */
static void derive_direction(const uint8_t secret[TERN_UNICAST_SECRET], uint8_t dir,
                             struct tern_aes128 *tag_key, uint8_t iv[TERN_CCM_NONCE],
                             uint8_t epoch_key[32]) {
    uint8_t tk[TERN_AES128_KEY];
    expand(tk, sizeof tk, secret, TERN_UNICAST_SECRET, LABEL("tern v0 tag"), &dir, 1);
    tern_aes128_init(tag_key, tk);
    tern_wipe(tk, sizeof tk);
    expand(iv, TERN_CCM_NONCE, secret, TERN_UNICAST_SECRET, LABEL("tern v0 iv"), &dir, 1);
    expand(epoch_key, 32, secret, TERN_UNICAST_SECRET, LABEL("tern v0 epoch"), &dir, 1);
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
    derive_direction(secret, dir, &tx->tag_key, tx->iv, tx->epoch_key);
    tx->next = 0;
    tx->dir = dir;
    tx->spent = false;
}

static void rx_init(struct tern_unicast_rx *rx, const uint8_t secret[TERN_UNICAST_SECRET],
                    uint8_t dir) {
    tern_wipe(rx, sizeof *rx);
    derive_direction(secret, dir, &rx->tag_key, rx->iv, rx->epoch_keys[0]);
    rx->dir = dir;
    rx->first_epoch = 0;
    rx->epochs = 1;
    for (uint32_t n = 0; n <= FIRST_WINDOW; n++) {
        dtag(&rx->tag_key, n, rx->dtags[n % 64]);
    }
}

void tern_session_init(struct tern_session *s, uint8_t secret[TERN_UNICAST_SECRET],
                       enum tern_role role) {
    uint8_t send = (uint8_t)role;
    uint8_t receive = (uint8_t)(role == TERN_INITIATOR ? TERN_RESPONDER : TERN_INITIATOR);
    tx_init(&s->tx, secret, send);
    rx_init(&s->rx, secret, receive);
    /* The Keys section: S regenerates every key in the session, so it goes first. */
    tern_wipe(secret, TERN_UNICAST_SECRET);
}

void tern_session_wipe(struct tern_session *s) { tern_wipe(s, sizeof *s); }

/* --- Sending ---------------------------------------------------------------------------------- */

int tern_unicast_seal(struct tern_unicast_tx *tx, uint8_t hop, uint16_t label,
                      const uint8_t *plaintext, size_t len, uint8_t *frame, size_t frame_cap) {
    if (tx->spent) {
        return TERN_ESPENT;
    }
    if (len > TERN_UNICAST_MAX_PLAINTEXT || frame == NULL ||
        frame_cap < len + TERN_UNICAST_OVERHEAD || (plaintext == NULL && len > 0)) {
        return TERN_EINVAL;
    }

    uint32_t n = tx->next;
    uint8_t mk[TERN_AES128_KEY], nc[TERN_CCM_NONCE], aad[1 + TERN_UNICAST_DTAG];

    frame[0] = TERN_UNICAST_HDR;
    frame[1] = hop;
    frame[2] = (uint8_t)(label >> 8);
    frame[3] = (uint8_t)label;
    dtag(&tx->tag_key, n, &frame[4]);

    aad[0] = TERN_UNICAST_HDR;
    copy(&aad[1], &frame[4], TERN_UNICAST_DTAG);
    message_key(tx->epoch_key, n, mk);
    nonce(tx->iv, n, nc);
    (void)tern_ccm_seal(mk, nc, aad, sizeof aad, plaintext, len, &frame[8], &frame[8 + len]);
    tern_wipe(mk, sizeof mk);

    /* Sending, steps 1, 2 and 5: one more each time, never past 2^32 - 1, and once the last
     * message of an epoch has gone, its key gives way to the next. */
    if (n == UINT32_MAX) {
        tx->spent = true;
        tern_wipe(tx->epoch_key, sizeof tx->epoch_key);
    } else {
        tx->next = n + 1;
        if (tx->next % 32 == 0) {
            next_epoch_key(tx->epoch_key, tx->epoch_key);
        }
    }
    return TERN_OK;
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

/* Keeps exactly the epoch keys for the counters still in the window (Receiving, step 6). An epoch
 * whose counters have all left the window, by falling below it or by being accepted, is erased,
 * but only once the key after it is held. */
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
}

/* Receiving, step 5. */
static void accept(struct tern_unicast_rx *rx, uint32_t n) {
    uint32_t old_hi = window_hi(rx);

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

    /* The counters that have entered the window get their tags. Those that have left it need
     * nothing: their slots are the entering counters' (the window is 64 wide, so the counters
     * sharing a slot are never in it together), and until overwritten they are never read. */
    uint32_t lo = window_lo(rx), hi = window_hi(rx);
    uint64_t from = (uint64_t)old_hi + 1 > lo ? (uint64_t)old_hi + 1 : lo;
    for (uint64_t m = from; m <= hi; m++) {
        dtag(&rx->tag_key, (uint32_t)m, rx->dtags[m % 64]);
    }

    keep_epochs(rx);
}

static bool same_dtag(const uint8_t *a, const uint8_t *b) {
    /* Tags travel in the clear, so comparing them need not take constant time. */
    for (int i = 0; i < TERN_UNICAST_DTAG; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

int tern_unicast_open(struct tern_unicast_rx *const *rx, size_t count, const uint8_t *frame,
                      size_t len, uint8_t *plaintext, size_t plaintext_cap,
                      struct tern_unicast_received *out) {
    if ((rx == NULL && count > 0) || frame == NULL || out == NULL) {
        return TERN_EINVAL;
    }
    out->session = 0;
    out->counter = 0;
    out->len = 0;

    /* Receiving, step 1. */
    if (len < TERN_UNICAST_OVERHEAD || len > TERN_UNICAST_MAX_FRAME ||
        frame[0] != TERN_UNICAST_HDR) {
        out->verdict = TERN_UNICAST_MALFORMED;
        return TERN_OK;
    }
    size_t pt_len = len - TERN_UNICAST_OVERHEAD;
    if (pt_len > plaintext_cap || (plaintext == NULL && pt_len > 0)) {
        return TERN_EINVAL;
    }

    const uint8_t *tag = &frame[4];
    const uint8_t *ct = &frame[8];
    uint8_t aad[1 + TERN_UNICAST_DTAG];
    aad[0] = frame[0];
    copy(&aad[1], tag, TERN_UNICAST_DTAG);

    /* Steps 2 to 4: every window entry with this tag, in every session, until one authenticates.
     * A linear scan stands in for the specification's table; it gives the same answer, and a
     * faster structure is a later change. */
    bool matched = false;
    for (size_t i = 0; i < count; i++) {
        struct tern_unicast_rx *r = rx[i];
        uint32_t lo = window_lo(r), hi = window_hi(r);
        for (uint64_t m = lo; m <= hi; m++) {
            uint32_t n = (uint32_t)m;
            if (accepted(r, n) || !same_dtag(r->dtags[m % 64], tag)) {
                continue;
            }
            matched = true;

            uint8_t mk[TERN_AES128_KEY], nc[TERN_CCM_NONCE];
            message_key(r->epoch_keys[(n >> EPOCH_SHIFT) - r->first_epoch], n, mk);
            nonce(r->iv, n, nc);
            bool ok = tern_ccm_open(mk, nc, aad, sizeof aad, ct, pt_len, &ct[pt_len], plaintext);
            tern_wipe(mk, sizeof mk);
            if (ok) {
                accept(r, n);
                out->verdict = TERN_UNICAST_ACCEPTED;
                out->session = i;
                out->counter = n;
                out->len = pt_len;
                return TERN_OK;
            }
        }
    }

    out->verdict = matched ? TERN_UNICAST_FORGED : TERN_UNICAST_NOT_OURS;
    return TERN_OK;
}
