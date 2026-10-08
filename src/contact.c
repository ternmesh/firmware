#include "tern/contact.h"

#include "tern/err.h"
#include "tern/route.h"

/* First contact, as draft/first-contact.md specifies it, with EDHOC (RFC 9528) fixed to the
 * profile there: method 3, cipher suite 0, credentials holding Ed25519 addresses, the responder
 * named by kid 0 and the initiator by value, one-byte connection identifiers, no EAD. Every
 * message has a fixed length and layout, so each is built and checked byte by byte rather than
 * through a general CBOR codec. Section and step references are to RFC 9528. */

#define HDR(n) (0x50 | (n)) /* format 01, type 010, flags = message number */
#define HEAD 11             /* hdr, and the ten bytes of the routing layer's */
#define AT_DEST 7           /* where the head keeps the destination */
#define PREFIX 15           /* the head and the contact tag: where message_2 to message_4 start */
#define PREFIX_1 19         /* and message_1, after the routing id it came from */
#define M1_LEN 37
#define M2_LEN 45
#define M3_LEN 65
#define M4_LEN 9
#define PT2_LEN 11
#define PT3_LEN 55
#define CRED_LEN 44
#define ID_CRED_I_LEN 46
#define MAC_LEN 8
#define EXPORTER_LABEL 32768u

/* CRED_x is these 12 bytes and the address: {8: {1: {1: 1, -1: 6, -2: A}}}. */
static const uint8_t cred_prefix[12] = {0xa1, 0x08, 0xa1, 0x01, 0xa3, 0x01,
                                        0x01, 0x20, 0x06, 0x21, 0x58, 0x20};
/* ID_CRED_R, {4: h'00'}, as it goes into context_2; in PLAINTEXT_2 it is the one byte 0x00. */
static const uint8_t id_cred_r[4] = {0xa1, 0x04, 0x41, 0x00};
static const uint8_t ctag_label[] = "tern v0 contact"; /* 15 bytes and a terminator not used */

/* A connection identifier sent as one byte: the CBOR encoding of an integer from -24 to 23. */
static bool one_byte_id(uint8_t b) { return b <= 0x17 || (b >= 0x20 && b <= 0x37); }

static void cred(uint8_t out[CRED_LEN], const uint8_t address[TERN_ADDRESS_LEN]) {
    for (int i = 0; i < 12; i++) {
        out[i] = cred_prefix[i];
    }
    for (int i = 0; i < TERN_ADDRESS_LEN; i++) {
        out[12 + i] = address[i];
    }
}

static void sha256_parts(uint8_t out[32], const uint8_t *a, size_t a_len, const uint8_t *b,
                         size_t b_len, const uint8_t *c, size_t c_len) {
    struct tern_sha256 h;
    tern_sha256_init(&h);
    tern_sha256_update(&h, a, a_len);
    tern_sha256_update(&h, b, b_len);
    tern_sha256_update(&h, c, c_len);
    tern_sha256_final(&h, out);
}

/* EDHOC_Extract with SHA-256 is HKDF-Extract: HMAC keyed with the salt. */
static void extract(uint8_t out[32], const uint8_t *salt, size_t salt_len, const uint8_t ikm[32]) {
    struct tern_hmac_sha256 h;
    tern_hmac_sha256_init(&h, salt, salt_len);
    tern_hmac_sha256_update(&h, ikm, 32);
    tern_hmac_sha256_final(&h, out);
}

/* EDHOC_KDF (section 4.1.2): HKDF-Expand with info the CBOR sequence (label, context as a byte
 * string, length). Labels here are 0 to 10 and 32768; contexts are at most 124 bytes and lengths
 * at most 32, so every CBOR head is one or two bytes. */
static void kdf(uint8_t *out, size_t len, const uint8_t prk[32], uint32_t label,
                const uint8_t *context, size_t context_len) {
    uint8_t info[3 + 2 + 124 + 2];
    size_t n = 0;
    if (label < 24) {
        info[n++] = (uint8_t)label;
    } else {
        info[n++] = 0x19;
        info[n++] = (uint8_t)(label >> 8);
        info[n++] = (uint8_t)label;
    }
    if (context_len < 24) {
        info[n++] = (uint8_t)(0x40 | context_len);
    } else {
        info[n++] = 0x58;
        info[n++] = (uint8_t)context_len;
    }
    for (size_t i = 0; i < context_len; i++) {
        info[n++] = context[i];
    }
    if (len < 24) {
        info[n++] = (uint8_t)len;
    } else {
        info[n++] = 0x18;
        info[n++] = (uint8_t)len;
    }
    (void)tern_hkdf_expand(out, len, prk, 32, info, n);
    tern_wipe(info, sizeof info);
}

/* A transcript hash or other 32-byte value as a CBOR byte string: 0x58 0x20 and the bytes. */
static void bstr32(uint8_t out[34], const uint8_t v[32]) {
    out[0] = 0x58;
    out[1] = 0x20;
    for (int i = 0; i < 32; i++) {
        out[2 + i] = v[i];
    }
}

/* The COSE_Encrypt0 associated data (section 5.4.2): ["Encrypt0", h'', TH]. */
static void enc0_aad(uint8_t out[45], const uint8_t th[32]) {
    static const uint8_t head[11] = {0x83, 0x68, 'E', 'n', 'c', 'r', 'y', 'p', 't', '0', 0x40};
    for (int i = 0; i < 11; i++) {
        out[i] = head[i];
    }
    bstr32(out + 11, th);
}

static void contact_tags(struct tern_contact *c, const uint8_t g_x[32], const uint8_t g_rx[32]) {
    uint8_t prk[32], info[16];
    extract(prk, g_x, 32, g_rx);
    for (int i = 0; i < 15; i++) {
        info[i] = ctag_label[i];
    }
    for (uint8_t n = 1; n <= 4; n++) {
        info[15] = n;
        (void)tern_hkdf_expand(c->ctag[n - 1], 4, prk, 32, info, sizeof info);
    }
    tern_wipe(prk, sizeof prk);
}

static uint32_t get32(const uint8_t *b) {
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static void put32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}

static bool an_id(uint32_t id) { return id != 0 && id != TERN_ROUTE_EVERYONE; }

/* The head and the tag of frame n, for the node with that routing id. hops, power and the next
 * hop are left 0: they are the forwarder's. */
static void put_frame(uint8_t *out, uint8_t n, const uint8_t ctag[4], uint32_t destination) {
    out[0] = HDR(n);
    for (int i = 1; i < AT_DEST; i++) {
        out[i] = 0;
    }
    put32(out + AT_DEST, destination);
    for (int i = 0; i < 4; i++) {
        out[HEAD + i] = ctag[i];
    }
}

uint32_t tern_contact_destination(const uint8_t *frame) { return get32(frame + AT_DEST); }

bool tern_contact_same(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len) {
    size_t from = HEAD;
    if (a_len != b_len || a_len < PREFIX || a[0] != b[0]) {
        return false;
    }
    if (a[0] == HDR(1)) {
        if (a_len < PREFIX_1 || !tern_equal_ct(a + HEAD, b + HEAD, PREFIX - HEAD)) {
            return false;
        }
        from = PREFIX_1; /* where it came from is not part of the message */
    }
    return tern_equal_ct(a + from, b + from, a_len - from);
}

/* S = EDHOC_Exporter(32768, h'', 32), from PRK_4e3m and TH_4 (section 4.2.1). */
static void session_secret(uint8_t s[32], const uint8_t prk_4e3m[32], const uint8_t th_4[32]) {
    uint8_t prk_out[32], prk_exporter[32];
    kdf(prk_out, 32, prk_4e3m, 7, th_4, 32);
    kdf(prk_exporter, 32, prk_out, 10, NULL, 0);
    kdf(s, 32, prk_exporter, EXPORTER_LABEL, NULL, 0);
    tern_wipe(prk_out, sizeof prk_out);
    tern_wipe(prk_exporter, sizeof prk_exporter);
}

/* MAC_2 (section 5.3.2): context_2 = C_R, ID_CRED_R, TH_2, CRED_R. */
static void mac_2(uint8_t out[MAC_LEN], const uint8_t prk_3e2m[32], uint8_t c_r,
                  const uint8_t th_2[32], const uint8_t cred_r[CRED_LEN]) {
    uint8_t ctx[1 + 4 + 34 + CRED_LEN];
    ctx[0] = c_r;
    for (int i = 0; i < 4; i++) {
        ctx[1 + i] = id_cred_r[i];
    }
    bstr32(ctx + 5, th_2);
    for (int i = 0; i < CRED_LEN; i++) {
        ctx[39 + i] = cred_r[i];
    }
    kdf(out, MAC_LEN, prk_3e2m, 2, ctx, sizeof ctx);
}

/* MAC_3 (section 5.4.2): context_3 = ID_CRED_I, TH_3, CRED_I, where ID_CRED_I = {14: CRED_I}. */
static void mac_3(uint8_t out[MAC_LEN], const uint8_t prk_4e3m[32], const uint8_t th_3[32],
                  const uint8_t cred_i[CRED_LEN]) {
    uint8_t ctx[ID_CRED_I_LEN + 34 + CRED_LEN];
    ctx[0] = 0xa1;
    ctx[1] = 0x0e;
    for (int i = 0; i < CRED_LEN; i++) {
        ctx[2 + i] = cred_i[i];
        ctx[ID_CRED_I_LEN + 34 + i] = cred_i[i];
    }
    bstr32(ctx + ID_CRED_I_LEN, th_3);
    kdf(out, MAC_LEN, prk_4e3m, 6, ctx, sizeof ctx);
}

/* PRK_2e and TH_2, from G_Y, H(message_1) and G_XY. */
static void prk_2e_of(uint8_t prk_2e[32], uint8_t th_2[32], const uint8_t g_y[32],
                      const uint8_t h_m1[32], const uint8_t g_xy[32]) {
    uint8_t a[34], b[34];
    bstr32(a, g_y);
    bstr32(b, h_m1);
    sha256_parts(th_2, a, 34, b, 34, NULL, 0);
    extract(prk_2e, th_2, 32, g_xy);
}

/* PRK_3e2m = Extract(SALT_3e2m, G_RX), PRK_4e3m = Extract(SALT_4e3m, G_IY). */
static void next_prk(uint8_t out[32], const uint8_t prk[32], uint32_t salt_label,
                     const uint8_t th[32], const uint8_t g[32]) {
    uint8_t salt[32];
    kdf(salt, 32, prk, salt_label, th, 32);
    extract(out, salt, 32, g);
    tern_wipe(salt, sizeof salt);
}

static void abort_contact(struct tern_contact *c) { tern_contact_wipe(c); }

int tern_contact_start(struct tern_contact *c, uint32_t source,
                       const uint8_t target[TERN_ADDRESS_LEN],
                       const uint8_t ephemeral[TERN_CONTACT_EPHEMERAL], uint8_t c_i, uint8_t *frame,
                       size_t frame_cap, size_t *len) {
    uint8_t u_r[32], g_x[32];
    if (frame_cap < PREFIX_1 + M1_LEN || !one_byte_id(c_i) || !an_id(source) ||
        !tern_address_x25519(u_r, target)) {
        return TERN_EINVAL;
    }
    tern_contact_wipe(c);
    for (int i = 0; i < 32; i++) {
        c->ephemeral[i] = ephemeral[i];
        c->peer[i] = target[i];
    }
    tern_x25519_base(g_x, ephemeral);
    if (!tern_x25519(c->g_rx, ephemeral, u_r)) {
        tern_contact_wipe(c);
        return TERN_EINVAL;
    }
    contact_tags(c, g_x, c->g_rx);

    uint8_t *m1 = frame + PREFIX_1;
    put_frame(frame, 1, c->ctag[0], tern_route_id(target));
    put32(frame + PREFIX, source);
    m1[0] = 0x03; /* METHOD 3 */
    m1[1] = 0x00; /* SUITES_I 0 */
    m1[2] = 0x58;
    m1[3] = 0x20;
    for (int i = 0; i < 32; i++) {
        m1[4 + i] = g_x[i];
    }
    m1[36] = c_i;
    sha256_parts(c->h_message_1, m1, M1_LEN, NULL, 0, NULL, 0);

    c->role = TERN_INITIATOR;
    c->expect = 2;
    *len = PREFIX_1 + M1_LEN;
    return TERN_OK;
}

int tern_contact_respond(struct tern_contact *c, const struct tern_identity *me,
                         const uint8_t ephemeral[TERN_CONTACT_EPHEMERAL], uint8_t c_r,
                         const uint8_t *frame, size_t len, uint8_t *out, size_t out_cap,
                         size_t *out_len) {
    if (out_cap < PREFIX + M2_LEN || !one_byte_id(c_r)) {
        return TERN_EINVAL;
    }
    *out_len = 0;
    tern_contact_wipe(c);

    /* message_1 must be exactly the profile's: 03 00 58 20 G_X C_I, nothing after. */
    if (len != PREFIX_1 + M1_LEN || frame[0] != HDR(1)) {
        return TERN_CONTACT_NOT_OURS;
    }
    /* message_2 goes where message_1 says it came from, which must be somewhere. */
    uint32_t source = get32(frame + PREFIX);
    if (!an_id(source) || source == tern_route_id(me->address)) {
        return TERN_CONTACT_NOT_OURS;
    }
    const uint8_t *m1 = frame + PREFIX_1;
    if (m1[0] != 0x03 || m1[1] != 0x00 || m1[2] != 0x58 || m1[3] != 0x20 || !one_byte_id(m1[36])) {
        return TERN_CONTACT_NOT_OURS;
    }
    const uint8_t *g_x = m1 + 4;

    uint8_t k[32], g_rx[32];
    tern_identity_x25519(me, k);
    bool ok = tern_x25519(g_rx, k, g_x);
    tern_wipe(k, sizeof k);
    if (!ok) {
        tern_wipe(g_rx, sizeof g_rx);
        return TERN_CONTACT_NOT_OURS;
    }
    contact_tags(c, g_x, g_rx);
    if (!tern_equal_ct(c->ctag[0], frame + HEAD, 4)) {
        tern_wipe(g_rx, sizeof g_rx);
        tern_contact_wipe(c);
        return TERN_CONTACT_NOT_OURS; /* someone else's first contact */
    }

    uint8_t g_y[32], g_xy[32], h_m1[32], th_2[32], prk_2e[32], cred_r[CRED_LEN];
    uint8_t pt_2[PT2_LEN], ks[PT2_LEN];
    tern_x25519_base(g_y, ephemeral);
    if (!tern_x25519(g_xy, ephemeral, g_x)) {
        tern_wipe(g_rx, sizeof g_rx);
        tern_contact_wipe(c);
        return TERN_CONTACT_FAILED;
    }
    sha256_parts(h_m1, m1, M1_LEN, NULL, 0, NULL, 0);
    prk_2e_of(prk_2e, th_2, g_y, h_m1, g_xy);
    next_prk(c->prk, prk_2e, 1, th_2, g_rx); /* PRK_3e2m */

    /* PLAINTEXT_2 = C_R, ID_CRED_R (the kid 0x00, compact), MAC_2 as an 8-byte string. */
    cred(cred_r, me->address);
    pt_2[0] = c_r;
    pt_2[1] = 0x00;
    pt_2[2] = 0x48;
    mac_2(pt_2 + 3, c->prk, c_r, th_2, cred_r);
    kdf(ks, PT2_LEN, prk_2e, 0, th_2, 32); /* KEYSTREAM_2 */

    uint8_t *m2 = out + PREFIX;
    put_frame(out, 2, c->ctag[1], source);
    m2[0] = 0x58;
    m2[1] = 0x2b;
    for (int i = 0; i < 32; i++) {
        m2[2 + i] = g_y[i];
    }
    for (int i = 0; i < PT2_LEN; i++) {
        m2[34 + i] = pt_2[i] ^ ks[i];
    }

    /* TH_3 = H(TH_2, PLAINTEXT_2, CRED_R). */
    uint8_t th_2_bstr[34];
    bstr32(th_2_bstr, th_2);
    sha256_parts(c->th, th_2_bstr, 34, pt_2, PT2_LEN, cred_r, CRED_LEN);

    for (int i = 0; i < 32; i++) {
        c->ephemeral[i] = ephemeral[i];
    }
    c->role = TERN_RESPONDER;
    c->expect = 3;
    *out_len = PREFIX + M2_LEN;

    tern_wipe(g_rx, sizeof g_rx);
    tern_wipe(g_xy, sizeof g_xy);
    tern_wipe(th_2, sizeof th_2);
    tern_wipe(prk_2e, sizeof prk_2e);
    tern_wipe(pt_2, sizeof pt_2);
    tern_wipe(ks, sizeof ks);
    return TERN_CONTACT_PROCESSED;
}

/* The initiator, given message_2: verify it, send message_3 (section 5.3.3, then 5.4.2). */
static int initiator_message_2(struct tern_contact *c, const struct tern_identity *me,
                               const uint8_t *m2, uint8_t *out) {
    uint8_t g_xy[32], th_2[32], prk_2e[32], prk_3e2m[32], ks[PT2_LEN], pt_2[PT2_LEN];
    uint8_t cred_r[CRED_LEN], cred_i[CRED_LEN], want[MAC_LEN], th_3[32], g_iy[32], k[32];
    int verdict = TERN_CONTACT_FAILED;

    if (m2[0] != 0x58 || m2[1] != 0x2b) {
        goto done;
    }
    const uint8_t *g_y = m2 + 2;
    if (!tern_x25519(g_xy, c->ephemeral, g_y)) {
        goto done;
    }
    prk_2e_of(prk_2e, th_2, g_y, c->h_message_1, g_xy);
    kdf(ks, PT2_LEN, prk_2e, 0, th_2, 32);
    for (int i = 0; i < PT2_LEN; i++) {
        pt_2[i] = m2[34 + i] ^ ks[i];
    }
    if (!one_byte_id(pt_2[0]) || pt_2[1] != 0x00 || pt_2[2] != 0x48) {
        goto done;
    }
    next_prk(prk_3e2m, prk_2e, 1, th_2, c->g_rx);
    cred(cred_r, c->peer);
    mac_2(want, prk_3e2m, pt_2[0], th_2, cred_r);
    if (!tern_equal_ct(want, pt_2 + 3, MAC_LEN)) {
        goto done;
    }

    uint8_t th_2_bstr[34];
    bstr32(th_2_bstr, th_2);
    sha256_parts(th_3, th_2_bstr, 34, pt_2, PT2_LEN, cred_r, CRED_LEN);

    tern_identity_x25519(me, k);
    bool ok = tern_x25519(g_iy, k, g_y);
    tern_wipe(k, sizeof k);
    if (!ok) {
        goto done;
    }
    next_prk(c->prk, prk_3e2m, 5, th_3, g_iy); /* PRK_4e3m */

    /* PLAINTEXT_3 = ID_CRED_I ({14: CRED_I}), MAC_3 as an 8-byte string. */
    uint8_t pt_3[PT3_LEN], k_3[16], iv_3[13], aad[45];
    cred(cred_i, me->address);
    pt_3[0] = 0xa1;
    pt_3[1] = 0x0e;
    for (int i = 0; i < CRED_LEN; i++) {
        pt_3[2 + i] = cred_i[i];
    }
    pt_3[ID_CRED_I_LEN] = 0x48;
    mac_3(pt_3 + ID_CRED_I_LEN + 1, c->prk, th_3, cred_i);

    kdf(k_3, 16, prk_3e2m, 3, th_3, 32);
    kdf(iv_3, 13, prk_3e2m, 4, th_3, 32);
    enc0_aad(aad, th_3);
    uint8_t *m3 = out + PREFIX;
    put_frame(out, 3, c->ctag[2], tern_route_id(c->peer));
    m3[0] = 0x58;
    m3[1] = 0x3f;
    (void)tern_ccm_seal(k_3, iv_3, aad, sizeof aad, pt_3, PT3_LEN, m3 + 2, m3 + 2 + PT3_LEN);

    /* TH_4 = H(TH_3, PLAINTEXT_3, CRED_I). */
    uint8_t th_3_bstr[34];
    bstr32(th_3_bstr, th_3);
    sha256_parts(c->th, th_3_bstr, 34, pt_3, PT3_LEN, cred_i, CRED_LEN);

    tern_wipe(c->ephemeral, sizeof c->ephemeral);
    tern_wipe(c->g_rx, sizeof c->g_rx);
    tern_wipe(c->h_message_1, sizeof c->h_message_1);
    tern_wipe(pt_3, sizeof pt_3);
    tern_wipe(k_3, sizeof k_3);
    tern_wipe(iv_3, sizeof iv_3);
    c->expect = 4;
    verdict = TERN_CONTACT_PROCESSED;

done:
    tern_wipe(g_xy, sizeof g_xy);
    tern_wipe(th_2, sizeof th_2);
    tern_wipe(prk_2e, sizeof prk_2e);
    tern_wipe(prk_3e2m, sizeof prk_3e2m);
    tern_wipe(ks, sizeof ks);
    tern_wipe(pt_2, sizeof pt_2);
    tern_wipe(want, sizeof want);
    tern_wipe(th_3, sizeof th_3);
    tern_wipe(g_iy, sizeof g_iy);
    return verdict;
}

/* The responder, given message_3: verify it, send message_4, derive S (sections 5.4.3, 5.5.2). */
static int responder_message_3(struct tern_contact *c, const uint8_t *m3, uint8_t *out) {
    uint8_t k_3[16], iv_3[13], aad[45], pt_3[PT3_LEN], u_i[32], g_iy[32], prk_4e3m[32];
    uint8_t want[MAC_LEN], cred_i[CRED_LEN], th_4[32];
    int verdict = TERN_CONTACT_FAILED;

    if (m3[0] != 0x58 || m3[1] != 0x3f) {
        goto done;
    }
    kdf(k_3, 16, c->prk, 3, c->th, 32);
    kdf(iv_3, 13, c->prk, 4, c->th, 32);
    enc0_aad(aad, c->th);
    if (!tern_ccm_open(k_3, iv_3, aad, sizeof aad, m3 + 2, PT3_LEN, m3 + 2 + PT3_LEN, pt_3)) {
        goto done;
    }

    /* PLAINTEXT_3 must be exactly {14: CRED_I} and an 8-byte MAC, CRED_I holding a valid
     * address. */
    bool shape = pt_3[0] == 0xa1 && pt_3[1] == 0x0e && pt_3[ID_CRED_I_LEN] == 0x48;
    for (int i = 0; i < 12; i++) {
        shape = shape && pt_3[2 + i] == cred_prefix[i];
    }
    const uint8_t *a_i = pt_3 + 14;
    if (!shape || !tern_address_x25519(u_i, a_i)) {
        goto done;
    }
    if (!tern_x25519(g_iy, c->ephemeral, u_i)) {
        goto done;
    }
    next_prk(prk_4e3m, c->prk, 5, c->th, g_iy);
    cred(cred_i, a_i);
    mac_3(want, prk_4e3m, c->th, cred_i);
    if (!tern_equal_ct(want, pt_3 + ID_CRED_I_LEN + 1, MAC_LEN)) {
        goto done;
    }

    uint8_t th_3_bstr[34], k_4[16], iv_4[13];
    bstr32(th_3_bstr, c->th);
    sha256_parts(th_4, th_3_bstr, 34, pt_3, PT3_LEN, cred_i, CRED_LEN);
    kdf(k_4, 16, prk_4e3m, 8, th_4, 32);
    kdf(iv_4, 13, prk_4e3m, 9, th_4, 32);
    enc0_aad(aad, th_4);
    uint8_t *m4 = out + PREFIX;
    put_frame(out, 4, c->ctag[3], tern_route_id(a_i));
    m4[0] = 0x48;
    (void)tern_ccm_seal(k_4, iv_4, aad, sizeof aad, NULL, 0, NULL, m4 + 1);

    for (int i = 0; i < TERN_ADDRESS_LEN; i++) {
        c->peer[i] = a_i[i];
    }
    session_secret(c->secret, prk_4e3m, th_4);
    tern_wipe(c->ephemeral, sizeof c->ephemeral);
    tern_wipe(c->prk, sizeof c->prk);
    tern_wipe(c->th, sizeof c->th);
    tern_wipe(k_4, sizeof k_4);
    tern_wipe(iv_4, sizeof iv_4);
    c->expect = 0;
    c->complete = true;
    verdict = TERN_CONTACT_PROCESSED;

done:
    tern_wipe(k_3, sizeof k_3);
    tern_wipe(iv_3, sizeof iv_3);
    tern_wipe(pt_3, sizeof pt_3);
    tern_wipe(g_iy, sizeof g_iy);
    tern_wipe(prk_4e3m, sizeof prk_4e3m);
    tern_wipe(want, sizeof want);
    tern_wipe(th_4, sizeof th_4);
    return verdict;
}

/* The initiator, given message_4: verify it and derive S (section 5.5.3). */
static int initiator_message_4(struct tern_contact *c, const uint8_t *m4) {
    uint8_t k_4[16], iv_4[13], aad[45];
    int verdict = TERN_CONTACT_FAILED;
    if (m4[0] != 0x48) {
        return verdict;
    }
    kdf(k_4, 16, c->prk, 8, c->th, 32);
    kdf(iv_4, 13, c->prk, 9, c->th, 32);
    enc0_aad(aad, c->th);
    if (tern_ccm_open(k_4, iv_4, aad, sizeof aad, NULL, 0, m4 + 1, NULL)) {
        session_secret(c->secret, c->prk, c->th);
        tern_wipe(c->prk, sizeof c->prk);
        tern_wipe(c->th, sizeof c->th);
        c->expect = 0;
        c->complete = true;
        verdict = TERN_CONTACT_PROCESSED;
    }
    tern_wipe(k_4, sizeof k_4);
    tern_wipe(iv_4, sizeof iv_4);
    return verdict;
}

int tern_contact_receive(struct tern_contact *c, const struct tern_identity *me,
                         const uint8_t *frame, size_t len, uint8_t *out, size_t out_cap,
                         size_t *out_len) {
    static const size_t frame_len[5] = {0, PREFIX_1 + M1_LEN, PREFIX + M2_LEN, PREFIX + M3_LEN,
                                        PREFIX + M4_LEN};
    if (out_cap < TERN_CONTACT_MAX_FRAME) {
        return TERN_EINVAL;
    }
    *out_len = 0;

    /* Only the message this side waits for, with its contact tag, is this handshake's. Anything
     * else, a repeat of an earlier message included, is not processed. */
    uint8_t n = c->expect;
    bool mine = n >= 2 && n <= 4 && len >= PREFIX && frame[0] == HDR(n) &&
                tern_equal_ct(c->ctag[n - 1], frame + HEAD, 4);
    if (!mine) {
        return TERN_CONTACT_NOT_OURS;
    }

    int verdict = TERN_CONTACT_FAILED;
    if (len == frame_len[n]) {
        if (n == 2 && c->role == TERN_INITIATOR) {
            verdict = initiator_message_2(c, me, frame + PREFIX, out);
            *out_len = verdict == TERN_CONTACT_PROCESSED ? PREFIX + M3_LEN : 0;
        } else if (n == 3 && c->role == TERN_RESPONDER) {
            verdict = responder_message_3(c, frame + PREFIX, out);
            *out_len = verdict == TERN_CONTACT_PROCESSED ? PREFIX + M4_LEN : 0;
        } else if (n == 4 && c->role == TERN_INITIATOR) {
            verdict = initiator_message_4(c, frame + PREFIX);
        }
    }
    if (verdict != TERN_CONTACT_PROCESSED) {
        abort_contact(c);
    }
    return verdict;
}

bool tern_contact_complete(const struct tern_contact *c) { return c->complete; }

int tern_contact_finish(struct tern_contact *c, struct tern_session *s,
                        uint8_t peer[TERN_ADDRESS_LEN]) {
    if (!c->complete) {
        return TERN_EINVAL;
    }
    for (int i = 0; i < TERN_ADDRESS_LEN; i++) {
        peer[i] = c->peer[i];
    }
    tern_session_init(s, c->secret, (enum tern_role)c->role); /* erases c->secret */
    tern_contact_wipe(c);
    return TERN_OK;
}

void tern_contact_wipe(struct tern_contact *c) { tern_wipe(c, sizeof *c); }
