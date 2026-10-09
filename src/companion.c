#include "tern/companion.h"

#include <string.h>

#include "tern/crypto.h"

#define MAGIC0 0xF5
#define MAGIC1 0x54
#define FIELDS_MAX 10

/* Each frame is a list of fields, read and written in order. A field is a member of the message
 * and the kind of bytes it is on the wire. */
enum field {
    END = 0,
    VERSION,   /* u8 */
    SETTING,   /* u8, and then the setting's value */
    CODE,      /* u8 */
    ROLE,      /* u8 */
    SESSION,   /* u8 */
    FLAGS,     /* u8 */
    STATE,     /* u8 */
    REASON,    /* u8 */
    PERCENT,   /* u8 */
    WHY,       /* u8 */
    NEWS,      /* u8 */
    PRECISION, /* u8 */
    SHARED,    /* u8: SHARE's and SHARING's fields */
    ACC8,      /* u8, into accuracy */
    POWER,     /* i8 */
    SNR,       /* i8 */
    HEARD,     /* u16 */
    MV,        /* u16 */
    WAIT16,    /* u16, into wait */
    ALTITUDE,  /* i16 */
    ACC16,     /* u16, into accuracy */
    AGE16,     /* u16, into age */
    INTERVAL,  /* u16 */
    MINUTES,   /* u16 */
    AFTER,     /* u32 */
    TIME,      /* u32 */
    REF,       /* u32 */
    THROUGH,   /* u32 */
    ID,        /* u32 */
    RID,       /* u32 */
    PERIOD,    /* u32 */
    ALLOWED,   /* u32 */
    USED,      /* u32 */
    WAIT32,    /* u32, into wait */
    PASSKEY,   /* u32 */
    FROM,      /* u32 */
    SIZE,      /* u32 */
    OFFSET,    /* u32 */
    LAT,       /* i32 */
    LON,       /* i32 */
    AGE32,     /* u32, into age */
    ADDRESS,   /* 32 bytes */
    GROUP,     /* 8 bytes: a group's id */
    DIGEST,    /* 32 bytes: a SHA-256 */
    TEXT,      /* a string of up to TERN_COMPANION_TEXT_MAX */
    NAME,      /* a string of up to TERN_COMPANION_NAME_MAX, into text */
    FIRMWARE,  /* a string of up to TERN_COMPANION_FIRMWARE_MAX, into text */
    REGION,    /* a string of up to TERN_COMPANION_REGION_MAX, into text */
    BOARD,     /* a string of up to TERN_COMPANION_BOARD_MAX, into board */
    RELEASE,   /* a string of up to TERN_COMPANION_RELEASE_MAX, into release */
    DATA,      /* up to TERN_COMPANION_UPDATE_CHUNK bytes of anything, into data */
};

struct layout {
    uint8_t type;
    uint8_t fields[FIELDS_MAX];
};

static const struct layout layouts[] = {
    {TERN_C_HELLO, {VERSION}},
    {TERN_C_SYNC, {AFTER}},
    {TERN_C_PING, {END}},
    {TERN_C_SET_TIME, {TIME}},
    {TERN_C_SET, {SETTING}},
    {TERN_C_SEND, {REF, ADDRESS, TEXT}},
    {TERN_C_READ, {THROUGH}},
    {TERN_C_SAVE_CONTACT, {ADDRESS, NAME}},
    {TERN_C_REMOVE_CONTACT, {ADDRESS}},
    {TERN_C_END_SESSION, {ADDRESS}},
    {TERN_C_MAKE_GROUP, {NAME}},
    {TERN_C_LEAVE_GROUP, {GROUP}},
    {TERN_C_NAME_GROUP, {GROUP, NAME}},
    {TERN_C_SEND_GROUP, {REF, GROUP, TEXT}},
    {TERN_C_SEND_INVITE, {GROUP, ADDRESS}},
    {TERN_C_JOIN, {ID}},
    {TERN_C_UPDATE_BEGIN, {SIZE, DIGEST}},
    {TERN_C_UPDATE_DATA, {OFFSET, DATA}},
    {TERN_C_UPDATE_END, {END}},
    {TERN_C_SET_POSITION, {LAT, LON, ALTITUDE, ACC16, AGE16}},
    {TERN_C_SHARE, {ADDRESS, PRECISION, SHARED, INTERVAL, MINUTES}},
    {TERN_C_SHARE_GROUP, {GROUP, PRECISION, SHARED, INTERVAL, MINUTES}},
    {TERN_C_OK, {END}},
    {TERN_C_ERROR, {CODE}},
    {TERN_C_INFO, {VERSION, FIRMWARE, BOARD, RELEASE}},
    {TERN_C_SYNCED, {NEWS}},
    {TERN_C_QUEUED, {ID}},
    {TERN_C_MADE, {GROUP}},
    {TERN_C_UPDATING, {OFFSET}},
    {TERN_C_SELF, {ADDRESS, ROLE, REGION, POWER, TIME}},
    {TERN_C_CONTACT, {ADDRESS, SESSION, NAME}},
    {TERN_C_CONTACT_GONE, {ADDRESS}},
    {TERN_C_MESSAGE, {ID, ADDRESS, TIME, FLAGS, STATE, REASON, WAIT16, TEXT}},
    {TERN_C_STATE, {ID, STATE, REASON, WAIT16}},
    {TERN_C_NEIGHBOUR, {RID, ROLE, SNR, HEARD}},
    {TERN_C_NEIGHBOUR_GONE, {RID}},
    {TERN_C_AIRTIME, {PERIOD, ALLOWED, USED, WAIT32}},
    {TERN_C_POWER, {MV, PERCENT, FLAGS}},
    {TERN_C_ASKED, {ADDRESS, WHY}},
    {TERN_C_GROUP, {GROUP, NAME}},
    {TERN_C_GROUP_GONE, {GROUP}},
    {TERN_C_GROUP_MESSAGE, {ID, GROUP, FROM, TIME, FLAGS, STATE, REASON, WAIT16, TEXT}},
    {TERN_C_INVITE, {ID, ADDRESS, GROUP, TIME, FLAGS, STATE, REASON, WAIT16, NAME}},
    {TERN_C_POSITION, {ADDRESS, PRECISION, LAT, LON, ALTITUDE, ACC8, AGE32}},
    {TERN_C_GROUP_POSITION, {GROUP, FROM, PRECISION, LAT, LON, ALTITUDE, ACC8, AGE32}},
    {TERN_C_SHARING, {ADDRESS, PRECISION, SHARED, INTERVAL, MINUTES}},
    {TERN_C_GROUP_SHARING, {GROUP, PRECISION, SHARED, INTERVAL, MINUTES}},
};

/* A later version adds fields only at the end of a frame: from field `from` on, a type's fields
 * are `since`'s, and an earlier version's frame stops before them. */
static const struct {
    uint8_t type, from, since;
} later_fields[] = {
    {TERN_C_SYNCED, 0, 3},
    {TERN_C_INFO, 2, 4},
};

static bool added_later(uint8_t type, size_t i, uint8_t version) {
    for (size_t k = 0; k < sizeof later_fields / sizeof later_fields[0]; k++) {
        if (later_fields[k].type == type && i >= later_fields[k].from &&
            version < later_fields[k].since) {
            return true;
        }
    }
    return false;
}

/* SET's value, by setting. */
static enum field setting_value(uint8_t setting) {
    switch (setting) {
    case TERN_C_SET_REGION:
        return REGION;
    case TERN_C_SET_ROLE:
        return ROLE;
    case TERN_C_SET_POWER:
        return POWER;
    case TERN_C_SET_PASSKEY:
        return PASSKEY;
    default:
        return END;
    }
}

static const struct layout *layout_of(uint8_t type) {
    for (size_t i = 0; i < sizeof layouts / sizeof layouts[0]; i++) {
        if (layouts[i].type == type) {
            return &layouts[i];
        }
    }
    return NULL;
}

uint8_t tern_companion_since(uint8_t type) {
    if ((type >= TERN_C_SET_POSITION && type <= TERN_C_SHARE_GROUP) ||
        (type >= TERN_C_POSITION && type <= TERN_C_GROUP_SHARING)) {
        return 5;
    }
    if ((type >= TERN_C_UPDATE_BEGIN && type <= TERN_C_UPDATE_END) || type == TERN_C_UPDATING) {
        return 4;
    }
    if ((type >= TERN_C_MAKE_GROUP && type <= TERN_C_JOIN) || type == TERN_C_MADE ||
        (type >= TERN_C_GROUP && type <= TERN_C_INVITE)) {
        return 2;
    }
    return type == TERN_C_END_SESSION || type == TERN_C_ASKED ? 1 : 0;
}

void tern_companion_group_id(const uint8_t secret[16], uint8_t id[TERN_COMPANION_GROUP]) {
    static const uint8_t info[] = "tern v0 group id";
    (void)tern_hkdf_expand(id, TERN_COMPANION_GROUP, secret, 16, info, sizeof info - 1);
}

bool tern_companion_request(uint8_t type) { return type >= 0x01 && type <= 0x3F; }

bool tern_companion_news(uint8_t type) { return type >= 0x80 && type <= 0xBF; }

static size_t width(enum field f) {
    if (f >= ADDRESS) {
        return f == ADDRESS  ? TERN_ADDRESS_LEN
               : f == GROUP  ? TERN_COMPANION_GROUP
               : f == DIGEST ? TERN_COMPANION_DIGEST
                             : 0; /* strings and data vary */
    }
    if (f >= AFTER) {
        return 4;
    }
    return f >= HEARD ? 2 : 1;
}

static size_t string_max(enum field f) {
    switch (f) {
    case TEXT:
        return TERN_COMPANION_TEXT_MAX;
    case NAME:
        return TERN_COMPANION_NAME_MAX;
    case FIRMWARE:
        return TERN_COMPANION_FIRMWARE_MAX;
    case BOARD:
        return TERN_COMPANION_BOARD_MAX;
    case RELEASE:
        return TERN_COMPANION_RELEASE_MAX;
    case DATA:
        return TERN_COMPANION_UPDATE_CHUNK;
    default:
        return TERN_COMPANION_REGION_MAX;
    }
}

/* Where a fixed-width field lives in the message, as a pointer to its integer. */
static void *member(struct tern_companion_msg *m, enum field f) {
    switch (f) {
    case VERSION:
        return &m->version;
    case SETTING:
        return &m->setting;
    case CODE:
        return &m->code;
    case ROLE:
        return &m->role;
    case SESSION:
        return &m->session;
    case FLAGS:
        return &m->flags;
    case STATE:
        return &m->state;
    case REASON:
        return &m->reason;
    case PERCENT:
        return &m->percent;
    case WHY:
        return &m->why;
    case NEWS:
        return &m->news;
    case PRECISION:
        return &m->precision;
    case SHARED:
        return &m->fields;
    case ALTITUDE:
        return &m->altitude;
    case ACC8:
    case ACC16:
        return &m->accuracy;
    case INTERVAL:
        return &m->interval;
    case MINUTES:
        return &m->minutes;
    case LAT:
        return &m->lat;
    case LON:
        return &m->lon;
    case AGE16:
    case AGE32:
        return &m->age;
    case POWER:
        return &m->power;
    case SNR:
        return &m->snr;
    case HEARD:
        return &m->heard;
    case MV:
        return &m->millivolts;
    case AFTER:
        return &m->after;
    case TIME:
        return &m->time;
    case REF:
        return &m->ref;
    case THROUGH:
        return &m->through;
    case ID:
        return &m->id;
    case RID:
        return &m->routing_id;
    case PERIOD:
        return &m->period;
    case ALLOWED:
        return &m->allowed;
    case USED:
        return &m->used;
    case WAIT16:
    case WAIT32:
        return &m->wait;
    case PASSKEY:
        return &m->passkey;
    case FROM:
        return &m->from;
    case SIZE:
        return &m->size;
    case OFFSET:
        return &m->offset;
    default:
        return NULL;
    }
}

/* Where a field of fixed bytes lives in the message. */
static uint8_t *bytes_of(struct tern_companion_msg *m, enum field f) {
    return f == ADDRESS ? m->address : f == GROUP ? m->group : m->digest;
}

/* Where a field of varying length lives in the message, and its length. */
static uint8_t *variable_of(struct tern_companion_msg *m, enum field f, uint8_t **len) {
    switch (f) {
    case BOARD:
        *len = &m->board_len;
        return m->board;
    case RELEASE:
        *len = &m->release_len;
        return m->release;
    case DATA:
        *len = &m->data_len;
        return m->data;
    default:
        *len = &m->text_len;
        return m->text;
    }
}

static uint32_t get_member(const struct tern_companion_msg *m, enum field f) {
    const void *p = member((struct tern_companion_msg *)m, f);
    if (f == WAIT16) {
        return (uint16_t)m->wait;
    }
    if (f == AGE16) {
        return (uint16_t)m->age;
    }
    if (f == ACC8) {
        return (uint8_t)m->accuracy;
    }
    switch (width(f)) {
    case 1:
        return f == POWER || f == SNR ? (uint8_t) * (const int8_t *)p : *(const uint8_t *)p;
    case 2:
        return *(const uint16_t *)p;
    default:
        return *(const uint32_t *)p;
    }
}

static void set_member(struct tern_companion_msg *m, enum field f, uint32_t v) {
    void *p = member(m, f);
    if (f == WAIT16) {
        m->wait = v;
        return;
    }
    if (f == AGE16) {
        m->age = v;
        return;
    }
    if (f == ACC8) {
        m->accuracy = (uint16_t)v;
        return;
    }
    switch (width(f)) {
    case 1:
        if (f == POWER || f == SNR) {
            *(int8_t *)p = (int8_t)(uint8_t)v;
        } else {
            *(uint8_t *)p = (uint8_t)v;
        }
        break;
    case 2:
        *(uint16_t *)p = (uint16_t)v;
        break;
    default:
        *(uint32_t *)p = v;
        break;
    }
}

/* The next field of a frame as `version` has it: from its layout, then, after SET's setting, its
 * value. */
static enum field next_field(const struct layout *l, size_t i, const struct tern_companion_msg *m,
                             uint8_t version) {
    if (added_later(l->type, i, version)) {
        return END;
    }
    if (i < FIELDS_MAX && l->fields[i] != END) {
        return (enum field)l->fields[i];
    }
    /* SET's layout is its setting alone: the field after it is that setting's value. */
    if (l->type == TERN_C_SET && i == 1) {
        return setting_value(m->setting);
    }
    return END;
}

enum tern_companion_read tern_companion_read(struct tern_companion_msg *m, const uint8_t *frame,
                                             size_t len) {
    return tern_companion_read_as(m, frame, len, TERN_COMPANION_VERSION);
}

enum tern_companion_read tern_companion_read_as(struct tern_companion_msg *m, const uint8_t *frame,
                                                size_t len, uint8_t version) {
    if (len < 2) {
        return TERN_C_READ_SHORT;
    }
    if (len > TERN_COMPANION_MAX_FRAME) {
        return TERN_C_READ_MALFORMED;
    }
    const struct layout *l = layout_of(frame[0]);
    /* A type the version spoken does not have is unknown before its fields are looked at. */
    if (l == NULL || tern_companion_since(frame[0]) > version) {
        return TERN_C_READ_UNKNOWN;
    }
    m->type = frame[0];
    m->seq = frame[1];
    size_t at = 2;
    for (size_t i = 0;; i++) {
        enum field f = next_field(l, i, m, version);
        if (f == END) {
            if (l->type == TERN_C_SET && i == 1) {
                return TERN_C_READ_UNKNOWN; /* a setting this version does not define */
            }
            return TERN_C_READ_OK;
        }
        size_t w = width(f);
        if (f == ADDRESS || f == GROUP || f == DIGEST) {
            if (at + w > len) {
                return TERN_C_READ_MALFORMED;
            }
            memcpy(bytes_of(m, f), frame + at, w);
            at += w;
        } else if (w == 0) {
            if (at + 1 > len || at + 1 + frame[at] > len) {
                return TERN_C_READ_MALFORMED;
            }
            size_t n = frame[at];
            if (n > string_max(f) || (f != DATA && !tern_companion_utf8(frame + at + 1, n))) {
                return TERN_C_READ_MALFORMED;
            }
            uint8_t *len_of;
            memcpy(variable_of(m, f, &len_of), frame + at + 1, n);
            *len_of = (uint8_t)n;
            at += 1 + n;
        } else {
            if (at + w > len) {
                return TERN_C_READ_MALFORMED;
            }
            uint32_t v = 0;
            for (size_t k = 0; k < w; k++) {
                v = v << 8 | frame[at + k];
            }
            set_member(m, f, v);
            at += w;
        }
    }
}

size_t tern_companion_write(const struct tern_companion_msg *m, uint8_t *out) {
    return tern_companion_write_as(m, out, TERN_COMPANION_VERSION);
}

size_t tern_companion_write_as(const struct tern_companion_msg *m, uint8_t *out, uint8_t version) {
    const struct layout *l = layout_of(m->type);
    if (l == NULL || tern_companion_since(m->type) > version) {
        return 0;
    }
    out[0] = m->type;
    out[1] = m->seq;
    size_t at = 2;
    for (size_t i = 0;; i++) {
        enum field f = next_field(l, i, m, version);
        if (f == END) {
            return l->type == TERN_C_SET && i == 1 ? 0 : at;
        }
        size_t w = width(f);
        if (f == ADDRESS || f == GROUP || f == DIGEST) {
            memcpy(out + at, bytes_of((struct tern_companion_msg *)m, f), w);
            at += w;
        } else if (w == 0) {
            uint8_t *n;
            const uint8_t *v = variable_of((struct tern_companion_msg *)m, f, &n);
            if (*n > string_max(f)) {
                return 0;
            }
            out[at] = *n;
            memcpy(out + at + 1, v, *n);
            at += 1 + *n;
        } else {
            uint32_t v = get_member(m, f);
            for (size_t k = 0; k < w; k++) {
                out[at + k] = (uint8_t)(v >> (8 * (w - 1 - k)));
            }
            at += w;
        }
    }
}

/* The length of the character starting at text, if it is a whole and valid one; else 0. */
static size_t utf8_char(const uint8_t *text, size_t len) {
    uint8_t c = text[0];
    size_t n;
    uint32_t cp, least;
    if (c < 0x80) {
        return 1;
    } else if (c >= 0xC2 && c <= 0xDF) {
        n = 2, cp = c & 0x1Fu, least = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
        n = 3, cp = c & 0x0Fu, least = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 4, cp = c & 0x07u, least = 0x10000;
    } else {
        return 0;
    }
    if (n > len) {
        return 0;
    }
    for (size_t i = 1; i < n; i++) {
        if ((text[i] & 0xC0) != 0x80) {
            return 0;
        }
        cp = cp << 6 | (text[i] & 0x3Fu);
    }
    if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    return n;
}

bool tern_companion_utf8(const uint8_t *text, size_t len) {
    return tern_companion_utf8_prefix(text, len, len) == len;
}

size_t tern_companion_utf8_prefix(const uint8_t *text, size_t len, size_t max) {
    size_t at = 0;
    if (len > max) {
        len = max;
    }
    while (at < len) {
        size_t n = utf8_char(text + at, len - at);
        if (n == 0) {
            break;
        }
        at += n;
    }
    return at;
}

/* --- Byte streams --------------------------------------------------------------------------- */

uint16_t tern_companion_crc(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000 ? crc << 1 ^ 0x1021 : crc << 1) & 0xFFFF;
        }
    }
    return (uint16_t)crc;
}

size_t tern_companion_wrap(const uint8_t *frame, size_t len, uint8_t *out) {
    if (len < 2 || len > TERN_COMPANION_MAX_FRAME) {
        return 0;
    }
    out[0] = MAGIC0;
    out[1] = MAGIC1;
    out[2] = (uint8_t)(len >> 8);
    out[3] = (uint8_t)len;
    memmove(out + 4, frame, len);
    uint16_t crc = tern_companion_crc(out + 2, len + 2);
    out[4 + len] = (uint8_t)(crc >> 8);
    out[5 + len] = (uint8_t)crc;
    return len + 6;
}

void tern_companion_parser_init(struct tern_companion_parser *p) {
    p->len = 0;
    p->last = 0;
}

/* Gives up the first byte held as text, and keeps the rest to look at again. */
static void give_up_one(struct tern_companion_parser *p, const struct tern_companion_sink *sink) {
    sink->text(sink->ctx, p->buf[0]);
    memmove(p->buf, p->buf + 1, --p->len);
}

/* Decides what it can about the bytes held: frames out, text out, and the rest kept. */
static void scan(struct tern_companion_parser *p, const struct tern_companion_sink *sink) {
    while (p->len > 0) {
        if (p->buf[0] != MAGIC0 || (p->len >= 2 && p->buf[1] != MAGIC1)) {
            give_up_one(p, sink);
            continue;
        }
        if (p->len < 4) {
            return;
        }
        size_t n = (size_t)p->buf[2] << 8 | p->buf[3];
        if (n < 2 || n > TERN_COMPANION_MAX_FRAME) {
            give_up_one(p, sink);
            continue;
        }
        size_t whole = n + 6;
        if (p->len < whole) {
            return;
        }
        uint16_t crc = (uint16_t)((unsigned)p->buf[whole - 2] << 8 | p->buf[whole - 1]);
        if (crc != tern_companion_crc(p->buf + 2, n + 2)) {
            give_up_one(p, sink);
            continue;
        }
        sink->frame(sink->ctx, p->buf + 4, n);
        p->len -= whole;
        memmove(p->buf, p->buf + whole, p->len);
    }
}

void tern_companion_push(struct tern_companion_parser *p, tern_time now, uint8_t byte,
                         const struct tern_companion_sink *sink) {
    /* A full buffer is always decided by scan(), so there is room for one more. */
    p->buf[p->len++] = byte;
    p->last = now;
    scan(p, sink);
}

void tern_companion_idle(struct tern_companion_parser *p, tern_time now,
                         const struct tern_companion_sink *sink) {
    if (p->len > 0 && now - p->last >= TERN_COMPANION_GAP) {
        give_up_one(p, sink);
        scan(p, sink);
        p->last = now;
    }
}
