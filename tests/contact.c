#include "tern/contact.h"

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "tern/err.h"

/* First contact against the specification's vectors (tests/vectors/first-contact.json). Each role
 * is driven by the vectors' frames, never by the other role's output, so a mistake both sides
 * make the same way cannot hide. */

struct address_case {
    uint8_t seed[32], address[32], x25519_private[32], x25519_public[32];
};
struct rejected_address_case {
    const char *reason;
    uint8_t address[32];
};
struct frame {
    const uint8_t *bytes;
    size_t len;
};
struct handshake_case {
    const char *name;
    uint8_t initiator_seed[32], responder_seed[32], x[32], y[32];
    uint8_t c_i, c_r;
    struct frame frames[4];
    uint8_t secret[32];
};
struct not_for_me_case {
    const char *name;
    uint8_t responder_seed[32];
    const uint8_t *frame;
    size_t len;
};
struct rejected_case {
    const char *name;
    size_t handshake;
    int message;
    const uint8_t *frame;
    size_t len;
};

#include "first_contact.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static bool same(const uint8_t *a, size_t a_len, const struct frame *f) {
    return a_len == f->len && memcmp(a, f->bytes, a_len) == 0;
}

static void addresses_match(void) {
    for (size_t i = 0; i < COUNT(addresses); i++) {
        const struct address_case *c = &addresses[i];
        struct tern_identity id;
        uint8_t k[32], u[32];
        tern_identity_init(&id, c->seed);
        CHECK(memcmp(id.address, c->address, 32) == 0);
        tern_identity_x25519(&id, k);
        CHECK(memcmp(k, c->x25519_private, 32) == 0);
        CHECK(tern_address_valid(c->address));
        CHECK(tern_address_x25519(u, c->address));
        CHECK(memcmp(u, c->x25519_public, 32) == 0);
        tern_identity_wipe(&id);
    }
}

static void rejected_addresses_are_refused(void) {
    for (size_t i = 0; i < COUNT(rejected_addresses); i++) {
        const struct rejected_address_case *c = &rejected_addresses[i];
        struct tern_contact ct;
        uint8_t u[32], frame[TERN_CONTACT_MAX_FRAME];
        size_t len = 0;
        if (tern_address_valid(c->address)) {
            fprintf(stderr, "accepted %s\n", c->reason);
            check_failures++;
        }
        CHECK(!tern_address_x25519(u, c->address));
        CHECK_EQ_I64(
            tern_contact_start(&ct, c->address, handshakes[0].x, 0, frame, sizeof frame, &len),
            TERN_EINVAL);
    }
}

/* The initiator's half of a handshake: send frames[0], answer frames[1] with frames[2], complete
 * on frames[3]. */
static void run_initiator(const struct handshake_case *h, struct tern_identity *me,
                          const uint8_t target[32], struct tern_contact *c) {
    uint8_t out[TERN_CONTACT_MAX_FRAME];
    size_t len = 0;
    CHECK_EQ_I64(tern_contact_start(c, target, h->x, h->c_i, out, sizeof out, &len), TERN_OK);
    CHECK(same(out, len, &h->frames[0]));
    CHECK_EQ_I64(
        tern_contact_receive(c, me, h->frames[1].bytes, h->frames[1].len, out, sizeof out, &len),
        TERN_CONTACT_PROCESSED);
    CHECK(same(out, len, &h->frames[2]));
    CHECK(!tern_contact_complete(c));
    CHECK_EQ_I64(
        tern_contact_receive(c, me, h->frames[3].bytes, h->frames[3].len, out, sizeof out, &len),
        TERN_CONTACT_PROCESSED);
    CHECK_EQ_I64(len, 0);
    CHECK(tern_contact_complete(c));
    CHECK(memcmp(c->secret, h->secret, 32) == 0);
}

/* The responder's half: answer frames[0] with frames[1], and frames[2] with frames[3]. */
static void run_responder(const struct handshake_case *h, struct tern_identity *me,
                          const uint8_t initiator[32], struct tern_contact *c) {
    uint8_t out[TERN_CONTACT_MAX_FRAME];
    size_t len = 0;
    CHECK_EQ_I64(tern_contact_respond(c, me, h->y, h->c_r, h->frames[0].bytes, h->frames[0].len,
                                      out, sizeof out, &len),
                 TERN_CONTACT_PROCESSED);
    CHECK(same(out, len, &h->frames[1]));
    CHECK_EQ_I64(
        tern_contact_receive(c, me, h->frames[2].bytes, h->frames[2].len, out, sizeof out, &len),
        TERN_CONTACT_PROCESSED);
    CHECK(same(out, len, &h->frames[3]));
    CHECK(tern_contact_complete(c));
    CHECK(memcmp(c->secret, h->secret, 32) == 0);
    CHECK(memcmp(c->peer, initiator, 32) == 0);
}

static void handshakes_match(void) {
    for (size_t i = 0; i < COUNT(handshakes); i++) {
        const struct handshake_case *h = &handshakes[i];
        struct tern_identity ini, res;
        struct tern_contact ci, cr;
        tern_identity_init(&ini, h->initiator_seed);
        tern_identity_init(&res, h->responder_seed);
        run_initiator(h, &ini, res.address, &ci);
        run_responder(h, &res, ini.address, &cr);

        /* The two sessions they start talk to each other. */
        struct tern_session si, sr;
        uint8_t peer_of_i[32], peer_of_r[32], frame[64], pt[48];
        struct tern_unicast_received got;
        struct tern_unicast_rx *rx = &sr.rx;
        CHECK_EQ_I64(tern_contact_finish(&ci, &si, peer_of_i), TERN_OK);
        CHECK_EQ_I64(tern_contact_finish(&cr, &sr, peer_of_r), TERN_OK);
        CHECK(memcmp(peer_of_i, res.address, 32) == 0);
        CHECK(memcmp(peer_of_r, ini.address, 32) == 0);
        CHECK_EQ_I64(tern_unicast_seal(&si.tx, (const uint8_t *)"hello", 5, frame, sizeof frame),
                     TERN_OK);
        CHECK_EQ_I64(
            tern_unicast_open(&rx, 1, frame, 5 + TERN_UNICAST_OVERHEAD, pt, sizeof pt, &got),
            TERN_OK);
        CHECK_EQ_I64(got.verdict, TERN_UNICAST_ACCEPTED);
        CHECK(got.len == 5 && memcmp(pt, "hello", 5) == 0);

        /* finish erased the handshakes. */
        static const uint8_t zero[sizeof ci] = {0};
        CHECK(memcmp(&ci, zero, sizeof ci) == 0);
        CHECK(memcmp(&cr, zero, sizeof cr) == 0);
        tern_session_wipe(&si);
        tern_session_wipe(&sr);
        tern_identity_wipe(&ini);
        tern_identity_wipe(&res);
    }
}

static void not_for_me_gets_no_reply(void) {
    for (size_t i = 0; i < COUNT(not_for_me); i++) {
        const struct not_for_me_case *c = &not_for_me[i];
        struct tern_identity me;
        struct tern_contact ct;
        uint8_t out[TERN_CONTACT_MAX_FRAME];
        size_t len = 99;
        tern_identity_init(&me, c->responder_seed);
        CHECK_EQ_I64(tern_contact_respond(&ct, &me, handshakes[0].y, 0x01, c->frame, c->len, out,
                                          sizeof out, &len),
                     TERN_CONTACT_NOT_OURS);
        CHECK_EQ_I64(len, 0);
        CHECK(!tern_contact_complete(&ct));
        tern_identity_wipe(&me);
    }
}

/* Each rejected frame takes the place of one message, with its receiver in the state the
 * handshake leaves it in: the receiver sends nothing, completes nothing, and if the frame's tag
 * was this handshake's, it aborts. */
static void rejected_frames_are_refused(void) {
    for (size_t i = 0; i < COUNT(rejected); i++) {
        const struct rejected_case *r = &rejected[i];
        const struct handshake_case *h = &handshakes[r->handshake];
        struct tern_identity ini, res;
        struct tern_contact c;
        uint8_t out[TERN_CONTACT_MAX_FRAME];
        size_t len = 0;
        int verdict = 0;
        tern_identity_init(&ini, h->initiator_seed);
        tern_identity_init(&res, h->responder_seed);

        switch (r->message) {
        case 1:
            verdict = tern_contact_respond(&c, &res, h->y, h->c_r, r->frame, r->len, out,
                                           sizeof out, &len);
            break;
        case 2:
        case 4:
            CHECK_EQ_I64(tern_contact_start(&c, res.address, h->x, h->c_i, out, sizeof out, &len),
                         TERN_OK);
            if (r->message == 4) {
                CHECK_EQ_I64(tern_contact_receive(&c, &ini, h->frames[1].bytes, h->frames[1].len,
                                                  out, sizeof out, &len),
                             TERN_CONTACT_PROCESSED);
            }
            verdict = tern_contact_receive(&c, &ini, r->frame, r->len, out, sizeof out, &len);
            break;
        case 3:
            CHECK_EQ_I64(tern_contact_respond(&c, &res, h->y, h->c_r, h->frames[0].bytes,
                                              h->frames[0].len, out, sizeof out, &len),
                         TERN_CONTACT_PROCESSED);
            verdict = tern_contact_receive(&c, &res, r->frame, r->len, out, sizeof out, &len);
            break;
        }
        if (verdict == TERN_CONTACT_PROCESSED || len != 0 || tern_contact_complete(&c)) {
            fprintf(stderr, "accepted %s\n", r->name);
            check_failures++;
        }
        if (verdict == TERN_CONTACT_FAILED) {
            static const uint8_t zero[sizeof c] = {0};
            CHECK(memcmp(&c, zero, sizeof c) == 0);
        }
        tern_identity_wipe(&ini);
        tern_identity_wipe(&res);
    }
}

/* A repeated message_2 after message_3 has gone is not processed again, and changes nothing. */
static void repeats_are_not_processed(void) {
    const struct handshake_case *h = &handshakes[0];
    struct tern_identity ini, res;
    struct tern_contact c;
    uint8_t out[TERN_CONTACT_MAX_FRAME];
    size_t len = 0;
    tern_identity_init(&ini, h->initiator_seed);
    tern_identity_init(&res, h->responder_seed);
    CHECK_EQ_I64(tern_contact_start(&c, res.address, h->x, h->c_i, out, sizeof out, &len), TERN_OK);
    CHECK_EQ_I64(
        tern_contact_receive(&c, &ini, h->frames[1].bytes, h->frames[1].len, out, sizeof out, &len),
        TERN_CONTACT_PROCESSED);
    CHECK_EQ_I64(
        tern_contact_receive(&c, &ini, h->frames[1].bytes, h->frames[1].len, out, sizeof out, &len),
        TERN_CONTACT_NOT_OURS);
    CHECK_EQ_I64(
        tern_contact_receive(&c, &ini, h->frames[3].bytes, h->frames[3].len, out, sizeof out, &len),
        TERN_CONTACT_PROCESSED);
    CHECK(tern_contact_complete(&c));
    CHECK(memcmp(c.secret, h->secret, 32) == 0);
    tern_contact_wipe(&c);
    tern_identity_wipe(&ini);
    tern_identity_wipe(&res);
}

static void arguments_are_checked(void) {
    const struct handshake_case *h = &handshakes[0];
    struct tern_identity res;
    struct tern_contact c;
    uint8_t out[TERN_CONTACT_MAX_FRAME];
    size_t len = 0;
    tern_identity_init(&res, h->responder_seed);
    CHECK_EQ_I64(tern_contact_start(&c, res.address, h->x, 0x18, out, sizeof out, &len),
                 TERN_EINVAL); /* 0x18 is not a one-byte CBOR integer */
    CHECK_EQ_I64(tern_contact_start(&c, res.address, h->x, 0x00, out, 44, &len), TERN_EINVAL);
    CHECK_EQ_I64(tern_contact_respond(&c, &res, h->y, 0x38, h->frames[0].bytes, h->frames[0].len,
                                      out, sizeof out, &len),
                 TERN_EINVAL);
    tern_identity_wipe(&res);
}

/* Every length from 0 to 90 bytes, in each state that receives, in a buffer of exactly that size so
 * AddressSanitizer sees any read past the end. The bytes are the start of a real frame, so the
 * header and tag checks pass where the length allows. None is processed except a real frame. */
static void every_length_is_handled(void) {
    const struct handshake_case *h = &handshakes[0];
    struct tern_identity ini, res;
    tern_identity_init(&ini, h->initiator_seed);
    tern_identity_init(&res, h->responder_seed);
    for (int message = 1; message <= 4; message++) {
        const struct frame *real = &h->frames[message - 1];
        for (size_t n = 0; n <= 90; n++) {
            uint8_t *f = malloc(n ? n : 1);
            uint8_t out[TERN_CONTACT_MAX_FRAME];
            struct tern_contact c;
            size_t len = 0;
            int verdict = 0;
            for (size_t i = 0; i < n; i++) {
                f[i] = i < real->len ? real->bytes[i] : 0;
            }
            switch (message) {
            case 1:
                verdict = tern_contact_respond(&c, &res, h->y, h->c_r, f, n, out, sizeof out, &len);
                break;
            case 2:
            case 4:
                (void)tern_contact_start(&c, res.address, h->x, h->c_i, out, sizeof out, &len);
                if (message == 4) {
                    (void)tern_contact_receive(&c, &ini, h->frames[1].bytes, h->frames[1].len, out,
                                               sizeof out, &len);
                }
                verdict = tern_contact_receive(&c, &ini, f, n, out, sizeof out, &len);
                break;
            case 3:
                (void)tern_contact_respond(&c, &res, h->y, h->c_r, h->frames[0].bytes,
                                           h->frames[0].len, out, sizeof out, &len);
                verdict = tern_contact_receive(&c, &res, f, n, out, sizeof out, &len);
                break;
            }
            CHECK((verdict == TERN_CONTACT_PROCESSED) == (n == real->len));
            tern_contact_wipe(&c);
            free(f);
        }
    }
    tern_identity_wipe(&ini);
    tern_identity_wipe(&res);
}

int main(void) {
    RUN(addresses_match);
    RUN(rejected_addresses_are_refused);
    RUN(handshakes_match);
    RUN(not_for_me_gets_no_reply);
    RUN(rejected_frames_are_refused);
    RUN(repeats_are_not_processed);
    RUN(arguments_are_checked);
    RUN(every_length_is_handled);
    return CHECK_DONE();
}
