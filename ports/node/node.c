/* The node: boards make first contact and send each other secured unicast frames,
 * which follow routes: each is handed to the forwarder (tern/forward.h), sent to the next hop its
 * route gives, sent again if nothing is heard of it, and acknowledged by the board it is for. A
 * board built as a relay passes other boards' frames on.
 *
 * Over the USB serial port (115200 baud where it is a UART) it takes these commands:
 *
 *   contact <address>        make first contact with the board whose address that is (its
 *                            'status' shows it)
 *   accept                   for two minutes, let a board that is not yet a peer make contact
 *   peers                    list the boards this one has a session with, by number
 *   to <number>              choose the peer 'send' and the button send to
 *   send <text>              send a message to that peer: the one last made contact with,
 *                            written to or heard from, unless 'to' chose another
 *   drop <number>            end the session with a peer, and forget it
 *   groups                   list the groups this board holds, and the invites it has had
 *   group new <name>         make a group
 *   group invite <number>    invite the peer 'send' sends to, to that group
 *   group join <id>          take the group an invite was to
 *   group send <number> <text>   write to a group
 *   group leave <number>     leave one
 *   status                   show this board's address, its sessions and the radio settings
 *   selftest                 run a handshake between two nodes in memory, and time it
 *
 * and, for measuring on a bench what one radio hears of another (see README.md):
 *
 *   bench on|off             stop routing and first contact, and count what the radio receives
 *   sync <hex>               the sync word, in its one-byte form
 *   power <dBm>              the power frames are sent at
 *   freq <Hz>, sf <n>, bw <Hz>   the channel and modulation, within the region's band
 *   beacon <count> <ms>      send so many test frames, so far apart
 *   counts [reset]           what has been sent and received since the last reset
 *
 * The same port speaks the companion protocol (draft/companion.md in ternmesh/spec), for a phone or
 * a computer to drive the board: its frames are told from typed text byte by byte, and are
 * answered by link.c. A message a client sends, and one sent with 'send', waits in the link's
 * list until this loop seals it and hands it to the forwarder.
 *
 * Pressing PRG shows the screen's next page (ui.h), and holding it for a second acts on the page
 * shown: on Messages it shows the one before, on Nearby the next nodes, on Groups a group's join
 * code and then the next group, and on Phones and Reset it
 * asks to be sure, then forgets every paired phone or erases the board. 'screen bench on' adds the
 * bench screen's pages (status.h) after them, where holding PRG sends a ping, and a board that
 * receives a ping answers with a pong saying how well it heard it. (With no screen, a press sends a
 * ping.) The LED lights while a frame is on the air or has just arrived, and blinks while a message
 * is unread.
 *
 * Everything runs in one loop: the core never runs in interrupt context, so the loop polls the
 * radio, the serial port, the button and the screen in turn. */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble.h"
#include "board.h"
#include "demo.h"
#include "display.h"
#include "link.h"
#include "platform.h"
#include "power.h"
#include "status.h"
#include "tern/card.h"
#include "tern/companion.h"
#include "tern/duty.h"
#include "tern/err.h"
#include "tern/flood.h"
#include "tern/forward.h"
#include "tern/group.h"
#include "tern/lora.h"
#include "tern/position.h"
#include "tern/radio.h"
#include "tern/region.h"
#include "tern/route.h"
#include "tern/share.h"
#include "ui.h"

#define CONSOLE_LINE 300
#define LED_MS 150
#define UNREAD_BLINK_MS 100  /* while a message is unread, the LED blinks this long */
#define UNREAD_EVERY_MS 4000 /* this often */
#if CONFIG_TERN_UNREAD_LED   /* a bool Kconfig leaves undefined when it is off */
#define UNREAD_LED true
#else
#define UNREAD_LED false
#endif
#define ACCEPT_S 120
#define NEIGHBOURS 64 /* 2.5 kB; in a crowd, 32 held a tenth fewer routes in the simulator */
/* A relay's table is how large a network it can carry: RELAY_PLACES, 1024, in the specification's
 * routing draft, at 64 bytes each. A board with the RAM for it says so in its build; the rest keep
 * 128 until they are measured, which with the default route is room enough for a leaf. */
#ifdef CONFIG_TERN_DESTINATIONS
#define DESTINATIONS CONFIG_TERN_DESTINATIONS
#else
#define DESTINATIONS 128
#endif
#define FORWARD_SLOTS 8 /* frames in hand at once, this board's and those it passes on: 2.4 kB */
#define PENDING 4       /* of them, this board's own messages not yet acknowledged */
#define TX_MIN_DBM board_power_min() /* the least this board puts into its antenna */
#define POWER_UNSET INT8_MIN
#define SCREEN_MS 500      /* how often the bench screen is drawn again */
#define BOOT_MS 2500       /* how long the boot screen stays up once the board has started */
#define BATTERY_S 30       /* how often the battery is read */
#define HOLD_MS 1000       /* how long PRG is held to send a ping */
#define OFF_WARN_MS 2000   /* held this long, PRG warns that the board is turning off */
#define OFF_MS 5000        /* and this long, it turns off */
#define CONFIRM_S 10       /* held once on Phones or Reset, how long it asks to be sure */
#define EMPTY_CHECK_S 1800 /* turned off for an empty battery, how often it wakes to look again */
#define SCREEN_TRIES 5     /* writes failed in a row before the screen is given up */
#define FIRMWARE "tern " CONFIG_TERN_VERSION " " CONFIG_TERN_BOARD_NAME
/* The name a client finds an image by: tern-<board>-<region>-... */
#define BOARD CONFIG_TERN_BOARD_NAME
/* An update with nothing new for this long is no longer shown on the screen. */
#define UPDATE_SHOWN_S 30
_Static_assert(sizeof FIRMWARE - 1 <= TERN_COMPANION_FIRMWARE_MAX, "the version fits in INFO");
#define SETTINGS_MAGIC 0x54530001u
#define IDS_EARLIER 0x10000u /* past the message ids of a build that did not keep them */
#define PASSKEY_RANDOM 0xFFFFFFFFu

static struct tern_radio radio;
static struct tern_radio_config cfg;
static const struct tern_region *region;
static bool off_profile; /* the frequency, modulation or sync word is not the region's */
static struct tern_duty duty;
static struct demo demo;
static bool transmitting;
static struct tern_route route;
static struct tern_route_neighbour neighbours[NEIGHBOURS];
static struct tern_route_dest destinations[DESTINATIONS];
static uint16_t route_seq_saved;
/* Announce numbers are stored this far ahead, so that none is sent twice across a restart: the
 * specification's NUMBER_SAVE. */
#define NUMBER_SAVE 256
/* Whether a routing frame has gone on the air since the board started: until one has, numbers are
 * stored ahead one at a time, so that a board that keeps restarting before anything goes uses up
 * one number a restart, not NUMBER_SAVE. */
static bool route_on_air;
static bool route_out;   /* the frame on the air is the router's */
static int8_t power_now; /* what the radio is set to send at, or POWER_UNSET */
static uint8_t route_frame[TERN_ROUTE_FRAME_MAX]; /* the router's, until it has gone */
static size_t route_len;
static int8_t route_dbm;
static tern_time route_retry; /* when to try it again, if the radio refused it */
static struct tern_forward forward;
static struct tern_forward_slot forward_slots[FORWARD_SLOTS];

/* Frames for every node: a group's (tern/flood.h). */
#define FLOOD_SLOTS 4 /* this board's and those it passes on, waiting their turn: 1.1 kB */
#define FLOODING 4    /* this board's own group messages not yet on the air */
static struct tern_flood flood;
static struct tern_flood_slot flood_slots[FLOOD_SLOTS];
static uint8_t flood_seen[TERN_FLOOD_SEEN][TERN_FLOOD_ID];
static bool flood_out; /* the frame on the air is the flooder's */
static uint8_t flood_handle;
static tern_time flood_retry;
/* A group message handed to the flooder, by the nonce its frame carries: sent once it has gone. */
static struct {
    uint32_t id;
    uint8_t nonce[TERN_GROUP_NONCE];
    uint8_t frame_id[TERN_FLOOD_ID]; /* the flooder's name for its frame */
} flooding[FLOODING];
/* A position handed to the flooder for a group, by the group's place and id: let go of if the
 * group is left, or no longer shared with, before it goes (link_group_position_wanted()). */
static struct {
    bool on;
    bool stopped; /* the stopped position, which goes though sharing is off */
    size_t place;
    uint8_t group[TERN_COMPANION_GROUP];
    uint8_t frame_id[TERN_FLOOD_ID];
} positioning[FLOODING];
static int flood_own = -1;     /* which of them the frame on the air is, or -1 */
static bool forward_out;       /* the frame on the air is the forwarder's */
static uint8_t forward_handle; /* and this is the forwarder's name for it */
static tern_time forward_retry;
static tern_time forward_quiet; /* how long to listen once the frame on the air has gone */
/* This board's messages that the forwarder has and that are not yet acknowledged or given up. */
/* A position's place among the pending: it has no message id, and what becomes of it is the
 * link's to know (link_position_done()), not news. */
#define PENDING_POSITION UINT32_MAX

static struct pending {
    uint32_t id; /* the link's, PENDING_POSITION, or 0 for a free place */
    uint32_t counter;
    int slot; /* the session it was sealed in (demo.s) */
    uint8_t tag[TERN_FORWARD_TAG];
    uint8_t goes;                 /* times it has gone on the air */
    tern_time at;                 /* when it was handed over */
    uint8_t to[TERN_ADDRESS_LEN]; /* a position's: whose it is, though its session be replaced */
} pending[PENDING];
/* The handshake frame of this board's that the forwarder keeps, and sends again until its answer
 * comes: message_1 or message_3 of a handshake this board began. */
static struct {
    bool kept;
    uint8_t hdr;
    uint8_t tag[TERN_FORWARD_TAG];
} contact_kept;
static uint32_t held_back; /* times a frame waited for one being received to end */
static bool holding;
static tern_time tx_deadline; /* when a frame on the air should certainly have finished */
static tern_time led_until;
/* What the bench screen shows that nothing else keeps. */
static uint32_t frames_out, frames_in;
static tern_time air_total;
static tern_time heard_total; /* the airtime of every frame received whole */
static char last_text[STATUS_TEXT];
static tern_time last_at; /* when last_text was heard, or 0 for nothing yet */
static struct display screen;
static bool have_screen;
/* The page shown: one of the user's (enum ui_page), or past them, the bench screen's. */
static int screen_page;
static uint8_t message_shown; /* which message the Messages page shows, counted from the newest */
static uint16_t nearby_first; /* the first node the Nearby page shows, most recently heard first */
static uint8_t group_shown;   /* which group the Groups page shows, counted from the first held */
/* The user held PRG on the Groups page to see the join code of the group it shows. Only while that
 * page is shown and the screen is lit: the code is the group's secret (draft/groups.md). */
static bool group_code;
static uint8_t group_code_id[TERN_COMPANION_GROUP]; /* the group it was asked for, and no other */
static int confirm_page = -1; /* the page asking to be sure of what a second hold does, or -1 */
static tern_time confirm_until;
/* The page shown was reached by a press, so its reader has seen it: a message shown because it
 * has just arrived is not read until a press says someone is looking. */
static bool screen_looked;
static tern_time screen_due;
static tern_time screen_retry; /* after a failed write, when to try again */
static unsigned screen_failures;
static bool screen_asleep;
static struct ui_start start_info; /* what the boot screen says, filled in as the board starts */
static tern_time boot_until;       /* until when the boot screen stays up, unless PRG is pressed */
static tern_time screen_woken;     /* the last reason to be on: a press, a message, a pairing */
static void screen_wake(void);

/* The companion link: the client's half of the conversation is link.c's; this is the port. */
static struct link companion;
static struct tern_companion_parser parser;
static uint32_t clock_base; /* seconds since 1970 as a client last set them, or 0 */
static tern_time clock_at;
static bool restart_due; /* a setting saved that takes a restart, once its answer has gone */
static bool have_ble;
static char ble_name[BLE_NAME_LEN + 1]; /* what Bluetooth advertises the node as (ble.h) */
static uint16_t battery_mv;             /* 0 for none, or not read */
static struct power_watch watch;        /* whether the battery charges, or is empty (power.h) */
static unsigned off_shown;       /* while PRG is held to turn off, the seconds left shown, or 0 */
static bool phone;               /* a client is connected over Bluetooth, for Home to say */
static uint32_t pairing_passkey; /* shown on the screen while pairing, or PAIRING_NONE */

/* An update over the companion link: whether one is being written, and when its last bytes came. */
static bool update_open;
static tern_time update_at;
#define PAIRING_NONE 0xFFFFFFFFu
/* A message sealed and not yet with the forwarder: sealing takes a counter and saves the session,
 * so a frame the forwarder had no room for, or the flash refused, is kept and tried again, a
 * second apart, not sealed again on every turn of the loop. Of no use once its session is
 * replaced or dropped. */
static uint32_t sealed_id;
static int sealed_slot;
static uint8_t sealed[TERN_UNICAST_MAX_FRAME];
static size_t sealed_len;
static uint32_t sealed_counter;
static tern_time outgoing_retry;
/* The handshake begun for a message, and with whom. */
static bool contacting;
static uint8_t contacting_peer[TERN_ADDRESS_LEN];
/* The SNR each neighbour's last announce was heard at, which the router does not keep. */
static struct {
    uint32_t id;
    int8_t snr; /* quarter-dB */
} heard_snr[NEIGHBOURS];
static size_t heard_snr_next;

/* What a client changed that outlives a restart, over what the build chose. */
struct settings {
    uint32_t magic;
    uint8_t region; /* enum tern_region_id, or 0 for the build's */
    uint8_t role;   /* 0 leaf, 1 relay, or 0xFF for the build's */
    int8_t power;   /* dBm, or POWER_UNSET for the build's */
    uint32_t passkey;
    /* Added later: a board that saved its settings before has none, and takes the build's. */
    uint16_t screen_sleep; /* seconds, or 0 for never */
    /* Later still. Four bytes, so that the struct grows: one would sit in what was padding, and
     * read whatever an earlier build left there. */
    uint32_t screen_flags;
    /* Later again: presence cards (draft/cards.md), off on a board that saved none. */
    uint8_t cards; /* 1 to send them */
    uint8_t card_name_len;
    uint8_t card_name[TERN_CARD_NAME_MAX];
};
#define SCREEN_BENCH 0x01u /* screen_flags: the bench screen's pages follow the user's */
#if CONFIG_TERN_SCREEN_BENCH
#define SCREEN_FLAGS SCREEN_BENCH
#else
#define SCREEN_FLAGS 0u
#endif
static struct settings settings = {
    SETTINGS_MAGIC, 0, 0xFF, POWER_UNSET, PASSKEY_RANDOM, CONFIG_TERN_SCREEN_SLEEP_S,
    SCREEN_FLAGS,   0, 0,    {0}};

/* The bench: test frames sent on a timer, and counts of what was received. */
#define BEACON_LEN 24
static const char beacon_text[BEACON_LEN - 4] = "tern sync word test ";
static bool bench;
static uint32_t beacon_left, beacon_sent, beacon_number;
static tern_time beacon_gap, beacon_next;
static bool beacon_running;
static uint32_t bench_ours, bench_others; /* frames received: test frames, and anything else */
static int32_t bench_rssi, bench_snr_cdb; /* summed over the test frames */

/* --- Storage (platform.h) --------------------------------------------------------------------- */

static bool store_load(void *ctx, const char *key, void *buf, size_t len) {
    (void)ctx;
    return plat_store_load(key, buf, len);
}

static bool store_save(void *ctx, const char *key, const void *buf, size_t len) {
    (void)ctx;
    return plat_store_save(key, buf, len);
}

/* Whether the board had an identity when it started, or made one (link_load_ids()). */
static bool had_identity;

/* The platform's generator, a true one (platform.h). */
static bool board_random(void *ctx, uint8_t *buf, size_t len) {
    (void)ctx;
    return plat_random(buf, len);
}

/* --- Updates (platform.h) --------------------------------------------------------------------- */

static void update_abandon(void) {
    if (update_open) {
        plat_update_abandon();
        update_open = false;
    }
}

static bool link_update_begin(void *ctx, uint32_t size) {
    (void)ctx;
    update_abandon();
    if (!plat_update_begin(size)) {
        return false;
    }
    update_open = true;
    update_at = board_now();
    /* The region is the build's until a client sets one, and an image of another region's build
     * would start on that region's frequency: the one this board is on is kept, whatever the
     * image. */
    if (settings.region == 0) {
        struct settings next = settings;
        for (int id = TERN_REGION_US915; id < TERN_REGION_END; id++) {
            next.region =
                tern_region((enum tern_region_id)id) == region ? (uint8_t)id : next.region;
        }
        if (!store_save(NULL, "settings", &next, sizeof next)) {
            update_abandon(); /* the region could not be kept: not now */
            return false;
        }
        settings = next;
    }
    printf("an update of %lu bytes begins\n", (unsigned long)size);
    return true;
}

static bool link_update_write(void *ctx, uint32_t offset, const uint8_t *data, size_t len) {
    (void)ctx;
    (void)offset; /* the link gives the bytes in order */
    if (!update_open || !plat_update_write(data, len)) {
        printf("the update could not be written to flash; it starts again\n");
        update_open = false;
        return false;
    }
    update_at = board_now();
    screen_wake();
    return true;
}

/* The image is whole and its digest right: it runs if the platform finds it firmware for this
 * board (plat_update_finish()). */
static uint8_t link_update_run(void *ctx) {
    (void)ctx;
    if (!update_open) {
        return TERN_C_ERR_NOT_THERE;
    }
    update_open = false;
    char version[32];
    uint8_t err = plat_update_finish(version, sizeof version);
    if (err == TERN_C_ERR_NOT_AN_IMAGE) {
        printf("the update is not an image this board runs\n");
    }
    if (err != 0) {
        return err;
    }
    printf("the update is whole: restarting into %s\n", version);
    restart_due = true;
    return 0;
}

/* --- Sending and receiving -------------------------------------------------------------------- */

static void flash_led(void) {
    board_led(true);
    led_until = board_now() + (tern_time)LED_MS * 1000000;
}

static const char *result_text(enum demo_result r) {
    switch (r) {
    case DEMO_OK:
        return "ok";
    case DEMO_UNPAIRED:
        return "no session yet: type 'contact <the other board's address>' here, or 'contact "
               "<this board's address>' on the other board";
    case DEMO_OWN_ADDRESS:
        return "that is this board's own address; give the other board's";
    case DEMO_BAD_ADDRESS:
        return "that is not a valid address";
    case DEMO_NO_RANDOM:
        return "the board has no random numbers to make a key from";
    case DEMO_STORE_FAILED:
        return "could not save the session to flash, so nothing was sent";
    case DEMO_FULL:
        return "this board holds as many sessions as it can; 'peers' lists them, and 'drop "
               "<number>' ends one";
    case DEMO_SPENT:
        return "this session has used every counter; make contact again";
    case DEMO_TOO_LONG:
        return "too long: at most 232 bytes";
    }
    return "?";
}

/* Whether a frame must wait: the radio is receiving one, and a node does not start to send over
 * it (draft/forwarding.md, "Listening first"). Asked of the chip just before each frame, since a
 * frame can begin at any moment; the bench's frames are not held, which measures the radio and
 * not the protocol. A radio with no way to tell holds nothing; one that fails to answer holds the
 * frame, since it may be receiving. */
static bool held(void) {
    bool is = tern_radio_receiving(&radio) != 0;
    held_back += is && !holding;
    holding = is;
    return is;
}

/* Puts a frame on the air at so many dBm. Returns its time on air, or 0 if the radio refused it. */
static tern_time transmit_at(const uint8_t *frame, size_t len, int8_t dbm) {
    tern_time air = tern_lora_airtime(&cfg.mod, (uint32_t)len);
    if (!tern_duty_allows(&duty, board_now(), air)) {
        printf("not sent: this board has transmitted as much as %s allows in %lu s. Wait, and "
               "try again.\n",
               region->name, (unsigned long)region->duty_window_s);
        return 0;
    }
    /* Counted, and the count saved, before the frame goes: a restart must not forget it. */
    tern_duty_charge(&duty, board_now(), air);
    if (region->duty_ppm < TERN_DUTY_UNLIMITED) {
        tern_time used = tern_duty_used(&duty, board_now());
        if (!store_save(NULL, "airtime", &used, sizeof used)) {
            printf("not sent: could not save the count of time on air to flash\n");
            return 0;
        }
    }
    int err = TERN_OK;
    if (dbm != power_now) {
        /* The radio interface sets power with everything else. */
        struct tern_radio_config at = cfg;
        at.tx_power_dbm = dbm;
        err = tern_radio_configure(&radio, &at);
        /* A configuration that fails leaves the radio set to nothing: whatever is asked for
         * next, it is configured again. */
        power_now = err == TERN_OK ? dbm : POWER_UNSET;
    }
    if (err == TERN_OK) {
        err = tern_radio_transmit(&radio, frame, (uint32_t)len);
    }
    if (err != TERN_OK) {
        printf("radio error %d: not sent\n", err);
        if (power_now == POWER_UNSET && tern_radio_configure(&radio, &cfg) == TERN_OK) {
            power_now = cfg.tx_power_dbm;
            tern_radio_receive(&radio);
        }
        return 0;
    }
    transmitting = true;
    frames_out++;
    air_total += air;
    tx_deadline = board_now() + air + 2000000000LL;
    flash_led();
    return air;
}

static tern_time transmit(const uint8_t *frame, size_t len) {
    return transmit_at(frame, len, cfg.tx_power_dbm);
}

static uint32_t clock_now(void) {
    if (clock_base == 0) {
        return 0;
    }
    return clock_base + (uint32_t)((board_now() - clock_at) / 1000000000LL);
}

/* Queues a message to the peer last used or chosen, as a client's would be: poll_outgoing()
 * sends it. */
static void send(const char *text) {
    if (demo.last < 0) {
        printf("not sent: %s\n", result_text(DEMO_UNPAIRED));
        return;
    }
    if (strlen(text) > TERN_COMPANION_TEXT_MAX) {
        printf("not sent: too long: at most %d bytes\n", TERN_COMPANION_TEXT_MAX);
        return;
    }
    if (link_add(&companion, demo.s[demo.last].peer, clock_now(), TERN_C_WAITING,
                 TERN_C_WAIT_UNNAMED, (const uint8_t *)text, strlen(text)) == 0) {
        printf("not sent: no room for another message, or nothing to send\n");
    }
}

static struct pending *pending_free(void) {
    for (int i = 0; i < PENDING; i++) {
        if (pending[i].id == 0) {
            return &pending[i];
        }
    }
    return NULL;
}

static struct pending *pending_tagged(const uint8_t *tag) {
    for (int i = 0; i < PENDING; i++) {
        if (pending[i].id != 0 && memcmp(pending[i].tag, tag, TERN_FORWARD_TAG) == 0) {
            return &pending[i];
        }
    }
    return NULL;
}

/* Seals a message, once, and hands it to the forwarder, which sends it, and sends it again, until
 * the peer acknowledges it or it is given up. False if it could not be handed over now, with
 * *gone set if it never will be. */
static bool hand_over(const struct link_message *x, struct pending *p, bool *gone) {
    *gone = false;
    int slot = demo_peer(&demo, x->address);
    if (slot < 0) {
        return false; /* dropped since it was asked for: poll_outgoing() makes contact again */
    }
    if (sealed_id != x->id) {
        /* An invite is a message for the peer's node: its group's secret and name, sealed as a
         * message is, with the header that says whom it is for. */
        uint8_t invite[TERN_GROUP_INVITE_MAX];
        const uint8_t *plain = x->text;
        size_t len = x->text_len;
        if (x->kind == LINK_KIND_INVITE) {
            len = link_invite_write(&companion, x, invite);
            plain = invite;
            if (len == 0) {
                *gone = true; /* its group was left since */
                return false;
            }
        }
        sealed_counter = demo.s[slot].session.tx.next;
        enum demo_result r = x->kind == LINK_KIND_INVITE
                                 ? demo_seal_node(&demo, slot, plain, len, sealed)
                                 : demo_seal(&demo, slot, plain, len, sealed);
        tern_wipe(invite, sizeof invite);
        if (r != DEMO_OK) {
            printf("not sent: %s\n", result_text(r));
            *gone = r == DEMO_SPENT;
            return false;
        }
        sealed_id = x->id;
        sealed_slot = slot;
        sealed_len = len + TERN_UNICAST_OVERHEAD;
    }
    /* An acknowledgement names its message by its tag alone, and so does the forwarder. Four
     * bytes can be the tag of two messages, so one that shares a tag with a message still on its
     * way waits for that one to end: the tags of those on their way are then all different. */
    if (pending_tagged(&sealed[TERN_FORWARD_HEAD]) != NULL) {
        return false;
    }
    if (!tern_forward_send(&forward, board_now(), tern_route_id(x->address), sealed, sealed_len,
                           true, INT8_MIN)) {
        return false; /* no room: every slot holds a frame */
    }
    *p = (struct pending){.id = x->id, .counter = sealed_counter, .slot = slot, .at = board_now()};
    memcpy(p->tag, &sealed[TERN_FORWARD_HEAD], TERN_FORWARD_TAG);
    sealed_id = 0;
    return true;
}

/* A session was replaced or dropped: what was sent in it can no longer be acknowledged, so the
 * forwarder lets go of it, and the client is told. Other sessions' messages go on. */
static void pending_drop(int slot, bool tell) {
    if (sealed_id != 0 && sealed_slot == slot) {
        sealed_id = 0; /* a frame sealed in the old session is no use in the new */
    }
    for (int i = 0; i < PENDING; i++) {
        if (pending[i].id != 0 && pending[i].slot == slot) {
            /* The forwarder's only way to let a message go: as if it had been acknowledged. */
            (void)tern_forward_acked(&forward, pending[i].tag);
            if (pending[i].id == PENDING_POSITION) {
                link_position_done(&companion, pending[i].to);
            } else if (tell) {
                link_state(&companion, pending[i].id, TERN_C_NOT_DELIVERED, 0, 0);
            }
            pending[i].id = 0;
        }
    }
}

/* The handshake frame the forwarder keeps has been answered, or its handshake is over. */
static void contact_let_go(void) {
    if (contact_kept.kept) {
        (void)tern_forward_done(&forward, contact_kept.hdr, contact_kept.tag);
        contact_kept.kept = false;
    }
}

/* Hands a handshake frame to the forwarder, which sends it by a route as it does a message. The
 * two a board sends when it begins a handshake are kept there until answered; the two it sends
 * in answer go once, and again when the other board asks again. `back` is, for a frame that
 * answers one received, what the forwarder said the node it came from needs, and INT8_MIN for
 * message_1, which answers none. False if it could not be taken. */
static bool send_contact(const uint8_t *frame, size_t len, int8_t back) {
    bool keep = frame[0] == TERN_HDR_CONTACT_1 || frame[0] == TERN_HDR_CONTACT_1 + 2;
    if (keep) {
        contact_let_go();
    }
    if (!tern_forward_send(&forward, board_now(), tern_contact_destination(frame), frame, len, keep,
                           back)) {
        printf("first contact: message_%d not sent: %s\n", frame[0] - 0x50,
               keep ? "no room for it" : "no route back yet, or no room; it goes when asked again");
        return false;
    }
    if (keep) {
        contact_kept.kept = true;
        contact_kept.hdr = frame[0];
        memcpy(contact_kept.tag, frame + TERN_FORWARD_HEAD, TERN_FORWARD_TAG);
    }
    return true;
}

/* The handshake this board began is over, with no session made. */
static void contact_gave_up(void) {
    contact_let_go();
    (void)demo_abandon(&demo);
    if (contacting) {
        contacting = false;
        link_unreachable(&companion, contacting_peer);
    }
}

/* An address in its text form (draft/sharing.md): sixty-four upper-case hex digits. */
static void print_address(const uint8_t address[TERN_ADDRESS_LEN]) {
    char text[TERN_ADDRESS_TEXT_LEN + 1];
    tern_address_text(address, text);
    fputs(text, stdout);
}

/* An announce's sender, at offset 1 (draft/routing.md), and the SNR it was heard at. */
static void note_snr(const uint8_t *frame, int8_t snr) {
    uint32_t id =
        (uint32_t)frame[1] << 24 | (uint32_t)frame[2] << 16 | (uint32_t)frame[3] << 8 | frame[4];
    for (size_t i = 0; i < NEIGHBOURS; i++) {
        if (heard_snr[i].id == id) {
            heard_snr[i].snr = snr;
            return;
        }
    }
    heard_snr[heard_snr_next].id = id;
    heard_snr[heard_snr_next].snr = snr;
    heard_snr_next = (heard_snr_next + 1) % NEIGHBOURS;
}

/* An acknowledgement that came to this board: taken only if it is of a message this board is
 * still waiting on, by the peer that message went to. */
static void heard_ack(const struct tern_radio_event *ev) {
    struct pending *p =
        ev->len == TERN_ACK_LEN ? pending_tagged(&ev->data[TERN_FORWARD_HEAD]) : NULL;
    if (p == NULL || !demo_acked(&demo, p->slot, p->counter, ev->data, ev->len)) {
        printf("(an acknowledgement of nothing this board is waiting on, at %d dBm)\n",
               ev->rssi_dbm);
        return;
    }
    flash_led();
    (void)tern_forward_acked(&forward, p->tag);
    printf("delivered #%lu: acknowledged %lld ms after it was handed over, having gone %u "
           "time%s\n",
           (unsigned long)p->counter, (long long)((board_now() - p->at) / 1000000), p->goes,
           p->goes == 1 ? "" : "s");
    if (p->id == PENDING_POSITION) {
        link_position_done(&companion, p->to);
    } else {
        link_state(&companion, p->id, TERN_C_DELIVERED, 0, 0);
    }
    p->id = 0;
}

/* A flooded frame: taken once however many copies come, passed on by the flooder if this board is
 * a relay, and opened if it is for a group this board holds. */
static void heard_flood(const struct tern_radio_event *ev) {
    struct tern_group *groups[LINK_GROUPS];
    static uint8_t text[TERN_GROUP_MAX_CONTENT + 1];
    struct tern_group_received got;
    if (!tern_flood_heard(&flood, board_now(), ev->data, ev->len)) {
        return;
    }
    if (ev->data[0] == TERN_HDR_CARD) {
        /* Who is about. One the link did not accept, forged or no newer than the one held among
         * them, is not passed on either (draft/cards.md, "Receiving"). */
        if (link_card_received(&companion, board_now(), ev->data, ev->len) != TERN_CARD_ACCEPTED) {
            uint8_t id[TERN_FLOOD_ID];
            tern_flood_id(ev->data, ev->len, id);
            (void)tern_flood_refuse(&flood, id);
        }
        return;
    }
    link_groups(&companion, groups);
    if (tern_group_open(groups, LINK_GROUPS, route.id, ev->data, ev->len, text,
                        TERN_GROUP_MAX_CONTENT, &got) != TERN_OK ||
        got.verdict != TERN_GROUP_ACCEPTED) {
        return; /* another group's, most often: nothing to say */
    }
    link_group_heard(&companion);
    flash_led();
    if (got.node) {
        /* For this board, not words: a position, or a kind it does not know and lets go of. */
        if (got.len > 0 && text[0] == TERN_POSITION_KIND) {
            link_group_position_received(&companion, board_now(), got.group, got.from, text,
                                         got.len);
        }
        return;
    }
    text[got.len] = '\0';
    const struct link_group *g = &companion.groups[got.group];
    /* Who wrote is what the frame says, which any member could have written. */
    printf("heard in \"%.*s\", from %08lx: \"%s\" at %d dBm\n", (int)g->name_len,
           (const char *)g->name, (unsigned long)got.from, (const char *)text, ev->rssi_dbm);
    if (link_add_group(&companion, got.group, got.from, clock_now(), text, got.len) != 0) {
        screen_wake();
    }
}

/* A message for this board itself, from a peer it shares a session with. Its first byte says
 * what it is: a position, which the link keeps if the peer is a contact, or an invite to a group,
 * which is kept for the user to take or leave: the board holds no group it was not told to.
 * Anything else is acknowledged, as the specification has it, and let go. */
static void heard_for_node(const uint8_t *plain, size_t len, const uint8_t peer[TERN_ADDRESS_LEN],
                           uint32_t counter) {
    uint8_t secret[TERN_GROUP_SECRET], name[TERN_GROUP_NAME_MAX];
    size_t name_len;
    if (len > 0 && plain[0] == TERN_POSITION_KIND) {
        link_position_received(&companion, board_now(), peer, counter, plain, len);
        return;
    }
    if (!tern_group_invite_read(plain, len, secret, name, &name_len)) {
        printf("(a message for this board that it does not know what to do with)\n");
        return;
    }
    printf("invited to the group \"%.*s\" by ", (int)name_len, (const char *)name);
    print_address(peer);
    printf(". A client can join it.\n");
    if (link_add_invite(&companion, peer, clock_now(), secret, name, name_len) != 0) {
        screen_wake();
    }
    tern_wipe(secret, sizeof secret);
}

static void heard(const struct tern_radio_event *ev) {
    uint8_t msg[TERN_UNICAST_MAX_PLAINTEXT + 1];
    struct demo_received got;
    struct tern_forward_heard routed = {TERN_FORWARD_NOTHING, INT8_MIN};
    /* SNR comes in centibels: -725 is -7.25 dB. */
    const char *snr_sign = ev->snr_cdb < 0 ? "-" : "";
    int snr_abs = ev->snr_cdb < 0 ? -ev->snr_cdb : ev->snr_cdb;
    int snr_whole = snr_abs / 100, snr_frac = snr_abs % 100;

    if (tern_route_frame(ev->data, ev->len)) {
        /* The router takes quarters of a decibel, as the radio measures. */
        tern_time now = board_now();
        tern_route_heard(&route, now, ev->data, ev->len, (int16_t)(ev->snr_cdb / 25));
        /* Shown for a neighbour only once the router has taken its announce: one it could not
         * check says nothing of who sent it. */
        if (ev->len >= 5 && ev->data[0] == TERN_HDR_ANNOUNCE) {
            uint32_t id = (uint32_t)ev->data[1] << 24 | (uint32_t)ev->data[2] << 16 |
                          (uint32_t)ev->data[3] << 8 | ev->data[4];
            for (size_t i = 0; i < NEIGHBOURS; i++) {
                if (neighbours[i].used && neighbours[i].id == id && neighbours[i].heard == now) {
                    note_snr(ev->data, (int8_t)(ev->snr_cdb / 25));
                    break;
                }
            }
        }
        if (route.seq != route_seq_saved && store_save(NULL, "seq", &route.seq, sizeof route.seq)) {
            route_seq_saved = route.seq;
        }
        return;
    }
    if (tern_flood_frame(ev->data, ev->len)) {
        heard_flood(ev);
        return;
    }
    if (tern_forward_frame(ev->data, ev->len)) {
        /* The forwarder says whose it is: one for another board is passed on, if this board is a
         * relay and on its way, and otherwise let go by. */
        tern_forward_heard(&forward, board_now(), ev->data, ev->len, (int16_t)(ev->snr_cdb / 25),
                           &routed);
        if (routed.got == TERN_FORWARD_ACK) {
            heard_ack(ev);
        }
        if (routed.got != TERN_FORWARD_MESSAGE && routed.got != TERN_FORWARD_CONTACT) {
            return;
        }
    }
    enum demo_heard what = demo_receive(&demo, board_now(), ev->data, ev->len, msg, &got);
    if (demo.h.phase != DEMO_INITIATING) {
        contact_let_go(); /* the handshake this board began is made, or failed */
    }
    switch (what) {
    case DEMO_HEARD_MESSAGE:
        flash_led();
        if (got.node) {
            heard_for_node(msg, got.msg_len, got.peer, got.counter);
            break;
        }
        msg[got.msg_len] = '\0';
        printf("heard #%lu \"%s\" at %d dBm, SNR %s%d.%02d dB\n", (unsigned long)got.counter,
               (const char *)msg, ev->rssi_dbm, snr_sign, snr_whole, snr_frac);
        /* As much as the screen keeps of it. */
        size_t keep = got.msg_len < sizeof last_text - 1 ? got.msg_len : sizeof last_text - 1;
        memcpy(last_text, msg, keep);
        last_text[keep] = '\0';
        last_at = board_now();
        screen_wake();
        uint32_t kept =
            link_add(&companion, got.peer, clock_now(), TERN_C_RECEIVED, 0, msg, got.msg_len);
        if (kept != 0 && have_screen) {
            /* Shown at once, and read once someone presses PRG to say they saw it. */
            screen_page = UI_MESSAGES;
            message_shown = 0;
            screen_looked = false;
            screen_due = 0;
        }
        if (strncmp((const char *)msg, "ping", 4) == 0) {
            char reply[64];
            snprintf(reply, sizeof reply, "pong to #%lu: %d dBm, SNR %s%d.%02d dB",
                     (unsigned long)got.counter, ev->rssi_dbm, snr_sign, snr_whole, snr_frac);
            send(reply);
        }
        break;
    case DEMO_HEARD_COPY:
        printf("heard #%lu again: its acknowledgement did not get back, and is sent again\n",
               (unsigned long)got.counter);
        break;
    case DEMO_HEARD_CONTACT:
        flash_led();
        printf("first contact: heard message_%d at %d dBm, SNR %s%d.%02d dB\n", ev->data[0] - 0x50,
               ev->rssi_dbm, snr_sign, snr_whole, snr_frac);
        break;
    case DEMO_HEARD_PAIRED:
        flash_led();
        printf("first contact: complete. Session started, as the %s, with ",
               demo.s[got.slot].role == TERN_INITIATOR ? "initiator" : "responder");
        print_address(got.peer);
        printf(". It is peer %d of %u, and where 'send' now goes.\n", got.slot + 1,
               (unsigned)demo_peers(&demo));
        /* If it took the place of an older session with the same peer. */
        pending_drop(got.slot, true);
        if (contacting && memcmp(contacting_peer, got.peer, TERN_ADDRESS_LEN) == 0) {
            contacting = false;
        }
        link_session_changed(&companion, got.peer);
        break;
    case DEMO_HEARD_FULL:
        printf("first contact: ");
        print_address(got.peer);
        printf(" made contact, but this board holds as many sessions as it can. 'peers' lists "
               "them, and 'drop <number>' ends one.\n");
        link_asked(&companion, board_now(), got.peer, TERN_C_ASKED_NO_ROOM);
        break;
    case DEMO_HEARD_REFUSED:
        printf("first contact: refused ");
        print_address(got.peer);
        printf(", which is not one of this board's peers or contacts. Save it as a contact, or "
               "type 'accept', to let it in, and have it try again.\n");
        link_asked(&companion, board_now(), got.peer, TERN_C_ASKED_NOT_CONTACT);
        break;
    case DEMO_HEARD_FAILED:
        printf("first contact: a frame of the handshake failed its checks; abandoned\n");
        break;
    case DEMO_HEARD_UNPAIRED:
        printf("first contact: complete, but the session could not be saved to flash, so it "
               "was not started\n");
        break;
    case DEMO_HEARD_UNSAVED:
        printf("a message arrived, but the session could not be saved to flash, so it is not "
               "shown\n");
        break;
    case DEMO_HEARD_OTHER:
        printf("(a %u-byte frame for someone else, at %d dBm)\n", ev->len, ev->rssi_dbm);
        break;
    case DEMO_HEARD_FORGED:
        printf("(a frame with our tag that failed authentication, at %d dBm)\n", ev->rssi_dbm);
        break;
    case DEMO_HEARD_MALFORMED:
        printf("(a %u-byte frame that is not Tern's, at %d dBm)\n", ev->len, ev->rssi_dbm);
        break;
    }
    if (got.reply_len != 0 && !send_contact(got.reply, got.reply_len, routed.back) &&
        got.reply[0] == TERN_HDR_CONTACT_1 + 2) {
        contact_gave_up();
    }
    /* The acknowledgement goes back by a route, no quieter than the node the message came from
     * needs to hear it. With no route to the peer it is not sent: the peer sends again. */
    for (size_t i = 0; i < got.acks; i++) {
        if (!tern_forward_send(&forward, board_now(), tern_route_id(demo.s[got.ack_slot[i]].peer),
                               got.ack[i], sizeof got.ack[i], false, routed.back)) {
            printf("(not acknowledged: no route back to the peer yet, or no room)\n");
        }
    }
}

static void poll_radio(void) {
    struct tern_radio_event ev;
    int n = tern_radio_poll(&radio, &ev);
    if (n < 0) {
        printf("radio error %d\n", n);
        return;
    }
    if (n == 0) {
        return;
    }
    switch (ev.kind) {
    case TERN_RADIO_TX_DONE:
        transmitting = false;
        if (route_out) {
            tern_route_sent(&route, board_now());
            route_out = false;
            route_on_air = true;
        }
        if (flood_out) {
            tern_flood_sent(&flood, board_now(), flood_handle);
            flood_out = false;
            if (flood_own >= 0) {
                /* Gone, which is as much as a group message is ever known to be. */
                link_state(&companion, flooding[flood_own].id, TERN_C_SENT, 0, 0);
                flooding[flood_own].id = 0;
                flood_own = -1;
            }
        }
        if (forward_out) {
            tern_forward_sent(&forward, board_now(), forward_handle);
            forward_out = false;
            /* The answer is listened for before the forwarder's next frame goes: the radio
             * cannot hear while it sends, and a board with several messages waiting would
             * otherwise send the second over the acknowledgement of the first. */
            forward_retry = board_now() + forward_quiet;
        }
        tern_radio_receive(&radio);
        break;
    case TERN_RADIO_RX_DONE:
        frames_in++;
        heard_total += tern_lora_airtime(&cfg.mod, ev.len);
        if (bench) {
            if (ev.len == BEACON_LEN && memcmp(ev.data, beacon_text, sizeof beacon_text) == 0) {
                bench_ours++;
                bench_rssi += ev.rssi_dbm;
                bench_snr_cdb += ev.snr_cdb;
            } else {
                bench_others++;
                printf("other: %u bytes at %d dBm, SNR %d cB:", ev.len, ev.rssi_dbm, ev.snr_cdb);
                for (unsigned i = 0; i < ev.len && i < 24; i++) {
                    printf(" %02x", ev.data[i]);
                }
                printf("\n");
            }
            break;
        }
        heard(&ev);
        break;
    case TERN_RADIO_RX_ERROR:
        if (!bench) {
            printf("(a damaged frame)\n");
        }
        break;
    }
}

/* Gives up the handshake this board began, once the forwarder has sent its frame as often as it
 * may with no answer, and forgets a handshake another board began and left. */
static void poll_contact(void) {
    uint8_t hdr, tag[TERN_FORWARD_TAG];
    while (tern_forward_contact_failed(&forward, &hdr, tag)) {
        if (!contact_kept.kept || hdr != contact_kept.hdr ||
            memcmp(tag, contact_kept.tag, sizeof tag) != 0) {
            continue;
        }
        contact_kept.kept = false;
        printf("first contact: no answer to message_%d after %d tries; given up. Is the other "
               "board on, on the same radio settings, and in range or reached by a relay? "
               "'routes' shows whether there is a route to it.\n",
               hdr - 0x50, forward.config.retries + 1);
        contact_gave_up();
    }
    if (demo_tick(&demo, board_now()) == DEMO_TICK_LAPSED) {
        printf("first contact: the board that began it went quiet; forgotten\n");
    }
}

/* Hands the oldest message waiting to the forwarder, or says why it waits. A message to a node
 * this board has no session with starts first contact with it, once no handshake is under way. */
/* Seals a group message under a nonce drawn for it and hands it to the flooder, which sends it
 * once, when the board's allowance for its own floods can pay. */
static void send_group(struct link_message *x) {
    static uint8_t frame[TERN_GROUP_MAX_FRAME];
    const struct tern_group *g = link_group_of(&companion, x);
    int place = -1;
    for (int i = 0; i < FLOODING; i++) {
        place = flooding[i].id == 0 && place < 0 ? i : place;
    }
    if (g == NULL) {
        link_state(&companion, x->id, TERN_C_NOT_DELIVERED, 0, 0); /* left since it was written */
        return;
    }
    if (place < 0 || board_now() < outgoing_retry) {
        link_state(&companion, x->id, TERN_C_WAITING, TERN_C_WAIT_RADIO, 0);
        return;
    }
    uint8_t nonce[TERN_GROUP_NONCE];
    uint32_t count;
    size_t len = x->text_len + TERN_GROUP_OVERHEAD;
    /* A count is spent whether or not the frame goes: the next is higher, which is all it must
     * be. */
    if (!board_random(NULL, nonce, sizeof nonce) || !link_group_count(&companion, &count) ||
        tern_group_seal(g, nonce, route.id, count, x->text, x->text_len, frame, sizeof frame) !=
            TERN_OK ||
        !tern_flood_send(&flood, board_now(), frame, len)) {
        outgoing_retry = board_now() + 1000000000LL; /* the flooder's room */
        return;
    }
    flooding[place].id = x->id;
    memcpy(flooding[place].nonce, nonce, sizeof nonce);
    tern_flood_id(frame, len, flooding[place].frame_id);
    link_taken(&companion, x->id);
}

/* Floods a position to a group, sealed with the flag `node` set, once the allowance for the
 * board's own floods would still hold a 255-byte frame of words after it (draft/positions.md,
 * "Words first"). Nothing answers it. */
static void send_group_position(const struct link_position_out *out) {
    static uint8_t frame[TERN_GROUP_MAX_FRAME];
    size_t len = out->len + TERN_GROUP_OVERHEAD;
    uint8_t nonce[TERN_GROUP_NONCE];
    uint32_t count;
    int place = -1;
    for (int i = 0; i < FLOODING; i++) {
        place = !positioning[i].on && place < 0 ? i : place;
    }
    if (place < 0 || !tern_flood_own_room(&flood, board_now(), len)) {
        return; /* asked again on the next poll: the allowance fills as time goes */
    }
    if (!board_random(NULL, nonce, sizeof nonce) || !link_group_count(&companion, &count) ||
        tern_group_seal_node(&companion.groups[out->place].g, nonce, route.id, count,
                             out->plaintext, out->len, frame, sizeof frame) != TERN_OK ||
        !tern_flood_send(&flood, board_now(), frame, len)) {
        outgoing_retry = board_now() + 1000000000LL; /* the flooder's room */
        return;
    }
    positioning[place].on = true;
    positioning[place].stopped = out->cell.precision == 0;
    positioning[place].place = out->place;
    memcpy(positioning[place].group, companion.groups[out->place].id, TERN_COMPANION_GROUP);
    tern_flood_id(frame, len, positioning[place].frame_id);
    link_position_sent(&companion, out, board_now());
    printf("position to the group \"%.*s\": %u bytes, precision %u\n",
           (int)companion.groups[out->place].name_len,
           (const char *)companion.groups[out->place].name, (unsigned)len,
           (unsigned)out->cell.precision);
}

/* Seals the position the link says is due, to a contact as a message for its node, which the
 * forwarder sends until it is acknowledged or given up, as it does a message; or to a group. */
static void send_position(void) {
    static uint8_t frame[TERN_FORWARD_FRAME_MAX];
    struct link_position_out out;
    if (board_now() < outgoing_retry || !link_position_next(&companion, board_now(), true, &out)) {
        return;
    }
    if (out.group) {
        send_group_position(&out);
        return;
    }
    struct pending *p = pending_free();
    int slot = demo_peer(&demo, out.address);
    if (p == NULL || slot < 0) {
        return; /* no room, or no session: a position does not begin first contact */
    }
    uint32_t counter = demo.s[slot].session.tx.next;
    if (demo_seal_node(&demo, slot, out.plaintext, out.len, frame) != DEMO_OK) {
        outgoing_retry = board_now() + 1000000000LL;
        return;
    }
    size_t len = out.len + TERN_UNICAST_OVERHEAD;
    if (pending_tagged(&frame[TERN_FORWARD_HEAD]) != NULL ||
        !tern_forward_send(&forward, board_now(), tern_route_id(out.address), frame, len, true,
                           INT8_MIN)) {
        outgoing_retry = board_now() + 1000000000LL; /* sealed again when next due */
        return;
    }
    *p = (struct pending){
        .id = PENDING_POSITION, .counter = counter, .slot = slot, .at = board_now()};
    memcpy(p->tag, &frame[TERN_FORWARD_HEAD], TERN_FORWARD_TAG);
    memcpy(p->to, out.address, TERN_ADDRESS_LEN);
    link_position_sent(&companion, &out, board_now());
    printf("position #%lu to share: %u bytes, precision %u\n", (unsigned long)counter,
           (unsigned)len, (unsigned)out.cell.precision);
}

static void poll_outgoing(void) {
    struct link_message *x = link_outgoing(&companion);
    if (x == NULL) {
        send_position(); /* words first */
        return;
    }
    if (x->kind == LINK_KIND_GROUP) {
        send_group(x);
        return;
    }
    if (demo_peer(&demo, x->address) < 0) {
        link_state(&companion, x->id, TERN_C_WAITING, TERN_C_WAIT_SESSION, 0);
        bool idle = demo.h.phase == DEMO_IDLE || demo.h.phase == DEMO_ANSWERED;
        if (!idle) {
            return;
        }
        uint8_t frame[TERN_CONTACT_MAX_FRAME];
        size_t len;
        enum demo_result r = demo_contact(&demo, x->address, board_now(), frame, &len);
        if (r != DEMO_OK) {
            printf("no contact made: %s\n", result_text(r));
            link_state(&companion, x->id, TERN_C_NOT_DELIVERED, 0, 0);
            return;
        }
        contacting = true;
        memcpy(contacting_peer, x->address, TERN_ADDRESS_LEN);
        if (!send_contact(frame, len, INT8_MIN)) {
            contact_gave_up();
        }
        return;
    }
    struct pending *p = pending_free();
    if (p == NULL) {
        /* As many of this board's messages are on their way as it keeps track of. */
        link_state(&companion, x->id, TERN_C_WAITING, TERN_C_WAIT_RADIO, 0);
        return;
    }
    if (board_now() < outgoing_retry) {
        return;
    }
    bool gone;
    if (hand_over(x, p, &gone)) {
        link_taken(&companion, x->id);
    } else if (gone) {
        link_state(&companion, x->id, TERN_C_NOT_DELIVERED, 0, 0);
    } else {
        outgoing_retry = board_now() + 1000000000LL; /* the forwarder's room, or the flash */
    }
}

/* Why a message the forwarder has for that address is not yet delivered, as far as the board can
 * name it. */
static uint8_t pending_reason(tern_time now, const uint8_t address[TERN_ADDRESS_LEN]) {
    uint32_t next;
    uint16_t metric;
    if (!tern_route_next(&route, tern_route_id(address), &next, &metric)) {
        return TERN_C_WAIT_ROUTE;
    }
    if (!tern_duty_allows(&duty, now, tern_lora_airtime(&cfg.mod, TERN_FORWARD_FRAME_MAX))) {
        return TERN_C_WAIT_REGION;
    }
    return TERN_C_WAIT_UNNAMED;
}

/* Sends what the forwarder has to send: this board's messages and acknowledgements, and frames it
 * passes on. A frame it hands over is the forwarder's still: once it has gone, or if it cannot go
 * now, the forwarder is told, and keeps count of the tries itself. */
static void poll_forward(void) {
    static uint8_t frame[TERN_FORWARD_FRAME_MAX];
    tern_time now = board_now();
    uint8_t tag[TERN_FORWARD_TAG];
    while (tern_forward_failed(&forward, tag)) {
        struct pending *p = pending_tagged(tag);
        if (p != NULL) {
            printf("not delivered #%lu: no acknowledgement, having gone %u time%s. Is the other "
                   "board on and in range? 'routes' shows whether there is a route to it.\n",
                   (unsigned long)p->counter, p->goes, p->goes == 1 ? "" : "s");
            if (p->id == PENDING_POSITION) {
                link_position_done(&companion, p->to);
            } else {
                link_state(&companion, p->id, TERN_C_NOT_DELIVERED, 0, 0);
            }
            p->id = 0;
        }
    }
    for (int i = 0; i < PENDING; i++) {
        if (pending[i].id == PENDING_POSITION) {
            continue; /* not a message: nothing to tell of it */
        }
        if (pending[i].id != 0 && !link_wanted(&companion, pending[i].id)) {
            /* An invite whose group was left since it was handed over: the forwarder lets go of
             * it as if it had been acknowledged, and it is not sent again. */
            (void)tern_forward_acked(&forward, pending[i].tag);
            pending[i].id = 0;
        }
        if (pending[i].id != 0) {
            link_state(&companion, pending[i].id, TERN_C_WAITING,
                       pending_reason(now, demo.s[pending[i].slot].peer), 0);
        }
    }
    if (transmitting || now < forward_retry || now < tern_forward_due(&forward) || held()) {
        return;
    }
    int8_t dbm;
    enum tern_forward_kind kind;
    size_t len = tern_forward_poll(&forward, now, frame, &dbm, &kind, &forward_handle);
    if (len == 0) {
        return;
    }
    /* Not offered to the radio if the region's limit would refuse it: it goes back to the
     * forwarder, and is asked for again a second on. */
    tern_time air = 0;
    if (tern_forward_wanted(&forward, forward_handle) &&
        tern_duty_allows(&duty, now, tern_lora_airtime(&cfg.mod, (uint32_t)len))) {
        air = transmit_at(frame, len, dbm);
    }
    if (air == 0) {
        tern_forward_withdrawn(&forward, now, forward_handle);
        forward_retry = now + 1000000000LL;
        return;
    }
    forward_out = true;
    /* The answer waits up to JITTER airtimes of itself and then takes its own: the same frame
     * passed on, or an acknowledgement, which is shorter. */
    forward_quiet = (forward.config.jitter + 1) * air;
    struct pending *p = kind == TERN_FORWARD_OWN ? pending_tagged(&frame[TERN_FORWARD_HEAD]) : NULL;
    if (p != NULL) {
        const struct link_message *x = NULL;
        for (size_t i = 0; i < LINK_MESSAGES; i++) {
            x = companion.messages[i].used && companion.messages[i].id == p->id
                    ? &companion.messages[i]
                    : x;
        }
        p->goes += p->goes < UINT8_MAX;
        printf("sent #%lu%s \"%.*s\": %u bytes at %d dBm by %02x%02x%02x%02x, %lld.%03lld ms on "
               "the air\n",
               (unsigned long)p->counter, p->goes > 1 ? " again" : "", x ? (int)x->text_len : 0,
               x ? (const char *)x->text : "", (unsigned)len, dbm, frame[3], frame[4], frame[5],
               frame[6], (long long)(air / 1000000), (long long)(air / 1000 % 1000));
    } else if (kind == TERN_FORWARD_OWN && tern_forward_contact(frame[0])) {
        printf("first contact: sent message_%d, %u bytes at %d dBm by %02x%02x%02x%02x\n",
               frame[0] - 0x50, (unsigned)len, dbm, frame[3], frame[4], frame[5], frame[6]);
    } else if (kind == TERN_FORWARD_RELAY) {
        printf("passed on a %u-byte frame for %02x%02x%02x%02x by %02x%02x%02x%02x, at %d dBm\n",
               (unsigned)len, frame[7], frame[8], frame[9], frame[10], frame[3], frame[4], frame[5],
               frame[6], dbm);
    }
}

/* Sends what the flooder has to send: this board's group messages, and frames for every node
 * that it passes on. A frame of its own waits for the board's allowance, and the client is told
 * so; one to pass on that the allowance cannot pay for is dropped by the flooder. */
/* The board's presence card: what poll_card() keeps, and poll_flood() tells it when one goes. */
static bool card_sent;                 /* one has gone since the board started */
static tern_time card_last;            /* when */
static tern_time card_next;            /* when the next is due, or 0 for not yet drawn */
static bool card_pending;              /* one is with the flooder, not yet on the air */
static uint8_t card_id[TERN_FLOOD_ID]; /* and its id */
static void card_went(tern_time now);

static void poll_flood(void) {
    static uint8_t frame[TERN_FLOOD_FRAME_MAX];
    tern_time now = board_now();
    /* A group message whose group was left since it was handed over is not to go. */
    for (int i = 0; i < FLOODING; i++) {
        if (flooding[i].id != 0 && !link_wanted(&companion, flooding[i].id)) {
            (void)tern_flood_cancel(&flood, now, flooding[i].frame_id);
            if (flood_own != i) {
                flooding[i].id = 0; /* one on the air is let go of when it has gone */
            }
        }
    }
    /* A position whose group was left, or is no longer shared with, is not to go. */
    for (int i = 0; i < FLOODING; i++) {
        if (positioning[i].on &&
            !link_group_position_wanted(&companion, positioning[i].place, positioning[i].group,
                                        positioning[i].stopped)) {
            (void)tern_flood_cancel(&flood, now, positioning[i].frame_id);
            positioning[i].on = false;
        }
    }
    /* How busy the radio has been, for a relay to pass fewer on by: what it sent and what it
     * received whole. A frame it lost part-way is not counted, the radio not saying how long it
     * was. */
    tern_flood_radio(&flood, now, air_total + heard_total);
    tern_route_busy(&route, flood.busy);
    tern_time due = tern_flood_due(&flood);
    if (due != INT64_MAX && due > now + 1000000000LL) {
        for (int i = 0; i < FLOODING; i++) {
            if (flooding[i].id != 0 && link_wanted(&companion, flooding[i].id)) {
                tern_time wait = (due - now) / 1000000000LL + 1;
                link_state(&companion, flooding[i].id, TERN_C_WAITING, TERN_C_WAIT_BUDGET,
                           (uint16_t)(wait > UINT16_MAX ? UINT16_MAX : wait));
            }
        }
    }
    if (transmitting || now < flood_retry || now < due || held()) {
        return;
    }
    int8_t dbm;
    enum tern_flood_kind kind;
    size_t len = tern_flood_poll(&flood, now, frame, &dbm, &kind, &flood_handle);
    if (len == 0) {
        return;
    }
    tern_time air = 0;
    if (tern_flood_wanted(&flood, flood_handle) &&
        tern_duty_allows(&duty, now, tern_lora_airtime(&cfg.mod, (uint32_t)len))) {
        air = transmit_at(frame, len, dbm);
    }
    if (air == 0) {
        /* A frame of this board's goes back to wait; one to pass on is let go. */
        tern_flood_withdrawn(&flood, now, flood_handle);
        flood_retry = now + 1000000000LL;
        return;
    }
    flood_out = true;
    if (kind == TERN_FLOOD_RELAY) {
        printf("passed on a %u-byte frame for every node, at %d dBm\n", (unsigned)len, dbm);
        return;
    }
    uint8_t id[TERN_FLOOD_ID];
    tern_flood_id(frame, len, id);
    if (card_pending && memcmp(card_id, id, TERN_FLOOD_ID) == 0) {
        card_went(now);
    }
    for (int i = 0; i < FLOODING; i++) {
        if (positioning[i].on && memcmp(positioning[i].frame_id, id, TERN_FLOOD_ID) == 0) {
            positioning[i].on = false; /* on the air: nothing more to let go of */
        }
    }
    for (int i = 0; i < FLOODING; i++) {
        if (flooding[i].id != 0 &&
            memcmp(flooding[i].nonce, frame + TERN_FLOOD_HEAD, TERN_GROUP_NONCE) == 0) {
            printf("sent to a group: %u bytes at %d dBm, %lld.%03lld ms on the air\n",
                   (unsigned)len, dbm, (long long)(air / 1000000), (long long)(air / 1000 % 1000));
            flood_own = i; /* said to be sent once the radio says it has gone */
        }
    }
}

/* --- The board's presence card (draft/cards.md, "Sending") -------------------------------------
 *
 * None until a client turns cards on; the first then, and each next between CARD_EVERY / 2 and
 * 3 CARD_EVERY / 2 after the last, drawn afresh each time. Turning cards off and on again, or
 * changing the name, sends one no sooner than CARD_EVERY / 2 after the last, so that no client
 * makes the board send them more often. A card's number is one more than the last, kept in flash
 * before the card goes, so a restart never sends one again. */

#define CARD_RETRY TERN_S(60) /* after flash or the flooder said no */

/* Cards were turned off, or renamed: a card still waiting for the air is let go of, so that none
 * goes with cards off or with the old name, and the next is drawn again. */
static void card_changed(void) {
    if (card_pending) {
        (void)tern_flood_cancel(&flood, board_now(), card_id);
        card_pending = false;
    }
    card_next = 0;
}

/* The board's card went on the air: the next is drawn from now. */
static void card_went(tern_time now) {
    uint32_t r = 0;
    (void)board_random(NULL, (uint8_t *)&r, sizeof r);
    card_pending = false;
    card_sent = true;
    card_last = now;
    card_next = now + TERN_CARD_EVERY / 2 + (tern_time)(r % 1000001u) * (TERN_CARD_EVERY / 1000000);
}

static void poll_card(void) {
    static uint8_t frame[TERN_CARD_MAX];
    tern_time now = board_now();
    if (!settings.cards) {
        card_next = 0;
        return;
    }
    if (card_pending) {
        return; /* one at a time: the next is drawn when this one goes */
    }
    if (card_next == 0) {
        tern_time soonest = card_sent ? card_last + TERN_CARD_EVERY / 2 : now;
        card_next = soonest > now ? soonest : now;
    }
    if (now < card_next) {
        return;
    }
    uint32_t number = 0;
    (void)store_load(NULL, "card", &number, sizeof number);
    number++;
    size_t len =
        tern_card_write(&demo.id, number, settings.card_name, settings.card_name_len, frame);
    if (len == 0 || !store_save(NULL, "card", &number, sizeof number) ||
        !tern_flood_send(&flood, now, frame, len)) {
        card_next = now + CARD_RETRY;
        return;
    }
    card_pending = true;
    tern_flood_id(frame, len, card_id);
    printf("card %lu waits for the air: %u bytes\n", (unsigned long)number, (unsigned)len);
}

/* Sends what the router has to send: its announces, and its requests for routes. A frame the
 * router has handed over is its only copy, and what it said is counted as said - a retraction as
 * one of its three - so the frame is kept until it has gone, and the router is not asked for
 * another while the region's limit would refuse one. */
static void poll_route(void) {
    tern_time now = board_now();
    if (transmitting) {
        return;
    }
    if (route_len == 0) {
        if (now < tern_route_due(&route) ||
            !tern_duty_allows(&duty, now, tern_lora_airtime(&cfg.mod, TERN_ROUTE_FRAME_MAX))) {
            return;
        }
        /* Numbers are stored ahead only once something is due, and only one until something has
         * gone on the air, so that a board restarting over and over without sending uses up at
         * most one a restart. */
        if (route_on_air ? tern_route_numbers_left(&route) < NUMBER_SAVE / 2
                         : tern_route_numbers_left(&route) == 0) {
            uint16_t kept = (uint16_t)(route.number + (route_on_air ? NUMBER_SAVE : 1));
            if (store_save(NULL, "number", &kept, sizeof kept)) {
                tern_route_kept(&route, kept);
            }
        }
        route_len = tern_route_poll(&route, now, route_frame, &route_dbm);
        route_retry = 0;
    }
    if (route_len == 0 || now < route_retry ||
        !tern_duty_allows(&duty, now, tern_lora_airtime(&cfg.mod, (uint32_t)route_len)) || held()) {
        return;
    }
    if (transmit_at(route_frame, route_len, route_dbm) != 0) {
        route_out = true;
        route_len = 0;
    } else {
        route_retry = now + 1000000000LL; /* the radio, or the flash: not every turn of the loop */
    }
}

/* --- The bench -------------------------------------------------------------------------------- */

/* Sets the radio up again after a change to cfg, and listens. */
static bool bench_configure(void) {
    int err = tern_radio_configure(&radio, &cfg);
    power_now = err == TERN_OK ? cfg.tx_power_dbm : POWER_UNSET;
    if (err == TERN_OK) {
        err = tern_radio_receive(&radio);
    }
    if (err != TERN_OK) {
        printf("radio error %d\n", err);
    }
    return err == TERN_OK;
}

static void bench_counts(void) {
    struct board_radio_counts counts;
    const struct board_radio_counts *c = &counts;
    board_radio_counts(&counts);
    int32_t rssi = bench_ours ? bench_rssi / (int32_t)bench_ours : 0;
    int32_t snr = bench_ours ? bench_snr_cdb / (int32_t)bench_ours : 0;
    printf("counts: sync 0x%02X, %d dBm, sent %lu, preambles %lu, headers %lu, header errors %lu, "
           "crc errors %lu, frames %lu, test frames %lu, others %lu, mean %ld dBm, SNR %ld cB\n",
           cfg.sync_word, cfg.tx_power_dbm, (unsigned long)beacon_sent, (unsigned long)c->preambles,
           (unsigned long)c->headers, (unsigned long)c->header_errors, (unsigned long)c->crc_errors,
           (unsigned long)c->frames, (unsigned long)bench_ours, (unsigned long)bench_others,
           (long)rssi, (long)snr);
}

static void poll_beacon(void) {
    uint8_t frame[BEACON_LEN];
    if (!beacon_running || transmitting) {
        return;
    }
    if (beacon_left == 0) {
        beacon_running = false;
        printf("beacon: done, %lu sent\n", (unsigned long)beacon_sent);
        return;
    }
    if (board_now() < beacon_next) {
        return;
    }
    memcpy(frame, beacon_text, sizeof beacon_text);
    frame[BEACON_LEN - 4] = (uint8_t)(beacon_number >> 24);
    frame[BEACON_LEN - 3] = (uint8_t)(beacon_number >> 16);
    frame[BEACON_LEN - 2] = (uint8_t)(beacon_number >> 8);
    frame[BEACON_LEN - 1] = (uint8_t)beacon_number;
    if (transmit(frame, sizeof frame) != 0) {
        beacon_next = board_now() + beacon_gap;
        beacon_left--;
        beacon_sent++;
        beacon_number++;
    } else {
        /* Refused, and said why: it is still owed, and tried again no sooner than a second on.
         * 'bench off' ends a run that cannot finish. */
        beacon_next = board_now() + (beacon_gap > 1000000000LL ? beacon_gap : 1000000000LL);
    }
}

/* What the router signs its announces with, and checks its neighbours' with: the node's identity
 * key, Ed25519, as the specification's "Signed" requires. */
static void route_sign(void *ctx, const uint8_t *m, size_t len, uint8_t sig[TERN_ANNOUNCE_SIG]) {
    tern_identity_sign(ctx, m, len, sig);
}

static bool route_verify(void *ctx, const uint8_t address[TERN_ADDRESS_LEN], const uint8_t *m,
                         size_t len, const uint8_t sig[TERN_ANNOUNCE_SIG]) {
    (void)ctx;
    return tern_address_valid(address) && tern_address_verify(address, m, len, sig);
}

/* The band a region's radios keep to, for a frequency set by hand: New Zealand allows its
 * profile's power only above 920 MHz. */
static void bench_band(uint32_t *lo, uint32_t *hi) {
    if (region == tern_region(TERN_REGION_EU868)) {
        *lo = 863000000;
        *hi = 870000000;
    } else if (region == tern_region(TERN_REGION_AU915)) {
        *lo = 915000000;
        *hi = 928000000;
    } else if (region == tern_region(TERN_REGION_NZ915)) {
        *lo = 920000000;
        *hi = 928000000;
    } else {
        *lo = 902000000;
        *hi = 928000000;
    }
}

/* The commands that move the board to another channel or modulation. */
static bool bench_channel(const char *line) {
    unsigned long v;
    struct tern_radio_config was = cfg;
    uint32_t lo, hi;
    bench_band(&lo, &hi);
    if (sscanf(line, "freq %lu", &v) == 1) {
        /* The whole channel inside the band, not only its centre. */
        if (v < lo + cfg.mod.bw_hz / 2 || v > hi - cfg.mod.bw_hz / 2) {
            printf("not in %s's band, %lu to %lu Hz, with %lu Hz of bandwidth\n", region->name,
                   (unsigned long)lo, (unsigned long)hi, (unsigned long)cfg.mod.bw_hz);
            return true;
        }
        cfg.freq_hz = (uint32_t)v;
    } else if (sscanf(line, "sf %lu", &v) == 1) {
        if (v < 7 || v > 12) {
            printf("a spreading factor is 7 to 12\n");
            return true;
        }
        cfg.mod.sf = (uint8_t)v;
    } else if (sscanf(line, "bw %lu", &v) == 1) {
        if ((v != 62500 && v != 125000 && v != 250000 && v != 500000) || cfg.freq_hz < lo + v / 2 ||
            cfg.freq_hz > hi - v / 2) {
            printf("a bandwidth is 62500, 125000, 250000 or 500000 Hz, and inside the band\n");
            return true;
        }
        cfg.mod.bw_hz = (uint32_t)v;
    } else {
        return false;
    }
    if (!bench) {
        printf("that is for the bench: 'bench on' first\n");
        cfg = was;
    } else if (transmitting || beacon_running) {
        printf("busy: frames are still being sent\n");
        cfg = was;
    } else {
        off_profile = true;
        if (bench_configure()) {
            printf("radio: %lu Hz, SF%u, %lu Hz\n", (unsigned long)cfg.freq_hz, cfg.mod.sf,
                   (unsigned long)cfg.mod.bw_hz);
        }
    }
    return true;
}

/* The bench's commands. False if the line is not one of them. */
static bool bench_command(char *line) {
    unsigned long a, b;
    long dbm;
    if (strcmp(line, "bench on") == 0 || strcmp(line, "bench off") == 0) {
        bench = line[7] == 'n';
        beacon_running = false;
        printf("bench %s\n", bench ? "on: no routing or first contact until 'bench off'" : "off");
        return true;
    }
    if (strcmp(line, "counts") == 0) {
        bench_counts();
        return true;
    }
    if (strcmp(line, "counts reset") == 0) {
        board_radio_counts_reset();
        beacon_sent = bench_ours = bench_others = 0;
        bench_rssi = bench_snr_cdb = 0;
        printf("counts reset\n");
        return true;
    }
    if (bench_channel(line)) {
        return true;
    }
    bool is_sync = sscanf(line, "sync %lx", &a) == 1;
    bool is_power = !is_sync && sscanf(line, "power %ld", &dbm) == 1;
    bool is_beacon = !is_sync && !is_power && sscanf(line, "beacon %lu %lu", &a, &b) == 2;
    if (!is_sync && !is_power && !is_beacon) {
        return false;
    }
    if (!bench) {
        printf("that is for the bench: 'bench on' first\n");
    } else if (transmitting || beacon_running) {
        printf("busy: frames are still being sent\n");
    } else if (is_sync) {
        if (a > 0xFF) {
            printf("a sync word is one byte\n");
            return true;
        }
        cfg.sync_word = (uint8_t)a;
        off_profile = off_profile || cfg.sync_word != TERN_SYNC_WORD;
        if (bench_configure()) {
            printf("sync word 0x%02X, which an SX126x takes as 0x%04X\n", cfg.sync_word,
                   tern_sync_word_sx126x(cfg.sync_word));
        }
    } else if (is_power) {
        struct tern_radio_config allowed;
        if (!board_power_ok((int)dbm) ||
            tern_region_radio(region, (int8_t)dbm, CONFIG_TERN_ANTENNA_DBI, &allowed) != TERN_OK) {
            printf("%ld dBm is not a power this radio gives and %s allows\n", dbm, region->name);
            return true;
        }
        cfg.tx_power_dbm = (int8_t)dbm;
        if (bench_configure()) {
            printf("power %d dBm\n", cfg.tx_power_dbm);
        }
    } else {
        beacon_left = (uint32_t)a;
        beacon_gap = (tern_time)b * 1000000;
        beacon_next = board_now();
        beacon_running = true;
        printf("beacon: %lu frames of %d bytes, %lu ms apart\n", a, BEACON_LEN, b);
    }
    return true;
}

/* --- The companion link ----------------------------------------------------------------------- */

/* One frame to a client: as a notification over Bluetooth, or on the USB port, wrapped, after
 * whatever the console has printed. */
/* The answer to what the board last asked of the link itself (ask()). */
static struct tern_companion_msg board_answer;

static void link_out(void *ctx, unsigned conn, const uint8_t *frame, size_t len) {
    uint8_t wrapped[TERN_COMPANION_STREAM_MAX];
    (void)ctx;
    if (conn == LINK_BOARD) {
        /* The board's own connection: its answers are kept for ask(), and it has no use for
         * news of what it can see for itself. */
        if (len >= 1 && frame[0] >= TERN_C_OK && frame[0] < TERN_C_SELF) {
            (void)tern_companion_read(&board_answer, frame, len);
        }
        return;
    }
    if (conn == LINK_BLE) {
        ble_send(frame, len);
        return;
    }
    size_t n = tern_companion_wrap(frame, len, wrapped);
    fflush(stdout);
    if (n != 0) {
        board_console_write(wrapped, n);
    }
}

/* How long until the longest frame could go, by the region's limit: worked out on a copy of the
 * account, a slice at a time. */
static uint32_t duty_wait_ms(tern_time now) {
    static struct tern_duty ahead;
    tern_time air = tern_lora_airtime(&cfg.mod, 255);
    if (tern_duty_allows(&duty, now, air)) {
        return 0;
    }
    ahead = duty;
    for (int k = 1; k <= TERN_DUTY_SLICES; k++) {
        if (tern_duty_allows(&ahead, now + k * duty.slice, air)) {
            return (uint32_t)(k * duty.slice / 1000000);
        }
    }
    return (uint32_t)(TERN_DUTY_SLICES * duty.slice / 1000000);
}

/* The SNR, in quarter-dB, a neighbour's last announce was heard at, or 0 if it is not known. */
static int8_t snr_of(uint32_t id) {
    int8_t snr = 0;
    for (size_t k = 0; k < NEIGHBOURS; k++) {
        snr = heard_snr[k].id == id ? heard_snr[k].snr : snr;
    }
    return snr;
}

static void link_view(void *ctx, struct link_view *v) {
    tern_time now = board_now();
    (void)ctx;
    memset(v, 0, sizeof *v);
    memcpy(v->address, demo.id.address, TERN_ADDRESS_LEN);
    v->role = route.config.relay ? 1 : 0;
    v->region = region->name;
    v->power = cfg.tx_power_dbm;
    v->time = clock_now();
    v->cards = settings.cards;
    v->card_name_len = settings.card_name_len;
    memcpy(v->card_name, settings.card_name, settings.card_name_len);
    for (int i = 0; i < NEIGHBOURS && v->n_neighbours < LINK_NEIGHBOURS; i++) {
        const struct tern_route_neighbour *n = &neighbours[i];
        if (!n->used) {
            continue;
        }
        int8_t snr = snr_of(n->id);
        tern_time ago = (now - n->heard) / 1000000000LL;
        v->neighbours[v->n_neighbours++] = (struct link_neighbour){
            .id = n->id,
            .role = n->relay ? 1 : 0,
            .snr = snr,
            .heard = (uint16_t)(ago > 0xFFFF ? 0xFFFF : ago),
        };
    }
    if (region->duty_ppm < TERN_DUTY_UNLIMITED) {
        v->period_s = region->duty_window_s;
        v->allowed_ms = (uint32_t)(duty.limit / 1000000);
        v->used_ms = (uint32_t)(tern_duty_used(&duty, now) / 1000000);
        v->wait_ms = duty_wait_ms(now);
    }
    v->millivolts = battery_mv;
    v->percent = power_percent(battery_mv); /* 255, unknown, when there is no battery */
}

static const struct tern_region *region_named(const uint8_t *name, size_t len) {
    for (int id = TERN_REGION_US915; id < TERN_REGION_END; id++) {
        const struct tern_region *r = tern_region((enum tern_region_id)id);
        if (r != NULL && strlen(r->name) == len && memcmp(r->name, name, len) == 0) {
            return r;
        }
    }
    return NULL;
}

/* A setting from a client. Region, role and power take a restart, since the radio and the router
 * were set up with them; the passkey is for the next Bluetooth pairing. */
static uint8_t link_set(void *ctx, const struct tern_companion_msg *m) {
    struct settings next = settings;
    struct tern_radio_config check;
    bool restart = true;
    (void)ctx;
    switch (m->setting) {
    case TERN_C_SET_REGION: {
        const struct tern_region *r = region_named(m->text, m->text_len);
        if (r == NULL ||
            tern_region_radio(r, cfg.tx_power_dbm, CONFIG_TERN_ANTENNA_DBI, &check) != TERN_OK) {
            return TERN_C_ERR_REFUSED;
        }
        if (r == region) {
            return 0;
        }
        for (int id = TERN_REGION_US915; id < TERN_REGION_END; id++) {
            next.region = tern_region((enum tern_region_id)id) == r ? (uint8_t)id : next.region;
        }
        break;
    }
    case TERN_C_SET_ROLE:
        if (m->role > 1) {
            return TERN_C_ERR_REFUSED;
        }
        if (m->role == (route.config.relay ? 1 : 0)) {
            return 0;
        }
        next.role = m->role;
        break;
    case TERN_C_SET_POWER:
        if (!board_power_ok(m->power) ||
            tern_region_radio(region, m->power, CONFIG_TERN_ANTENNA_DBI, &check) != TERN_OK) {
            return TERN_C_ERR_REFUSED;
        }
        if (m->power == cfg.tx_power_dbm) {
            return 0;
        }
        next.power = m->power;
        break;
    case TERN_C_SET_PASSKEY:
        if (m->passkey > 999999 && m->passkey != PASSKEY_RANDOM) {
            return TERN_C_ERR_REFUSED;
        }
        next.passkey = m->passkey;
        restart = false;
        break;
    case TERN_C_SET_CARDS:
        if (m->cards > 1) {
            return TERN_C_ERR_REFUSED;
        }
        next.cards = m->cards;
        restart = false;
        break;
    case TERN_C_SET_CARD_NAME: /* UTF-8 and short enough: the link read it so */
        memcpy(next.card_name, m->text, m->text_len);
        next.card_name_len = m->text_len;
        restart = false;
        break;
    default:
        return TERN_C_ERR_UNKNOWN;
    }
    if (!store_save(NULL, "settings", &next, sizeof next)) {
        return TERN_C_ERR_NOT_NOW;
    }
    bool renamed = next.card_name_len != settings.card_name_len ||
                   memcmp(next.card_name, settings.card_name, next.card_name_len) != 0;
    bool off = settings.cards && !next.cards;
    settings = next;
    restart_due = restart_due || restart;
    ble_passkey(settings.passkey);
    if (renamed || off) {
        card_changed();
    }
    return 0;
}

static void link_set_time(void *ctx, uint32_t time) {
    (void)ctx;
    clock_base = time;
    clock_at = board_now();
}

static bool link_session(void *ctx, const uint8_t address[TERN_ADDRESS_LEN]) {
    (void)ctx;
    return demo_peer(&demo, address) >= 0;
}

static uint8_t link_end_session(void *ctx, const uint8_t address[TERN_ADDRESS_LEN]) {
    (void)ctx;
    int slot = demo_peer(&demo, address);
    if (slot < 0) {
        return 0;
    }
    if (!demo_forget(&demo, slot)) {
        return TERN_C_ERR_NOT_NOW; /* the flash would not forget it, so it is kept */
    }
    /* The link says what became of them, after it has answered. */
    pending_drop(slot, false);
    return 0;
}

static bool link_trusted(void *ctx, const uint8_t address[TERN_ADDRESS_LEN]) {
    (void)ctx;
    return link_contact(&companion, address);
}

static uint8_t link_why(void *ctx, const uint8_t address[TERN_ADDRESS_LEN]) {
    return link_session(ctx, address) ? pending_reason(board_now(), address) : TERN_C_WAIT_SESSION;
}

static bool link_load(void *ctx, void *buf, size_t len) {
    return store_load(ctx, "contacts", buf, len);
}

static bool link_save(void *ctx, const void *buf, size_t len) {
    return store_save(ctx, "contacts", buf, len);
}

/* A board that had an identity before it kept its message ids ran a build that counted them from
 * 1 at every start. Clients may hold ids of that address, so it goes on from past any such a
 * build is likely to have given, and not from 1 again. */
static bool link_load_groups(void *ctx, void *buf, size_t len) {
    return store_load(ctx, "groups", buf, len);
}

static bool link_save_groups(void *ctx, const void *buf, size_t len) {
    return store_save(ctx, "groups", buf, len);
}

static bool link_load_ids(void *ctx, uint32_t *next) {
    if (store_load(ctx, "ids", next, sizeof *next)) {
        return true;
    }
    if (had_identity) {
        *next = IDS_EARLIER;
        return true;
    }
    return false;
}

static bool link_save_ids(void *ctx, uint32_t next) {
    return store_save(ctx, "ids", &next, sizeof next);
}

static bool link_load_count(void *ctx, uint32_t *next) {
    return store_load(ctx, "gcount", next, sizeof *next);
}

static bool link_save_count(void *ctx, uint32_t next) {
    return store_save(ctx, "gcount", &next, sizeof next);
}

static size_t link_load_writers(void *ctx, uint8_t *buf, size_t cap) {
    (void)ctx;
    return plat_store_read("writers", buf, cap);
}

/* The groups' writers are saved like a message: only where the store has room to spare for what
 * the board must always be able to save (plat_store_save_if_room()). One that has not keeps what it
 * last saved, and after a restart knows its writers as they were then. */
static bool link_save_writers(void *ctx, const uint8_t *buf, size_t len) {
    (void)ctx;
    return plat_store_save_if_room("writers", buf, len);
}

/* A board that keeps hearing its groups writes their writers to flash this often and no more,
 * for the flash's sake. A restart with no warning can so forget the counts of the last minute,
 * and read those frames again if someone sends them again. */
#define WRITERS_EVERY_S 60
static tern_time writers_kept; /* when they were last written, or 0 if not since the start */

static void poll_writers(void) {
    tern_time now = board_now();
    if (writers_kept != 0 && now - writers_kept < (tern_time)WRITERS_EVERY_S * 1000000000LL) {
        return;
    }
    if (link_keep_writers(&companion)) {
        writers_kept = now != 0 ? now : 1;
    }
}

static size_t link_load_message(void *ctx, size_t place, uint8_t *buf, size_t cap) {
    (void)ctx;
    return plat_message_load(place, buf, cap);
}

static enum link_saved link_save_message(void *ctx, size_t place, const uint8_t *buf, size_t len) {
    (void)ctx;
    return plat_message_save(place, buf, len);
}

static bool link_load_state(void *ctx, size_t place, uint64_t *state) {
    (void)ctx;
    return plat_state_load(place, state);
}

static bool link_save_state(void *ctx, size_t place, uint64_t state) {
    (void)ctx;
    return plat_state_save(place, state);
}

/* --- The console ------------------------------------------------------------------------------ */

/* The sessions this board holds, by the numbers 'to' and 'drop' take. */
/* Asks the link for something as a client would, and says so if it is refused. True if it was
 * done: the answer is then in board_answer. */
static bool ask(struct tern_companion_msg *q) {
    uint8_t frame[TERN_COMPANION_MAX_FRAME];
    q->seq = 1;
    size_t len = tern_companion_write(q, frame);
    board_answer = (struct tern_companion_msg){.type = TERN_C_ERROR, .code = TERN_C_ERR_MALFORMED};
    if (len != 0) {
        link_receive(&companion, LINK_BOARD, board_now(), frame, len);
    }
    if (board_answer.type != TERN_C_ERROR) {
        return true;
    }
    switch (board_answer.code) {
    case TERN_C_ERR_FULL:
        printf("no room: this board holds %d groups and %d messages at most\n", LINK_GROUPS,
               LINK_MESSAGES);
        break;
    case TERN_C_ERR_NOT_HELD:
        printf("no such group, or no such invite: 'groups' lists both\n");
        break;
    case TERN_C_ERR_REFUSED:
        printf("nothing to send\n");
        break;
    case TERN_C_ERR_NOT_NOW:
        printf("not now: the flash would not take it\n");
        break;
    default:
        printf("not done: too long, or not text\n");
        break;
    }
    return false;
}

static void groups_list(void) {
    int n = 0;
    for (int i = 0; i < LINK_GROUPS; i++) {
        const struct link_group *g = &companion.groups[i];
        if (g->used) {
            printf("  %d  \"%.*s\"  id ", i + 1, (int)g->name_len, (const char *)g->name);
            for (size_t k = 0; k < sizeof g->id; k++) {
                printf("%02x", g->id[k]);
            }
            printf("\n");
            n++;
        }
    }
    if (n == 0) {
        printf("no groups: 'group new <name>' makes one, and 'group join <id>' takes an "
               "invite\n");
    }
    for (size_t i = 0; i < LINK_MESSAGES; i++) {
        const struct link_message *x = &companion.messages[i];
        if (x->used && x->kind == LINK_KIND_INVITE && x->state == TERN_C_RECEIVED) {
            printf("  invite %lu to \"%.*s\" from ", (unsigned long)x->id, (int)x->text_len,
                   (const char *)x->text);
            print_address(x->address);
            printf("\n");
        }
    }
}

/* The group a number typed at the console names, as 'groups' lists them; false, and said, if
 * there is none. */
static bool group_numbered(const char *text, struct tern_companion_msg *q, const char **rest) {
    char *end;
    long n = strtol(text, &end, 10);
    if (end == text || n < 1 || n > LINK_GROUPS || !companion.groups[n - 1].used) {
        printf("no such group: 'groups' lists them by number\n");
        return false;
    }
    memcpy(q->group, companion.groups[n - 1].id, sizeof q->group);
    while (*end == ' ') {
        end++;
    }
    *rest = end;
    return true;
}

/* 'group ...': everything a client can do with groups, from the console. */
static void group_command(const char *line) {
    struct tern_companion_msg q = {0};
    const char *rest = "";
    if (strncmp(line, "new ", 4) == 0) {
        size_t len = strlen(line + 4);
        if (len > TERN_COMPANION_NAME_MAX) {
            printf("too long: a group's name is at most %d bytes\n", TERN_COMPANION_NAME_MAX);
            return;
        }
        q.type = TERN_C_MAKE_GROUP;
        q.text_len = (uint8_t)len;
        memcpy(q.text, line + 4, len);
        if (ask(&q)) {
            printf("made the group \"%s\". 'group invite <number>' asks a peer in.\n", line + 4);
        }
    } else if (strncmp(line, "invite ", 7) == 0) {
        if (demo.last < 0) {
            printf("nobody to invite: %s\n", result_text(DEMO_UNPAIRED));
        } else if (group_numbered(line + 7, &q, &rest)) {
            q.type = TERN_C_SEND_INVITE;
            memcpy(q.address, demo.s[demo.last].peer, TERN_ADDRESS_LEN);
            if (ask(&q)) {
                printf("inviting ");
                print_address(q.address);
                printf("\n");
            }
        }
    } else if (strncmp(line, "join ", 5) == 0) {
        q.type = TERN_C_JOIN;
        q.id = (uint32_t)strtoul(line + 5, NULL, 10);
        if (ask(&q)) {
            printf("joined\n");
        }
    } else if (strncmp(line, "send ", 5) == 0) {
        if (group_numbered(line + 5, &q, &rest)) {
            size_t len = strlen(rest);
            if (len > TERN_COMPANION_TEXT_MAX) {
                printf("not sent: too long: at most %d bytes\n", TERN_COMPANION_TEXT_MAX);
                return;
            }
            q.type = TERN_C_SEND_GROUP;
            q.ref = plat_random32();
            q.text_len = (uint8_t)len;
            memcpy(q.text, rest, len);
            (void)ask(&q);
        }
    } else if (strncmp(line, "leave ", 6) == 0) {
        if (group_numbered(line + 6, &q, &rest)) {
            q.type = TERN_C_LEAVE_GROUP;
            if (ask(&q)) {
                printf("left\n");
            }
        }
    } else {
        printf("group new <name>, group invite <number>, group join <id>, group send <number> "
               "<text>, group leave <number>\n");
    }
}

static void peers(void) {
    if (demo_peers(&demo) == 0) {
        printf("sessions: none. %s\n", result_text(DEMO_UNPAIRED));
        return;
    }
    printf("sessions: %u of %d\n", (unsigned)demo_peers(&demo), DEMO_PEERS);
    for (int i = 0; i < DEMO_PEERS; i++) {
        const struct demo_state *s = &demo.s[i];
        if (s->role == 0) {
            continue;
        }
        printf("  %d%s as %s, %lu sent (next counter %lu), %lu heard, with ", i + 1,
               i == demo.last ? " ('send' goes here)" : "",
               s->role == TERN_INITIATOR ? "initiator" : "responder", (unsigned long)s->sent,
               (unsigned long)s->session.tx.next, (unsigned long)s->heard);
        print_address(s->peer);
        printf("\n");
    }
}

static void status(void) {
    char link[TERN_ADDRESS_LINK_LEN + 1], code[TERN_SHORT_CODE_LEN + 1];
    tern_address_link(demo.id.address, link);
    tern_short_code(demo.id.address, code);
    printf("this board's address: ");
    print_address(demo.id.address);
    printf("\nas a link: %s\nits short code: %s\n", link, code);
    printf("radio: %s%s, %lu Hz, SF%u, %lu Hz, CR 4/%u, %d dBm, sync word 0x%02X\n", region->name,
           off_profile ? " (not the region's settings)" : "", (unsigned long)cfg.freq_hz,
           cfg.mod.sf, (unsigned long)cfg.mod.bw_hz, 4 + cfg.mod.cr, cfg.tx_power_dbm,
           cfg.sync_word);
    if (region->duty_ppm < TERN_DUTY_UNLIMITED) {
        printf("transmitted: %lld ms of the %lld ms allowed in any %lu s\n",
               (long long)(tern_duty_used(&duty, board_now()) / 1000000),
               (long long)(duty.limit / 1000000), (unsigned long)region->duty_window_s);
    }
    unsigned heard_n = 0, up_n = 0, routed = 0;
    for (int i = 0; i < NEIGHBOURS; i++) {
        heard_n += neighbours[i].used;
        up_n += neighbours[i].used && neighbours[i].up;
    }
    for (int i = 0; i < DESTINATIONS; i++) {
        routed += destinations[i].used && destinations[i].sel != 0;
    }
    printf("routing: id %08lx, a %s, hears %u, %u of them both ways, routes to %u. 'routes' lists "
           "them.\n",
           (unsigned long)route.id, route.config.relay ? "relay" : "leaf", heard_n, up_n, routed);
    const struct tern_forward_counts *fc = &forward.counts;
    printf("forwarding: passed on %lu, sent %lu hops again and gave %lu up; dropped %lu with no "
           "route, %lu with no room; waited %lu times for a frame being received\n",
           (unsigned long)fc->passed_on, (unsigned long)fc->sent_again, (unsigned long)fc->given_up,
           (unsigned long)fc->no_route, (unsigned long)fc->no_room, (unsigned long)held_back);
    if (demo.h.phase == DEMO_INITIATING || demo.h.phase == DEMO_RESPONDING) {
        printf("first contact: under way, as the %s\n",
               demo.h.phase == DEMO_INITIATING ? "initiator" : "responder");
    }
    peers();
    if (board_now() < demo.accept_until) {
        printf("accepting contact from a new peer for another %lld s\n",
               (long long)((demo.accept_until - board_now()) / 1000000000LL));
    }
    unsigned kept = 0, held = 0;
    for (size_t i = 0; i < LINK_MESSAGES; i++) {
        kept += companion.messages[i].used && companion.messages[i].saved;
        held += companion.messages[i].used && !companion.messages[i].saved;
    }
    unsigned used, total, spare;
    if (plat_store_usage(&used, &total, &spare)) {
        printf("messages: %u saved, %u held only until a restart; storage: %u of %u entries used, "
               "%u to spare\n",
               kept, held, used, total, spare);
    }
}

static void routes(void) {
    tern_time now = board_now();
    printf("routing id %08lx, seq %u, announcing every %lld s at most\n", (unsigned long)route.id,
           route.seq, (long long)(route.interval / 1000000000LL));
    printf("neighbours:\n");
    for (int i = 0; i < NEIGHBOURS; i++) {
        const struct tern_route_neighbour *n = &neighbours[i];
        if (!n->used) {
            continue;
        }
        printf("  %08lx  %s  %s  needs %ld dBm to reach", (unsigned long)n->id,
               n->relay ? "relay" : "leaf ", n->up ? "up  " : "down", (long)(n->floor / 16));
        if (n->theirs != 0) {
            printf(", hears us with %d dB to spare", n->theirs - 128);
        } else {
            printf(", has not said it hears us");
        }
        printf(", heard %lld s ago\n", (long long)((now - n->heard) / 1000000000LL));
    }
    printf("routes:\n");
    for (int i = 0; i < DESTINATIONS; i++) {
        uint32_t next;
        uint16_t metric;
        if (destinations[i].used && tern_route_next(&route, destinations[i].id, &next, &metric)) {
            printf("  %08lx  by %08lx  %u ms on the air\n", (unsigned long)destinations[i].id,
                   (unsigned long)next, metric);
        }
    }
}

/* --- The self-test: a handshake between two nodes in this board's memory ---------------------- */

/* The board has one radio and, on a bench with one board, nobody to talk to. This runs the same
 * code two boards would, handing each node's frames to the other, to show that it works on this
 * chip and how long the arithmetic takes. Nothing goes on the air, and nothing is saved. */
struct memory_store {
    bool have_identity, have_session;
    uint8_t identity[36];
    struct demo_state session; /* the first session's record: the self-test makes one */
};

static bool memory_slot(struct memory_store *m, const char *key, size_t len, void **slot,
                        bool **have) {
    bool identity = strcmp(key, "identity") == 0;
    if (!identity && strcmp(key, "session") != 0) {
        return false; /* another session's record: there is none, and none is made */
    }
    *slot = identity ? (void *)m->identity : (void *)&m->session;
    *have = identity ? &m->have_identity : &m->have_session;
    return len == (identity ? sizeof m->identity : sizeof m->session);
}

static bool memory_load(void *ctx, const char *key, void *buf, size_t len) {
    void *slot;
    bool *have;
    if (!memory_slot(ctx, key, len, &slot, &have) || !*have) {
        return false;
    }
    memcpy(buf, slot, len);
    return true;
}

static bool memory_save(void *ctx, const char *key, const void *buf, size_t len) {
    void *slot;
    bool *have;
    if (!memory_slot(ctx, key, len, &slot, &have)) {
        return false;
    }
    memcpy(slot, buf, len);
    *have = true;
    return true;
}

static void selftest(void) {
    /* Two whole nodes, borrowed from the heap while the test runs rather than kept for it: the
     * classic ESP32 has too little static RAM to hold them besides the node's own. */
    struct selftest_memory {
        struct memory_store flash[2];
        struct demo node[2];
        uint8_t msg[TERN_UNICAST_MAX_PLAINTEXT];
    } *m = calloc(1, sizeof *m);
    if (m == NULL) {
        printf("selftest: no memory for it\n");
        return;
    }
    struct memory_store *flash = m->flash;
    struct demo *node = m->node;
    uint8_t *msg = m->msg;
    static const char *const step[] = {"respond to message_1", "message_2 to message_3",
                                       "message_3 to message_4", "accept message_4"};
    struct demo_received got;
    uint8_t frame[TERN_CONTACT_MAX_FRAME];
    size_t len = 0;
    bool ok = true;
    tern_time t0 = board_now(), t;

    for (int i = 0; i < 2; i++) {
        struct demo_store st = {&flash[i], memory_load, memory_save, board_random};
        ok = demo_start(&node[i], &st, 1000000000LL) && ok;
    }
    printf("selftest: two identities in %lld ms\n", (long long)((board_now() - t0) / 1000000));

    t = board_now();
    ok = ok && demo_contact(&node[0], node[1].id.address, 0, frame, &len) == DEMO_OK;
    printf("selftest: %-24s %4lld ms, %u-byte frame\n", "start",
           (long long)((board_now() - t) / 1000000), (unsigned)len);
    for (int i = 0; ok && i < 4; i++) {
        /* message_1 and message_3 go to the responder, node 1; the others come back. */
        t = board_now();
        enum demo_heard h = demo_receive(&node[i % 2 ? 0 : 1], 0, frame, len, msg, &got);
        ok = h == (i < 2 ? DEMO_HEARD_CONTACT : DEMO_HEARD_PAIRED);
        printf("selftest: %-24s %4lld ms, %u-byte frame\n", step[i],
               (long long)((board_now() - t) / 1000000), (unsigned)got.reply_len);
        memcpy(frame, got.reply, got.reply_len);
        len = got.reply_len;
    }
    for (int i = 0; ok && i < 2; i++) {
        /* A message each way, in the session the handshake made, and its acknowledgement. */
        uint8_t hello[5 + TERN_UNICAST_OVERHEAD];
        ok = demo_seal(&node[i], 0, (const uint8_t *)"hello", 5, hello) == DEMO_OK &&
             demo_receive(&node[1 - i], 0, hello, sizeof hello, msg, &got) == DEMO_HEARD_MESSAGE &&
             got.msg_len == 5 && memcmp(msg, "hello", 5) == 0 && got.acks == 1 &&
             demo_acked(&node[i], 0, 0, got.ack[0], sizeof got.ack[0]);
    }
    printf("selftest: %s, in %lld ms; %u bytes of this task's stack never used\n",
           ok ? "passed" : "FAILED", (long long)((board_now() - t0) / 1000000),
           (unsigned)plat_stack_unused());
    memset(m, 0, sizeof *m); /* the keys, gone before the memory is given back */
    free(m);
}

/* 'screen' says how long the screen stays on and whether it shows the bench pages; 'screen sleep
 * <seconds>' changes the first, 0 for never, and 'screen bench on|off' the second. */
static void screen_command(const char *rest) {
    unsigned long s;
    char extra;
    struct settings next = settings;
    if (rest[0] == '\0') {
        /* as it is */
    } else if (sscanf(rest, " sleep %lu %c", &s, &extra) == 1 && s <= UINT16_MAX) {
        next.screen_sleep = (uint16_t)s;
    } else if (strcmp(rest, " bench on") == 0) {
        next.screen_flags |= SCREEN_BENCH;
    } else if (strcmp(rest, " bench off") == 0) {
        next.screen_flags &= ~SCREEN_BENCH;
    } else {
        printf("screen sleep <seconds>, 0 to %u, or 0 for never; screen bench on|off\n",
               (unsigned)UINT16_MAX);
        return;
    }
    if (rest[0] != '\0') {
        if (!store_save(NULL, "settings", &next, sizeof next)) {
            printf("not saved: the flash refused it\n");
            return;
        }
        settings = next;
        screen_due = 0;
        screen_wake();
    }
    printf("the bench pages are %s\n",
           settings.screen_flags & SCREEN_BENCH ? "after the others" : "not shown");
    if (settings.screen_sleep == 0) {
        printf("the screen stays on\n");
    } else {
        printf("the screen sleeps %u s after a press, a message or a pairing; a press wakes it\n",
               (unsigned)settings.screen_sleep);
    }
}

static void command(char *line) {
    if (bench_command(line)) {
        return;
    }
    if (bench && (strncmp(line, "contact ", 8) == 0 || strncmp(line, "send ", 5) == 0 ||
                  strcmp(line, "accept") == 0)) {
        printf("not on the bench, where only test frames are sent: 'bench off' first\n");
        return;
    }
    if (strncmp(line, "contact ", 8) == 0) {
        uint8_t peer[TERN_ADDRESS_LEN], frame[TERN_CONTACT_MAX_FRAME];
        size_t len;
        if (!tern_address_read(&line[8], peer)) {
            printf("'%s' is not an address: sixty-four hex digits, or its link, as 'status' "
                   "shows\n",
                   &line[8]);
            return;
        }
        enum demo_result r = demo_contact(&demo, peer, board_now(), frame, &len);
        if (r != DEMO_OK) {
            printf("no contact made: %s\n", result_text(r));
            return;
        }
        if (contacting) {
            /* It takes the place of a handshake a message was waiting on, which is not coming. */
            contacting = false;
            link_unreachable(&companion, contacting_peer);
        }
        if (!send_contact(frame, len, INT8_MIN)) {
            contact_gave_up();
        }
    } else if (strcmp(line, "accept") == 0) {
        demo_accept(&demo, board_now() + ACCEPT_S * 1000000000LL);
        printf("for %d s, a board that is not yet one of this one's peers may make contact\n",
               ACCEPT_S);
    } else if (strcmp(line, "peers") == 0) {
        peers();
    } else if (strncmp(line, "to ", 3) == 0 || strncmp(line, "drop ", 5) == 0) {
        bool drop = line[0] == 'd';
        int slot = atoi(&line[drop ? 5 : 3]) - 1;
        if (slot < 0 || slot >= DEMO_PEERS || demo.s[slot].role == 0) {
            printf("no such peer: 'peers' lists them by number\n");
        } else if (!drop) {
            demo.last = slot;
            printf("'send' now goes to peer %d, ", slot + 1);
            print_address(demo.s[slot].peer);
            printf("\n");
        } else {
            uint8_t address[TERN_ADDRESS_LEN];
            memcpy(address, demo.s[slot].peer, TERN_ADDRESS_LEN);
            if (!demo_forget(&demo, slot)) {
                printf("not dropped: the flash would not forget the session, so it is kept\n");
                return;
            }
            pending_drop(slot, true);
            link_session_changed(&companion, address);
            printf("dropped: this board no longer has a session with ");
            print_address(address);
            printf(". That board still thinks it has one, until it makes contact again.\n");
        }
    } else if (strncmp(line, "send ", 5) == 0) {
        send(&line[5]);
    } else if (strcmp(line, "groups") == 0) {
        groups_list();
    } else if (strncmp(line, "group ", 6) == 0) {
        group_command(&line[6]);
    } else if (strcmp(line, "status") == 0) {
        status();
    } else if (strcmp(line, "routes") == 0) {
        routes();
    } else if (strcmp(line, "selftest") == 0) {
        selftest();
    } else if (strncmp(line, "screen", 6) == 0) {
        screen_command(&line[6]);
    } else if (strcmp(line, "forget") == 0) {
        if (have_ble) {
            ble_forget();
            printf("every Bluetooth client is forgotten, and must pair again\n");
        } else {
            printf("Bluetooth did not start\n");
        }
    } else if (line[0] != '\0') {
        printf("commands: contact <address>, accept, peers, to <number>, send <text>, drop "
               "<number>, groups, group ..., status, routes, selftest, forget "
               "(Bluetooth clients), screen sleep <seconds>, screen bench on|off. Holding "
               "PRG on a bench page sends a ping. For the bench: bench on|off, sync <hex>, power "
               "<dBm>, freq <Hz>, "
               "sf <n>, bw <Hz>, beacon <count> <ms>, counts [reset].\n");
    }
}

/* A byte of typed text. */
static void console_text(void *ctx, uint8_t c) {
    static char line[CONSOLE_LINE];
    static size_t used;
    (void)ctx;
    if (c == '\r' || c == '\n') {
        if (used > 0 || c == '\n') {
            printf("\n");
            line[used] = '\0';
            command(line);
            used = 0;
        }
    } else if ((c == 0x08 || c == 0x7F) && used > 0) {
        used--;
        printf("\b \b");
    } else if (c >= 0x20 && used < CONSOLE_LINE - 1) {
        line[used++] = (char)c;
        putchar(c);
    }
}

/* A setting that takes a restart is applied once its answer has gone: out of the console, and given
 * a moment to leave as a notification. */
static void restart_if_due(void) {
    if (restart_due) {
        link_group_heard(&companion); /* whatever the minute has not yet written */
        (void)link_keep_writers(&companion);
        printf("restarting to apply a setting\n");
        fflush(stdout);
        board_console_flush(500);
        plat_sleep_ms(200);
        plat_restart();
    }
}

/* Bluetooth, and the randomness keys are made from: on some chips the one depends on the other
 * (plat_bluetooth_starting()). */
_Static_assert(PASSKEY_RANDOM == BLE_PASSKEY_RANDOM, "one spelling of a random passkey");
_Static_assert(UI_BATTERY_UNKNOWN == POWER_UNKNOWN, "one spelling of a battery not known");
/* The name Bluetooth advertises: "Tern" and a tag from bytes drawn at random the first time and
 * kept until the board is erased, so that it reads the same from one start to the next. Each byte
 * gives one of 32 characters, with no I, L, O or U to be taken for another. */
static void make_ble_name(void) {
    static const char digits[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    uint8_t tag[BLE_TAG_LEN];
    if (!plat_store_load("btag", tag, sizeof tag)) {
        if (!plat_random(tag, sizeof tag)) {
            snprintf(ble_name, sizeof ble_name, "Tern");
            return;
        }
        if (!plat_store_save("btag", tag, sizeof tag)) {
            printf("bluetooth: the name's tag was not saved; the next start draws another\n");
        }
    }
    char *p = ble_name + snprintf(ble_name, sizeof ble_name, "Tern ");
    for (size_t i = 0; i < BLE_TAG_LEN; i++) {
        *p++ = digits[tag[i] % (sizeof digits - 1)];
    }
    *p = '\0';
}

static void start_bluetooth(void) {
    make_ble_name();
    plat_bluetooth_starting(true);
    have_ble = ble_start(ble_name, settings.passkey, have_screen);
    if (!have_ble) {
        plat_bluetooth_starting(false);
        printf("Bluetooth did not start; the USB port is the only companion link\n");
        return;
    }
    printf("bluetooth: seen by phones as \"%s\"\n", ble_name);
}

/* A companion frame on the USB port. */
static void console_frame(void *ctx, const uint8_t *frame, size_t len) {
    (void)ctx;
    link_receive(&companion, LINK_SERIAL, board_now(), frame, len);
    restart_if_due();
}

/* What Bluetooth reported: a client paired, wrote a frame or went, or a passkey to show. */
static void poll_ble(void) {
    static struct ble_event e;
    while (ble_poll(&e)) {
        switch (e.kind) {
        case BLE_OPEN:
            pairing_passkey = PAIRING_NONE;
            link_open(&companion, LINK_BLE, 0, e.mtu);
            phone = true;
            screen_due = 0;
            printf("bluetooth: a client connected\n");
            break;
        case BLE_MTU:
            link_mtu(&companion, LINK_BLE, e.mtu);
            break;
        case BLE_FRAME:
            link_receive(&companion, LINK_BLE, board_now(), e.frame, e.len);
            restart_if_due();
            break;
        case BLE_CLOSE:
            pairing_passkey = PAIRING_NONE;
            link_close(&companion, LINK_BLE);
            phone = false;
            screen_due = 0;
            printf("bluetooth: the client went\n");
            break;
        case BLE_PASSKEY:
            pairing_passkey = e.passkey;
            screen_due = 0;
            screen_wake();
            printf("bluetooth: pairing; the passkey is on the screen\n");
            break;
        case BLE_PAIRED:
            if (pairing_passkey != PAIRING_NONE) {
                pairing_passkey = PAIRING_NONE;
                screen_due = 0;
                screen_wake();
            }
            break;
        }
    }
}

static void poll_console(void) {
    static const struct tern_companion_sink sink = {NULL, console_frame, console_text};
    uint8_t c;
    while (board_console_read(&c)) {
        tern_companion_push(&parser, board_now(), c, &sink);
        fflush(stdout);
    }
    tern_companion_idle(&parser, board_now(), &sink);
    link_tick(&companion, board_now());
}

/* --- The bench screen ------------------------------------------------------------------------- */

/* The board as the screen shows it (status.h). */
static void fill_status(struct node_status *st) {
    tern_time now = board_now();
    memset(st, 0, sizeof *st);
    st->id = route.id;
    st->relay = route.config.relay;
    st->region = region->name;
    st->off_profile = off_profile;
    st->freq_hz = cfg.freq_hz;
    st->bw_hz = cfg.mod.bw_hz;
    st->sf = cfg.mod.sf;
    st->dbm = cfg.tx_power_dbm;
    st->uptime_s = (uint32_t)(now / 1000000000LL);
    st->air_total_ms = air_total / 1000000;
    st->limited = region->duty_ppm < TERN_DUTY_UNLIMITED;
    if (st->limited) {
        st->air_used_ms = tern_duty_used(&duty, now) / 1000000;
        st->air_limit_ms = duty.limit / 1000000;
        st->window_s = region->duty_window_s;
    }
    st->frames_out = frames_out;
    st->frames_in = frames_in;

    /* Neighbours whose link is up are listed first. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < NEIGHBOURS; i++) {
            const struct tern_route_neighbour *n = &neighbours[i];
            if (!n->used || n->up != (pass == 0)) {
                continue;
            }
            st->heard++;
            st->up += n->up;
            if (st->n_neighbours < STATUS_LISTED) {
                st->neighbours[st->n_neighbours++] = (struct status_neighbour){
                    .id = n->id,
                    .relay = n->relay,
                    .up = n->up,
                    .floor_dbm = (int16_t)(n->floor / 16),
                    .spare_db = n->theirs != 0 ? (int16_t)(n->theirs - 128) : INT16_MIN,
                };
            }
        }
    }
    for (int i = 0; i < DESTINATIONS; i++) {
        uint32_t next;
        uint16_t metric;
        if (!destinations[i].used || !tern_route_next(&route, destinations[i].id, &next, &metric)) {
            continue;
        }
        st->routed++;
        if (st->n_routes < STATUS_LISTED) {
            st->routes[st->n_routes++] = (struct status_route){
                .dest = destinations[i].id, .next = next, .metric_ms = metric};
        }
    }

    const struct demo_state *to = demo.last >= 0 ? &demo.s[demo.last] : NULL;
    st->session = to != NULL;
    st->peers = (uint8_t)demo_peers(&demo);
    st->contacting = demo.h.phase == DEMO_INITIATING || demo.h.phase == DEMO_RESPONDING;
    st->peer = to == NULL ? 0
                          : (uint32_t)to->peer[0] << 24 | (uint32_t)to->peer[1] << 16 |
                                (uint32_t)to->peer[2] << 8 | to->peer[3];
    st->sent = to == NULL ? 0 : to->sent;
    st->received = to == NULL ? 0 : to->heard;
    st->have_last = last_at != 0;
    memcpy(st->last, last_text, sizeof st->last);
    st->last_s = (uint32_t)((now - last_at) / 1000000000LL);
}

/* --- The user's screen ------------------------------------------------------------------------ */

static int screen_pages(void) {
    return UI_PAGES + (settings.screen_flags & SCREEN_BENCH ? STATUS_PAGES : 0);
}

/* The link's messages, newest first: where each is in its list. */
static size_t newest_first(uint8_t order[LINK_MESSAGES]) {
    size_t n = 0;
    for (size_t i = 0; i < LINK_MESSAGES; i++) {
        if (!companion.messages[i].used) {
            continue;
        }
        size_t at = n++;
        while (at > 0 && companion.messages[order[at - 1]].id < companion.messages[i].id) {
            order[at] = order[at - 1];
            at--;
        }
        order[at] = (uint8_t)i;
    }
    return n;
}

/* Whom an address is to the user: the name they saved it as, or failing that the start of the
 * address, as the This node page shows it. */
static void name_of(const uint8_t address[TERN_ADDRESS_LEN], char out[UI_NAME + 1]) {
    for (size_t i = 0; i < LINK_CONTACTS; i++) {
        const struct link_contact *k = &companion.contacts[i];
        if (k->used && memcmp(k->address, address, TERN_ADDRESS_LEN) == 0 && k->name_len > 0) {
            size_t len = k->name_len < UI_NAME ? k->name_len : UI_NAME;
            memcpy(out, k->name, len);
            out[len] = '\0';
            return;
        }
    }
    snprintf(out, UI_NAME + 1, "%02X%02X%02X%02X", address[0], address[1], address[2], address[3]);
}

/* Whom a routing id is to the user: the contact whose address gives that id, by their name or
 * address, or failing that `bare` and the id itself. A group message's writer is named so, from
 * the id it claims: a claim, as the group draft says, since anyone with the group's key can write
 * any id. */
static void node_named(uint32_t id, const char *bare, char out[UI_NAME + 1]) {
    for (size_t i = 0; i < LINK_CONTACTS; i++) {
        const struct link_contact *k = &companion.contacts[i];
        if (k->used && tern_route_id(k->address) == id) {
            name_of(k->address, out);
            return;
        }
    }
    snprintf(out, UI_NAME + 1, "%s%08lX", bare, (unsigned long)id);
}

/* Whom a message is with, as the Messages page names it: a group by the user's name for it, and
 * anything else by its address. */
static void with_whom(const struct link_message *x, char out[UI_NAME + 1]) {
    if (x->kind != LINK_KIND_GROUP) {
        name_of(x->address, out);
        return;
    }
    snprintf(out, UI_NAME + 1, "a group");
    for (size_t i = 0; i < LINK_GROUPS; i++) {
        const struct link_group *g = &companion.groups[i];
        if (g->used && memcmp(g->id, x->group, sizeof g->id) == 0 && g->name_len > 0) {
            size_t len = g->name_len < UI_NAME ? g->name_len : UI_NAME;
            memcpy(out, g->name, len);
            out[len] = '\0';
        }
    }
}

static bool unread(const struct link_message *x) {
    return x->state == TERN_C_RECEIVED && !(x->flags & TERN_C_READ_FLAG);
}

/* The seconds the page shown has left of asking to be sure, rounded up, or 0 if it is not asking:
 * it stops at CONFIRM_S, or once another page is shown. */
static unsigned confirm_left(void) {
    tern_time left = confirm_until - board_now();
    if (confirm_page != screen_page || left <= 0) {
        confirm_page = -1;
        return 0;
    }
    return (unsigned)((left + 999999999) / 1000000000);
}

/* The node as the user's pages show it (ui.h). */
static void fill_ui(struct ui_node *u) {
    tern_time now = board_now();
    uint32_t clock = clock_now();
    uint8_t order[LINK_MESSAGES];
    memset(u, 0, sizeof *u);
    u->region = region->name;
    u->version = CONFIG_TERN_VERSION;
    u->relay = route.config.relay;
    u->dbm = cfg.tx_power_dbm;
    memcpy(u->address, demo.id.address, TERN_ADDRESS_LEN);
    u->bench = bench;
    u->battery = power_percent(battery_mv);
    u->charging = watch.charging;
    u->phone = phone;
    u->bluetooth = have_ble;
    u->ble_name = ble_name;
    unsigned paired = have_ble ? ble_paired() : 0;
    u->paired = (uint8_t)(paired > UINT8_MAX ? UINT8_MAX : paired);
    u->confirm_s = (uint8_t)confirm_left();
    /* The neighbours, most recently heard first, and of them the ones the Nearby page shows. */
    uint8_t heard[NEIGHBOURS];
    for (int i = 0; i < NEIGHBOURS; i++) {
        if (!neighbours[i].used) {
            continue;
        }
        size_t at = u->nearby++;
        while (at > 0 && neighbours[heard[at - 1]].heard < neighbours[i].heard) {
            heard[at] = heard[at - 1];
            at--;
        }
        heard[at] = (uint8_t)i;
    }
    if (nearby_first >= u->nearby) {
        nearby_first = 0; /* past the last: round to the first again */
    }
    u->nearby_first = nearby_first;
    for (size_t k = nearby_first; k < u->nearby && u->nearby_n < UI_NEARBY_ROWS; k++) {
        const struct tern_route_neighbour *g = &neighbours[heard[k]];
        struct ui_neighbour *v = &u->neighbour[u->nearby_n++];
        tern_time ago = (now - g->heard) / 1000000000LL;
        /* The id alone, with no "node " before it: the list's heading says what they are, and a
         * line has room for little more than a name beside the signal. */
        node_named(g->id, "", v->name);
        v->snr_db = (int8_t)(snr_of(g->id) / 4);
        v->ago_s = (uint32_t)(ago < 0 ? 0 : ago > UINT32_MAX ? UINT32_MAX : ago);
    }
    for (int i = 0; i < DESTINATIONS; i++) {
        uint32_t next;
        uint16_t metric;
        if (destinations[i].used && tern_route_next(&route, destinations[i].id, &next, &metric)) {
            u->reachable++;
        }
    }
    u->air_total_ms = air_total / 1000000;
    u->limited = region->duty_ppm < TERN_DUTY_UNLIMITED;
    if (u->limited) {
        u->air_used_ms = tern_duty_used(&duty, now) / 1000000;
        u->air_limit_ms = duty.limit / 1000000;
        u->window_s = region->duty_window_s;
        u->wait_ms = duty_wait_ms(now);
    }

    size_t n = newest_first(order);
    u->messages = (uint8_t)n;
    for (size_t k = 0; k < n; k++) {
        const struct link_message *x = &companion.messages[order[k]];
        if (unread(x) && u->unread++ == 0) {
            with_whom(x, u->from);
            u->from_group = x->kind == LINK_KIND_GROUP;
        }
        if (x->state == TERN_C_WAITING || x->state == TERN_C_SENT) {
            u->waiting++;
        }
    }
    if (message_shown >= n) {
        message_shown = 0; /* past the oldest: round to the newest again */
    }
    u->shown = message_shown;
    if (n > 0) {
        const struct link_message *x = &companion.messages[order[message_shown]];
        struct ui_message *m = &u->message;
        m->received = x->state == TERN_C_RECEIVED;
        m->unread = unread(x);
        m->group = x->kind == LINK_KIND_GROUP;
        with_whom(x, m->who);
        if (m->group && m->received) {
            node_named(x->from, "node ", m->writer);
        }
        m->aged = clock != 0 && x->time != 0 && clock >= x->time;
        m->ago_s = m->aged ? clock - x->time : 0;
        m->state = x->state;
        m->reason = x->reason;
        m->wait_s = x->wait;
        if (x->kind == LINK_KIND_INVITE) {
            /* An invite's text is its group's name: said to be one, since it is not words. */
            int len = snprintf((char *)m->text, sizeof m->text, "Invite to the group \"%.*s\"",
                               (int)x->text_len, (const char *)x->text);
            m->text_len = (uint8_t)(len < (int)sizeof m->text ? len : (int)sizeof m->text - 1);
        } else {
            m->text_len = x->text_len;
            memcpy(m->text, x->text, x->text_len);
        }
    }
    /* The groups, in the order the node holds them, and of them the one the Groups page shows,
     * with its join code only while the user has asked to see it. */
    const struct link_group *shown = NULL;
    for (size_t i = 0; i < LINK_GROUPS; i++) {
        const struct link_group *g = &companion.groups[i];
        if (g->used && u->groups++ == group_shown) {
            shown = g;
        }
    }
    if (shown == NULL && u->groups > 0) {
        group_shown = 0; /* past the last: round to the first again */
        for (size_t i = 0; shown == NULL && i < LINK_GROUPS; i++) {
            shown = companion.groups[i].used ? &companion.groups[i] : NULL;
        }
    }
    if (shown == NULL || screen_page != UI_GROUPS ||
        memcmp(shown->id, group_code_id, sizeof group_code_id) != 0) {
        group_code = false; /* off the page, or the groups changed under it: asked for again */
    }
    u->group_shown = group_shown;
    if (shown != NULL) {
        size_t len = shown->name_len < UI_NAME ? shown->name_len : UI_NAME;
        memcpy(u->group, shown->name, len);
        u->group[len] = '\0';
        if (group_code) {
            /* The name cut to what a version 3 code holds: a suggestion, for the same group. */
            tern_group_link(shown->g.secret, shown->name,
                            ui_join_name(shown->name, shown->name_len), u->join_link);
        }
    }
}

/* Someone is looking at the Messages page: the message it shows, and any before it, are read, here
 * and on every client. Not if it is not there to be seen: no screen, the screen off, or a pairing's
 * passkey over it. */
static void screen_seen(void) {
    uint8_t order[LINK_MESSAGES];
    size_t n = newest_first(order);
    if (!have_screen || screen_asleep || pairing_passkey != PAIRING_NONE ||
        screen_page != UI_MESSAGES || message_shown >= n) {
        return;
    }
    const struct link_message *x = &companion.messages[order[message_shown]];
    if (unread(x)) {
        link_read(&companion, x->id);
    }
}

/* A write the screen did not take: try again in a second, and after SCREEN_TRIES in a row, give
 * the screen up. */
static void screen_failed(void) {
    screen_retry = board_now() + 1000000000LL;
    screen_failures++;
    if (screen_failures >= SCREEN_TRIES) {
        have_screen = false;
        printf("the screen stopped answering; carrying on without it. PRG now sends a ping.\n");
    }
}

/* Something to see: the screen is on, or turned on by poll_screen(), for screen_sleep more. */
static void screen_wake(void) { screen_woken = board_now(); }

/* Draws the page shown every SCREEN_MS, and sends at most one changed page of the picture each
 * turn of the loop, about 3 ms, so the radio is never kept waiting long. A write that fails can
 * take 50 ms (board.c), so after one the screen waits a second, and after SCREEN_TRIES in a row
 * it is given up. */
static void poll_screen(void) {
    if (!have_screen || board_now() < screen_retry) {
        return;
    }
    if (pairing_passkey != PAIRING_NONE) {
        screen_woken = board_now(); /* the passkey stays up for as long as the pairing takes */
    }
    /* Turned on or off here only, so a write the screen did not take is tried again. */
    bool on = settings.screen_sleep == 0 || board_now() < boot_until ||
              board_now() - screen_woken < (tern_time)settings.screen_sleep * 1000000000LL;
    if (on == screen_asleep) {
        if (!board_screen_power(on)) {
            screen_failed();
            return;
        }
        screen_asleep = !on;
        group_code = false; /* dark, and so no longer shown to whoever asked */
        screen_failures = 0;
        screen_due = 0;
    }
    if (screen_asleep) {
        return; /* the picture is brought up to date when it wakes */
    }
    if (off_shown != 0) {
        if (board_now() >= screen_due) {
            ui_turning_off(off_shown, &screen); /* over everything: it is what PRG is doing */
            screen_due = board_now() + (tern_time)SCREEN_MS * 1000000;
        }
    } else if (pairing_passkey == PAIRING_NONE && board_now() < boot_until) {
        /* the boot screen, drawn by node_main(), until it has been seen */
    } else if (board_now() >= screen_due) {
        if (screen_page >= screen_pages()) {
            screen_page = UI_HOME; /* the bench pages were turned off while one was shown */
        }
        if (pairing_passkey != PAIRING_NONE) {
            /* A client is pairing: the passkey to type into it, over whatever page was shown. */
            ui_pairing(pairing_passkey, &screen);
        } else if (companion.update.on &&
                   board_now() - update_at < (tern_time)UPDATE_SHOWN_S * 1000000000LL) {
            ui_updating(companion.update.held, companion.update.size, &screen);
        } else if (screen_page < UI_PAGES) {
            static struct ui_node u;
            if (screen_looked) {
                screen_seen();
            }
            fill_ui(&u);
            ui_draw(&u, screen_page, &screen);
            tern_wipe(u.join_link, sizeof u.join_link);
        } else {
            static struct node_status st;
            char rows[STATUS_ROWS][STATUS_COLS + 1];
            fill_status(&st);
            status_page(&st, screen_page - UI_PAGES, rows);
            for (int i = 0; i < STATUS_ROWS; i++) {
                display_text(&screen, i, rows[i], i == 0);
            }
        }
        screen_due = board_now() + (tern_time)SCREEN_MS * 1000000;
    }
    int page = display_take(&screen);
    if (page < 0) {
        return;
    }
    if (board_screen_page(page, screen.px[page])) {
        screen_failures = 0;
        return;
    }
    screen.dirty |= (uint8_t)(1u << page);
    screen_failed();
}

/* Sends the whole picture now, not a page a turn: for the screens drawn outside the loop. A
 * page the screen did not take is left for poll_screen() to send again. */
static void screen_flush(void) {
    int page;
    while (have_screen && (page = display_take(&screen)) >= 0) {
        if (!board_screen_page(page, screen.px[page])) {
            screen.dirty |= (uint8_t)(1u << page);
            return;
        }
    }
}

/* Turns the board off, saying why on the screen for long enough to read. Nothing need be saved
 * first: what must outlast a restart (the identity, sessions, the time on the air) is saved as it
 * changes. The messages the link keeps in memory are lost, as at any restart. Never returns. */
__attribute__((noreturn)) static void turn_off(enum ui_off why) {
    if (why == UI_OFF_EMPTY) {
        printf("the battery is empty (%u mV): turning off. It turns on again once it is "
               "charged, or at a press of PRG.\n",
               (unsigned)battery_mv);
    } else {
        printf("PRG held: turning off. A press of PRG turns it on again.\n");
    }
    if (have_screen) {
        if (screen_asleep) {
            board_screen_power(true);
        }
        ui_off(why, &screen);
        screen_flush();
        plat_sleep_ms(2000);
    }
    /* The radio, asleep, even if the board stopped before starting it (halt()). */
    board_radio_sleep();
    board_off(why == UI_OFF_EMPTY ? EMPTY_CHECK_S : 0);
}

/* The seconds left shown while PRG is held to turn the board off, from how long it has been held:
 * 0 before the warning, and turn_off() at the end. */
static unsigned off_seconds(tern_time held) {
    if (held < (tern_time)OFF_WARN_MS * 1000000) {
        return 0;
    }
    tern_time left = (tern_time)OFF_MS * 1000000 - held;
    if (left <= 0) {
        turn_off(UI_OFF_PRESSED);
    }
    return (unsigned)((left + 999999999) / 1000000000);
}

/* Reads the battery every BATTERY_S: a couple of milliseconds, so not often, and never while the
 * radio sends, which pulls the voltage down and would read as the charger going. Two readings in
 * a row too low to run on turn the board off. */
static void poll_battery(void) {
    static tern_time due;
    if (board_now() < due || transmitting) {
        return;
    }
    due = board_now() + (tern_time)BATTERY_S * 1000000000LL;
    battery_mv = board_battery_mv();
    power_watch(&watch, battery_mv);
    if (power_empty(&watch)) {
        turn_off(UI_OFF_EMPTY);
    }
}

/* Whether any message is unread, here or on any client. */
static bool any_unread(void) {
    for (size_t i = 0; i < LINK_MESSAGES; i++) {
        if (companion.messages[i].used && unread(&companion.messages[i])) {
            return true;
        }
    }
    return false;
}

/* Puts the LED out once a flash of it is over and nothing is on the air, and while a message is
 * unread blinks it, a moment every few seconds, for someone who has the board in sight but not
 * its screen: until the message is read, on the screen or on a client. */
static void poll_led(void) {
    static tern_time blink_next;
    tern_time now = board_now();
    if (led_until != 0) {
        if (now > led_until && !transmitting) {
            board_led(false);
            led_until = 0;
        }
        return;
    }
    if (!UNREAD_LED || transmitting || !any_unread()) {
        blink_next = 0; /* the first blink comes at once, for a message that has just come */
        return;
    }
    if (now >= blink_next) {
        board_led(true);
        led_until = now + (tern_time)UNREAD_BLINK_MS * 1000000;
        blink_next = now + (tern_time)UNREAD_EVERY_MS * 1000000;
    }
}

/* Restarts the board to be erased, as the board next starts (plat_store_start()). Nothing is saved
 * first: all of it goes. */
__attribute__((noreturn)) static void erase_and_restart(void) {
    printf("PRG held on Reset: erasing this board and starting again, with a new address\n");
    if (have_screen) {
        ui_erasing(&screen);
        screen_flush();
    }
    plat_erase_and_restart();
}

static void ping(void) {
    static unsigned pings;
    char text[32];
    if (bench) {
        return; /* on the bench, only test frames are sent */
    }
    snprintf(text, sizeof text, "ping %u", ++pings);
    send(text);
}

/* Holding PRG acts on the page shown: on Messages, it shows the message before; on Nearby, the
 * next nodes; on Groups, the group's join code, and held again, the next group; on a bench page,
 * it sends a ping. On Phones and Reset, it first asks to be sure, and
 * only a second hold while it asks forgets the phones or erases the board. Elsewhere it does
 * nothing. */
static void screen_hold(void) {
    if (screen_page == UI_PHONES || screen_page == UI_RESET) {
        bool sure = confirm_left() > 0;
        screen_due = 0;
        if (screen_page == UI_PHONES && (!have_ble || ble_paired() == 0)) {
            return; /* nothing to forget */
        }
        if (!sure) {
            confirm_page = screen_page;
            confirm_until = board_now() + (tern_time)CONFIRM_S * 1000000000LL;
            return;
        }
        confirm_page = -1;
        if (screen_page == UI_RESET) {
            erase_and_restart();
        }
        ble_forget();
        printf("PRG held on Phones: every Bluetooth client is forgotten, and must pair again\n");
        return;
    }
    if (screen_page == UI_GROUPS) {
        /* Its join code, asked for; held again, the next group, its code not shown until asked. */
        if (group_code) {
            group_shown++; /* fill_ui() goes round to the first after the last */
            group_code = false;
        } else {
            for (size_t i = 0, n = 0; i < LINK_GROUPS; i++) {
                const struct link_group *g = &companion.groups[i];
                if (g->used && n++ == group_shown) {
                    memcpy(group_code_id, g->id, sizeof group_code_id);
                    group_code = true;
                }
            }
        }
        screen_due = 0;
    } else if (screen_page == UI_MESSAGES) {
        message_shown++; /* fill_ui() goes round to the newest after the oldest */
        screen_looked = true;
        screen_due = 0;
    } else if (screen_page == UI_NEARBY) {
        nearby_first += UI_NEARBY_ROWS; /* fill_ui() goes round to the first after the last */
        screen_due = 0;
    } else if (screen_page >= UI_PAGES) {
        ping();
    }
}

/* A press shows the next page; holding PRG for HOLD_MS acts on the page, once, while it is still
 * held. A press that wakes the screen shows the Home page, and does nothing else. With no screen,
 * a press sends a ping, as it did before there was one. Held on past OFF_WARN_MS, whatever it
 * began as, it counts down to turning the board off at OFF_MS; let go before, and it does not. */
static void poll_button(void) {
    static bool was, held, waking;
    static tern_time down, last;
    tern_time now = board_now();
    bool pressed = board_button();
    if (pressed && !was) {
        down = now;
        held = false;
        waking = have_screen && (screen_asleep || now < boot_until);
        boot_until = 0;
        if (waking) {
            screen_page = UI_HOME;
            screen_due = 0;
        } else {
            screen_seen(); /* someone is looking at what is shown */
        }
        screen_wake();
    } else if (pressed && off_seconds(now - down) != 0) {
        unsigned s = off_seconds(now - down);
        if (s != off_shown) {
            off_shown = s;
            screen_due = 0;
        }
        held = true; /* letting go is not a press */
        screen_wake();
    } else if (waking) {
        /* nothing until it is let go */
    } else if (have_screen && pressed && !held && now - down >= (tern_time)HOLD_MS * 1000000) {
        held = true;
        screen_hold();
    } else if (!pressed && was && !held && now - last > 300 * 1000000LL) {
        last = now;
        if (have_screen) {
            screen_page = (screen_page + 1) % screen_pages();
            message_shown = 0;
            nearby_first = 0;
            group_shown = 0;
            group_code = false; /* a join code is shown only on its page, while asked for */
            confirm_page = -1;  /* a press keeps what the page asked about */
            screen_looked = true;
            screen_due = 0;
        } else {
            ping();
        }
    }
    if (!pressed && was) {
        waking = false;
        if (off_shown != 0) {
            off_shown = 0; /* let go in time: the page it was on */
            screen_due = 0;
        }
    }
    was = pressed;
}

/* The boot screen, as far as the board knows itself yet. */
static void show_start(void) {
    if (have_screen) {
        ui_boot(&start_info, &screen);
        screen_flush();
    }
}

/* The board cannot start: why, on the screen, for someone with no console to read it. The screen
 * sleeps as it would have, so a board left like this does not run its battery down lighting it,
 * and PRG lights it again. PRG held turns it off, and an empty battery does, as when it runs.
 * Never returns. */
__attribute__((noreturn)) static void halt(enum ui_fault why, int code) {
    if (plat_update_on_trial()) {
        /* An update that cannot start: back to the firmware that could. */
        printf("this firmware, new from an update, cannot start: going back to the one before\n");
        fflush(stdout);
        plat_update_reject();
    }
    if (have_screen) {
        ui_fault(why, &start_info, code, &screen);
        screen_flush();
    }
    tern_time woken = board_now(), down = 0, battery_due = 0;
    bool was = false;
    for (;;) {
        tern_time now = board_now();
        bool pressed = board_button();
        if (pressed) {
            woken = now;
            down = was ? down : now;
            unsigned s = off_seconds(now - down);
            if (s != off_shown && have_screen) {
                ui_turning_off(s, &screen);
            }
            off_shown = s;
        } else if (off_shown != 0) {
            off_shown = 0; /* let go in time */
            if (have_screen) {
                ui_fault(why, &start_info, code, &screen);
            }
        }
        was = pressed;
        if (now >= battery_due) {
            battery_due = now + (tern_time)BATTERY_S * 1000000000LL;
            battery_mv = board_battery_mv();
            power_watch(&watch, battery_mv);
            if (power_empty(&watch)) {
                turn_off(UI_OFF_EMPTY);
            }
        }
        bool want = settings.screen_sleep == 0 ||
                    now - woken < (tern_time)settings.screen_sleep * 1000000000LL;
        if (have_screen && want == screen_asleep && board_screen_power(want)) {
            screen_asleep = !want;
        }
        if (!screen_asleep) {
            screen_flush(); /* what changed, and a page the screen did not take before */
        }
        plat_sleep_ms(50);
    }
}

void node_main(void) {
    /* Turned off for an empty battery, and woken to look at it again: off again at once, with
     * nothing lit, unless it has been charged a little. The radio is still asleep. */
    bool have_battery = board_battery_init();
    if (board_woke_by_timer()) {
        uint16_t mv = have_battery ? board_battery_mv() : 0;
        if (mv >= POWER_NONE_MV && mv < POWER_RESTART_MV) {
            board_off(EMPTY_CHECK_S);
        }
    }

    /* The screen first, so that if the board cannot start it can say why. */
    start_info.version = CONFIG_TERN_VERSION;
    have_screen = board_screen_init();
    if (have_screen) {
        display_init(&screen);
    }
    show_start();

    /* Too little left to run on: say so, and off, before anything draws more. */
    battery_mv = have_battery ? board_battery_mv() : 0;
    if (battery_mv >= POWER_NONE_MV && battery_mv < POWER_EMPTY_MV) {
        turn_off(UI_OFF_EMPTY);
    }
    power_watch(&watch, battery_mv);

    /* Erasing, if the Reset page asked, keeps the time on the air: the region's limit on
     * transmitting does not start again because the board did. */
    int code = 0;
    bool erasing = plat_erase_asked();
    switch (plat_store_start(erasing, "airtime", &code)) {
    case PLAT_STORE_OK:
        if (erasing) {
            printf("erased, as the Reset page asked: this board is new\n");
        }
        break;
    case PLAT_STORE_ERASE_FAILED:
        printf("could not erase the flash (%s). Not starting.\n", plat_error_name(code));
        halt(UI_FAULT_STORAGE, code);
    case PLAT_STORE_NEEDS_ERASE:
        /* Never erased to make room: the saved session is what stops counters repeating, and the
         * identity is the board's address. */
        printf("the storage needs erasing (%s). Not starting: this board's identity and session "
               "would be lost. Erase the flash yourself; the board will then have a new "
               "address.\n",
               plat_error_name(code));
        halt(UI_FAULT_STORAGE, 0);
    case PLAT_STORE_FAILED:
        printf("the storage did not start (%s). Not starting.\n", plat_error_name(code));
        halt(UI_FAULT_STORAGE, code);
    }
    if (!board_console_init()) {
        printf("the console did not start\n");
    }

    /* The battery has been read: the platform's generator may now take what it needs to be a true
     * one (on the ESP32, the ADC's noise source), before the identity and the router's seed are
     * made below. */
    plat_entropy_start();

#if CONFIG_TERN_REGION_EU868
    region = tern_region(TERN_REGION_EU868);
#elif CONFIG_TERN_REGION_AU915
    region = tern_region(TERN_REGION_AU915);
#elif CONFIG_TERN_REGION_NZ915
    region = tern_region(TERN_REGION_NZ915);
#else
    region = tern_region(TERN_REGION_US915);
#endif
    int8_t power = CONFIG_TERN_TX_POWER_DBM;
    /* What a client set, over what the build chose. Each was checked against the region and the
     * antenna when it was set; if the pair no longer fits, the build's settings are used. */
    struct settings saved;
    saved.screen_sleep = CONFIG_TERN_SCREEN_SLEEP_S;
    saved.screen_flags = SCREEN_FLAGS;
    saved.cards = 0;
    saved.card_name_len = 0;
    if ((store_load(NULL, "settings", &saved, sizeof saved) ||
         store_load(NULL, "settings", &saved, offsetof(struct settings, cards)) ||
         store_load(NULL, "settings", &saved, offsetof(struct settings, screen_flags)) ||
         store_load(NULL, "settings", &saved, offsetof(struct settings, screen_sleep))) &&
        saved.magic == SETTINGS_MAGIC) {
        settings = saved;
    }
    const struct tern_region *chosen =
        settings.region != 0 ? tern_region((enum tern_region_id)settings.region) : NULL;
    int8_t chosen_power = settings.power != POWER_UNSET ? settings.power : power;
    if (chosen == NULL) {
        chosen = region;
    }
    if (board_power_ok(chosen_power) &&
        tern_region_radio(chosen, chosen_power, CONFIG_TERN_ANTENNA_DBI, &cfg) == TERN_OK) {
        region = chosen;
        power = chosen_power;
    } else {
        printf("the settings a client saved do not fit this antenna; using the build's\n");
    }
    if (!board_power_ok(power)) {
        printf("%d dBm is not a power the %s gives (%d to %d dBm). Not starting.\n", power,
               board_title(), board_power_min(), board_power_max());
        halt(UI_FAULT_POWER, 0);
    }
    if (tern_region_radio(region, power, CONFIG_TERN_ANTENNA_DBI, &cfg) != TERN_OK) {
        printf("%d dBm into a %d dBi antenna is more than %s allows (%d dBm radiated). Not "
               "starting.\n",
               power, CONFIG_TERN_ANTENNA_DBI, region->name, region->max_eirp_dbm);
        halt(UI_FAULT_POWER, 0);
    }
    /* Experiments: a build may move the board off its region's profile. The region's limit on
     * transmitting still applies. */
    if (CONFIG_TERN_FREQ_HZ != 0) {
        cfg.freq_hz = CONFIG_TERN_FREQ_HZ;
    }
    if (CONFIG_TERN_SF != 0) {
        cfg.mod.sf = CONFIG_TERN_SF;
    }
    if (CONFIG_TERN_BW_HZ != 0) {
        cfg.mod.bw_hz = CONFIG_TERN_BW_HZ;
    }
    cfg.sync_word = CONFIG_TERN_SYNC_WORD;
    off_profile = cfg.freq_hz != region->freq_hz || cfg.mod.sf != region->sf ||
                  cfg.mod.bw_hz != region->bw_hz || cfg.sync_word != TERN_SYNC_WORD;
    start_info.region = region->name;
    show_start();
    /* The board's clock starts again at a restart, so the times of what it sent before are
     * lost. All of it is counted as sent now, which can only hold the board back longer. */
    tern_duty_init(&duty, region->duty_ppm, region->duty_window_s);
    tern_time before = 0;
    if (store_load(NULL, "airtime", &before, sizeof before)) {
        tern_duty_charge(&duty, board_now(), before);
    }

    struct demo_store store = {
        .ctx = NULL, .load = store_load, .save = store_save, .random = board_random};
    had_identity = plat_store_has("identity");
    if (!demo_start(&demo, &store, DEMO_HOLD)) {
        printf("could not make this board's identity and save it to flash. Not starting.\n");
        halt(UI_FAULT_IDENTITY, 0);
    }
    start_info.address = demo.id.address;
    start_info.new_address = !had_identity;
    show_start();

    /* Routing: its id from the address, the last sequence number used if one was kept, and
     * announces no louder than the build's power. */
#ifdef CONFIG_TERN_RELAY
    bool relay = true;
#else
    bool relay = false;
#endif
    if (settings.role <= 1) {
        relay = settings.role == 1;
    }
    struct tern_route_config rc =
        tern_route_defaults(&cfg.mod, cfg.tx_power_dbm, TX_MIN_DBM, relay);
    uint64_t seed;
    if (!board_random(NULL, (uint8_t *)&seed, sizeof seed)) {
        printf("no random numbers. Not starting.\n");
        halt(UI_FAULT_RANDOM, 0);
    }
    (void)store_load(NULL, "seq", &route_seq_saved, sizeof route_seq_saved);
    tern_route_init(&route, &rc, tern_route_id(demo.id.address), neighbours, NEIGHBOURS,
                    destinations, DESTINATIONS, route_seq_saved, seed, board_now());
    {
        /* The first number is the one stored ahead last time, or any with none stored. Nothing is
         * kept beyond it yet, so the first announce waits for poll_route() to store more: a board
         * that restarts again before it sends anything has used up no numbers. */
        uint16_t first = (uint16_t)seed;
        (void)store_load(NULL, "number", &first, sizeof first);
        tern_route_numbering_from(&route, first, first);
    }
    tern_route_auth(&route, &(struct tern_route_auth){.address = demo.id.address,
                                                      .ctx = &demo.id,
                                                      .sign = route_sign,
                                                      .verify = route_verify});
    power_now = cfg.tx_power_dbm;
    struct tern_forward_config fc = tern_forward_defaults();
    tern_forward_init(&forward, &fc, &route, forward_slots, FORWARD_SLOTS, seed ^ 0x666f7277u);
    struct tern_flood_config flc = tern_flood_defaults();
    tern_flood_init(&flood, &flc, &route, flood_slots, FLOOD_SLOTS, flood_seen, TERN_FLOOD_SEEN,
                    seed ^ 0x666c6f6fu, board_now());

    int err = board_init();
    radio = board_radio();
    if (err == TERN_OK) {
        err = tern_radio_configure(&radio, &cfg);
    }
    if (err == TERN_OK) {
        err = tern_radio_receive(&radio);
    }
    if (err != TERN_OK) {
        printf("the radio did not start (error %d)\n", err);
        halt(UI_FAULT_RADIO, err);
    }

    struct link_host host = {
        .ctx = NULL,
        .firmware = FIRMWARE,
        .out = link_out,
        .view = link_view,
        .set = link_set,
        .set_time = link_set_time,
        .session = link_session,
        .why = link_why,
        .end_session = link_end_session,
        .load = link_load,
        .save = link_save,
        .load_groups = link_load_groups,
        .save_groups = link_save_groups,
        .random = board_random,
        .load_ids = link_load_ids,
        .save_ids = link_save_ids,
        .load_count = link_load_count,
        .save_count = link_save_count,
        .load_writers = link_load_writers,
        .save_writers = link_save_writers,
        .load_message = link_load_message,
        .save_message = link_save_message,
        .load_state = link_load_state,
        .save_state = link_save_state,
        .update_begin = link_update_begin,
        .update_write = link_update_write,
        .update_run = link_update_run,
    };
    uint32_t room = plat_update_room();
    if (room != 0) {
        host.board = BOARD;
        host.update_room = room;
    }
    host.release = CONFIG_TERN_VERSION;
    link_init(&companion, &host);
    if (plat_update_on_trial()) {
        /* On the air and listening: an image that gets this far is one to keep. */
        plat_update_confirm();
        printf("this firmware, new from an update, started: it is kept\n");
    }
    demo_trust(&demo, link_trusted, NULL);
    link_open(&companion, LINK_SERIAL, LINK_LAPSE, 0);
    /* The board's own connection, for what is typed at its console. */
    link_open(&companion, LINK_BOARD, 0, 0);
    ask(&(struct tern_companion_msg){.type = TERN_C_HELLO, .version = TERN_COMPANION_VERSION});
    tern_companion_parser_init(&parser);

    if (!have_battery) {
        printf("the battery's ADC did not start: the battery is reported as unknown\n");
    }
    if (have_screen) {
        printf("\nTern demo on the %s. Type 'status'. PRG shows the screen's next page; "
               "'screen bench on' adds the bench pages.\n",
               board_title());
    } else {
        printf("\nTern demo on the %s, with no screen found. Type 'status', or press PRG "
               "to ping.\n",
               board_title());
    }
    pairing_passkey = PAIRING_NONE;
    start_bluetooth();
    status();
    boot_until = board_now() + (tern_time)BOOT_MS * 1000000;
    screen_wake();
    for (;;) {
        poll_radio();
        if (bench) {
            poll_beacon();
        } else {
            poll_contact();
            poll_outgoing();
            poll_forward();
            poll_card();
            poll_flood();
            poll_route();
            poll_writers();
        }
        poll_console();
        poll_ble();
        poll_button();
        poll_screen();
        poll_battery();
        if (transmitting && board_now() > tx_deadline) {
            /* TX_DONE never came. Listen again rather than stay busy for ever. */
            printf("the radio never said the frame had gone; listening again\n");
            transmitting = false;
            if (forward_out) {
                /* Counted as gone: the forwarder listens for it, and sends it again unheard. */
                tern_forward_sent(&forward, board_now(), forward_handle);
                forward_out = false;
            }
            if (flood_out) {
                /* Not known to have gone: a frame of this board's goes back to wait its turn,
                 * and its message stays waiting; one being passed on is let go. */
                tern_flood_withdrawn(&flood, board_now(), flood_handle);
                flood_out = false;
                flood_own = -1;
            }
            tern_radio_receive(&radio);
        }
        poll_led();
        plat_yield();
    }
}
