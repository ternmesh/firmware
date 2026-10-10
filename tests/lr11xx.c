#include "tern/lr11xx.h"

#include <string.h>

#include "check.h"
#include "tern/err.h"

/* The LR11xx driver against a fake SPI bus that records every transaction and answers the way the
 * user manual says the chip does: a command that answers is followed by a second transaction of
 * zeros, the first byte back the status and the answer after it, and a transaction of zeros with
 * no command before it is GetStatus. The expected bytes are the manual's, worked out by hand in the
 * comments, not read back from the driver. */

struct bus {
    uint8_t log[64][260]; /* each transaction's bytes out */
    size_t lens[64];
    size_t count;
    uint16_t pending; /* the command whose answer the next read is, or 0 */
    uint8_t pending_args[2];
    /* What the chip has to say. */
    uint8_t use_case;
    uint16_t fw;
    uint32_t irq;
    uint8_t rx_len, rx_start;
    uint8_t buffer[256];
    uint8_t rssi_raw, snr_raw;
    tern_time now;
};

static bool zeros(const uint8_t *tx, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (tx[i] != 0) {
            return false;
        }
    }
    return true;
}

static int bus_transfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    struct bus *b = ctx;
    CHECK(b->count < 64);
    memcpy(b->log[b->count], tx, len);
    b->lens[b->count++] = len;

    if (!zeros(tx, len)) {
        /* A command: remembered, if it answers, for the read that follows. */
        uint16_t op = (uint16_t)(tx[0] << 8 | tx[1]);
        b->pending = (op == 0x0101 || op == 0x0203 || op == 0x0204 || op == 0x010A) ? op : 0;
        if (len >= 4) {
            b->pending_args[0] = tx[2];
            b->pending_args[1] = tx[3];
        }
        CHECK(rx == NULL);
        return TERN_OK;
    }
    CHECK(rx != NULL);
    memset(rx, 0, len);
    rx[0] = 0x05; /* Stat1: command OK, no IRQ line */
    switch (b->pending) {
    case 0x0101: /* GetVersion: hardware, use case, firmware */
        rx[1] = 0x22;
        rx[2] = b->use_case;
        rx[3] = (uint8_t)(b->fw >> 8);
        rx[4] = (uint8_t)b->fw;
        break;
    case 0x0203: /* GetRxBufferStatus */
        rx[1] = b->rx_len;
        rx[2] = b->rx_start;
        break;
    case 0x0204: /* GetPacketStatus */
        rx[1] = b->rssi_raw;
        rx[2] = b->snr_raw;
        break;
    case 0x010A: /* ReadBuffer8: offset, length */
        CHECK(len == 1u + b->pending_args[1]);
        memcpy(&rx[1], &b->buffer[b->pending_args[0]], len - 1);
        break;
    default: /* GetStatus: Stat1, Stat2, then the IRQ flags most significant first */
        CHECK(len == 6);
        rx[1] = 0x02; /* Stat2: STBY_RC */
        rx[2] = (uint8_t)(b->irq >> 24);
        rx[3] = (uint8_t)(b->irq >> 16);
        rx[4] = (uint8_t)(b->irq >> 8);
        rx[5] = (uint8_t)b->irq;
        break;
    }
    b->pending = 0;
    return TERN_OK;
}

static tern_time bus_now(void *ctx) { return ((struct bus *)ctx)->now; }

#define SENT(b, n, ...)                                                                            \
    ((b).lens[n] == sizeof((const uint8_t[]){__VA_ARGS__}) &&                                      \
     memcmp((b).log[n], (const uint8_t[]){__VA_ARGS__}, (b).lens[n]) == 0)

static bool sent_any(const struct bus *b, const uint8_t *want, size_t len) {
    for (size_t i = 0; i < b->count; i++) {
        if (b->lens[i] == len && memcmp(b->log[i], want, len) == 0) {
            return true;
        }
    }
    return false;
}
#define SENT_ANY(b, ...)                                                                           \
    sent_any(&(b), (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))

/* A board with a 1.8 V TCXO, the DC-DC fitted, and a switch on DIO5 and DIO6: DIO5 to receive,
 * DIO5 and DIO6 to send from the low-power amplifier, DIO6 from the high-power one. */
static const struct tern_lr11xx_board board = {
    .tcxo_mv = 1800,
    .dcdc = true,
    .rf_switch = {.enable = TERN_LR11XX_RFSW_DIO5 | TERN_LR11XX_RFSW_DIO6,
                  .rx = TERN_LR11XX_RFSW_DIO5,
                  .tx = TERN_LR11XX_RFSW_DIO5 | TERN_LR11XX_RFSW_DIO6,
                  .tx_hp = TERN_LR11XX_RFSW_DIO6},
    .hp_pa = true,
};

static void start_with(struct tern_lr11xx *d, struct bus *b, const struct tern_lr11xx_board *bd) {
    memset(b, 0, sizeof *b);
    b->use_case = 0x01; /* LR1110 */
    b->fw = 0x0401;
    struct tern_lr11xx_bus lb = {b, bus_transfer, bus_now};
    CHECK(tern_lr11xx_init(d, &lb, bd) == TERN_OK);
}

static void start(struct tern_lr11xx *d, struct bus *b) { start_with(d, b, &board); }

static void init_sets_up_the_board(void) {
    static struct bus b;
    static struct tern_lr11xx d;
    start(&d, &b);
    CHECK(SENT(b, 0, 0x01, 0x01));                   /* GetVersion */
    CHECK(SENT(b, 1, 0x00, 0x00, 0x00, 0x00, 0x00)); /* its answer: status and four bytes */
    CHECK(SENT(b, 2, 0x01, 0x1C, 0x00));             /* SetStandby(RC) */
    CHECK(SENT(b, 3, 0x01, 0x10, 0x01));             /* SetRegMode(DC-DC) */
    /* SetDioAsRfSwitch: enable DIO5|DIO6, standby none, rx DIO5, tx both, tx HP DIO6, and 0 for
     * the 2.4 GHz amplifier, GNSS and Wi-Fi. */
    CHECK(SENT(b, 4, 0x01, 0x12, 0x03, 0x00, 0x01, 0x03, 0x02, 0x00, 0x00, 0x00));
    CHECK(SENT(b, 5, 0x01, 0x17, 0x02, 0x00, 0x00, 0xA4)); /* 1.8 V, 164 x 30.52 us = 5 ms */
    CHECK(SENT(b, 6, 0x01, 0x0E));                         /* ClearErrors */
    CHECK(SENT(b, 7, 0x01, 0x0F, 0x3F));                   /* Calibrate everything */
    CHECK(SENT(b, 8, 0x02, 0x0E, 0x02));                   /* SetPacketType(LoRa) */
    /* TxDone, RxDone, preamble, header valid, header error, CRC error and timeout: bits 2 to 7
     * and 10, 0x000004FC, on DIO9; none on DIO11. */
    CHECK(SENT(b, 9, 0x01, 0x13, 0x00, 0x00, 0x04, 0xFC, 0x00, 0x00, 0x00, 0x00));
    CHECK(SENT(b, 10, 0x01, 0x14, 0xFF, 0xFF, 0xFF, 0xFF)); /* ClearIrq, all */

    /* A voltage the chip cannot give is refused before anything is sent. */
    struct tern_lr11xx_board bad = board;
    bad.tcxo_mv = 1900;
    struct tern_lr11xx_bus lb = {&b, bus_transfer, bus_now};
    b.count = 0;
    CHECK(tern_lr11xx_init(&d, &lb, &bad) == TERN_EINVAL);
    CHECK(b.count == 0);

    /* A crystal: no TCXO mode, no recalibration. */
    struct tern_lr11xx_board xtal = board;
    xtal.tcxo_mv = 0;
    xtal.dcdc = false;
    start_with(&d, &b, &xtal);
    CHECK(!SENT_ANY(b, 0x01, 0x17, 0x02, 0x00, 0x00, 0xA4));
    CHECK(!SENT_ANY(b, 0x01, 0x10, 0x01));
    CHECK(!SENT_ANY(b, 0x01, 0x0F, 0x3F));
}

static void init_refuses_what_is_not_the_chip(void) {
    static struct bus b;
    static struct tern_lr11xx d;
    struct tern_lr11xx_bus lb = {&b, bus_transfer, bus_now};
    memset(&b, 0, sizeof b);
    b.use_case = 0xDF; /* the bootloader */
    CHECK(tern_lr11xx_init(&d, &lb, &board) == TERN_EIO);
    CHECK(b.count == 2); /* GetVersion and its answer, and nothing more */
    b.use_case = 0x00;   /* nothing there: all zeros */
    CHECK(tern_lr11xx_init(&d, &lb, &board) == TERN_EIO);
    b.use_case = 0x03; /* an LR1121 */
    CHECK(tern_lr11xx_init(&d, &lb, &board) == TERN_OK);
}

static struct tern_radio_config us_config(void) {
    struct tern_radio_config cfg = {
        .mod = tern_lora_default(7, 250000),
        .freq_hz = 919000000,
        .tx_power_dbm = 2,
        .sync_word = 0x24,
    };
    cfg.mod.preamble = 16;
    return cfg;
}

static void configure_sends_the_manuals_commands(void) {
    static struct bus b;
    static struct tern_lr11xx d;
    start(&d, &b);
    struct tern_radio r = tern_lr11xx_radio(&d);
    struct tern_radio_config cfg = us_config();
    b.count = 0;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);

    CHECK(SENT(b, 0, 0x01, 0x1C, 0x00));                   /* SetStandby(RC) */
    CHECK(SENT(b, 1, 0x02, 0x0E, 0x02));                   /* SetPacketType(LoRa), first */
    CHECK(SENT(b, 2, 0x02, 0x0B, 0x36, 0xC6, 0xD3, 0xC0)); /* 919000000 Hz = 0x36C6D3C0 */
    CHECK(SENT(b, 3, 0x01, 0x11, 0xE1, 0xE9));             /* CalibImage, 902-928 MHz */
    CHECK(SENT(b, 4, 0x02, 0x0F, 0x07, 0x05, 0x01, 0x00)); /* SF7, 250 kHz, 4/5, LDRO off */
    /* Preamble 16, explicit header, up to 255 bytes, CRC on, standard IQ. */
    CHECK(SENT(b, 5, 0x02, 0x10, 0x00, 0x10, 0x00, 0xFF, 0x01, 0x00));
    CHECK(SENT(b, 6, 0x02, 0x15, 0x00, 0x00, 0x04, 0x00)); /* low-power PA, regulator, duty 4 */
    CHECK(SENT(b, 7, 0x02, 0x11, 0x02, 0x02));             /* +2 dBm, 48 us ramp */
    CHECK(SENT(b, 8, 0x02, 0x2B, 0x24));                   /* SetLoRaSyncWord, one byte */

    /* Above +14 dBm, the high-power amplifier from the battery, for its +22 dBm. */
    cfg.tx_power_dbm = 22;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    CHECK(SENT_ANY(b, 0x02, 0x15, 0x01, 0x01, 0x04, 0x07));
    CHECK(SENT_ANY(b, 0x02, 0x11, 0x16, 0x02));
    cfg.tx_power_dbm = 14;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    CHECK(SENT(b, b.count - 3, 0x02, 0x15, 0x00, 0x00, 0x04, 0x00));
    cfg.tx_power_dbm = -17;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    CHECK(SENT_ANY(b, 0x02, 0x11, 0xEF, 0x02));

    cfg = us_config();
    cfg.freq_hz = 868100000;
    cfg.mod = tern_lora_default(12, 125000);
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    CHECK(SENT_ANY(b, 0x01, 0x11, 0xD7, 0xDB));             /* 863-870 MHz */
    CHECK(SENT_ANY(b, 0x02, 0x0F, 0x0C, 0x04, 0x01, 0x01)); /* SF12/125 kHz: LDRO on */
}

static void configure_refuses_what_the_chip_cannot_do(void) {
    static struct bus b;
    static struct tern_lr11xx d;
    start(&d, &b);
    struct tern_radio r = tern_lr11xx_radio(&d);
    struct tern_radio_config cfg = us_config();
    cfg.tx_power_dbm = 23;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_EINVAL);
    cfg.tx_power_dbm = -18;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_EINVAL);
    cfg = us_config();
    cfg.mod.bw_hz = 200000;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_EINVAL);
    cfg = us_config();
    cfg.freq_hz = 149999999;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_EINVAL);
    cfg.freq_hz = 960000001;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_EINVAL);
    cfg = us_config();
    cfg.mod.implicit_header = true;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_EINVAL);
    CHECK(tern_radio_transmit(&r, (const uint8_t *)"x", 1) == TERN_EINVAL);
    CHECK(tern_radio_receive(&r) == TERN_EINVAL);

    /* A board whose antenna only the low-power amplifier reaches stops at +14 dBm. */
    struct tern_lr11xx_board lp = board;
    lp.hp_pa = false;
    start_with(&d, &b, &lp);
    cfg = us_config();
    cfg.tx_power_dbm = 15;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_EINVAL);
    cfg.tx_power_dbm = 14;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
}

static void old_firmware_has_only_two_sync_words(void) {
    static struct bus b;
    static struct tern_lr11xx d;
    struct tern_lr11xx_bus lb = {&b, bus_transfer, bus_now};
    memset(&b, 0, sizeof b);
    b.use_case = 0x01;
    b.fw = 0x0302;
    CHECK(tern_lr11xx_init(&d, &lb, &board) == TERN_OK);
    struct tern_radio r = tern_lr11xx_radio(&d);
    struct tern_radio_config cfg = us_config();
    b.count = 0;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_EINVAL); /* 0x24 cannot be set */
    CHECK(b.count == 0);
    cfg.sync_word = 0x12;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    CHECK(SENT(b, b.count - 1, 0x02, 0x08, 0x00)); /* SetLoRaPublicNetwork(private) */
    cfg.sync_word = 0x34;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    CHECK(SENT(b, b.count - 1, 0x02, 0x08, 0x01));
}

static void transmit_writes_the_frame_and_starts(void) {
    static struct bus b;
    static struct tern_lr11xx d;
    start(&d, &b);
    struct tern_radio r = tern_lr11xx_radio(&d);
    struct tern_radio_config cfg = us_config();
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    b.count = 0;
    CHECK(tern_radio_transmit(&r, (const uint8_t *)"\x48\x01\x02", 3) == TERN_OK);
    CHECK(SENT(b, 0, 0x01, 0x1C, 0x00));
    CHECK(SENT(b, 1, 0x02, 0x10, 0x00, 0x10, 0x00, 0x03, 0x01, 0x00)); /* this frame's length */
    CHECK(SENT(b, 2, 0x01, 0x09, 0x48, 0x01, 0x02));                   /* WriteBuffer8 */
    CHECK(SENT(b, 3, 0x01, 0x14, 0xFF, 0xFF, 0xFF, 0xFF));             /* ClearIrq */
    CHECK(SENT(b, 4, 0x02, 0x0A, 0x00, 0x00, 0x00));                   /* SetTx, no timeout */

    b.count = 0;
    CHECK(tern_radio_receive(&r) == TERN_OK);
    CHECK(SENT(b, 1, 0x02, 0x10, 0x00, 0x10, 0x00, 0xFF, 0x01, 0x00));
    CHECK(SENT(b, 3, 0x02, 0x09, 0xFF, 0xFF, 0xFF)); /* SetRx, continuous */
}

static void poll_reports_what_happened(void) {
    static struct bus b;
    static struct tern_lr11xx d;
    start(&d, &b);
    struct tern_radio r = tern_lr11xx_radio(&d);
    struct tern_radio_event ev;

    b.irq = 0;
    b.count = 0;
    CHECK(tern_radio_poll(&r, &ev) == 0);
    CHECK(b.count == 1 && SENT(b, 0, 0, 0, 0, 0, 0, 0)); /* GetStatus, by six bytes of zeros */

    b.irq = 1u << 2;
    b.now = 1234;
    CHECK(tern_radio_poll(&r, &ev) == 1);
    CHECK(ev.kind == TERN_RADIO_TX_DONE);
    CHECK_EQ_I64(ev.at, 1234);
    CHECK(SENT(b, b.count - 1, 0x01, 0x14, 0x00, 0x00, 0x00, 0x04));

    /* A 5-byte frame at offset 0x80, at -73.5 dBm (raw 147) and -7.25 dB SNR (raw -29). */
    b.irq = 1u << 3;
    b.rx_len = 5;
    b.rx_start = 0x80;
    memcpy(&b.buffer[0x80], "hello", 5);
    b.rssi_raw = 147;
    b.snr_raw = (uint8_t)(int8_t)-29;
    CHECK(tern_radio_poll(&r, &ev) == 1);
    CHECK(ev.kind == TERN_RADIO_RX_DONE);
    CHECK(ev.len == 5 && memcmp(ev.data, "hello", 5) == 0);
    CHECK_EQ_I64(ev.rssi_dbm, -73);
    CHECK_EQ_I64(ev.snr_cdb, -725);
    CHECK(SENT_ANY(b, 0x01, 0x0A, 0x80, 0x05)); /* ReadBuffer8, 5 at 0x80 */
    CHECK(SENT(b, b.count - 1, 0x01, 0x14, 0x00, 0x00, 0x00, 0x08));

    b.irq = (1u << 3) | (1u << 7); /* RxDone with a CRC error */
    CHECK(tern_radio_poll(&r, &ev) == 1);
    CHECK(ev.kind == TERN_RADIO_RX_ERROR);
    b.irq = 1u << 6; /* a bad header, with no RxDone */
    CHECK(tern_radio_poll(&r, &ev) == 1);
    CHECK(ev.kind == TERN_RADIO_RX_ERROR);
    CHECK(d.counts.crc_errors == 1 && d.counts.header_errors == 1 && d.counts.frames == 1);

    b.irq = 1u << 10; /* a timeout, cleared and nothing reported */
    CHECK(tern_radio_poll(&r, &ev) == 0);
    CHECK(SENT(b, b.count - 1, 0x01, 0x14, 0x00, 0x00, 0x04, 0x00));
}

/* Listening first (tern/listen.h), as for the SX126x: at SF7 and 250 kHz a preamble of 16 with no
 * header after it holds the radio for 14.848 ms. */
static void receiving_follows_the_chips_flags(void) {
    static struct bus b;
    static struct tern_lr11xx d;
    start(&d, &b);
    struct tern_radio r = tern_lr11xx_radio(&d);
    struct tern_radio_config cfg = us_config();
    struct tern_radio_event ev;
    const tern_time wait = 14848000;

    CHECK(tern_radio_receiving(&r) == 0);
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    CHECK(tern_radio_receive(&r) == TERN_OK);
    b.count = 0;
    CHECK(tern_radio_receiving(&r) == 0);
    CHECK(b.count == 1);

    b.now = 5000000;
    b.irq = 1u << 4; /* a preamble: counted and cleared once */
    CHECK(tern_radio_receiving(&r) == 1);
    CHECK(SENT(b, b.count - 1, 0x01, 0x14, 0x00, 0x00, 0x00, 0x10));
    CHECK(d.counts.preambles == 1);
    b.irq = 0;
    b.now = 5000000 + wait - 1;
    CHECK(tern_radio_receiving(&r) == 1);
    b.now = 5000000 + wait;
    CHECK(tern_radio_receiving(&r) == 0);

    b.irq = (1u << 4) | (1u << 5); /* preamble and header */
    CHECK(tern_radio_receiving(&r) == 1);
    CHECK(d.counts.headers == 1);
    b.irq = 1u << 3; /* the frame has ended, not yet collected */
    b.rx_len = 1;
    CHECK(tern_radio_receiving(&r) == 0);
    CHECK(tern_radio_poll(&r, &ev) == 1 && ev.kind == TERN_RADIO_RX_DONE);

    b.irq = (1u << 4) | (1u << 5);
    CHECK(tern_radio_receiving(&r) == 1);
    b.irq = 0;
    CHECK(tern_radio_standby(&r) == TERN_OK);
    CHECK(tern_radio_receiving(&r) == 0);
}

static void sleep_is_from_standby_and_cold(void) {
    static struct bus b;
    static struct tern_lr11xx d;
    start(&d, &b);
    struct tern_radio r = tern_lr11xx_radio(&d);
    struct tern_radio_config cfg = us_config();
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    CHECK(tern_radio_receive(&r) == TERN_OK);
    b.count = 0;
    CHECK(tern_lr11xx_sleep(&d) == TERN_OK);
    CHECK_EQ_I64(b.count, 2);
    CHECK(SENT(b, 0, 0x01, 0x1C, 0x00));
    CHECK(SENT(b, 1, 0x01, 0x1B, 0x00, 0x00, 0x00, 0x00, 0x00)); /* SetSleep: cold, no timer */
    CHECK(!d.configured);
}

int main(void) {
    RUN(init_sets_up_the_board);
    RUN(init_refuses_what_is_not_the_chip);
    RUN(configure_sends_the_manuals_commands);
    RUN(configure_refuses_what_the_chip_cannot_do);
    RUN(old_firmware_has_only_two_sync_words);
    RUN(transmit_writes_the_frame_and_starts);
    RUN(poll_reports_what_happened);
    RUN(receiving_follows_the_chips_flags);
    RUN(sleep_is_from_standby_and_cold);
    return CHECK_DONE();
}
