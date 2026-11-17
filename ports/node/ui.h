#ifndef HELTEC_V3_UI_H
#define HELTEC_V3_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "display.h"
#include "scene.h"
#include "tern/address.h"
#include "tern/companion.h"
#include "tern/group.h"

/* The screen someone carrying the board sees: docs/ui.md, "The node's own screen".
 *
 * Its words are the user's. A node has messages, contacts, nodes nearby and a share of the air; it
 * does not have routing ids, sessions or frame counts, which are the bench screen's (status.h) and
 * are shown only when a developer asks for them. A message is never shown as more certain than
 * the node knows: waiting, sent, delivered or not delivered, as the companion link says.
 *
 * node.c fills a ui_node from the radio, the router and the companion link, and each page is made
 * from it alone, as a scene (scene.h) that a monochrome screen and a colour panel each draw their
 * own way. Nothing here touches the hardware, so tests/ui.c runs it on a host.
 *
 * The pages: a press of the button shows the next of the five main ones, Home, Messages, Nearby,
 * My code and More, and round again; dots at the foot of a page say which it is. Holding the
 * button acts on the page shown. On More, a hold opens its menu, the pages a user wants seldom:
 * there a press moves to the next choice and a hold opens it, and on the page it opens a press
 * comes back to the menu. Erasing the node, and forgetting its phones, are in that menu, never
 * among the pages a press passes through. */

enum ui_page {
    UI_HOME,          /* whether it is on the air, who is nearby, what is new, the air left */
    UI_MESSAGES,      /* one message at a time, newest first */
    UI_NEARBY,        /* the nodes heard directly, most recently heard first */
    UI_SHARE,         /* this node's address as a QR code, for a phone to scan */
    UI_MORE,          /* the menu of the rest */
    UI_MAIN,          /* the pages a press goes round; those after are More's */
    UI_AIR = UI_MAIN, /* the region's limit on transmitting, and what is used of it */
    UI_GROUPS, /* the groups it holds, one at a time, and a group's join code when asked for */
    UI_NODE,   /* this node: its short code, address, radio and firmware */
    UI_PHONES, /* the phones paired over Bluetooth, and forgetting them */
    UI_RESET,  /* erasing everything the node keeps, for a new owner or a fresh start */
    UI_PAGES
};

/* More's choices, in order: the page each opens, and its last, which closes the menu. */
#define UI_MENU 6
#define UI_MENU_BACK (UI_MENU - 1)

/* The page More's choice `choice` opens, or UI_MORE for the one that closes the menu. */
int ui_menu_page(int choice);

/* The choice of More's that opens `page`, or -1 if none does. */
int ui_menu_choice(int page);

#define UI_BATTERY_UNKNOWN 255
#define UI_BATTERY_LOW 10 /* percent, at or below which Home says the battery is low */

#define UI_NAME 20 /* bytes of a name kept to show, which is as many as a line has room for */

/* The bytes of a group's name the join code on the screen carries, at most: a code whose name is
 * longer needs a version 4 symbol, 33 modules, which at two pixels a module is more than the 64
 * rows the screen has. A shorter name is a suggestion all the same (draft/groups.md), and the
 * group the code joins is the same. */
#define UI_JOIN_NAME 12

/* How much of a name of `len` bytes, valid UTF-8, a join code on the screen carries: all of it up
 * to UI_JOIN_NAME bytes, and past that as many whole characters as fit. */
size_t ui_join_name(const uint8_t *name, size_t len);

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

#define UI_NEARBY_ROWS 7 /* nodes the Nearby page has room for at once, on a monochrome screen */
#define UI_NEARBY_ROWS_COLOUR 4 /* and on a colour panel, whose lines are taller */

/* A node heard directly: a neighbour, in the router's word. */
struct ui_neighbour {
    char name[UI_NAME + 1]; /* a contact's name, or its routing id */
    int8_t snr_db;          /* how well its last announce was heard */
    uint32_t ago_s;         /* since it was last heard */
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
    bool bluetooth;  /* Bluetooth started, so phones can pair */
    const char *ble_name; /* what it advertises the node as, which a phone lists it by */
    const char *button;   /* what the board's button is marked (board.h), or NULL for none */
    uint8_t paired;       /* phones that have paired and are remembered */
    /* Holding PRG on the Phones or Reset page asks first: held once, the page asks to be sure,
     * for `confirm_s` more seconds, and held again in that time, it acts. 0 when it is not
     * asking. */
    uint8_t confirm_s;
    bool bench; /* the console's bench mode: the board sends only the test frames asked for */
    /* More's menu: the choice shown as chosen, or -1 while it is closed. */
    int8_t menu;

    uint16_t nearby;    /* nodes heard directly */
    uint16_t reachable; /* nodes it has a route to, nearby ones included */
    /* The ones the Nearby page shows: `nearby_n` of them, from the `nearby_first`th most recently
     * heard. */
    uint16_t nearby_first;
    uint8_t nearby_n;
    struct ui_neighbour neighbour[UI_NEARBY_ROWS];

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

    /* The groups the node holds: how many, and the one the Groups page shows (`group_shown`,
     * counted from the first held), by the user's name for it. */
    uint8_t groups, group_shown;
    char group[UI_NAME + 1];
    /* The join code of the group shown, as its link, once the user held PRG on the Groups page to
     * see it; empty otherwise. It is the group's secret: the screen shows it only when asked, and
     * whoever fills it in wipes it once it is drawn. */
    char join_link[TERN_GROUP_LINK_MAX + 1];
};

/* Makes page `page` (an enum ui_page) of the node into a scene, to be drawn. */
void ui_scene(const struct ui_node *n, int page, struct scene *s);

/* Makes page `page` of the node, and draws it on a monochrome screen. */
void ui_draw(const struct ui_node *n, int page, struct display *d);

/* What the board says while it erases itself, from the Reset page: it starts again at once, as a
 * new node. */
void ui_erasing(struct scene *s);

/* What a Bluetooth client that is pairing needs: the passkey to type into it. */
void ui_pairing(uint32_t passkey, struct scene *s);

/* An update being taken over the companion link: how much of the image has arrived. */
void ui_updating(uint32_t held, uint32_t size, struct scene *s);
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
    UI_OFF_ASKED,   /* a client sent OFF */
};

/* The warning while PRG is held to turn the board off: `seconds` more and it goes. */
void ui_turning_off(unsigned seconds, struct scene *s);

/* What the board says as it turns off, and how to turn it on again: by `button`, what the board's
 * button is marked, or RESET if it is NULL. */
void ui_off(enum ui_off why, const char *button, struct scene *s);

/* The screen shown while the board starts: its name, its firmware's version, and, once they are
 * known, its region and short code. */
void ui_boot(const struct ui_start *st, struct scene *s);

/* Why the board stopped starting, and what to do about it; `code` is the error a driver gave, or
 * 0 if there is none to show. */
void ui_fault(enum ui_fault why, const struct ui_start *st, int code, struct scene *s);

/* The bench screen's page as a scene: its rows, the first the heading. */
void ui_rows(const char *const *rows, int n, struct scene *s);

#endif
