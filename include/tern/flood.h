#ifndef TERN_FLOOD_H
#define TERN_FLOOD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tern/route.h"
#include "tern/time.h"

/* Frames for every node: specification draft 0, draft/flooding.md in ternmesh/spec.
 *
 * A flooded frame names no node. It goes from the node that wrote it to every node within a few
 * relays: each relay that hears it for the first time sends it once more, after a wait, unless it
 * hears another copy first. A node knows a frame it has had by a hash of everything after the
 * three bytes that change on the way.
 *
 * A node gives floods a bounded share of its time, in two parts: one for its own and one for
 * those it passes on.
 *
 * Like the forwarder (tern/forward.h) it owns no clock, radio or memory, and what a frame holds
 * past its head is the caller's: for a group's frame, tern/group.h.
 *
 *     if (tern_flood_heard(&f, now, frame, len)) { new: hand it to tern_group_open() }
 *     while ((len = tern_flood_poll(&f, now, frame, &dbm, &kind, &h)) != 0) { queue it }
 *     ... tern_flood_sent(&f, now, h) once each has gone on the air ...
 *     sleep until tern_flood_due(&f), or a frame arrives
 */

#define TERN_HDR_GROUP 0x60 /* a group's frame */

#define TERN_FLOOD_HEAD 3 /* hdr, hops and power */
#define TERN_FLOOD_ID 8
#define TERN_FLOOD_FRAME_MAX 255
#define TERN_FLOOD_GROUP_MIN 27 /* a group frame with nothing to say */
#define TERN_FLOOD_SEEN 128     /* frames a node must be able to hold as seen */
#define TERN_FLOOD_SLOTS_MAX 255
#define TERN_FLOOD_FLOOR_UNKNOWN INT32_MAX

/* Whether a frame is one of this layer's: by its first byte, and long enough to be one. */
static inline bool tern_flood_frame(const uint8_t *frame, size_t len) {
    return len >= TERN_FLOOD_GROUP_MIN && len <= TERN_FLOOD_FRAME_MAX && frame[0] == TERN_HDR_GROUP;
}

/* --- Rules, each as the specification states it, checked against its vectors. --- */

/* A frame's id: the same for every copy of it, wherever it was heard. */
void tern_flood_id(const uint8_t *frame, size_t len, uint8_t id[TERN_FLOOD_ID]);

/* The hops a node that had not seen a frame sends it on with, or -1 for not at all. `relay` is
 * whether the node is one, `neighbours` how many relay neighbours it has, `hops` what the frame
 * said, `most` the hops a flood starts with and `sparse` the relay neighbours, at most, of a
 * relay that spends no hop. */
int tern_flood_passes(bool relay, unsigned neighbours, uint8_t hops, uint8_t most, uint8_t sparse);

/* What a flooded frame goes at, in dBm: `every` is what a frame for every neighbour goes at,
 * `floors` the floors, in sixteenths of a dBm, of the relay neighbours the node's selected routes
 * go through, TERN_FLOOD_FLOOR_UNKNOWN for one not known. */
int8_t tern_flood_power(int8_t every, const int32_t *floors, size_t n, uint8_t margin_db,
                        int8_t lowest, int8_t full);

/* A token bucket, as the router's cap is: nanoseconds of airtime, in millionths. */
struct tern_flood_bucket {
    int64_t have;
    int64_t most;
    uint32_t ppm;
    tern_time at;
};

/* A bucket that fills at `ppm` millionths of the node's time and holds `window` of that, or
 * `frame`, the airtime of the longest frame, if that is more. It starts full. */
void tern_flood_bucket_init(struct tern_flood_bucket *b, uint32_t ppm, tern_time window,
                            tern_time frame, tern_time now);

/* Whether the bucket holds `airtime` at `now`; if it does, that much is taken. */
bool tern_flood_bucket_pays(struct tern_flood_bucket *b, tern_time now, tern_time airtime);

/* --- The flooder --- */

struct tern_flood_config {
    uint8_t hops;   /* what a flood starts with */
    uint8_t sparse; /* relay neighbours, at most, of a relay that spends no hop */
    uint8_t wait;   /* airtimes a relay waits before passing a frame on, at most */
    uint8_t copies; /* copies heard, the first included, that drop a frame still waiting */
    uint32_t own_ppm;
    tern_time own_window;
    uint32_t relay_ppm;
    tern_time relay_window;
};

/* The specification's parameters. */
struct tern_flood_config tern_flood_defaults(void);

enum tern_flood_state {
    TERN_FLOOD_FREE,
    TERN_FLOOD_WAITING, /* to be sent at `at` */
    TERN_FLOOD_OUT,     /* with the caller, not yet on the air */
};

/* One frame in hand. The caller allocates these; their fields are the flooder's. */
struct tern_flood_slot {
    uint8_t state;
    bool own;       /* this node's, not one passed on */
    bool dropped;   /* while out: enough copies were heard, and it is no longer wanted */
    uint8_t copies; /* heard so far, the first included */
    uint8_t len;
    int8_t power;
    tern_time at;
    uint8_t id[TERN_FLOOD_ID];
    uint8_t frame[TERN_FLOOD_FRAME_MAX];
};

struct tern_flood_counts {
    uint32_t heard;     /* frames heard for the first time */
    uint32_t copies;    /* and copies of frames already seen */
    uint32_t passed_on; /* frames taken to pass on */
    uint32_t cancelled; /* of them, dropped on hearing enough copies */
    uint32_t unpaid;    /* and dropped because the allowance could not pay */
    uint32_t hop_limit; /* frames that had come as far as they may */
    uint32_t no_room;   /* and those there was no slot for */
};

struct tern_flood {
    struct tern_flood_config config;
    struct tern_route *route;
    struct tern_flood_slot *slot;
    size_t cap;
    uint8_t (*seen)[TERN_FLOOD_ID]; /* a ring: the last seen_cap ids taken */
    size_t seen_cap;
    size_t seen_count;
    size_t seen_next;
    struct tern_flood_bucket own;
    struct tern_flood_bucket relay;
    uint64_t rng;
    struct tern_flood_counts counts;
};

/* Starts a flooder over a router. `seen` holds the ids of frames had: the specification asks for
 * room for TERN_FLOOD_SEEN, and an id is held until that many more have come, however long that
 * takes. `seed` is for the waits and need not be secret. Neither table need be cleared. */
void tern_flood_init(struct tern_flood *f, const struct tern_flood_config *config,
                     struct tern_route *route, struct tern_flood_slot *slots, size_t cap,
                     uint8_t (*seen)[TERN_FLOOD_ID], size_t seen_cap, uint64_t seed, tern_time now);

/* Floods a frame of this node's. `frame` is whole but for hops and power, which are filled in
 * here. It goes when the allowance for the node's own frames can pay for it. False if it is not a
 * frame of this layer's, or there is no slot for it. */
bool tern_flood_send(struct tern_flood *f, tern_time now, const uint8_t *frame, size_t len);

/* A frame of this layer's was received. True if the node had not seen it: the caller then hands
 * it on to whatever opens its kind. A relay passes it on by itself. */
bool tern_flood_heard(struct tern_flood *f, tern_time now, const uint8_t *frame, size_t len);

/* When tern_flood_poll() next has something to do, or INT64_MAX. */
tern_time tern_flood_due(const struct tern_flood *f);

enum tern_flood_kind {
    TERN_FLOOD_OWN,   /* this node's */
    TERN_FLOOD_RELAY, /* another's, passed on */
};

/* Does what is due at `now`. Returns the length of a frame to send, written to `frame`, the power
 * to send it at, what it is and a handle to name it by, or 0 when there is nothing to send for
 * now. Every frame returned is the caller's until it says what became of it, with
 * tern_flood_sent() or tern_flood_withdrawn(). */
size_t tern_flood_poll(struct tern_flood *f, tern_time now, uint8_t frame[TERN_FLOOD_FRAME_MAX],
                       int8_t *power_dbm, enum tern_flood_kind *kind, uint8_t *handle);

/* Whether a frame the caller still holds is to be sent: enough copies may have been heard since.
 * A caller that finds it is not says so with tern_flood_withdrawn() in place of
 * tern_flood_sent(), and what the frame was charged goes back. */
bool tern_flood_wanted(const struct tern_flood *f, uint8_t handle);
void tern_flood_withdrawn(struct tern_flood *f, tern_time now, uint8_t handle);

/* A frame tern_flood_poll() returned has gone on the air. */
void tern_flood_sent(struct tern_flood *f, tern_time now, uint8_t handle);

#endif
