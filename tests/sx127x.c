#include "tern/sx127x.h"

#include <string.h>

#include "check.h"
#include "tern/err.h"

/* The SX1276/SX1278 driver against a fake SPI bus that keeps the chip's registers and FIFO the way
 * the datasheet says the chip does: an address byte with the write bit, then data; the FIFO read
 * and written from RegFifoAddrPtr, which each byte moves on; and RegIrqFlags cleared by writing
 * ones. The expected values are the datasheet's, worked out by hand in the comments, not read back
 * from the driver. */

struct bus {
    uint8_t regs[0x80];
    uint8_t fifo[256];
    uint8_t writes[128][2]; /* each single-register write, address and value, in order */
    size_t count;
    size_t transfers;
    tern_time now;
};

static int bus_transfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    struct bus *b = ctx;
    uint8_t addr = tx[0] & 0x7F;
    bool write = (tx[0] & 0x80) != 0;
    b->transfers++;
    if (rx != NULL) {
        memset(rx, 0, len);
    }
    for (size_t i = 1; i < len; i++) {
        if (addr == 0x00) { /* the FIFO */
            uint8_t *ptr = &b->regs[0x0D];
            if (write) {
                b->fifo[(*ptr)++] = tx[i];
            } else if (rx != NULL) {
                rx[i] = b->fifo[(*ptr)++];
            }
            continue;
        }
        if (write) {
            CHECK(len == 2 && b->count < 128);
            b->writes[b->count][0] = addr;
            b->writes[b->count++][1] = tx[i];
            if (addr == 0x12) {
                b->regs[addr] &= (uint8_t)~tx[i];
            } else {
                b->regs[addr] = tx[i];
            }
        } else if (rx != NULL) {
            rx[i] = b->regs[addr];
        }
    }
    return TERN_OK;
}

static tern_time bus_now(void *ctx) { return ((struct bus *)ctx)->now; }

/* Whether the n-th register write was this. */
static bool wrote(const struct bus *b, size_t n, uint8_t addr, uint8_t value) {
    return n < b->count && b->writes[n][0] == addr && b->writes[n][1] == value;
}

/* The register's last write, or -1 if none since the log was cleared. */
static int last(const struct bus *b, uint8_t addr) {
    for (size_t i = b->count; i > 0; i--) {
        if (b->writes[i - 1][0] == addr) {
            return b->writes[i - 1][1];
        }
    }
    return -1;
}

static const struct tern_sx127x_board module = {.chip = TERN_SX1276, .pa_boost = true};

static void start_on(struct tern_sx127x *d, struct bus *b, const struct tern_sx127x_board *board) {
    memset(b, 0, sizeof *b);
    b->regs[0x42] = 0x12; /* RegVersion */
    b->regs[0x31] = 0xC3; /* RegDetectOptimize at reset */
    b->regs[0x4B] = 0x09; /* RegTcxo at reset */
    struct tern_sx127x_bus sb = {b, bus_transfer, bus_now};
    CHECK(tern_sx127x_init(d, &sb, board) == TERN_OK);
}

static void start(struct tern_sx127x *d, struct bus *b) { start_on(d, b, &module); }

static struct tern_radio_config us_config(void) {
    struct tern_radio_config cfg = {
        .mod = tern_lora_default(7, 250000),
        .freq_hz = 915000000,
        .tx_power_dbm = 17,
        .sync_word = 0x24,
    };
    cfg.mod.preamble = 16;
    return cfg;
}

static void init_checks_the_chip_and_sleeps_in_lora(void) {
    static struct bus b;
    static struct tern_sx127x d;
    start(&d, &b);
    CHECK(wrote(&b, 0, 0x01, 0x00)); /* to the FSK modem's sleep, */
    CHECK(wrote(&b, 1, 0x01, 0x80)); /* then LongRangeMode, still asleep */
    CHECK(last(&b, 0x11) == 0x00);   /* no flag masked */
    CHECK(last(&b, 0x4B) == -1);     /* a crystal: RegTcxo left alone */
    CHECK(!d.configured);

    struct tern_sx127x_board tcxo = module;
    tcxo.tcxo = true;
    start_on(&d, &b, &tcxo);
    CHECK(b.regs[0x4B] == 0x19); /* TcxoInputOn, the rest as they were */

    /* Anything but the family's version is not one of these chips, and is left alone. */
    memset(&b, 0, sizeof b);
    b.regs[0x42] = 0x22;
    struct tern_sx127x_bus sb = {&b, bus_transfer, bus_now};
    CHECK(tern_sx127x_init(&d, &sb, &module) == TERN_EIO);
    CHECK(b.count == 0);
}

static void configure_sets_the_datasheet_registers(void) {
    static struct bus b;
    static struct tern_sx127x d;
    start(&d, &b);
    struct tern_radio r = tern_sx127x_radio(&d);
    struct tern_radio_config cfg = us_config();
    b.count = 0;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);

    CHECK(wrote(&b, 0, 0x01, 0x80)); /* asleep while it is set up */
    /* 915 MHz * 2^19 / 32 MHz = 14991360 = 0xE4C000. */
    CHECK(b.regs[0x06] == 0xE4 && b.regs[0x07] == 0xC0 && b.regs[0x08] == 0x00);
    /* +17 dBm on PA_BOOST: 2 + OutputPower 15, MaxPower 7; PaDac and the current limit as they
     * are at reset. */
    CHECK(b.regs[0x09] == 0xFF && b.regs[0x4D] == 0x84 && b.regs[0x0B] == 0x2B);
    CHECK(b.regs[0x0C] == 0x23);                       /* LNA: G1, boosted on the HF port */
    CHECK(b.regs[0x1D] == 0x82);                       /* 250 kHz, 4/5, explicit header */
    CHECK(b.regs[0x1E] == 0x74);                       /* SF7, CRC on */
    CHECK(b.regs[0x26] == 0x04);                       /* AGC on, LDRO off */
    CHECK(b.regs[0x20] == 0x00 && b.regs[0x21] == 16); /* preamble */
    CHECK(b.regs[0x23] == 0xFF);
    CHECK(b.regs[0x39] == 0x24); /* the one-byte sync word, as it is */
    CHECK(b.regs[0x37] == 0x0A && b.regs[0x31] == 0xC3);
    CHECK(b.regs[0x0E] == 0x00 && b.regs[0x0F] == 0x00);
    CHECK(wrote(&b, b.count - 1, 0x01, 0x81)); /* and left in standby */
    CHECK(d.configured);

    /* +20 dBm: PaDac on, 5 + OutputPower 15, and the current limit raised to 140 mA. +2 dBm, the
     * least. */
    cfg.tx_power_dbm = 20;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    CHECK(b.regs[0x09] == 0xFF && b.regs[0x4D] == 0x87 && b.regs[0x0B] == 0x31);
    cfg.tx_power_dbm = 2;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    CHECK(b.regs[0x09] == 0xF0 && b.regs[0x4D] == 0x84 && b.regs[0x0B] == 0x2B);

    /* SF12 at 125 kHz: 32.768 ms symbols, so LDRO on. */
    cfg.mod = tern_lora_default(12, 125000);
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    CHECK(b.regs[0x1D] == 0x72 && b.regs[0x1E] == 0xC4 && b.regs[0x26] == 0x0C);
}

static void an_sx1278_on_rfo_at_433_mhz(void) {
    static struct bus b;
    static struct tern_sx127x d;
    struct tern_sx127x_board board = {.chip = TERN_SX1278, .pa_boost = false};
    start_on(&d, &b, &board);
    struct tern_radio r = tern_sx127x_radio(&d);
    struct tern_radio_config cfg = us_config();
    cfg.freq_hz = 433000000;
    cfg.tx_power_dbm = 10;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    /* 433 MHz * 2^19 / 32 MHz = 7094272 = 0x6C4000. */
    CHECK(b.regs[0x06] == 0x6C && b.regs[0x07] == 0x40 && b.regs[0x08] == 0x00);
    CHECK(b.regs[0x09] == 0x7A); /* RFO, MaxPower 7: Pout = OutputPower */
    CHECK(b.regs[0x0C] == 0x20); /* no boost on the LF port */

    cfg.freq_hz = 868000000; /* past the SX1278's band */
    CHECK(tern_radio_configure(&r, &cfg) == TERN_EINVAL);
    cfg.freq_hz = 433000000;
    cfg.tx_power_dbm = 16; /* past RFO's */
    CHECK(tern_radio_configure(&r, &cfg) == TERN_EINVAL);
    cfg.tx_power_dbm = 0;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
}

static void configure_refuses_what_the_chip_cannot_do(void) {
    static struct bus b;
    static struct tern_sx127x d;
    start(&d, &b);
    struct tern_radio r = tern_sx127x_radio(&d);
    struct tern_radio_config cfg = us_config();
    cfg.tx_power_dbm = 21;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_EINVAL);
    cfg.tx_power_dbm = 1; /* below PA_BOOST's least */
    CHECK(tern_radio_configure(&r, &cfg) == TERN_EINVAL);
    cfg = us_config();
    cfg.mod.bw_hz = 200000;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_EINVAL);
    cfg = us_config();
    cfg.freq_hz = 136999999;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_EINVAL);
    cfg.freq_hz = 1020000001;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_EINVAL);
    cfg = us_config();
    cfg.mod.implicit_header = true;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_EINVAL);
    CHECK(tern_radio_transmit(&r, (const uint8_t *)"x", 1) == TERN_EINVAL);
    CHECK(tern_radio_receive(&r) == TERN_EINVAL);

    /* The top of the SX1276's range is allowed, and a refused configuration after it changes
     * nothing. */
    cfg = us_config();
    cfg.freq_hz = 1020000000;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    b.count = 0;
    b.transfers = 0;
    cfg.freq_hz = 100000000;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_EINVAL);
    CHECK(b.transfers == 0);
    CHECK(tern_radio_transmit(&r, (const uint8_t *)"x", 1) == TERN_OK);
}

static void transmit_fills_the_fifo_and_starts(void) {
    static struct bus b;
    static struct tern_sx127x d;
    start(&d, &b);
    struct tern_radio r = tern_sx127x_radio(&d);
    struct tern_radio_config cfg = us_config();
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    b.count = 0;
    b.regs[0x12] = 0xFF;
    CHECK(tern_radio_transmit(&r, (const uint8_t *)"\x48\x01\x02", 3) == TERN_OK);
    CHECK(wrote(&b, 0, 0x01, 0x81)); /* standby, where the FIFO is written */
    CHECK(wrote(&b, 1, 0x0D, 0x00)); /* from its start */
    CHECK(memcmp(b.fifo, "\x48\x01\x02", 3) == 0);
    CHECK(wrote(&b, 2, 0x22, 0x03)); /* this frame's length */
    CHECK(wrote(&b, 3, 0x12, 0xFF)); /* every flag cleared */
    CHECK(wrote(&b, 4, 0x01, 0x83)); /* and sent */
    CHECK(b.regs[0x12] == 0x00);

    b.count = 0;
    CHECK(tern_radio_receive(&r) == TERN_OK);
    CHECK(wrote(&b, 0, 0x01, 0x81));
    CHECK(wrote(&b, b.count - 1, 0x01, 0x85)); /* RXCONTINUOUS */
}

static void poll_reports_what_happened(void) {
    static struct bus b;
    static struct tern_sx127x d;
    start(&d, &b);
    struct tern_radio r = tern_sx127x_radio(&d);
    struct tern_radio_config cfg = us_config();
    struct tern_radio_event ev;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);

    CHECK(tern_radio_poll(&r, &ev) == 0);

    b.regs[0x12] = 0x08; /* TxDone */
    b.now = 1234;
    CHECK(tern_radio_poll(&r, &ev) == 1);
    CHECK(ev.kind == TERN_RADIO_TX_DONE);
    CHECK_EQ_I64(ev.at, 1234);
    CHECK(b.regs[0x12] == 0x00);

    /* A 5-byte frame at 0x80, 7.25 dB under the noise (raw -29): -157 + 100 - 7 = -64 dBm. */
    b.regs[0x12] = 0x40;
    b.regs[0x13] = 5;
    b.regs[0x10] = 0x80;
    memcpy(&b.fifo[0x80], "hello", 5);
    b.regs[0x19] = (uint8_t)(int8_t)-29;
    b.regs[0x1A] = 100;
    CHECK(tern_radio_poll(&r, &ev) == 1);
    CHECK(ev.kind == TERN_RADIO_RX_DONE);
    CHECK(ev.len == 5 && memcmp(ev.data, "hello", 5) == 0);
    CHECK_EQ_I64(ev.rssi_dbm, -64);
    CHECK_EQ_I64(ev.snr_cdb, -725);
    CHECK(b.regs[0x12] == 0x00);
    CHECK(d.counts.frames == 1);

    /* Above the noise, 10 dB (raw 40): -157 + 90 * 16 / 15 = -61 dBm. */
    b.regs[0x12] = 0x40;
    b.regs[0x19] = 40;
    b.regs[0x1A] = 90;
    CHECK(tern_radio_poll(&r, &ev) == 1);
    CHECK_EQ_I64(ev.rssi_dbm, -61);
    CHECK_EQ_I64(ev.snr_cdb, 1000);

    b.regs[0x12] = 0x60; /* RxDone with a CRC error */
    CHECK(tern_radio_poll(&r, &ev) == 1);
    CHECK(ev.kind == TERN_RADIO_RX_ERROR);
    CHECK(b.regs[0x12] == 0x00);
    CHECK(d.counts.crc_errors == 1 && d.counts.frames == 2);

    b.regs[0x12] = 0x80; /* RxTimeout, which continuous reception never raises: just cleared */
    CHECK(tern_radio_poll(&r, &ev) == 0);
    CHECK(b.regs[0x12] == 0x00);
}

static void rssi_on_the_lf_port(void) {
    static struct bus b;
    static struct tern_sx127x d;
    struct tern_sx127x_board board = {.chip = TERN_SX1278, .pa_boost = true};
    start_on(&d, &b, &board);
    struct tern_radio r = tern_sx127x_radio(&d);
    struct tern_radio_config cfg = us_config();
    struct tern_radio_event ev;
    cfg.freq_hz = 433000000;
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    b.regs[0x12] = 0x40;
    b.regs[0x13] = 1;
    b.regs[0x19] = 40;
    b.regs[0x1A] = 90;
    CHECK(tern_radio_poll(&r, &ev) == 1);
    CHECK_EQ_I64(ev.rssi_dbm, -68); /* -164 + 96 */
}

/* Listening first (tern/listen.h), as RegModemStat and ValidHeader drive it. At SF7 and 250 kHz a
 * symbol is 0.512 ms, so a preamble of 16 with no header after it holds the radio for 29 symbols,
 * 14.848 ms, and a header for as long as 255 bytes are on the air. */
static void receiving_follows_the_modem(void) {
    static struct bus b;
    static struct tern_sx127x d;
    start(&d, &b);
    struct tern_radio r = tern_sx127x_radio(&d);
    struct tern_radio_config cfg = us_config();
    struct tern_radio_event ev;
    const tern_time wait = 14848000, longest = tern_lora_airtime(&cfg.mod, 255);

    b.transfers = 0;
    CHECK(tern_radio_receiving(&r) == 0); /* not yet configured: nothing is asked of the chip */
    CHECK(b.transfers == 0);
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    CHECK(tern_radio_receive(&r) == TERN_OK);
    b.transfers = 0;
    CHECK(tern_radio_receiving(&r) == 0);
    CHECK(b.transfers == 2); /* RegIrqFlags and RegModemStat, and no more */

    /* The modem finds a signal: a preamble, counted once however often it is asked. */
    b.now = 5000000;
    b.regs[0x18] = 0x01;
    CHECK(tern_radio_receiving(&r) == 1);
    b.now = 5000000 + wait - 1;
    CHECK(tern_radio_receiving(&r) == 1);
    CHECK(d.counts.preambles == 1);
    b.now = 5000000 + wait;
    CHECK(tern_radio_receiving(&r) == 0); /* no header came, though the modem still has it */
    CHECK(d.counts.preambles == 1);

    /* It lets go, then finds another, and a header: held past the wait for a header, and the
     * header counted and cleared once. */
    b.regs[0x18] = 0x00;
    CHECK(tern_radio_receiving(&r) == 0);
    b.now = 100000000;
    b.regs[0x18] = 0x03;
    CHECK(tern_radio_poll(&r, &ev) == 0);
    CHECK(d.counts.preambles == 2);
    b.now += 10000000;
    b.regs[0x18] = 0x0F;
    b.regs[0x12] = 0x10;
    CHECK(tern_radio_receiving(&r) == 1);
    CHECK(b.regs[0x12] == 0x00 && d.counts.headers == 1);
    b.now = 100000000 + longest - 1;
    CHECK(tern_radio_receiving(&r) == 1);
    CHECK(d.counts.headers == 1 && d.counts.preambles == 2);

    /* The frame has ended and poll has not collected it: no longer receiving, and the frame is
     * still there to collect. */
    b.regs[0x18] = 0x00;
    b.regs[0x12] = 0x40;
    b.regs[0x13] = 1;
    CHECK(tern_radio_receiving(&r) == 0);
    CHECK(tern_radio_poll(&r, &ev) == 1 && ev.kind == TERN_RADIO_RX_DONE);
    CHECK(tern_radio_receiving(&r) == 0);

    /* A signal the modem drops with no header, as a frame with another sync word is: over at
     * once, without waiting out the wait for a header. */
    b.regs[0x18] = 0x01;
    CHECK(tern_radio_receiving(&r) == 1);
    b.regs[0x18] = 0x00;
    CHECK(tern_radio_receiving(&r) == 0);
    CHECK(d.counts.preambles == 3 && d.counts.headers == 1);

    /* Sending, listening afresh, standby and a new configuration each drop what it was on. */
    int (*const drops[])(struct tern_radio *) = {tern_radio_receive, tern_radio_standby};
    for (size_t i = 0; i < 2; i++) {
        b.regs[0x18] = 0x0F;
        b.regs[0x12] = 0x10;
        CHECK(tern_radio_receiving(&r) == 1);
        CHECK(drops[i](&r) == TERN_OK);
        b.regs[0x18] = 0x00;
        CHECK(tern_radio_receiving(&r) == 0);
    }
    b.regs[0x18] = 0x0F;
    CHECK(tern_radio_receiving(&r) == 1);
    CHECK(tern_radio_transmit(&r, (const uint8_t *)"x", 1) == TERN_OK);
    b.regs[0x18] = 0x00;
    CHECK(tern_radio_receiving(&r) == 0);
    b.regs[0x18] = 0x0F;
    CHECK(tern_radio_receiving(&r) == 1);
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    b.regs[0x18] = 0x00;
    CHECK(tern_radio_receiving(&r) == 0);
}

static void sleep_is_lora_sleep(void) {
    static struct bus b;
    static struct tern_sx127x d;
    start(&d, &b);
    struct tern_radio r = tern_sx127x_radio(&d);
    struct tern_radio_config cfg = us_config();
    CHECK(tern_radio_configure(&r, &cfg) == TERN_OK);
    CHECK(tern_radio_receive(&r) == TERN_OK);
    b.count = 0;
    CHECK(tern_sx127x_sleep(&d) == TERN_OK);
    CHECK(b.count == 1 && wrote(&b, 0, 0x01, 0x80));
    CHECK(!d.configured);
    CHECK(tern_radio_transmit(&r, (const uint8_t *)"x", 1) == TERN_EINVAL);
}

int main(void) {
    RUN(init_checks_the_chip_and_sleeps_in_lora);
    RUN(configure_sets_the_datasheet_registers);
    RUN(an_sx1278_on_rfo_at_433_mhz);
    RUN(configure_refuses_what_the_chip_cannot_do);
    RUN(transmit_fills_the_fifo_and_starts);
    RUN(poll_reports_what_happened);
    RUN(rssi_on_the_lf_port);
    RUN(receiving_follows_the_modem);
    RUN(sleep_is_lora_sleep);
    return CHECK_DONE();
}
