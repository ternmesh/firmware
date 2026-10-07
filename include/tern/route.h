#ifndef TERN_ROUTE_H
#define TERN_ROUTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tern/address.h"
#include "tern/lora.h"
#include "tern/time.h"

/* Routes: specification draft 0, draft/routing.md in ternmesh/spec.
 *
 * How a node learns which neighbour to hand a frame to for each node it can reach. Nodes announce
 * themselves, the neighbours they hear and the routes they have selected; a node judges each link
 * by how strongly its two ends hear each other, selects routes under Babel's feasibility condition
 * (RFC 8966), so that they never loop while they change, and asks for a newer sequence number when
 * that condition leaves it with none. Trickle (RFC 6206) times the announces, under a cap on the
 * share of a node's time they may take.
 *
 * This is the simulator's candidate 3 (ternmesh/sim, src/distvec.c) as its defaults leave it, and
 * as far as choosing routes: the frames that follow them, broadcast, electing relays and leaves
 * that move are not here yet. Every parameter is the simulator's default, and provisional.
 *
 * The router owns no clock, radio or memory. The caller gives it tables to keep its neighbours and
 * destinations in, tells it the time, hands it the routing frames the radio receives, and asks it
 * for frames to send:
 *
 *     tern_route_heard(&r, now, frame, len, snr);          for each routing frame received
 *     while ((len = tern_route_poll(&r, now, frame, &dbm)) != 0) { send it at dbm }
 *     ... tern_route_sent(&r, now) once each has gone on the air ...
 *     sleep until tern_route_due(&r), or a frame arrives
 *
 * A node that restarts has lost what Babel's condition rests on, so a router starts by saying so
 * and selecting nothing through a neighbour until it has: the specification's "Starting". Nothing
 * here is authenticated yet: the specification leaves that open. */

#define TERN_ROUTE_EVERYONE 0xFFFFFFFFu /* in a request: every neighbour */
#define TERN_ROUTE_INF 0xFFFFu          /* a metric that retracts a route */
#define TERN_ROUTE_FRAME_MAX 255
#define TERN_ROUTE_KEPT 4 /* routes kept for each destination */

#define TERN_HDR_ANNOUNCE 0x59
#define TERN_HDR_REQUEST 0x5A

#define TERN_ANNOUNCE_HEAD 17
#define TERN_ANNOUNCE_NAMED_MAX 47  /* neighbours an announce can hold */
#define TERN_ANNOUNCE_ROUTES_MAX 29 /* routes an announce can hold */
#define TERN_REQUEST_HEAD 6
#define TERN_REQUEST_MAX 35 /* requests a frame can hold */

/* Whether a frame is one of this layer's, by its first byte. */
static inline bool tern_route_frame(const uint8_t *frame, size_t len) {
    return len > 0 && (frame[0] == TERN_HDR_ANNOUNCE || frame[0] == TERN_HDR_REQUEST);
}

/* --- Rules, each as the specification states it. The router is built from these, and the tests
 * check them against the specification's vectors. --- */

/* A node's routing id, from its address. */
uint32_t tern_route_id(const uint8_t address[TERN_ADDRESS_LEN]);

/* Whether sequence number a is newer than b, modulo 2^16. */
bool tern_route_newer(uint16_t a, uint16_t b);

/* A promise of so many seconds as it goes in an announce, rounded up; and what a code promises,
 * or TERN_ROUTE_NO_PROMISE. */
#define TERN_ROUTE_NO_PROMISE (INT64_MAX / 4)
uint16_t tern_route_promise_code(uint64_t seconds);
tern_time tern_route_promise_time(uint16_t code);

struct tern_announce_named {
    uint32_t id;
    uint8_t margin;
};

struct tern_announce_route {
    uint32_t destination;
    uint16_t seq;
    uint16_t metric;
};

struct tern_announce {
    uint32_t sender;
    uint16_t number;
    uint16_t seq;
    bool relay;
    bool starting;
    uint16_t promise; /* as it goes on the air */
    uint16_t round;
    int8_t power;
    uint8_t named_count;
    uint8_t route_count;
    struct tern_announce_named named[TERN_ANNOUNCE_NAMED_MAX];
    struct tern_announce_route routes[TERN_ANNOUNCE_ROUTES_MAX];
};

struct tern_request_ask {
    uint32_t destination;
    uint16_t seq;
    uint8_t hops;
};

struct tern_request {
    uint32_t next;
    uint8_t count;
    struct tern_request_ask asks[TERN_REQUEST_MAX];
};

/* Writes a frame, returning its length, or 0 if it would not fit in a frame. */
size_t tern_announce_write(const struct tern_announce *a, uint8_t frame[TERN_ROUTE_FRAME_MAX]);
size_t tern_request_write(const struct tern_request *q, uint8_t frame[TERN_ROUTE_FRAME_MAX]);

/* Reads a frame. Returns false for one the specification says to discard, but for an announce
 * from the reader's own id, which only the reader can know. */
bool tern_announce_read(struct tern_announce *a, const uint8_t *frame, size_t len);
bool tern_request_read(struct tern_request *q, const uint8_t *frame, size_t len);

/* A neighbour's floor, in sixteenths of a dBm, after an announce it sent at `power` dBm is heard
 * with a signal-to-noise ratio of `snr_q` quarters of a decibel at spreading factor `sf`. `floor`
 * is the floor before, and is not read if `first`. */
int32_t tern_route_floor(bool first, int32_t floor, int8_t power, int16_t snr_q, uint8_t sf);

/* The margin a node whose full power is `full` dBm gives a neighbour with that floor. */
uint8_t tern_route_margin(int8_t full, int32_t floor);

/* Whether a link is up, having been `up` before: `own` is this node's full power less its floor
 * for the neighbour, in sixteenths of a decibel, and `theirs` the margin the neighbour gave, or 0
 * for none. */
bool tern_route_link_up(bool up, int32_t own, uint8_t theirs);

/* Whether the margin a neighbour gave in its announce numbered `named` is withdrawn by the time
 * it sends `number`, naming every neighbour once in `round` announces. */
bool tern_route_withdrawn(uint16_t named, uint16_t number, uint16_t round);

/* What a node does with an announce numbered `number` from a neighbour whose last was `last`:
 * takes it, discards it as a copy or late, or forgets the neighbour and takes it as found again,
 * because it has started again and lost what it announced. `promise_passed` is whether nothing was
 * heard from the neighbour for one of its promises, `starting` whether the announce says its
 * sender is starting, and `was_starting` whether the neighbour's last did. */
enum tern_route_numbering { TERN_ROUTE_TAKE, TERN_ROUTE_DISCARD, TERN_ROUTE_AGAIN };
enum tern_route_numbering tern_route_numbering(uint16_t last, uint16_t number, bool promise_passed,
                                               bool starting, bool was_starting);

/* What a link that is up costs: the reference frame's time on air in milliseconds, rounded up. */
uint16_t tern_route_link_cost(const struct tern_lora *lora);

/* A feasibility distance: the best (seq, metric) a node has announced for a destination. */
struct tern_route_fd {
    bool has;
    uint16_t seq;
    uint16_t metric;
};

/* A route as a neighbour announced it, and what the link to that neighbour costs: TERN_ROUTE_INF
 * if the node may not use the neighbour. A metric of TERN_ROUTE_INF is no route. */
struct tern_route_choice {
    uint16_t seq;
    uint16_t metric;
    uint16_t cost;
};

bool tern_route_feasible(const struct tern_route_fd *fd, uint16_t seq, uint16_t metric);

/* Which of `n` routes a node selects, having selected `selected` (-1 for none): an index, or -1. */
int tern_route_select(const struct tern_route_fd *fd, const struct tern_route_choice *routes, int n,
                      int selected);

/* Which of the TERN_ROUTE_KEPT routes a node holds, `selected` among them, a route offered takes
 * the place of: an index, or -1 if it is worth no more than any. */
int tern_route_replace(const struct tern_route_fd *fd, const struct tern_route_choice *routes,
                       int selected, const struct tern_route_choice *offered);

/* --- The router --- */

struct tern_route_config {
    struct tern_lora lora; /* the profile's modulation */
    int8_t full_dbm;       /* the most the node sends at */
    int8_t min_dbm;        /* and the least */
    bool relay;

    tern_time i_min;   /* Trickle's shortest interval */
    uint8_t doublings; /* its longest is i_min times 2^doublings */
    uint8_t redundancy;
    uint8_t quiet_max;
    uint32_t announce_ppm; /* the cap: millionths of the node's time for announces */
    uint32_t request_ppm;  /* and for requests */
    tern_time cap_window;
    uint8_t burst;
    uint8_t named_max; /* neighbours named in a frame */
    tern_time silent_max;
    uint8_t power_k;
    uint8_t power_margin_db;
    tern_time request_interval;
    uint8_t request_tries;
    uint8_t hop_max;
    uint8_t step_db;         /* more for a neighbour, for each frame to it given up on */
    uint8_t dead_hops;       /* frames given up on running, unheard between, that forget one */
    uint8_t jitter;          /* airtimes a request waits, at most */
    uint8_t start_announces; /* announces a node is starting for */
};

/* The specification's parameters. */
struct tern_route_config tern_route_defaults(const struct tern_lora *lora, int8_t full_dbm,
                                             int8_t min_dbm, bool relay);

/* The tables below are the caller's to allocate and the router's to fill. Their fields are for
 * the router, and for showing a person what it knows. With more nodes to hear than it has places
 * for neighbours, the router keeps every link that is up and otherwise the nearest. */

struct tern_route_neighbour {
    uint32_t id;
    bool used;
    bool relay;
    bool owed;     /* found since the last announce: named before the rest */
    bool starting; /* its last announce said so */
    bool up;
    uint8_t theirs;    /* the margin it gave this node, or 0 for none */
    uint16_t number;   /* its last announce's */
    uint16_t named;    /* the announce it last named this node in */
    int32_t floor;     /* sixteenths of a dBm */
    uint8_t lost;      /* frames sent to it and given up on since it was last heard */
    uint8_t boost;     /* decibels more a frame to it goes at, for those it has lost */
    tern_time heard;   /* when it was last heard */
    tern_time promise; /* how soon it said it would announce again */
};

/* Which neighbour of a full table of `n` a node just heard takes the place of, `floor` being what
 * its one frame gives: the one with the highest floor of those whose link is not up, if the node
 * heard is 6 dB nearer; an index, or -1. A link that is up is never given up for one that might
 * come up. */
int tern_route_place(const struct tern_route_neighbour *full, size_t n, int32_t floor);

struct tern_route_entry {
    uint8_t slot; /* the neighbour's place in the table plus 1; 0 for none */
    uint16_t seq;
    uint16_t metric; /* the neighbour's, without the link */
};

struct tern_route_dest {
    uint32_t id;
    bool used;
    struct tern_route_entry e[TERN_ROUTE_KEPT];
    uint8_t sel; /* the selected route's slot, or 0 */
    struct tern_route_fd fd;
    bool advertised;  /* a route to it has been announced, and not retracted since */
    bool urgent;      /* changed, and waiting to be announced */
    bool listed;      /* in the frame being built */
    uint8_t retracts; /* times its retraction is still to go */
    uint8_t tries;    /* requests sent while starved; 0 when not */
    uint16_t adv_seq; /* what was last announced */
    uint16_t adv_metric;
    uint16_t asked_seq;
    tern_time asked; /* when a request about it was last sent or passed on, or -1 */
};

struct tern_route_bucket {
    int64_t have; /* nanoseconds of airtime, in millionths */
    int64_t most;
    uint32_t ppm;
    tern_time at;
};

#define TERN_ROUTE_ASKS 16 /* requests waiting to go */

struct tern_route {
    struct tern_route_config config;
    uint32_t id;
    uint16_t seq;     /* of this node's route to itself */
    uint16_t number;  /* of its next announce */
    uint16_t cost;    /* a link's */
    uint8_t starting; /* announces it has still to send before it selects routes through others */

    struct tern_route_neighbour *nb;
    size_t nb_cap;
    struct tern_route_dest *dest;
    size_t dest_cap;

    size_t named_cursor;
    size_t urgent_cursor;
    size_t route_cursor;
    uint32_t urgent_count;
    uint32_t selected;
    uint32_t retracting;
    bool changed; /* something this node announces changed: a Trickle inconsistency */
    bool asked;   /* a request for this node's seq waits on its next announce */

    tern_time interval; /* Trickle */
    tern_time interval_end;
    tern_time fire_at;
    bool fired;
    uint8_t heard;
    uint8_t quiet;

    struct tern_route_bucket announces; /* the cap, in two */
    struct tern_route_bucket requests;
    bool announcing; /* an announce is due, when its bucket can pay */
    uint8_t burst;   /* frames sent of it so far */
    tern_time announce_at;

    struct tern_route_request_waiting {
        uint32_t next;
        uint32_t destination;
        uint16_t seq;
        uint8_t hops;
    } asks[TERN_ROUTE_ASKS];
    size_t ask_count;
    tern_time ask_at;     /* when they go, or -1 */
    tern_time request_at; /* when starved destinations are asked about again, or -1 */
    uint32_t starving;
    tern_time house_at;

    bool out;           /* the last frame polled was an announce not yet reported sent */
    tern_time out_at;   /* when it was built */
    tern_time mac_wait; /* the longest its announces have lately waited to go */
    tern_time promised; /* in its last announce */
    uint64_t rng;
    tern_time now; /* as the call in progress was told */
};

/* Starts a router for the node with routing id `id`, at `now`. `seq` is the sequence number it
 * last used, if it kept one, and any value if not: its neighbours will tell it. `seed` is for the
 * random times Trickle and jitter need, and need not be secret. The tables must outlive the router;
 * neither may hold more than 255 neighbours. */
void tern_route_init(struct tern_route *r, const struct tern_route_config *config, uint32_t id,
                     struct tern_route_neighbour *neighbours, size_t neighbour_cap,
                     struct tern_route_dest *dests, size_t dest_cap, uint16_t seq, uint64_t seed,
                     tern_time now);

/* A routing frame was received, with this signal-to-noise ratio in quarters of a decibel. Frames
 * that are not this layer's, or that the specification says to discard, are ignored. */
void tern_route_heard(struct tern_route *r, tern_time now, const uint8_t *frame, size_t len,
                      int16_t snr_q);

/* When tern_route_poll() next has something to do. */
tern_time tern_route_due(const struct tern_route *r);

/* Does what is due at `now`. Returns the length of a frame to send, written to `frame`, and the
 * power to send it at, or 0 when there is nothing more to send for now. */
size_t tern_route_poll(struct tern_route *r, tern_time now, uint8_t frame[TERN_ROUTE_FRAME_MAX],
                       int8_t *power_dbm);

/* The frame tern_route_poll() last returned has gone on the air. */
void tern_route_sent(struct tern_route *r, tern_time now);

/* The neighbour to hand a frame for `destination` to, and the route's metric: false if the node
 * has no route. */
bool tern_route_next(const struct tern_route *r, uint32_t destination, uint32_t *next,
                     uint16_t *metric);

/* --- For the frames that follow routes (tern/forward.h) --- */

/* What a frame for one neighbour goes at; a frame for one this node does not keep goes as a frame
 * for every neighbour does. */
int8_t tern_route_power(const struct tern_route *r, uint32_t neighbour);

/* What a frame must go at to be heard by the node a frame came from, sent at `power` dBm and heard
 * at `snr_q`: its floor by that one frame, and the margin. */
int8_t tern_route_power_back(const struct tern_route *r, int8_t power, int16_t snr_q);

/* Another neighbour to hand a frame for `destination` to, having given up on the `tried` ones:
 * the best feasible route through one that is none of them, so that the frame cannot come back.
 * False if there is none. */
bool tern_route_other(const struct tern_route *r, uint32_t destination, const uint32_t *tried,
                      int tried_count, uint32_t *next);

/* A frame sent to a neighbour was given up on: the next goes louder, and after dead_hops of them
 * with nothing heard from it between, it is forgotten, and every route through it. */
void tern_route_lost(struct tern_route *r, tern_time now, uint32_t neighbour);

/* A neighbour was heard passing on a frame sent to it, which it sent at `power` dBm and was heard
 * at `snr_q`: it is there, and how well it is heard is known as from an announce. */
void tern_route_passed(struct tern_route *r, tern_time now, uint32_t neighbour, int8_t power,
                       int16_t snr_q);

/* There is a frame for `destination` and no route: asks the neighbours for one, unless it asked
 * within request_interval. */
void tern_route_want(struct tern_route *r, tern_time now, uint32_t destination);

#endif
