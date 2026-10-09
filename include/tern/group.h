#ifndef TERN_GROUP_H
#define TERN_GROUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tern/crypto.h"

/* Groups: specification draft 0, draft/groups.md in ternmesh/spec.
 *
 * A group is the nodes that hold one 16-byte secret. A member seals what it has to say under the
 * group's key with a random nonce, and the frame is flooded (tern/flood.h). Its four clear bytes
 * of tag are a function of the nonce that only members can work out, so a member tells its
 * groups' frames from the rest with one AES block for each group, and an observer cannot tell
 * one group's frames from another's.
 *
 * Every member holds the same key. So a member can write as any other, whoever comes to hold the
 * secret can read all that was ever sent under it, and nobody can be removed: the specification
 * says so first, and so should whatever shows a group to a person.
 *
 * A frame starts with the flood's three bytes. Only the first is this layer's: hops and power are
 * written as zeros here and never read.
 *
 * The core has no source of randomness, so the nonce is the caller's to draw, from the generator
 * it makes keys with. */

#define TERN_GROUP_HDR 0x60
#define TERN_GROUP_HDR_NODE 0x61 /* the flag `node` set: content is for the node, not words */
#define TERN_GROUP_SECRET 16
#define TERN_GROUP_NONCE 8
#define TERN_GROUP_TAG 4
#define TERN_GROUP_OVERHEAD 27 /* the flood's head, the nonce, the tag, from and the AEAD tag */
#define TERN_GROUP_MAX_FRAME 255
#define TERN_GROUP_MAX_CONTENT (TERN_GROUP_MAX_FRAME - TERN_GROUP_OVERHEAD)
#define TERN_GROUP_RECENT 64 /* nonces kept, so that a frame is accepted once */

struct tern_group {
    uint8_t secret[TERN_GROUP_SECRET]; /* G: kept, since a member hands it on in an invite */
    uint8_t key[TERN_AES128_KEY];      /* GK */
    struct tern_aes128 tag_key;        /* GT, expanded */
    uint8_t recent[TERN_GROUP_RECENT][TERN_GROUP_NONCE];
    uint8_t count; /* nonces held */
    uint8_t next;  /* where the next goes: the oldest's place, once full */
};

/* Takes a group from its secret, with no nonces held. */
void tern_group_init(struct tern_group *g, const uint8_t secret[TERN_GROUP_SECRET]);

/* Leaves it: erases the secret, the keys and the nonces. */
void tern_group_wipe(struct tern_group *g);

/* Seals len bytes of content (at most TERN_GROUP_MAX_CONTENT) from the node whose routing id is
 * `from` into a frame of len + 27 bytes, under `nonce`, which the caller has drawn at random for
 * this frame alone. content and frame must not overlap. Returns TERN_OK, or TERN_EINVAL if len is
 * too long, frame_cap too small or `from` a reserved id. */
int tern_group_seal(const struct tern_group *g, const uint8_t nonce[TERN_GROUP_NONCE],
                    uint32_t from, const uint8_t *content, size_t len, uint8_t *frame,
                    size_t frame_cap);

/* The same, with the flag `node` set: content is for the members' nodes, and its first byte says
 * what it is, as a unicast message for the node's does (TERN_POSITION_KIND, tern/position.h). */
int tern_group_seal_node(const struct tern_group *g, const uint8_t nonce[TERN_GROUP_NONCE],
                         uint32_t from, const uint8_t *content, size_t len, uint8_t *frame,
                         size_t frame_cap);

/* What became of a received frame. */
enum tern_group_verdict {
    TERN_GROUP_ACCEPTED = 1,
    TERN_GROUP_NOT_OURS,  /* its tag is no held group's: someone else's frame, not an error */
    TERN_GROUP_MALFORMED, /* too short, too long, or not a group frame */
    TERN_GROUP_FORGED,    /* its tag matched, but no matching group's key authenticated it */
    TERN_GROUP_REFUSED,   /* sealed rightly, and from nobody, everybody or this node itself */
    TERN_GROUP_AGAIN,     /* sealed rightly, under a nonce already accepted for the group */
};

struct tern_group_received {
    enum tern_group_verdict verdict;
    /* ACCEPTED only. */
    size_t group;  /* index into the array of groups given */
    uint32_t from; /* the routing id the frame says wrote it: a member's claim, not a proof */
    size_t len;    /* content bytes written */
    bool node;     /* the flag `node` was set: content is for the node, never words to show */
};

/* Receives a frame on behalf of every group the node holds; a NULL entry is a place with no
 * group in it. Every group whose tag matches is tried, so a tag two groups share never loses a
 * frame. On acceptance the content (len - 27 bytes) is written to content and the nonce is kept.
 * `self` is this node's routing id. content must not overlap frame.
 *
 * Returns TERN_OK with out filled in, or TERN_EINVAL if content_cap cannot hold the content of a
 * frame of this length. */
int tern_group_open(struct tern_group *const *g, size_t count, uint32_t self, const uint8_t *frame,
                    size_t len, uint8_t *content, size_t content_cap,
                    struct tern_group_received *out);

/* --- Invites: the plaintext of a unicast message for the node (tern_unicast_seal_node()) --- */

#define TERN_NODE_INVITE 0x01 /* the kind such a plaintext begins with */
#define TERN_GROUP_NAME_MAX 31
#define TERN_GROUP_INVITE_MAX (1 + TERN_GROUP_SECRET + TERN_GROUP_NAME_MAX)

/* Writes an invite to a group, with what the inviter calls it: UTF-8, at most
 * TERN_GROUP_NAME_MAX bytes. Returns its length, or 0 if the name is too long or not UTF-8. */
size_t tern_group_invite_write(const uint8_t secret[TERN_GROUP_SECRET], const uint8_t *name,
                               size_t name_len, uint8_t out[TERN_GROUP_INVITE_MAX]);

/* Reads one. False for a plaintext that is not an invite, or one the specification says to
 * ignore. `name` holds TERN_GROUP_NAME_MAX bytes. */
bool tern_group_invite_read(const uint8_t *plaintext, size_t len, uint8_t secret[TERN_GROUP_SECRET],
                            uint8_t *name, size_t *name_len);

#endif
