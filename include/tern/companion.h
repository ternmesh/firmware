#ifndef TERN_COMPANION_H
#define TERN_COMPANION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tern/address.h"
#include "tern/time.h"

/* The companion protocol: version 7 of draft/companion.md in ternmesh/spec.
 *
 * The link between a node and the client driving it, a phone or a computer, over USB serial, TCP
 * or Bluetooth LE. It never goes over LoRa. This is the part every node and every client needs:
 * a frame's fields, read and written, and how frames are found in a byte stream shared with a
 * text console. What a node answers and when is the node's business, not the core's (in the node
 * every port shares, ports/node/link.c). */

#define TERN_COMPANION_VERSION 7
#define TERN_COMPANION_MAX_FRAME 180
#define TERN_COMPANION_STREAM_MAX (TERN_COMPANION_MAX_FRAME + 6) /* magic, length, CRC */
#define TERN_COMPANION_NAME_MAX 31
#define TERN_COMPANION_TEXT_MAX 128
#define TERN_COMPANION_FIRMWARE_MAX 31
#define TERN_COMPANION_REGION_MAX 15
#define TERN_COMPANION_BOARD_MAX 31
#define TERN_COMPANION_RELEASE_MAX 31
#define TERN_COMPANION_UPDATE_CHUNK 172 /* the longest data an UPDATE_DATA carries */
#define TERN_COMPANION_LINK_MAX 102     /* a join code's link: tern/group.h's TERN_GROUP_LINK_MAX */
#define TERN_COMPANION_DIGEST 32        /* a SHA-256 */
#define TERN_COMPANION_GROUP 8          /* a group's id */
#define TERN_COMPANION_GAP TERN_MS(500) /* a partial frame idle this long is not a frame */

enum tern_companion_type {
    /* Requests, client to node. */
    TERN_C_HELLO = 0x01,
    TERN_C_SYNC = 0x02,
    TERN_C_PING = 0x03,
    TERN_C_SET_TIME = 0x04,
    TERN_C_SET = 0x05,
    TERN_C_SEND = 0x10,
    TERN_C_READ = 0x11,
    TERN_C_SAVE_CONTACT = 0x18,
    TERN_C_REMOVE_CONTACT = 0x19,
    TERN_C_END_SESSION = 0x1A, /* version 1 */
    TERN_C_MAKE_GROUP = 0x20,  /* version 2, as are the five after it */
    TERN_C_LEAVE_GROUP = 0x21,
    TERN_C_NAME_GROUP = 0x22,
    TERN_C_SEND_GROUP = 0x23,
    TERN_C_SEND_INVITE = 0x24,
    TERN_C_JOIN = 0x25,
    TERN_C_GROUP_LINK = 0x26, /* version 7, as are the one after it and LINK */
    TERN_C_JOIN_LINK = 0x27,
    TERN_C_UPDATE_BEGIN = 0x30, /* version 4, as are the two after it */
    TERN_C_UPDATE_DATA = 0x31,
    TERN_C_UPDATE_END = 0x32,
    TERN_C_SET_POSITION = 0x33, /* version 5, as are the two after it */
    TERN_C_SHARE = 0x34,
    TERN_C_SHARE_GROUP = 0x35,
    /* Answers, node to client. */
    TERN_C_OK = 0x40,
    TERN_C_ERROR = 0x41,
    TERN_C_INFO = 0x42,   /* its board and release, version 4 */
    TERN_C_SYNCED = 0x43, /* its news count, version 3 */
    TERN_C_QUEUED = 0x44,
    TERN_C_MADE = 0x45,     /* version 2 */
    TERN_C_UPDATING = 0x46, /* version 4 */
    TERN_C_LINK = 0x47,     /* version 7 */
    /* News, node to client. */
    TERN_C_SELF = 0x80,
    TERN_C_CONTACT = 0x81,
    TERN_C_CONTACT_GONE = 0x82,
    TERN_C_MESSAGE = 0x83,
    TERN_C_STATE = 0x84,
    TERN_C_NEIGHBOUR = 0x85,
    TERN_C_NEIGHBOUR_GONE = 0x86,
    TERN_C_AIRTIME = 0x87,
    TERN_C_POWER = 0x88,
    TERN_C_ASKED = 0x89, /* version 1 */
    TERN_C_GROUP = 0x8A, /* version 2, as are the three after it */
    TERN_C_GROUP_GONE = 0x8B,
    TERN_C_GROUP_MESSAGE = 0x8C,
    TERN_C_INVITE = 0x8D,
    TERN_C_POSITION = 0x8E, /* version 5, as are the three after it */
    TERN_C_GROUP_POSITION = 0x8F,
    TERN_C_SHARING = 0x90,
    TERN_C_GROUP_SHARING = 0x91,
    TERN_C_CARD = 0x92, /* version 6, as is the one after it */
    TERN_C_CARD_GONE = 0x93,
};

/* The first version of the protocol that defines a type: a node sends a client no frame its
 * version lacks, and answers a request its version lacks as one it does not know. */
uint8_t tern_companion_since(uint8_t type);

/* Which range a type is in. */
bool tern_companion_request(uint8_t type);
bool tern_companion_news(uint8_t type);

enum tern_companion_setting {
    TERN_C_SET_REGION = 1,    /* text */
    TERN_C_SET_ROLE = 2,      /* role */
    TERN_C_SET_POWER = 3,     /* power */
    TERN_C_SET_PASSKEY = 4,   /* passkey */
    TERN_C_SET_CARDS = 5,     /* cards: version 6, as is the one after it */
    TERN_C_SET_CARD_NAME = 6, /* text */
};

/* The first version of the protocol that defines a setting of SET's. */
uint8_t tern_companion_setting_since(uint8_t setting);

enum tern_companion_error {
    TERN_C_ERR_UNKNOWN = 1,
    TERN_C_ERR_MALFORMED = 2,
    TERN_C_ERR_REFUSED = 3,
    TERN_C_ERR_ADDRESS = 4,
    TERN_C_ERR_FULL = 5,
    TERN_C_ERR_HELLO_FIRST = 6,
    TERN_C_ERR_MTU = 7,
    TERN_C_ERR_NOT_NOW = 8,
    TERN_C_ERR_NOT_HELD = 9,      /* version 2 */
    TERN_C_ERR_NOT_THERE = 10,    /* version 4: no update under way, or not at that offset */
    TERN_C_ERR_NOT_AN_IMAGE = 11, /* version 4: the update is discarded */
    TERN_C_ERR_NOT_CONTACT = 12,  /* version 5: an address the node does not hold as a contact */
};

enum tern_companion_state {
    TERN_C_WAITING = 0,
    TERN_C_SENT = 1,
    TERN_C_DELIVERED = 2,
    TERN_C_NOT_DELIVERED = 3,
    TERN_C_RECEIVED = 4,
};

enum tern_companion_reason {
    TERN_C_WAIT_UNNAMED = 0,
    TERN_C_WAIT_ROUTE = 1,
    TERN_C_WAIT_SESSION = 2,
    TERN_C_WAIT_REGION = 3,
    TERN_C_WAIT_BUDGET = 4,
    TERN_C_WAIT_RADIO = 5,
};

/* ASKED's why: what first contact was refused for. */
enum tern_companion_why {
    TERN_C_ASKED_NOT_CONTACT = 1,
    TERN_C_ASKED_NO_ROOM = 2,
};

#define TERN_C_READ_FLAG 0x01      /* MESSAGE flags: a received message has been read */
#define TERN_C_CHARGING 0x01       /* POWER flags */
#define TERN_C_EXTERNAL_POWER 0x02 /* POWER flags */
#define TERN_C_SHARE_ALTITUDE 0x01 /* SHARE's and SHARING's fields */
#define TERN_C_SHARE_ACCURACY 0x02
#define TERN_C_NO_ALTITUDE (-32768) /* a position's altitude, when there is none */

/* Any frame, as its fields. Each type uses the members its table in the draft names, under the
 * same names (SYNCED's news is `news`), with three folded together: the one string a frame carries
 * (text, name, firmware, region, link, or SET's region or card name) is `text`; the one address
 * (to, address, contact) is `address`; and `wait` is MESSAGE's and STATE's u16 or AIRTIME's u32, as
 * `heard` is NEIGHBOUR's u16 or CARD's u32. INFO's board and release, SELF's card_name, and
 * UPDATE_DATA's data, have members of their own. A position's `accuracy` and `age` are one
 * member each, though SET_POSITION's are wider than the news'. SET's value is `text`,
 * `role`, `power`, `passkey` or `cards` as `setting` says. Members a type does not use are ignored
 * when writing and left as they were when reading. */
struct tern_companion_msg {
    uint8_t type, seq;
    uint8_t version, setting, code, role, session, flags, state, reason, percent, why, news, cards;
    int8_t power, snr;
    uint16_t millivolts;
    uint32_t heard;
    uint32_t after, time, ref, through, id, routing_id, period, allowed, used, wait, passkey;
    uint32_t from, size, offset;
    int32_t lat, lon;          /* positions' and SET_POSITION's, in 10^-7 degree */
    int16_t altitude;          /* metres, or TERN_C_NO_ALTITUDE */
    uint16_t accuracy;         /* metres, 0 for none */
    uint32_t age;              /* seconds */
    uint8_t precision, fields; /* fields: SHARE's and SHARING's */
    uint16_t interval, minutes;
    uint8_t address[TERN_ADDRESS_LEN];
    uint8_t group[TERN_COMPANION_GROUP];
    uint8_t digest[TERN_COMPANION_DIGEST];
    uint8_t text_len;
    uint8_t text[TERN_COMPANION_TEXT_MAX];
    uint8_t board_len, release_len; /* INFO's */
    uint8_t board[TERN_COMPANION_BOARD_MAX];
    uint8_t release[TERN_COMPANION_RELEASE_MAX];
    uint8_t card_name_len; /* SELF's */
    uint8_t card_name[TERN_COMPANION_NAME_MAX];
    uint8_t data_len; /* UPDATE_DATA's */
    uint8_t data[TERN_COMPANION_UPDATE_CHUNK];
};

enum tern_companion_read {
    TERN_C_READ_OK = 0,
    TERN_C_READ_UNKNOWN = TERN_C_ERR_UNKNOWN,     /* a type, or a setting, this version lacks */
    TERN_C_READ_MALFORMED = TERN_C_ERR_MALFORMED, /* cut short, a string too long or not UTF-8 */
    TERN_C_READ_SHORT = 3,                        /* under two bytes: nothing to answer */
};

/* A group's id, from its secret (tern/group.h): what a client knows the group by. */
void tern_companion_group_id(const uint8_t secret[16], uint8_t id[TERN_COMPANION_GROUP]);

/* Reads a frame's fields as this version has them. Bytes after the last field this version
 * defines are ignored. */
enum tern_companion_read tern_companion_read(struct tern_companion_msg *m, const uint8_t *frame,
                                             size_t len);

/* Reads a frame as `version` has it, the version both ends speak: a type a later version added
 * is unknown, however its fields read, and a field a later version added is not there. A node of
 * version 2's SYNCED is two bytes. */
enum tern_companion_read tern_companion_read_as(struct tern_companion_msg *m, const uint8_t *frame,
                                                size_t len, uint8_t version);

/* Writes a frame from its fields into out (TERN_COMPANION_MAX_FRAME bytes), as this version has
 * it. Returns its length, or 0 if the type is not one this version defines or a string is too
 * long for its field. The strings are not checked for UTF-8: a writer is trusted to give text. */
size_t tern_companion_write(const struct tern_companion_msg *m, uint8_t *out);

/* Writes a frame as `version` has it: without the fields a later version added, and not at all
 * (0) if a later version added the type. */
size_t tern_companion_write_as(const struct tern_companion_msg *m, uint8_t *out, uint8_t version);

/* Whether len bytes are UTF-8 with no overlong form, surrogate, or code point past U+10FFFF. */
bool tern_companion_utf8(const uint8_t *text, size_t len);

/* The longest prefix of text, at most max bytes, that ends on a whole character and is UTF-8:
 * what a node keeps of a received message to give a client. */
size_t tern_companion_utf8_prefix(const uint8_t *text, size_t len, size_t max);

/* --- Byte streams --------------------------------------------------------------------------- */

/* CRC-16/IBM-3740: polynomial 0x1021, initial value 0xFFFF, no reflection, no final XOR. */
uint16_t tern_companion_crc(const uint8_t *data, size_t len);

/* Wraps a frame of 2 to TERN_COMPANION_MAX_FRAME bytes for a byte stream: magic, length, the
 * frame, CRC. out holds TERN_COMPANION_STREAM_MAX bytes. Returns the length, or 0 if the frame's
 * length is out of range. */
size_t tern_companion_wrap(const uint8_t *frame, size_t len, uint8_t *out);

/* What a stream holds besides frames is text: the console's, both ways. */
struct tern_companion_sink {
    void *ctx;
    void (*frame)(void *ctx, const uint8_t *frame, size_t len);
    void (*text)(void *ctx, uint8_t byte);
};

/* Finds frames in a byte stream. Bytes are pushed as they arrive; each reaches the sink as part
 * of a frame or as text, in order, as soon as it can be told which. */
struct tern_companion_parser {
    uint8_t buf[TERN_COMPANION_STREAM_MAX];
    size_t len;
    tern_time last; /* when the last byte arrived */
};

void tern_companion_parser_init(struct tern_companion_parser *p);
void tern_companion_push(struct tern_companion_parser *p, tern_time now, uint8_t byte,
                         const struct tern_companion_sink *sink);
/* Call it now and then: a partial frame that has had nothing for TERN_COMPANION_GAP is not a
 * frame, and its first byte is given up as text. */
void tern_companion_idle(struct tern_companion_parser *p, tern_time now,
                         const struct tern_companion_sink *sink);

#endif
