#ifndef HELTEC_V3_UI_H
#define HELTEC_V3_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "display.h"
#include "tern/address.h"
#include "tern/companion.h"

/* The screen someone carrying the board sees: docs/ui.md, "The node's own screen".
 *
 * Its words are the user's. A node has messages, contacts, nodes nearby and a share of the air; it
 * does not have routing ids, sessions or frame counts, which are the bench screen's (status.h) and
 * are shown only when a developer asks for them. A message is never shown as more certain than
 * the node knows: waiting, sent, delivered or not delivered, as the companion link says.
 *
 * main.c fills a ui_node from the radio, the router and the companion link, and the pages are
 * drawn from it alone. Nothing here touches the hardware, so tests/ui.c runs it on a host. */

enum ui_page {
    UI_HOME,     /* whether it is on the air, who is nearby, what is new, the air left */
    UI_MESSAGES, /* one message at a time, newest first */
    UI_AIR,      /* the region's limit on transmitting, and what is used of it */
    UI_SHARE,    /* this node's address as a QR code, for a phone to scan */
    UI_NODE,     /* this node's address in digits, to read out or copy */
    UI_PAGES
};

#define UI_BATTERY_UNKNOWN 255
#define UI_BATTERY_LOW 10 /* percent, at or below which Home says the battery is low */

#define UI_NAME 20 /* bytes of a name kept to show, which is as many as a line has room for */

struct ui_message {
    bool received;            /* from someone, rather than to them */
    bool group;               /* to or from a group, which `who` then names */
    char writer[UI_NAME + 1]; /* a received group message's: whom it says it is from */
    bool unread;              /* received, and not yet seen here or on a client */
    char who[UI_NAME + 1];    /* the contact's name, or the first bytes of its address */
    bool aged;                /* the node's clock is set, and so `ago_s` means something */
    uint32_t ago_s;           /* since it was sent or received */
    uint8_t state, reason;    /* TERN_C_ states and TERN_C_WAIT_ reasons */
    uint16_t wait_s;          /* how long it is expected to wait, or 0 if that is not known */
    uint8_t text_len;
    uint8_t text[TERN_COMPANION_TEXT_MAX]; /* UTF-8, as the link keeps it */
};

struct ui_node {
    const char *region;
    const char *version;
    bool relay;
    int8_t dbm;
    uint8_t address[TERN_ADDRESS_LEN];
    uint8_t battery; /* percent, or UI_BATTERY_UNKNOWN */
    bool charging;   /* as far as the board can tell (power.h): it has no wire from its charger */
    bool phone;      /* a client is connected over Bluetooth */
    bool bench;      /* the console's bench mode: the board sends only the test frames asked for */

    uint16_t nearby;    /* nodes heard directly */
    uint16_t reachable; /* nodes it has a route to, nearby ones included */

    /* The air: in a region that limits transmitting, what is counted against the limit, the
     * limit, over what span, and how long until the next frame may go. Elsewhere, only what was
     * sent since starting. */
    bool limited;
    int64_t air_used_ms, air_limit_ms, air_total_ms;
    uint32_t window_s, wait_ms;

    /* The messages the node keeps: how many, how many are unread, how many of its own wait, who
     * sent the newest unread one, and the one the Messages page shows (`shown`, counted from the
     * newest). */
    uint8_t messages, unread, waiting;
    char from[UI_NAME + 1];
    bool from_group; /* and `from` is the group it was written to */
    uint8_t shown;
    struct ui_message message;
};

/* Draws page `page` (an enum ui_page) of the node into the picture. */
void ui_draw(const struct ui_node *n, int page, struct display *d);

/* Draws what a Bluetooth client that is pairing needs: the passkey to type into it. */
void ui_pairing(uint32_t passkey, struct display *d);

/* What the board knows of itself while it starts: shown at once, before anything else, and filled
 * in as it is learned. */
struct ui_start {
    const char *version;
    const char *region;     /* NULL until the settings are read */
    const uint8_t *address; /* TERN_ADDRESS_LEN bytes, or NULL until the identity is loaded */
    bool new_address;       /* the identity was made on this start, not loaded */
};

/* Why the board did not start. Each is something its owner can see and do something about
 * without a laptop, or at least know to take it to one. */
enum ui_fault {
    UI_FAULT_STORAGE,  /* the flash's saved data is from a layout this build cannot read */
    UI_FAULT_IDENTITY, /* the identity could not be made or saved */
    UI_FAULT_POWER,    /* the transmit power is more than the region allows into this antenna */
    UI_FAULT_RANDOM,   /* no random numbers to seed the router */
    UI_FAULT_RADIO,    /* the radio did not answer, or would not take its settings */
};

/* Why the board is turning itself off. */
enum ui_off {
    UI_OFF_PRESSED, /* PRG was held down */
    UI_OFF_EMPTY,   /* the battery is too low to run on */
};

/* Draws the warning while PRG is held to turn the board off: `seconds` more and it goes. */
void ui_turning_off(unsigned seconds, struct display *d);

/* Draws what the board says as it turns off, and how to turn it on again. */
void ui_off(enum ui_off why, struct display *d);

/* Draws the screen shown while the board starts: its name, its firmware's version, and, once they
 * are known, its region and short code. */
void ui_boot(const struct ui_start *s, struct display *d);

/* Draws why the board stopped starting, and what to do about it; `code` is the error a driver
 * gave, or 0 if there is none to show. */
void ui_fault(enum ui_fault why, const struct ui_start *s, int code, struct display *d);

#endif
