#include "tern/sx126x.h"

#include <string.h>

#include "check.h"
#include "tern/err.h"

/* The SX1262 driver against a fake SPI bus that records every command and answers the reads the
 * way the datasheet says the chip does. The expected bytes are the datasheet's, worked out by hand
 * in the comments, not read back from the driver. */

struct bus {
    uint8_t log[64][260]; /* each transfer's bytes out */
    size_t lens[64];
    size_t count;
    /* What the chip has to say. */
    uint16_t irq;
    uint8_t rx_len, rx_start;
    uint8_t buffer[256];
    uint8_t rssi_raw, snr_raw;
    uint8_t regs[0x1000];
    tern_time now;
};

static int bus_transfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    struct bus *b = ctx;
    CHECK(b->count < 64);
    memcpy(b->log[b->count], tx, len);
    b->lens[b->count++] = len;

    if (tx[0] == 0x0D) { /* WriteRegister */
        b->regs[(tx[1] << 8 | tx[2]) & 0xFFF] = tx[3];
    }
    if (rx == NULL) {
        return TERN_OK;
    }
    memset(rx, 0, len);
    switch (tx[0]) {
    case 0x12: /* GetIrqStatus: status, then IRQ most significant first */
        rx[2] = (uint8_t)(b->irq >> 8);
        rx[3] = (uint8_t)b->irq;
        break;
    case 0x13: /* GetRxBufferStatus */
        rx[2] = b->rx_len;
        rx[3] = b->rx_start;
        break;
    case 0x14: /* GetPacketStatus */
        rx[2] = b->rssi_raw;
        rx[3] = b->snr_raw;
        break;
    case 0x1D: /* ReadRegister: three bytes out, a status byte, then the value */
        rx[4] = b->regs[(tx[1] << 8 | tx[2]) & 0xFFF];
        break;
    case 0x1E: /* ReadBuffer: offset, a status byte, then the data */
        memcpy(&rx[3], &b->buffer[tx[1]], len - 3);
        break;
    }
    return TERN_OK;
}

static tern_time bus_now(void *ctx) { return ((struct bus *)ctx)->now; }

/* Whether the n-th transfer was exactly these bytes. */
#define SENT(b, n, ...)                                                                            \
    ((b).lens[n] == sizeof((const uint8_t[]){__VA_ARGS__}) &&                                      \
     memcmp((b).log[n], (const uint8_t[]){__VA_ARGS__}, (b).lens[n]) == 0)

/* Whether any transfer was exactly these bytes. */
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

static const struct tern_sx126x_board heltec = {
    .tcxo_mv = 1800, .dio2_rf_switch = true, .dcdc = true};

static void start(struct tern_sx126x *d, struct bus *b) {
    memset(b, 0, sizeof *b);
    struct tern_sx126x_bus sb = {b, bus_transfer, bus_now};
    CHECK(tern_sx126x_init(d, &sb, &heltec) == TERN_OK);
}

static void init_sets_up_the_board(void) {
    static struct bus b;
    static struct tern_sx126x d;
    start(&d, &b);
    CHECK(SENT(b, 0, 0x80, 0x00));                   /* SetStandby(STDBY_RC) */
    CHECK(SENT(b, 1, 0x96, 0x01));                   /* SetRegulatorMode(DC-DC) */
    CHECK(SENT(b, 2, 0x97, 0x02, 0x00, 0x01, 0x40)); /* DIO3 gives the TCXO 1.8 V, 5 ms */
    CHECK(SENT(b, 3, 0x07, 0x00, 0x00));             /* ClearDeviceErrors */
    CHECK(SENT(b, 4, 0x89, 0x7F));                   /* Calibrate everything */
    CHECK(SENT(b, 5, 0x9D, 0x01));                   /* DIO2 drives the RF switch */
    CHECK(b.regs[0x8D8] == 0x1E);                    /* errata 15.2, from a register of 0 */
    CHECK(SENT_ANY(b, 0x8A, 0x01));                  /* SetPacketType(LoRa) */
    /* TxDone, RxDone, PreambleDetected, HeaderValid, HeaderErr, CrcErr and Timeout: 0x0277, on
     * DIO1 too. */
    CHECK(SENT_ANY(b, 0x08, 0x02, 0x77, 0x02, 0x77, 0x00, 0x00, 0x00, 0x00));

    struct tern_sx126x_board bad = heltec;
    bad.tcxo_mv = 1900;
    struct tern_sx126x_bus sb = {&b, bus_transfer, bus_now};
    CHECK(tern_sx126x_init(&d, &sb, &bad) == TERN_EINVAL);
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

static void configure_sends_the_datasheet_commands(void) {
    static struct bus b;
    static struct tern_sx126x d;
    start(&d, &b);
    struct tern_radio r = tern_sx126x_radio(&d);
    struct tern_radio_config cfg = us_config();
    b.regs[0x889] = 0x00;
    b.regs[0x736] = 0x0D;
    b.count = 0;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);

    /* 919 MHz * 2^25 / 32 MHz = 963641344 = 0x39700000. */
    CHECK(SENT_ANY(b, 0x86, 0x39, 0x70, 0x00, 0x00));
    CHECK(SENT_ANY(b, 0x98, 0xE1, 0xE9));             /* CalibrateImage, 902-928 MHz */
    CHECK(SENT_ANY(b, 0x95, 0x04, 0x07, 0x00, 0x01)); /* PA for +22 dBm, SX1262 */
    CHECK(SENT_ANY(b, 0x8E, 0x02, 0x04));             /* +2 dBm, 200 us ramp */
    CHECK(SENT_ANY(b, 0x8B, 0x07, 0x05, 0x01, 0x00)); /* SF7, 250 kHz, 4/5, LDRO off */
    /* Preamble 16, explicit header, up to 255 bytes, CRC on, standard IQ. */
    CHECK(SENT_ANY(b, 0x8C, 0x00, 0x10, 0x00, 0xFF, 0x01, 0x00));
    CHECK(b.regs[0x740] == 0x24 && b.regs[0x741] == 0x44); /* 0x24 is 0x2444 on an SX126x */
    CHECK(b.regs[0x889] == 0x04);                          /* errata 15.1: set below 500 kHz */
    CHECK(b.regs[0x736] == 0x0D); /* errata 15.4: bit 2 set, the rest untouched */

    cfg.mod = tern_lora_default(12, 500000);
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    CHECK(b.regs[0x889] == 0x00);                     /* and cleared at 500 kHz */
    CHECK(SENT_ANY(b, 0x8B, 0x0C, 0x06, 0x01, 0x00)); /* SF12/500 kHz: 8.2 ms symbols, LDRO off */
    cfg.mod = tern_lora_default(12, 125000);
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    CHECK(SENT_ANY(b, 0x8B, 0x0C, 0x04, 0x01, 0x01)); /* SF12/125 kHz: LDRO on */
}

static void configure_refuses_what_the_chip_cannot_do(void) {
    static struct bus b;
    static struct tern_sx126x d;
    start(&d, &b);
    struct tern_radio r = tern_sx126x_radio(&d);
    struct tern_radio_config cfg = us_config();
    cfg.tx_power_dbm = 23;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_EINVAL);
    cfg.tx_power_dbm = -10;
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
    cfg.mod.implicit_header = true; /* receiving it needs a length the seam cannot pass */
    CHECK(tern_radio_configure(&r, &cfg) == TERN_EINVAL);
    /* Nothing goes out until it has been configured. */
    CHECK(tern_radio_transmit(&r, (const uint8_t *)"x", 1) == TERN_EINVAL);
    CHECK(tern_radio_receive(&r) == TERN_EINVAL);

    /* The top of the range is allowed, and a refused configuration after it changes nothing. */
    cfg = us_config();
    cfg.freq_hz = 960000000;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    b.count = 0;
    cfg.freq_hz = 100000000;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_EINVAL);
    CHECK(b.count == 0);
    CHECK(tern_radio_transmit(&r, (const uint8_t *)"x", 1) == TERN_OK);
}

static void transmit_writes_the_frame_and_starts(void) {
    static struct bus b;
    static struct tern_sx126x d;
    start(&d, &b);
    struct tern_radio r = tern_sx126x_radio(&d);
    struct tern_radio_config cfg = us_config();
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    b.count = 0;
    CHECK(tern_radio_transmit(&r, (const uint8_t *)"\x48\x01\x02", 3) == TERN_OK);
    CHECK(SENT(b, 0, 0x80, 0x00));
    CHECK(SENT(b, 1, 0x8C, 0x00, 0x10, 0x00, 0x03, 0x01, 0x00)); /* this frame's length */
    CHECK(SENT(b, 2, 0x0E, 0x00, 0x48, 0x01, 0x02));             /* WriteBuffer at 0 */
    CHECK(SENT(b, 3, 0x02, 0xFF, 0xFF));                         /* ClearIrqStatus */
    CHECK(SENT(b, 4, 0x83, 0x00, 0x00, 0x00));                   /* SetTx, no timeout */

    b.count = 0;
    CHECK(tern_radio_receive(&r) == TERN_OK);
    CHECK(SENT(b, 1, 0x8C, 0x00, 0x10, 0x00, 0xFF, 0x01, 0x00));
    CHECK(SENT(b, 3, 0x82, 0xFF, 0xFF, 0xFF)); /* SetRx, continuous */
}

static void poll_reports_what_happened(void) {
    static struct bus b;
    static struct tern_sx126x d;
    start(&d, &b);
    struct tern_radio r = tern_sx126x_radio(&d);
    struct tern_radio_event ev;

    b.irq = 0;
    CHECK(tern_radio_poll(&r, &ev) == 0);

    b.irq = 0x0001;
    b.now = 1234;
    CHECK(tern_radio_poll(&r, &ev) == 1);
    CHECK(ev.kind == TERN_RADIO_TX_DONE);
    CHECK_EQ_I64(ev.at, 1234);
    CHECK(SENT(b, b.count - 1, 0x02, 0x00, 0x01));

    /* A 5-byte frame at offset 0x80, at -73.5 dBm (raw 147) and -7.25 dB SNR (raw -29). */
    b.irq = 0x0002;
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
    CHECK(SENT_ANY(b, 0x1E, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00)); /* ReadBuffer 5 at 0x80 */

    b.irq = 0x0042; /* RxDone with a CRC error */
    CHECK(tern_radio_poll(&r, &ev) == 1);
    CHECK(ev.kind == TERN_RADIO_RX_ERROR);
    b.irq = 0x0020; /* a bad header, with no RxDone */
    CHECK(tern_radio_poll(&r, &ev) == 1);
    CHECK(ev.kind == TERN_RADIO_RX_ERROR);
}

static void poll_counts_what_the_receiver_saw(void) {
    static struct bus b;
    static struct tern_sx126x d;
    start(&d, &b);
    struct tern_radio r = tern_sx126x_radio(&d);
    struct tern_radio_event ev;

    /* A preamble and nothing after it, as a frame with another sync word is: no event, and the
     * chip is told it has been counted. */
    b.irq = 0x0004;
    CHECK(tern_radio_poll(&r, &ev) == 0);
    CHECK(d.counts.preambles == 1 && d.counts.headers == 0 && d.counts.frames == 0);
    CHECK(SENT(b, b.count - 1, 0x02, 0x00, 0x04));

    /* A whole frame, found all at once: preamble, header and RxDone. */
    b.irq = 0x0016;
    b.rx_len = 1;
    CHECK(tern_radio_poll(&r, &ev) == 1);
    CHECK(ev.kind == TERN_RADIO_RX_DONE);
    CHECK(d.counts.preambles == 2 && d.counts.headers == 1 && d.counts.frames == 1);

    b.irq = 0x0042; /* a CRC error */
    CHECK(tern_radio_poll(&r, &ev) == 1);
    b.irq = 0x0020; /* a bad header */
    CHECK(tern_radio_poll(&r, &ev) == 1);
    CHECK(d.counts.crc_errors == 1 && d.counts.header_errors == 1 && d.counts.frames == 1);
    CHECK(d.counts.preambles == 2 && d.counts.headers == 1);
}

/* Listening first (tern/listen.h), as the chip's flags drive it. At SF7 and 250 kHz a symbol is
 * 0.512 ms, so a preamble of 16 with no header after it holds the radio for 29 symbols,
 * 14.848 ms, and a header for as long as 255 bytes are on the air. */
static void receiving_follows_the_chips_flags(void) {
    static struct bus b;
    static struct tern_sx126x d;
    start(&d, &b);
    struct tern_radio r = tern_sx126x_radio(&d);
    struct tern_radio_config cfg = us_config();
    struct tern_radio_event ev;
    const tern_time wait = 14848000, longest = tern_lora_airtime(&cfg.mod, 255);

    CHECK(tern_radio_receiving(&r) == 0); /* not yet configured: nothing is asked of the chip */
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    CHECK(tern_radio_receive(&r) == TERN_OK);
    b.count = 0;
    CHECK(tern_radio_receiving(&r) == 0);
    CHECK(b.count == 1 && SENT(b, 0, 0x12, 0x00, 0x00, 0x00)); /* GetIrqStatus, and no more */

    /* A preamble: counted and cleared here as in poll, once. */
    b.now = 5000000;
    b.irq = 0x0004;
    CHECK(tern_radio_receiving(&r) == 1);
    CHECK(SENT(b, b.count - 1, 0x02, 0x00, 0x04));
    CHECK(d.counts.preambles == 1);
    b.irq = 0;
    b.now = 5000000 + wait - 1;
    CHECK(tern_radio_receiving(&r) == 1);
    b.now = 5000000 + wait;
    CHECK(tern_radio_receiving(&r) == 0); /* no header came */
    CHECK(d.counts.preambles == 1);

    /* A preamble that poll finds, then a header: held past the wait for a header. */
    b.now = 100000000;
    b.irq = 0x0004;
    CHECK(tern_radio_poll(&r, &ev) == 0);
    b.irq = 0x0010;
    b.now += 10000000;
    CHECK(tern_radio_receiving(&r) == 1);
    b.irq = 0;
    b.now = 100000000 + longest - 1;
    CHECK(tern_radio_receiving(&r) == 1);

    /* The frame has ended and poll has not collected it: no longer receiving, and the frame is
     * still there to collect. */
    b.irq = 0x0002;
    b.rx_len = 1;
    b.count = 0;
    CHECK(tern_radio_receiving(&r) == 0);
    CHECK(b.count == 1);
    CHECK(tern_radio_poll(&r, &ev) == 1 && ev.kind == TERN_RADIO_RX_DONE);
    b.irq = 0;
    CHECK(tern_radio_receiving(&r) == 0);

    /* A header that fails its check ends the frame too. */
    b.irq = 0x0004;
    CHECK(tern_radio_receiving(&r) == 1);
    b.irq = 0x0020;
    CHECK(tern_radio_receiving(&r) == 0);
    CHECK(tern_radio_poll(&r, &ev) == 1 && ev.kind == TERN_RADIO_RX_ERROR);
    b.irq = 0;
    CHECK(tern_radio_receiving(&r) == 0);

    /* Sending, listening afresh, standby and a new configuration each drop what it was on. */
    int (*const drops[])(struct tern_radio *) = {tern_radio_receive, tern_radio_standby};
    for (size_t i = 0; i < 2; i++) {
        b.irq = 0x0014;
        CHECK(tern_radio_receiving(&r) == 1);
        b.irq = 0;
        b.count = 0;
        CHECK(drops[i](&r) == TERN_OK);
        CHECK(tern_radio_receiving(&r) == 0);
    }
    b.irq = 0x0014;
    CHECK(tern_radio_receiving(&r) == 1);
    b.irq = 0;
    b.count = 0;
    CHECK(tern_radio_transmit(&r, (const uint8_t *)"x", 1) == TERN_OK);
    CHECK(tern_radio_receiving(&r) == 0);
    b.irq = 0x0014;
    CHECK(tern_radio_receiving(&r) == 1);
    b.irq = 0;
    b.count = 0;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    b.count = 0;
    CHECK(tern_radio_receiving(&r) == 0);
}

int main(void) {
    RUN(init_sets_up_the_board);
    RUN(configure_sends_the_datasheet_commands);
    RUN(configure_refuses_what_the_chip_cannot_do);
    RUN(transmit_writes_the_frame_and_starts);
    RUN(poll_reports_what_happened);
    RUN(poll_counts_what_the_receiver_saw);
    RUN(receiving_follows_the_chips_flags);
    return CHECK_DONE();
}
