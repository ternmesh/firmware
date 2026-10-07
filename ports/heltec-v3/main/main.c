/* The Heltec V3 demo: two boards make first contact and send each other secured unicast frames.
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
 * Pressing PRG sends a ping, and a board that receives a ping answers with a pong saying how
 * well it heard it. The LED lights while a frame is on the air or has just arrived.
 *
 * Everything runs in one loop: the core never runs in interrupt context, so the loop polls the
 * radio, the serial port and the button in turn. */

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "bootloader_random.h"
#include "demo.h"
#include "driver/uart.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "tern/duty.h"
#include "tern/err.h"
#include "tern/lora.h"
#include "tern/radio.h"
#include "tern/region.h"
#include "tern/route.h"
#include "tern/sx126x.h"

#define CONSOLE UART_NUM_0
#define CONSOLE_LINE 300
#define LED_MS 150
#define ACCEPT_S 120
#define NEIGHBOURS 32
#define DESTINATIONS 128
#define TX_MIN_DBM (-9) /* the SX1262's least */
#define POWER_UNSET INT8_MIN

static struct tern_sx126x sx;
static struct tern_radio radio;
static struct tern_radio_config cfg;
static const struct tern_region *region;
static bool off_profile; /* the build changed the region's frequency or modulation */
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
/* A handshake frame that had to wait for the one on the air. */
static uint8_t waiting[TERN_CONTACT_MAX_FRAME];
static size_t waiting_len;
static tern_time tx_deadline; /* when a frame on the air should certainly have finished */
static tern_time led_until;

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
        return "too long: at most 239 bytes";
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
    tx_deadline = board_now() + air + 2000000000LL;
    flash_led();
    return air;
}

static tern_time transmit(const uint8_t *frame, size_t len) {
    return transmit_at(frame, len, cfg.tx_power_dbm);
}

static void send(const char *text) {
    size_t len = strlen(text);
    uint8_t frame[TERN_UNICAST_MAX_FRAME];
    if (transmitting) {
        printf("busy: the last frame is still on the air\n");
        return;
    }
    uint32_t counter = demo.s.session.tx.next;
    enum demo_result r = demo_seal(&demo, (const uint8_t *)text, len, frame);
    if (r != DEMO_OK) {
        printf("not sent: %s\n", result_text(r));
        return;
    }
    size_t frame_len = len + TERN_UNICAST_OVERHEAD;
    tern_time air = transmit(frame, frame_len);
    if (air != 0) {
        printf("sent #%lu \"%s\": %u bytes, %lld.%03lld ms on the air\n", (unsigned long)counter,
               text, (unsigned)frame_len, (long long)(air / 1000000),
               (long long)(air / 1000 % 1000));
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

static void heard(const struct tern_radio_event *ev) {
    uint8_t msg[TERN_UNICAST_MAX_PLAINTEXT + 1];
    struct demo_received got;
    /* SNR comes in centibels: -725 is -7.25 dB. */
    const char *snr_sign = ev->snr_cdb < 0 ? "-" : "";
    int snr_abs = ev->snr_cdb < 0 ? -ev->snr_cdb : ev->snr_cdb;
    int snr_whole = snr_abs / 100, snr_frac = snr_abs % 100;

    if (tern_route_frame(ev->data, ev->len)) {
        /* The router takes quarters of a decibel, as the radio measures. */
        tern_route_heard(&route, board_now(), ev->data, ev->len, (int16_t)(ev->snr_cdb / 25));
        if (route.seq != route_seq_saved && nvs_save(NULL, "seq", &route.seq, sizeof route.seq)) {
            route_seq_saved = route.seq;
        }
        return;
    }
    enum demo_heard what = demo_receive(&demo, board_now(), ev->data, ev->len, msg, &got);
    switch (what) {
    case DEMO_HEARD_MESSAGE:
        flash_led();
        msg[got.msg_len] = '\0';
        printf("heard #%lu \"%s\" at %d dBm, SNR %s%d.%02d dB\n", (unsigned long)got.counter,
               (const char *)msg, ev->rssi_dbm, snr_sign, snr_whole, snr_frac);
        if (strncmp((const char *)msg, "ping", 4) == 0) {
            char reply[64];
            snprintf(reply, sizeof reply, "pong to #%lu: %d dBm, SNR %s%d.%02d dB",
                     (unsigned long)got.counter, ev->rssi_dbm, snr_sign, snr_whole, snr_frac);
            send(reply);
        }
        break;
    case DEMO_HEARD_CONTACT:
        flash_led();
        printf("first contact: heard message_%d at %d dBm, SNR %s%d.%02d dB\n", ev->data[0] - 0x50,
               ev->rssi_dbm, snr_sign, snr_whole, snr_frac);
        break;
    case DEMO_HEARD_PAIRED:
        flash_led();
        printf("first contact: complete. Session started, as the %s, with ",
               demo.s.role == TERN_INITIATOR ? "initiator" : "responder");
        print_address(got.peer);
        printf("\n");
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
        tern_radio_receive(&radio);
        if (waiting_len != 0) {
            size_t len = waiting_len;
            waiting_len = 0;
            send_contact(waiting, len);
        }
        break;
    case TERN_RADIO_RX_DONE:
        heard(&ev);
        break;
    case TERN_RADIO_RX_ERROR:
        printf("(a damaged frame)\n");
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
        break;
    case DEMO_TICK_LAPSED:
        printf("first contact: the board that began it went quiet; forgotten\n");
        break;
    case DEMO_TICK_NONE:
        break;
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
           off_profile ? " (changed by this build)" : "", (unsigned long)cfg.freq_hz, cfg.mod.sf,
           (unsigned long)cfg.mod.bw_hz, 4 + cfg.mod.cr, cfg.tx_power_dbm, cfg.sync_word);
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
        /* A message each way, in the session the handshake made. */
        uint8_t sealed[5 + TERN_UNICAST_OVERHEAD];
        ok =
            demo_seal(&node[i], (const uint8_t *)"hello", 5, sealed) == DEMO_OK &&
            demo_receive(&node[1 - i], 0, sealed, sizeof sealed, msg, &got) == DEMO_HEARD_MESSAGE &&
            got.msg_len == 5 && memcmp(msg, "hello", 5) == 0;
    }
    printf("selftest: %s, in %lld ms; %u bytes of this task's stack never used\n",
           ok ? "passed" : "FAILED", (long long)((board_now() - t0) / 1000000),
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
    memset(flash, 0, sizeof flash);
    memset(node, 0, sizeof node);
}

static void command(char *line) {
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
            "commands: contact <address>, accept, send <text>, status, routes, selftest. PRG sends "
            "a ping.\n");
    }
}

static void poll_console(void) {
    static char line[CONSOLE_LINE];
    static size_t used;
    uint8_t c;
    while (uart_read_bytes(CONSOLE, &c, 1, 0) == 1) {
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
        fflush(stdout);
    }
}

static void poll_button(void) {
    static bool was;
    static tern_time last;
    static unsigned pings;
    bool now = board_button();
    if (now && !was && board_now() - last > 300 * 1000000LL) {
        char text[32];
        last = board_now();
        snprintf(text, sizeof text, "ping %u", ++pings);
        send(text);
    }
    was = now;
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
    if (tern_region_radio(region, CONFIG_TERN_TX_POWER_DBM, CONFIG_TERN_ANTENNA_DBI, &cfg) !=
        TERN_OK) {
        printf("%d dBm into a %d dBi antenna is more than %s allows (%d dBm radiated). Not "
               "starting.\n",
               CONFIG_TERN_TX_POWER_DBM, CONFIG_TERN_ANTENNA_DBI, region->name,
               region->max_eirp_dbm);
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

    printf("\nTern demo on the Heltec V3. Type 'status', or press PRG to ping.\n");
    status();
    for (;;) {
        poll_radio();
        poll_contact();
        poll_route();
        poll_console();
        poll_button();
        if (transmitting && board_now() > tx_deadline) {
            /* TX_DONE never came. Listen again rather than stay busy for ever. */
            printf("the radio never said the frame had gone; listening again\n");
            transmitting = false;
            tern_radio_receive(&radio);
        }
        if (led_until != 0 && board_now() > led_until && !transmitting) {
            board_led(false);
            led_until = 0;
        }
        vTaskDelay(1);
    }
}
