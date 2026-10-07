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
#include "tern/err.h"
#include "tern/lora.h"
#include "tern/radio.h"
#include "tern/sx126x.h"

#define CONSOLE UART_NUM_0
#define CONSOLE_LINE 300
#define LED_MS 150
#define ACCEPT_S 120

static struct tern_sx126x sx;
static struct tern_radio radio;
static struct tern_radio_config cfg;
static struct demo demo;
static bool transmitting;
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

/* Puts a frame on the air. Returns its time on air, or 0 if the radio refused it. */
static tern_time transmit(const uint8_t *frame, size_t len) {
    int err = tern_radio_transmit(&radio, frame, (uint32_t)len);
    if (err != TERN_OK) {
        printf("radio error %d: not sent\n", err);
        return 0;
    }
    tern_time air = tern_lora_airtime(&cfg.mod, (uint32_t)len);
    transmitting = true;
    tx_deadline = board_now() + air + 2000000000LL;
    flash_led();
    return air;
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
    printf("radio: %lu Hz, SF%u, %lu Hz, CR 4/%u, %d dBm, sync word 0x%02X\n",
           (unsigned long)cfg.freq_hz, cfg.mod.sf, (unsigned long)cfg.mod.bw_hz, 4 + cfg.mod.cr,
           cfg.tx_power_dbm, cfg.sync_word);
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
    } else if (strcmp(line, "selftest") == 0) {
        selftest();
    } else if (line[0] != '\0') {
        printf("commands: contact <address>, accept, send <text>, status, selftest. PRG sends "
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

    cfg = (struct tern_radio_config){
        .mod = tern_lora_default(CONFIG_TERN_SF, CONFIG_TERN_BW_HZ),
        .freq_hz = CONFIG_TERN_FREQ_HZ,
        .tx_power_dbm = CONFIG_TERN_TX_POWER_DBM,
        .sync_word = CONFIG_TERN_SYNC_WORD,
    };
    cfg.mod.preamble = 16;

    /* How long the initiator waits for an answer: the answer's time on the air and its own
     * frame's, twice over, and two seconds for the other board to work it out. */
    tern_time retry = 2000000000LL + 4 * tern_lora_airtime(&cfg.mod, TERN_CONTACT_MAX_FRAME);
    struct demo_store store = {
        .ctx = NULL, .load = nvs_load, .save = nvs_save, .random = board_random};
    if (!demo_start(&demo, &store, retry)) {
        printf("could not make this board's identity and save it to flash. Not starting.\n");
        return;
    }

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
