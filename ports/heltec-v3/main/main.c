/* The Heltec V3 demo: two boards paired by a passphrase send each other secured unicast frames.
 *
 * Over the USB serial port (idf.py monitor, 115200 baud) it takes these commands:
 *
 *   pair i <passphrase>    pair as the initiator; the other board pairs as the responder
 *   pair r <passphrase>    pair as the responder
 *   send <text>            send a message
 *   status                 show the session and the radio settings
 *
 * Pressing PRG sends a ping, and a board that receives a ping answers with a pong saying how
 * well it heard it. The LED lights while a frame is on the air or has just arrived.
 *
 * Everything runs in one loop: the core never runs in interrupt context, so the loop polls the
 * radio, the serial port and the button in turn. */

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "demo.h"
#include "driver/uart.h"
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

static struct tern_sx126x sx;
static struct tern_radio radio;
static struct tern_radio_config cfg;
static struct demo demo;
static bool transmitting;
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
        return "not paired yet: use 'pair i <passphrase>' here and 'pair r <passphrase>' on "
               "the other board";
    case DEMO_REUSED:
        return "this board has paired with that passphrase before; choose a new one for both "
               "boards";
    case DEMO_FULL:
        return "this board has used up its passphrase list; erase its flash (idf.py "
               "erase-flash) and use a passphrase it has never had";
    case DEMO_STORE_FAILED:
        return "could not save the session to flash, so nothing was sent";
    case DEMO_CONFLICT:
        return "both boards took the same role, so this one no longer sends; pair both again "
               "with a new passphrase, one 'i' and one 'r'";
    case DEMO_SPENT:
        return "this session has used every counter; pair again with a new passphrase";
    case DEMO_TOO_LONG:
        return "too long: at most 239 bytes";
    }
    return "?";
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
    int err = tern_radio_transmit(&radio, frame, frame_len);
    if (err != TERN_OK) {
        printf("radio error %d: not sent\n", err);
        return;
    }
    tern_time air = tern_lora_airtime(&cfg.mod, (uint32_t)frame_len);
    transmitting = true;
    tx_deadline = board_now() + air + 2000000000LL;
    flash_led();
    printf("sent #%lu \"%s\": %u bytes, %lld.%03lld ms on the air\n", (unsigned long)counter, text,
           (unsigned)frame_len, (long long)(air / 1000000), (long long)(air / 1000 % 1000));
}

static void heard(const struct tern_radio_event *ev) {
    uint8_t msg[TERN_UNICAST_MAX_PLAINTEXT + 1];
    size_t len = 0;
    uint32_t counter = 0;
    /* SNR comes in centibels: -725 is -7.25 dB. */
    const char *snr_sign = ev->snr_cdb < 0 ? "-" : "";
    int snr_abs = ev->snr_cdb < 0 ? -ev->snr_cdb : ev->snr_cdb;
    int snr_whole = snr_abs / 100, snr_frac = snr_abs % 100;

    switch (demo_open(&demo, ev->data, ev->len, msg, &len, &counter)) {
    case DEMO_HEARD_MESSAGE:
        flash_led();
        msg[len] = '\0';
        printf("heard #%lu \"%s\" at %d dBm, SNR %s%d.%02d dB\n", (unsigned long)counter,
               (const char *)msg, ev->rssi_dbm, snr_sign, snr_whole, snr_frac);
        if (strncmp((const char *)msg, "ping", 4) == 0) {
            char reply[64];
            snprintf(reply, sizeof reply, "pong to #%lu: %d dBm, SNR %s%d.%02d dB",
                     (unsigned long)counter, ev->rssi_dbm, snr_sign, snr_whole, snr_frac);
            send(reply);
        }
        break;
    case DEMO_HEARD_CLASH:
        printf("!! a frame sent with this board's own keys: both boards are '%s'. This board "
               "will not send again until both are paired afresh, one 'i' and one 'r', with a "
               "new passphrase.\n",
               demo.s.role == TERN_INITIATOR ? "i" : "r");
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
        break;
    case TERN_RADIO_RX_DONE:
        heard(&ev);
        break;
    case TERN_RADIO_RX_ERROR:
        printf("(a damaged frame)\n");
        break;
    }
}

/* --- The console ---------------------------------------------------------------------------- */

static void status(void) {
    const struct demo_state *s = &demo.s;
    printf("radio: %lu Hz, SF%u, %lu Hz, CR 4/%u, %d dBm, sync word 0x%02X\n",
           (unsigned long)cfg.freq_hz, cfg.mod.sf, (unsigned long)cfg.mod.bw_hz, 4 + cfg.mod.cr,
           cfg.tx_power_dbm, cfg.sync_word);
    if (s->role == 0) {
        printf("session: none. %s\n", result_text(DEMO_UNPAIRED));
        return;
    }
    printf("session: %s, %lu sent (next counter %lu), %lu heard%s\n",
           s->role == TERN_INITIATOR ? "initiator" : "responder", (unsigned long)s->sent,
           (unsigned long)s->session.tx.next, (unsigned long)s->heard,
           s->conflict ? ", STOPPED: the roles clash" : "");
    printf("passphrases used on this board: %lu of %d\n", (unsigned long)demo.used.count,
           DEMO_MAX_PASSPHRASES);
}

static void command(char *line) {
    if (strncmp(line, "pair ", 5) == 0 && (line[5] == 'i' || line[5] == 'r') && line[6] == ' ' &&
        line[7] != '\0') {
        enum tern_role role = line[5] == 'i' ? TERN_INITIATOR : TERN_RESPONDER;
        enum demo_result r = demo_pair(&demo, role, &line[7]);
        memset(line, 0, strlen(line)); /* the passphrase */
        if (r == DEMO_OK) {
            printf("paired as %s\n", role == TERN_INITIATOR ? "initiator" : "responder");
        } else {
            printf("not paired: %s\n", result_text(r));
        }
    } else if (strncmp(line, "send ", 5) == 0) {
        send(&line[5]);
    } else if (strcmp(line, "status") == 0) {
        status();
    } else if (line[0] != '\0') {
        printf("commands: pair i|r <passphrase>, send <text>, status. PRG sends a ping.\n");
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
        /* Never erased to make room: the saved sessions are what stop counters repeating. */
        printf("NVS needs erasing (%s). Not starting: the saved session would be lost. Erase "
               "the flash yourself and pair both boards again with a new passphrase.\n",
               esp_err_to_name(e));
        return;
    }
    ESP_ERROR_CHECK(e);
    ESP_ERROR_CHECK(uart_driver_install(CONSOLE, 512, 0, 0, NULL, 0));

    struct demo_store store = {.ctx = NULL, .load = nvs_load, .save = nvs_save};
    demo_start(&demo, &store);

    int err = board_init(&sx);
    radio = tern_sx126x_radio(&sx);
    cfg = (struct tern_radio_config){
        .mod = tern_lora_default(CONFIG_TERN_SF, CONFIG_TERN_BW_HZ),
        .freq_hz = CONFIG_TERN_FREQ_HZ,
        .tx_power_dbm = CONFIG_TERN_TX_POWER_DBM,
        .sync_word = CONFIG_TERN_SYNC_WORD,
    };
    cfg.mod.preamble = 16;
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
