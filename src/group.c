#include "tern/group.h"

#include <string.h>

#include "tern/companion.h"
#include "tern/err.h"

#define AT_NONCE 3
#define AT_TAG (AT_NONCE + TERN_GROUP_NONCE)
#define AT_BODY (AT_TAG + TERN_GROUP_TAG)
#define FROM 4
#define AAD (1 + TERN_GROUP_NONCE + TERN_GROUP_TAG)

static uint32_t get32(const uint8_t *b) {
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static bool an_id(uint32_t id) { return id != 0 && id != 0xFFFFFFFFu; }

void tern_group_init(struct tern_group *g, const uint8_t secret[TERN_GROUP_SECRET]) {
    static const uint8_t key_info[] = "tern v0 group key", tag_info[] = "tern v0 group tag";
    uint8_t tk[TERN_AES128_KEY];
    memset(g, 0, sizeof *g);
    memcpy(g->secret, secret, TERN_GROUP_SECRET);
    (void)tern_hkdf_expand(g->key, sizeof g->key, secret, TERN_GROUP_SECRET, key_info,
                           sizeof key_info - 1);
    (void)tern_hkdf_expand(tk, sizeof tk, secret, TERN_GROUP_SECRET, tag_info, sizeof tag_info - 1);
    tern_aes128_init(&g->tag_key, tk);
    tern_wipe(tk, sizeof tk);
}

void tern_group_wipe(struct tern_group *g) { tern_wipe(g, sizeof *g); }

/* gtag(N): the first four bytes of the tag key's block of eight zeros and the nonce. */
static void gtag(const struct tern_group *g, const uint8_t *nonce, uint8_t tag[TERN_GROUP_TAG]) {
    uint8_t block[TERN_AES_BLOCK] = {0};
    memcpy(block + 8, nonce, TERN_GROUP_NONCE);
    tern_aes128_encrypt(&g->tag_key, block, block);
    memcpy(tag, block, TERN_GROUP_TAG);
}

static void ccm_nonce(const uint8_t *nonce, uint8_t out[TERN_CCM_NONCE]) {
    memset(out, 0, TERN_CCM_NONCE - TERN_GROUP_NONCE);
    memcpy(out + TERN_CCM_NONCE - TERN_GROUP_NONCE, nonce, TERN_GROUP_NONCE);
}

static int seal(const struct tern_group *g, uint8_t hdr, const uint8_t nonce[TERN_GROUP_NONCE],
                uint32_t from, const uint8_t *content, size_t len, uint8_t *frame,
                size_t frame_cap) {
    if (len > TERN_GROUP_MAX_CONTENT || frame == NULL || frame_cap < len + TERN_GROUP_OVERHEAD ||
        (content == NULL && len > 0) || !an_id(from)) {
        return TERN_EINVAL;
    }
    uint8_t nc[TERN_CCM_NONCE], aad[AAD], plain[FROM + TERN_GROUP_MAX_CONTENT];
    frame[0] = hdr;
    frame[1] = 0;
    frame[2] = 0;
    memcpy(frame + AT_NONCE, nonce, TERN_GROUP_NONCE);
    gtag(g, nonce, frame + AT_TAG);
    aad[0] = hdr;
    memcpy(aad + 1, frame + AT_NONCE, TERN_GROUP_NONCE + TERN_GROUP_TAG);
    plain[0] = (uint8_t)(from >> 24);
    plain[1] = (uint8_t)(from >> 16);
    plain[2] = (uint8_t)(from >> 8);
    plain[3] = (uint8_t)from;
    if (len > 0) {
        memcpy(plain + FROM, content, len);
    }
    ccm_nonce(nonce, nc);
    (void)tern_ccm_seal(g->key, nc, aad, sizeof aad, plain, FROM + len, frame + AT_BODY,
                        frame + AT_BODY + FROM + len);
    tern_wipe(plain, sizeof plain);
    return TERN_OK;
}

int tern_group_seal(const struct tern_group *g, const uint8_t nonce[TERN_GROUP_NONCE],
                    uint32_t from, const uint8_t *content, size_t len, uint8_t *frame,
                    size_t frame_cap) {
    return seal(g, TERN_GROUP_HDR, nonce, from, content, len, frame, frame_cap);
}

int tern_group_seal_node(const struct tern_group *g, const uint8_t nonce[TERN_GROUP_NONCE],
                         uint32_t from, const uint8_t *content, size_t len, uint8_t *frame,
                         size_t frame_cap) {
    return seal(g, TERN_GROUP_HDR_NODE, nonce, from, content, len, frame, frame_cap);
}

static bool held(const struct tern_group *g, const uint8_t *nonce) {
    for (size_t i = 0; i < g->count; i++) {
        if (memcmp(g->recent[i], nonce, TERN_GROUP_NONCE) == 0) {
            return true;
        }
    }
    return false;
}

static void keep(struct tern_group *g, const uint8_t *nonce) {
    memcpy(g->recent[g->next], nonce, TERN_GROUP_NONCE);
    g->next = (uint8_t)((g->next + 1) % TERN_GROUP_RECENT);
    g->count = (uint8_t)(g->count + (g->count < TERN_GROUP_RECENT));
}

int tern_group_open(struct tern_group *const *g, size_t count, uint32_t self, const uint8_t *frame,
                    size_t len, uint8_t *content, size_t content_cap,
                    struct tern_group_received *out) {
    if ((g == NULL && count > 0) || frame == NULL || out == NULL) {
        return TERN_EINVAL;
    }
    *out = (struct tern_group_received){0};
    if (len < TERN_GROUP_OVERHEAD || len > TERN_GROUP_MAX_FRAME ||
        (frame[0] != TERN_GROUP_HDR && frame[0] != TERN_GROUP_HDR_NODE)) {
        out->verdict = TERN_GROUP_MALFORMED;
        return TERN_OK;
    }
    size_t content_len = len - TERN_GROUP_OVERHEAD, plain_len = FROM + content_len;
    if (content_len > content_cap || (content == NULL && content_len > 0)) {
        return TERN_EINVAL;
    }
    const uint8_t *nonce = frame + AT_NONCE;
    uint8_t nc[TERN_CCM_NONCE], aad[AAD], tag[TERN_GROUP_TAG];
    uint8_t plain[FROM + TERN_GROUP_MAX_CONTENT];
    aad[0] = frame[0];
    memcpy(aad + 1, nonce, TERN_GROUP_NONCE + TERN_GROUP_TAG);
    ccm_nonce(nonce, nc);

    /* Every group whose tag matches is tried: the first that would be accepted is. What the
     * frame is called when none is depends on how far the best of them got. */
    out->verdict = TERN_GROUP_NOT_OURS;
    for (size_t i = 0; i < count; i++) {
        if (g[i] == NULL) {
            continue;
        }
        gtag(g[i], nonce, tag);
        if (memcmp(tag, frame + AT_TAG, TERN_GROUP_TAG) != 0) {
            continue;
        }
        if (!tern_ccm_open(g[i]->key, nc, aad, sizeof aad, frame + AT_BODY, plain_len,
                           frame + AT_BODY + plain_len, plain)) {
            if (out->verdict == TERN_GROUP_NOT_OURS) {
                out->verdict = TERN_GROUP_FORGED;
            }
            continue;
        }
        uint32_t from = get32(plain);
        if (!an_id(from) || from == self) {
            out->verdict = TERN_GROUP_REFUSED;
        } else if (held(g[i], nonce)) {
            out->verdict = TERN_GROUP_AGAIN;
        } else {
            keep(g[i], nonce);
            if (content_len > 0) {
                memcpy(content, plain + FROM, content_len);
            }
            out->verdict = TERN_GROUP_ACCEPTED;
            out->group = i;
            out->from = from;
            out->len = content_len;
            out->node = frame[0] == TERN_GROUP_HDR_NODE;
            tern_wipe(plain, sizeof plain);
            return TERN_OK;
        }
        tern_wipe(plain, sizeof plain);
    }
    return TERN_OK;
}

size_t tern_group_invite_write(const uint8_t secret[TERN_GROUP_SECRET], const uint8_t *name,
                               size_t name_len, uint8_t out[TERN_GROUP_INVITE_MAX]) {
    if (name_len > TERN_GROUP_NAME_MAX || (name == NULL && name_len > 0) ||
        !tern_companion_utf8(name, name_len)) {
        return 0;
    }
    out[0] = TERN_NODE_INVITE;
    memcpy(out + 1, secret, TERN_GROUP_SECRET);
    if (name_len > 0) {
        memcpy(out + 1 + TERN_GROUP_SECRET, name, name_len);
    }
    return 1 + TERN_GROUP_SECRET + name_len;
}

bool tern_group_invite_read(const uint8_t *plaintext, size_t len, uint8_t secret[TERN_GROUP_SECRET],
                            uint8_t *name, size_t *name_len) {
    if (len < 1 + TERN_GROUP_SECRET || len > TERN_GROUP_INVITE_MAX ||
        plaintext[0] != TERN_NODE_INVITE) {
        return false;
    }
    size_t n = len - 1 - TERN_GROUP_SECRET;
    if (!tern_companion_utf8(plaintext + 1 + TERN_GROUP_SECRET, n)) {
        return false;
    }
    memcpy(secret, plaintext + 1, TERN_GROUP_SECRET);
    if (n > 0) {
        memcpy(name, plaintext + 1 + TERN_GROUP_SECRET, n);
    }
    *name_len = n;
    return true;
}
