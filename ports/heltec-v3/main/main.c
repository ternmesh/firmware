/* The Heltec V3 demo: two boards make first contact and send each other secured unicast frames,
 * which follow routes: each is handed to the forwarder (tern/forward.h), sent to the next hop its
 * route gives, sent again if nothing is heard of it, and acknowledged by the board it is for. A
 * board built as a relay passes other boards' frames on.
 *
 * Over the USB serial port (idf.py monitor, 115200 baud) it takes these commands:
 *
 *   contact <address>        make first contact with the board whose address that is (its
 *                            'status' shows it)
 *   accept                   for two minutes, let a board other than the present peer make
 *                            contact
 *   send <text>              send a message
 *   status                   show this board's address, the session and the radio settings
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
 * Pressing PRG shows the bench screen's next page; holding it for a second sends a ping, and a
 * board that receives a ping answers with a pong saying how well it heard it. (With no screen,
 * a press sends a ping.) The LED lights while a frame is on the air or has just arrived.
 *
 * Everything runs in one loop: the core never runs in interrupt context, so the loop polls the
 * radio, the serial port, the button and the screen in turn. */

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "bootloader_random.h"
#include "demo.h"
#include "display.h"
#include "driver/uart.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "status.h"
#include "tern/companion.h"
#include "tern/duty.h"
#include "tern/err.h"
#include "tern/forward.h"
#include "tern/lora.h"
#include "tern/radio.h"
#include "tern/region.h"
#include "tern/route.h"
#include "tern/sx126x.h"

#define CONSOLE UART_NUM_0
#define CONSOLE_LINE 300
#define LED_MS 150
#define ACCEPT_S 120
#define NEIGHBOURS 64 /* 2.5 kB; in a crowd, 32 held a tenth fewer routes in the simulator */
#define DESTINATIONS 128
#define FORWARD_SLOTS 8 /* frames in hand at once, this board's and those it passes on: 2.4 kB */
#define PENDING 4       /* of them, this board's own messages not yet acknowledged */
#define TX_MIN_DBM (-9) /* the SX1262's least */
#define POWER_UNSET INT8_MIN
#define SCREEN_MS 500  /* how often the bench screen is drawn again */
#define HOLD_MS 1000   /* how long PRG is held to send a ping */
#define SCREEN_TRIES 5 /* writes failed in a row before the screen is given up */
#define FIRMWARE "tern 0.0.0 heltec-v3"
#define SETTINGS_MAGIC 0x54530001u
#define PASSKEY_RANDOM 0xFFFFFFFFu

static struct tern_sx126x sx;
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
static bool route_out;   /* the frame on the air is the router's */
static int8_t power_now; /* what the radio is set to send at, or POWER_UNSET */
static uint8_t route_frame[TERN_ROUTE_FRAME_MAX]; /* the router's, until it has gone */
static size_t route_len;
static int8_t route_dbm;
static tern_time route_retry; /* when to try it again, if the radio refused it */
static struct tern_forward forward;
static struct tern_forward_slot forward_slots[FORWARD_SLOTS];
static bool forward_out;       /* the frame on the air is the forwarder's */
static uint8_t forward_handle; /* and this is the forwarder's name for it */
static tern_time forward_retry;
static tern_time forward_quiet; /* how long to listen once the frame on the air has gone */
/* This board's messages that the forwarder has and that are not yet acknowledged or given up. */
static struct pending {
    uint32_t id; /* the link's, or 0 for a free place */
    uint32_t counter;
    uint8_t tag[TERN_FORWARD_TAG];
    uint8_t goes; /* times it has gone on the air */
    tern_time at; /* when it was handed over */
} pending[PENDING];
/* A handshake frame that had to wait for the one on the air. */
static uint8_t waiting[TERN_CONTACT_MAX_FRAME];
static size_t waiting_len;
static tern_time tx_deadline; /* when a frame on the air should certainly have finished */
static tern_time led_until;
/* What the bench screen shows that nothing else keeps. */
static uint32_t frames_out, frames_in;
static tern_time air_total;
static char last_text[STATUS_TEXT];
static tern_time last_at; /* when last_text was heard, or 0 for nothing yet */
static struct display screen;
static bool have_screen;
static int screen_page;
static tern_time screen_due;
static tern_time screen_retry; /* after a failed write, when to try again */
static unsigned screen_failures;

/* The companion link: the client's half of the conversation is link.c's; this is the port. */
static struct link companion;
static struct tern_companion_parser parser;
static uint32_t clock_base; /* seconds since 1970 as a client last set them, or 0 */
static tern_time clock_at;
static bool restart_due; /* a setting saved that takes a restart, once its answer has gone */
/* A message sealed and not yet with the forwarder: sealing takes a counter and saves the session,
 * so a frame the forwarder had no room for, or the flash refused, is kept and tried again, a
 * second apart, not sealed again on every turn of the loop. Kept for one session only. */
static uint32_t sealed_id;
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
};
static struct settings settings = {SETTINGS_MAGIC, 0, 0xFF, POWER_UNSET, PASSKEY_RANDOM};

/* The bench: test frames sent on a timer, and counts of what was received. */
#define BEACON_LEN 24
static const char beacon_text[BEACON_LEN - 4] = "tern sync word test ";
static bool bench;
static uint32_t beacon_left, beacon_sent, beacon_number;
static tern_time beacon_gap, beacon_next;
static bool beacon_running;
static uint32_t bench_ours, bench_others; /* frames received: test frames, and anything else */
static int32_t bench_rssi, bench_snr_cdb; /* summed over the test frames */

/* --- Storage: one NVS namespace ------------------------------------------------------------ */

static bool nvs_load(void *ctx, const char *key, void *buf, size_t len) {
    nvs_handle_t h;
    size_t got = len;
    (void)ctx;
    if (nvs_open("tern", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_get_blob(h, key, buf, &got) == ESP_OK && got == len;
    nvs_close(h);
    return ok;
}

static bool nvs_save(void *ctx, const char *key, const void *buf, size_t len) {
    nvs_handle_t h;
    (void)ctx;
    if (nvs_open("tern", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_blob(h, key, buf, len) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

/* The ESP32's generator, which is a true one while its entropy source is on (app_main()). */
static bool board_random(void *ctx, uint8_t *buf, size_t len) {
    (void)ctx;
    esp_fill_random(buf, len);
    return true;
}

/* --- Sending and receiving ------------------------------------------------------------------ */

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
    case DEMO_SPENT:
        return "this session has used every counter; make contact again";
    case DEMO_TOO_LONG:
        return "too long: at most 232 bytes";
    }
    return "?";
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
        if (!nvs_save(NULL, "airtime", &used, sizeof used)) {
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

/* Queues a message to the peer, as a client's would be: poll_outgoing() sends it. */
static void send(const char *text) {
    if (demo.s.role == 0) {
        printf("not sent: %s\n", result_text(DEMO_UNPAIRED));
        return;
    }
    if (strlen(text) > TERN_COMPANION_TEXT_MAX) {
        printf("not sent: too long: at most %d bytes\n", TERN_COMPANION_TEXT_MAX);
        return;
    }
    if (link_add(&companion, demo.s.peer, clock_now(), TERN_C_WAITING, TERN_C_WAIT_UNNAMED,
                 (const uint8_t *)text, strlen(text)) == 0) {
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
    if (sealed_id != x->id) {
        sealed_counter = demo.s.session.tx.next;
        enum demo_result r = demo_seal(&demo, x->text, x->text_len, sealed);
        if (r != DEMO_OK) {
            printf("not sent: %s\n", result_text(r));
            *gone = r == DEMO_SPENT;
            return false;
        }
        sealed_id = x->id;
        sealed_len = x->text_len + TERN_UNICAST_OVERHEAD;
    }
    if (!tern_forward_send(&forward, board_now(), tern_route_id(demo.s.peer), sealed, sealed_len,
                           true, INT8_MIN)) {
        return false; /* no room: every slot holds a frame */
    }
    *p = (struct pending){.id = x->id, .counter = sealed_counter, .at = board_now()};
    memcpy(p->tag, &sealed[TERN_FORWARD_HEAD], TERN_FORWARD_TAG);
    sealed_id = 0;
    return true;
}

/* The peer changed, or the session did: what was sent in the old one can no longer be
 * acknowledged, so the forwarder lets go of it, and the client is told. */
static void pending_drop(void) {
    for (int i = 0; i < PENDING; i++) {
        if (pending[i].id != 0) {
            /* The forwarder's only way to let a message go: as if it had been acknowledged. */
            (void)tern_forward_acked(&forward, pending[i].tag);
            link_state(&companion, pending[i].id, TERN_C_NOT_DELIVERED, 0, 0);
            pending[i].id = 0;
        }
    }
}

/* Sends a handshake frame, or keeps it until the frame on the air has gone. */
static void send_contact(const uint8_t *frame, size_t len) {
    if (transmitting) {
        memcpy(waiting, frame, len);
        waiting_len = len;
    } else if (transmit(frame, len) != 0) {
        printf("first contact: sent message_%d, %u bytes\n", frame[0] - 0x50, (unsigned)len);
    }
}

static void print_address(const uint8_t address[TERN_ADDRESS_LEN]) {
    for (int i = 0; i < TERN_ADDRESS_LEN; i++) {
        printf("%02x", address[i]);
    }
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

/* An acknowledgement that came to this board: taken only if it is the peer's, of a message this
 * board is still waiting on. */
static void heard_ack(const struct tern_radio_event *ev) {
    struct pending *p =
        ev->len == TERN_ACK_LEN ? pending_tagged(&ev->data[TERN_FORWARD_HEAD]) : NULL;
    if (p == NULL || !demo_acked(&demo, p->counter, ev->data, ev->len)) {
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
    link_state(&companion, p->id, TERN_C_DELIVERED, 0, 0);
    p->id = 0;
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
        if (ev->len >= 5 && ev->data[0] == 0x59) {
            note_snr(ev->data, (int8_t)(ev->snr_cdb / 25));
        }
        /* The router takes quarters of a decibel, as the radio measures. */
        tern_route_heard(&route, board_now(), ev->data, ev->len, (int16_t)(ev->snr_cdb / 25));
        if (route.seq != route_seq_saved && nvs_save(NULL, "seq", &route.seq, sizeof route.seq)) {
            route_seq_saved = route.seq;
        }
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
        if (routed.got != TERN_FORWARD_MESSAGE) {
            return;
        }
    }
    /* A handshake that completes replaces the session: the old peer is told of too. */
    bool had_peer = demo.s.role != 0;
    uint8_t old_peer[TERN_ADDRESS_LEN];
    memcpy(old_peer, demo.s.peer, TERN_ADDRESS_LEN);
    enum demo_heard what = demo_receive(&demo, board_now(), ev->data, ev->len, msg, &got);
    switch (what) {
    case DEMO_HEARD_MESSAGE:
        flash_led();
        msg[got.msg_len] = '\0';
        printf("heard #%lu \"%s\" at %d dBm, SNR %s%d.%02d dB\n", (unsigned long)got.counter,
               (const char *)msg, ev->rssi_dbm, snr_sign, snr_whole, snr_frac);
        /* As much as the screen keeps of it. */
        size_t keep = got.msg_len < sizeof last_text - 1 ? got.msg_len : sizeof last_text - 1;
        memcpy(last_text, msg, keep);
        last_text[keep] = '\0';
        last_at = board_now();
        link_add(&companion, got.peer, clock_now(), TERN_C_RECEIVED, 0, msg, got.msg_len);
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
        last_at = 0; /* the last message was the old session's */
        printf("first contact: complete. Session started, as the %s, with ",
               demo.s.role == TERN_INITIATOR ? "initiator" : "responder");
        print_address(got.peer);
        printf("\n");
        sealed_id = 0; /* a frame sealed in the old session is no use in the new */
        pending_drop();
        if (contacting && memcmp(contacting_peer, got.peer, TERN_ADDRESS_LEN) == 0) {
            contacting = false;
        }
        link_session_changed(&companion, got.peer);
        if (had_peer && memcmp(old_peer, got.peer, TERN_ADDRESS_LEN) != 0) {
            link_session_changed(&companion, old_peer);
        }
        break;
    case DEMO_HEARD_REFUSED:
        printf("first contact: refused ");
        print_address(got.peer);
        printf(", which is not this board's peer. Type 'accept' to let it in, and have it try "
               "again.\n");
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
    if (got.reply_len != 0) {
        send_contact(got.reply, got.reply_len);
    }
    /* The acknowledgement goes back by a route, no quieter than the node the message came from
     * needs to hear it. With no route to the peer it is not sent: the peer sends again. */
    for (size_t i = 0; i < got.acks; i++) {
        if (!tern_forward_send(&forward, board_now(), tern_route_id(got.peer), got.ack[i],
                               sizeof got.ack[i], false, routed.back)) {
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
        if (waiting_len != 0) {
            size_t len = waiting_len;
            waiting_len = 0;
            send_contact(waiting, len);
        }
        break;
    case TERN_RADIO_RX_DONE:
        frames_in++;
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

/* Sends a handshake frame again if its answer is overdue, and forgets handshakes gone stale. */
static void poll_contact(void) {
    uint8_t frame[TERN_CONTACT_MAX_FRAME];
    size_t len;
    if (transmitting) {
        return;
    }
    switch (demo_tick(&demo, board_now(), frame, &len)) {
    case DEMO_TICK_RESEND:
        printf("first contact: no answer yet\n");
        send_contact(frame, len);
        break;
    case DEMO_TICK_GAVE_UP:
        printf("first contact: no answer after %d tries; given up. Is the other board on, in "
               "range, and on the same radio settings?\n",
               DEMO_TRIES);
        if (contacting) {
            contacting = false;
            link_unreachable(&companion, contacting_peer);
        }
        break;
    case DEMO_TICK_LAPSED:
        printf("first contact: the board that began it went quiet; forgotten\n");
        break;
    case DEMO_TICK_NONE:
        break;
    }
}

/* Hands the oldest message waiting to the forwarder, or says why it waits. One session at a time:
 * a message to another node starts first contact with it, which replaces the session the demo
 * has, once no handshake is under way. */
static void poll_outgoing(void) {
    struct link_message *x = link_outgoing(&companion);
    if (x == NULL) {
        return;
    }
    bool session = demo.s.role != 0 && memcmp(demo.s.peer, x->address, TERN_ADDRESS_LEN) == 0;
    if (!session) {
        link_state(&companion, x->id, TERN_C_WAITING, TERN_C_WAIT_SESSION, 0);
        bool idle = demo.h.phase == DEMO_IDLE || demo.h.phase == DEMO_ANSWERED;
        if (!idle || transmitting) {
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
        send_contact(frame, len);
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

/* Why a message the forwarder has is not yet delivered, as far as the board can name it. */
static uint8_t pending_reason(tern_time now) {
    uint32_t next;
    uint16_t metric;
    if (!tern_route_next(&route, tern_route_id(demo.s.peer), &next, &metric)) {
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
            link_state(&companion, p->id, TERN_C_NOT_DELIVERED, 0, 0);
            p->id = 0;
        }
    }
    for (int i = 0; i < PENDING; i++) {
        if (pending[i].id != 0) {
            link_state(&companion, pending[i].id, TERN_C_WAITING, pending_reason(now), 0);
        }
    }
    if (transmitting || waiting_len != 0 || now < forward_retry ||
        now < tern_forward_due(&forward)) {
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
    } else if (kind == TERN_FORWARD_RELAY) {
        printf("passed on a %u-byte frame for %02x%02x%02x%02x by %02x%02x%02x%02x, at %d dBm\n",
               (unsigned)len, frame[7], frame[8], frame[9], frame[10], frame[3], frame[4], frame[5],
               frame[6], dbm);
    }
}

/* Sends what the router has to send: its announces, and its requests for routes. A frame the
 * router has handed over is its only copy, and what it said is counted as said - a retraction as
 * one of its three - so the frame is kept until it has gone, and the router is not asked for
 * another while the region's limit would refuse one. */
static void poll_route(void) {
    tern_time now = board_now();
    if (transmitting || waiting_len != 0) {
        return;
    }
    if (route_len == 0) {
        if (now < tern_route_due(&route) ||
            !tern_duty_allows(&duty, now, tern_lora_airtime(&cfg.mod, TERN_ROUTE_FRAME_MAX))) {
            return;
        }
        route_len = tern_route_poll(&route, now, route_frame, &route_dbm);
        route_retry = 0;
    }
    if (route_len == 0 || now < route_retry ||
        !tern_duty_allows(&duty, now, tern_lora_airtime(&cfg.mod, (uint32_t)route_len))) {
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
    const struct tern_sx126x_counts *c = &sx.counts;
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

/* The band a region's radios keep to, for a frequency set by hand. */
static void bench_band(uint32_t *lo, uint32_t *hi) {
#if CONFIG_TERN_REGION_EU868
    *lo = 863000000;
    *hi = 870000000;
#else
    *lo = 902000000;
    *hi = 928000000;
#endif
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
        sx.counts = (struct tern_sx126x_counts){0};
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
            printf("sync word 0x%02X, which the radio takes as 0x%04X\n", cfg.sync_word,
                   tern_sync_word_sx126x(cfg.sync_word));
        }
    } else if (is_power) {
        struct tern_radio_config allowed;
        if (dbm < TX_MIN_DBM || dbm > 22 ||
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

/* --- The companion link --------------------------------------------------------------------- */

/* One frame to the client, after whatever the console has printed. */
static void link_out(void *ctx, const uint8_t *frame, size_t len) {
    uint8_t wrapped[TERN_COMPANION_STREAM_MAX];
    (void)ctx;
    size_t n = tern_companion_wrap(frame, len, wrapped);
    fflush(stdout);
    if (n != 0) {
        uart_write_bytes(CONSOLE, wrapped, n);
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

static void link_view(void *ctx, struct link_view *v) {
    tern_time now = board_now();
    (void)ctx;
    memset(v, 0, sizeof *v);
    memcpy(v->address, demo.id.address, TERN_ADDRESS_LEN);
    v->role = route.config.relay ? 1 : 0;
    v->region = region->name;
    v->power = cfg.tx_power_dbm;
    v->time = clock_now();
    for (int i = 0; i < NEIGHBOURS && v->n_neighbours < LINK_NEIGHBOURS; i++) {
        const struct tern_route_neighbour *n = &neighbours[i];
        if (!n->used) {
            continue;
        }
        int8_t snr = 0;
        for (size_t k = 0; k < NEIGHBOURS; k++) {
            snr = heard_snr[k].id == n->id ? heard_snr[k].snr : snr;
        }
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
    /* The board's battery is not read yet. */
    v->millivolts = 0;
    v->percent = 255;
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
 * were set up with them; the passkey is kept for Bluetooth, which this build does not have. */
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
        if (m->power < TX_MIN_DBM || m->power > 22 ||
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
    default:
        return TERN_C_ERR_UNKNOWN;
    }
    if (!nvs_save(NULL, "settings", &next, sizeof next)) {
        return TERN_C_ERR_NOT_NOW;
    }
    settings = next;
    restart_due = restart_due || restart;
    return 0;
}

static void link_set_time(void *ctx, uint32_t time) {
    (void)ctx;
    clock_base = time;
    clock_at = board_now();
}

static bool link_session(void *ctx, const uint8_t address[TERN_ADDRESS_LEN]) {
    (void)ctx;
    return demo.s.role != 0 && memcmp(demo.s.peer, address, TERN_ADDRESS_LEN) == 0;
}

static uint8_t link_why(void *ctx, const uint8_t address[TERN_ADDRESS_LEN]) {
    return link_session(ctx, address) ? pending_reason(board_now()) : TERN_C_WAIT_SESSION;
}

static bool link_load(void *ctx, void *buf, size_t len) {
    return nvs_load(ctx, "contacts", buf, len);
}

static bool link_save(void *ctx, const void *buf, size_t len) {
    return nvs_save(ctx, "contacts", buf, len);
}

/* --- The console ---------------------------------------------------------------------------- */

/* Sixty-four hex digits. */
static bool parse_address(const char *text, uint8_t address[TERN_ADDRESS_LEN]) {
    int digits = 0;
    for (; *text != '\0'; text++) {
        char c = *text;
        int v = c >= '0' && c <= '9'   ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                       : -1;
        if (v < 0 || digits == 2 * TERN_ADDRESS_LEN) {
            return false;
        }
        address[digits / 2] = (uint8_t)(digits % 2 ? address[digits / 2] << 4 | v : v);
        digits++;
    }
    return digits == 2 * TERN_ADDRESS_LEN;
}

static void status(void) {
    const struct demo_state *s = &demo.s;
    printf("this board's address: ");
    print_address(demo.id.address);
    printf("\n");
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
           "route, %lu with no room\n",
           (unsigned long)fc->passed_on, (unsigned long)fc->sent_again, (unsigned long)fc->given_up,
           (unsigned long)fc->no_route, (unsigned long)fc->no_room);
    if (demo.h.phase == DEMO_INITIATING || demo.h.phase == DEMO_RESPONDING) {
        printf("first contact: under way, as the %s\n",
               demo.h.phase == DEMO_INITIATING ? "initiator" : "responder");
    }
    if (s->role == 0) {
        printf("session: none. %s\n", result_text(DEMO_UNPAIRED));
        return;
    }
    printf("session: as %s, %lu sent (next counter %lu), %lu heard, with ",
           s->role == TERN_INITIATOR ? "initiator" : "responder", (unsigned long)s->sent,
           (unsigned long)s->session.tx.next, (unsigned long)s->heard);
    print_address(s->peer);
    printf("\n");
    if (board_now() < demo.accept_until) {
        printf("accepting contact from a new peer for another %lld s\n",
               (long long)((demo.accept_until - board_now()) / 1000000000LL));
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

/* --- The self-test: a handshake between two nodes in this board's memory ------------------- */

/* The board has one radio and, on a bench with one board, nobody to talk to. This runs the same
 * code two boards would, handing each node's frames to the other, to show that it works on this
 * chip and how long the arithmetic takes. Nothing goes on the air, and nothing is saved. */
struct memory_store {
    bool have_identity, have_session;
    uint8_t identity[36];
    struct demo_state session;
};

static bool memory_slot(struct memory_store *m, const char *key, size_t len, void **slot,
                        bool **have) {
    bool identity = strcmp(key, "identity") == 0;
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
    static struct memory_store flash[2];
    static struct demo node[2];
    static uint8_t msg[TERN_UNICAST_MAX_PLAINTEXT];
    static const char *const step[] = {"respond to message_1", "message_2 to message_3",
                                       "message_3 to message_4", "accept message_4"};
    struct demo_received got;
    uint8_t frame[TERN_CONTACT_MAX_FRAME];
    size_t len = 0;
    bool ok = true;
    tern_time t0 = board_now(), t;

    memset(flash, 0, sizeof flash);
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
        uint8_t sealed[5 + TERN_UNICAST_OVERHEAD];
        ok =
            demo_seal(&node[i], (const uint8_t *)"hello", 5, sealed) == DEMO_OK &&
            demo_receive(&node[1 - i], 0, sealed, sizeof sealed, msg, &got) == DEMO_HEARD_MESSAGE &&
            got.msg_len == 5 && memcmp(msg, "hello", 5) == 0 && got.acks == 1 &&
            demo_acked(&node[i], 0, got.ack[0], sizeof got.ack[0]);
    }
    printf("selftest: %s, in %lld ms; %u bytes of this task's stack never used\n",
           ok ? "passed" : "FAILED", (long long)((board_now() - t0) / 1000000),
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
    memset(flash, 0, sizeof flash);
    memset(node, 0, sizeof node);
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
        if (!parse_address(&line[8], peer)) {
            printf("'%s' is not an address: sixty-four hex digits, as 'status' shows\n", &line[8]);
            return;
        }
        if (transmitting) {
            printf("busy: the last frame is still on the air\n");
            return;
        }
        enum demo_result r = demo_contact(&demo, peer, board_now(), frame, &len);
        if (r != DEMO_OK) {
            printf("no contact made: %s\n", result_text(r));
            return;
        }
        send_contact(frame, len);
    } else if (strcmp(line, "accept") == 0) {
        demo_accept(&demo, board_now() + ACCEPT_S * 1000000000LL);
        printf("for %d s, a board that is not this one's peer may make contact, and will "
               "replace it\n",
               ACCEPT_S);
    } else if (strncmp(line, "send ", 5) == 0) {
        send(&line[5]);
    } else if (strcmp(line, "status") == 0) {
        status();
    } else if (strcmp(line, "routes") == 0) {
        routes();
    } else if (strcmp(line, "selftest") == 0) {
        selftest();
    } else if (line[0] != '\0') {
        printf(
            "commands: contact <address>, accept, send <text>, status, routes, selftest. Holding "
            "PRG sends a ping. For the bench: bench on|off, sync <hex>, power <dBm>, freq <Hz>, "
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

/* A companion frame. A setting that takes a restart is applied once its answer has gone. */
static void console_frame(void *ctx, const uint8_t *frame, size_t len) {
    (void)ctx;
    link_receive(&companion, board_now(), frame, len);
    if (restart_due) {
        printf("restarting to apply a setting\n");
        fflush(stdout);
        uart_wait_tx_done(CONSOLE, pdMS_TO_TICKS(500));
        esp_restart();
    }
}

static void poll_console(void) {
    static const struct tern_companion_sink sink = {NULL, console_frame, console_text};
    uint8_t c;
    while (uart_read_bytes(CONSOLE, &c, 1, 0) == 1) {
        tern_companion_push(&parser, board_now(), c, &sink);
        fflush(stdout);
    }
    tern_companion_idle(&parser, board_now(), &sink);
    link_tick(&companion, board_now());
}

/* --- The bench screen ------------------------------------------------------------------------ */

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

    st->session = demo.s.role != 0;
    st->contacting = demo.h.phase == DEMO_INITIATING || demo.h.phase == DEMO_RESPONDING;
    st->peer = (uint32_t)demo.s.peer[0] << 24 | (uint32_t)demo.s.peer[1] << 16 |
               (uint32_t)demo.s.peer[2] << 8 | demo.s.peer[3];
    st->sent = demo.s.sent;
    st->received = demo.s.heard;
    st->have_last = last_at != 0;
    memcpy(st->last, last_text, sizeof st->last);
    st->last_s = (uint32_t)((now - last_at) / 1000000000LL);
}

/* Draws the page shown every SCREEN_MS, and sends at most one changed page of the picture each
 * turn of the loop, about 3 ms, so the radio is never kept waiting long. A write that fails can
 * take 50 ms (board.c), so after one the screen waits a second, and after SCREEN_TRIES in a row
 * it is given up. */
static void poll_screen(void) {
    if (!have_screen) {
        return;
    }
    if (board_now() >= screen_due) {
        static struct node_status st;
        char rows[STATUS_ROWS][STATUS_COLS + 1];
        fill_status(&st);
        status_page(&st, screen_page, rows);
        for (int i = 0; i < STATUS_ROWS; i++) {
            display_text(&screen, i, rows[i], i == 0);
        }
        screen_due = board_now() + (tern_time)SCREEN_MS * 1000000;
    }
    if (board_now() < screen_retry) {
        return;
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
    screen_retry = board_now() + 1000000000LL;
    if (++screen_failures >= SCREEN_TRIES) {
        have_screen = false;
        printf("the screen stopped answering; carrying on without it. PRG now sends a ping.\n");
    }
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

/* A press shows the next page; holding PRG for HOLD_MS sends a ping, once, while it is still
 * held. With no screen, a press sends a ping, as it did before there was one. */
static void poll_button(void) {
    static bool was, held;
    static tern_time down, last;
    tern_time now = board_now();
    bool pressed = board_button();
    if (pressed && !was) {
        down = now;
        held = false;
    } else if (have_screen && pressed && !held && now - down >= (tern_time)HOLD_MS * 1000000) {
        held = true;
        ping();
    } else if (!pressed && was && !held && now - last > 300 * 1000000LL) {
        last = now;
        if (have_screen) {
            screen_page = (screen_page + 1) % STATUS_PAGES;
            screen_due = 0;
        } else {
            ping();
        }
    }
    was = pressed;
}

void app_main(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Never erased to make room: the saved session is what stops counters repeating, and the
         * identity is the board's address. */
        printf("NVS needs erasing (%s). Not starting: this board's identity and session would "
               "be lost. Erase the flash yourself; the board will then have a new address.\n",
               esp_err_to_name(e));
        return;
    }
    ESP_ERROR_CHECK(e);
    ESP_ERROR_CHECK(uart_driver_install(CONSOLE, 512, 0, 0, NULL, 0));

    /* With Wi-Fi and Bluetooth off, the chip's generator has no entropy of its own until this
     * turns on its noise source. It stays on: this demo uses nothing it conflicts with (the
     * ADC, I2S, Wi-Fi and Bluetooth), and a port that does must turn it off around them. */
    bootloader_random_enable();

#if CONFIG_TERN_REGION_EU868
    region = tern_region(TERN_REGION_EU868);
#else
    region = tern_region(TERN_REGION_US915);
#endif
    int8_t power = CONFIG_TERN_TX_POWER_DBM;
    /* What a client set, over what the build chose. Each was checked against the region and the
     * antenna when it was set; if the pair no longer fits, the build's settings are used. */
    struct settings saved;
    if (nvs_load(NULL, "settings", &saved, sizeof saved) && saved.magic == SETTINGS_MAGIC) {
        settings = saved;
    }
    const struct tern_region *chosen =
        settings.region != 0 ? tern_region((enum tern_region_id)settings.region) : NULL;
    int8_t chosen_power = settings.power != POWER_UNSET ? settings.power : power;
    if (chosen == NULL) {
        chosen = region;
    }
    if (tern_region_radio(chosen, chosen_power, CONFIG_TERN_ANTENNA_DBI, &cfg) == TERN_OK) {
        region = chosen;
        power = chosen_power;
    } else {
        printf("the settings a client saved do not fit this antenna; using the build's\n");
    }
    if (tern_region_radio(region, power, CONFIG_TERN_ANTENNA_DBI, &cfg) != TERN_OK) {
        printf("%d dBm into a %d dBi antenna is more than %s allows (%d dBm radiated). Not "
               "starting.\n",
               power, CONFIG_TERN_ANTENNA_DBI, region->name, region->max_eirp_dbm);
        return;
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
    /* The board's clock starts again at a restart, so the times of what it sent before are
     * lost. All of it is counted as sent now, which can only hold the board back longer. */
    tern_duty_init(&duty, region->duty_ppm, region->duty_window_s);
    tern_time before = 0;
    if (nvs_load(NULL, "airtime", &before, sizeof before)) {
        tern_duty_charge(&duty, board_now(), before);
    }

    /* How long the initiator waits for an answer: the answer's time on the air and its own
     * frame's, twice over, and two seconds for the other board to work it out. */
    tern_time retry = 2000000000LL + 4 * tern_lora_airtime(&cfg.mod, TERN_CONTACT_MAX_FRAME);
    struct demo_store store = {
        .ctx = NULL, .load = nvs_load, .save = nvs_save, .random = board_random};
    if (!demo_start(&demo, &store, retry)) {
        printf("could not make this board's identity and save it to flash. Not starting.\n");
        return;
    }

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
        return;
    }
    (void)nvs_load(NULL, "seq", &route_seq_saved, sizeof route_seq_saved);
    tern_route_init(&route, &rc, tern_route_id(demo.id.address), neighbours, NEIGHBOURS,
                    destinations, DESTINATIONS, route_seq_saved, seed, board_now());
    power_now = cfg.tx_power_dbm;
    struct tern_forward_config fc = tern_forward_defaults();
    tern_forward_init(&forward, &fc, &route, forward_slots, FORWARD_SLOTS, seed ^ 0x666f7277u);

    int err = board_init(&sx);
    radio = tern_sx126x_radio(&sx);
    if (err == TERN_OK) {
        err = tern_radio_configure(&radio, &cfg);
    }
    if (err == TERN_OK) {
        err = tern_radio_receive(&radio);
    }
    if (err != TERN_OK) {
        printf("the radio did not start (error %d)\n", err);
        return;
    }

    struct link_host host = {
        .ctx = NULL,
        .firmware = FIRMWARE,
        .lapse = LINK_LAPSE,
        .out = link_out,
        .view = link_view,
        .set = link_set,
        .set_time = link_set_time,
        .session = link_session,
        .why = link_why,
        .load = link_load,
        .save = link_save,
    };
    link_init(&companion, &host);
    tern_companion_parser_init(&parser);

    have_screen = board_screen_init();
    if (have_screen) {
        display_init(&screen);
        printf("\nTern demo on the Heltec V3. Type 'status'. PRG shows the screen's next page; "
               "hold it to ping.\n");
    } else {
        printf("\nTern demo on the Heltec V3, with no screen found. Type 'status', or press PRG "
               "to ping.\n");
    }
    status();
    for (;;) {
        poll_radio();
        if (bench) {
            poll_beacon();
        } else {
            poll_contact();
            poll_outgoing();
            poll_forward();
            poll_route();
        }
        poll_console();
        poll_button();
        poll_screen();
        if (transmitting && board_now() > tx_deadline) {
            /* TX_DONE never came. Listen again rather than stay busy for ever. */
            printf("the radio never said the frame had gone; listening again\n");
            transmitting = false;
            if (forward_out) {
                /* Counted as gone: the forwarder listens for it, and sends it again unheard. */
                tern_forward_sent(&forward, board_now(), forward_handle);
                forward_out = false;
            }
            tern_radio_receive(&radio);
        }
        if (led_until != 0 && board_now() > led_until && !transmitting) {
            board_led(false);
            led_until = 0;
        }
        vTaskDelay(1);
    }
}
