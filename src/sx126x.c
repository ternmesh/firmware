#include "tern/sx126x.h"

#include "tern/err.h"

/* Opcodes and registers from the SX1261/2 datasheet, section 13 (commands) and 15 (errata). */
enum {
    CMD_SET_STANDBY = 0x80,
    CMD_SET_RX = 0x82,
    CMD_SET_TX = 0x83,
    CMD_SET_RF_FREQUENCY = 0x86,
    CMD_CALIBRATE = 0x89,
    CMD_SET_PACKET_TYPE = 0x8A,
    CMD_SET_MODULATION_PARAMS = 0x8B,
    CMD_SET_PACKET_PARAMS = 0x8C,
    CMD_SET_TX_PARAMS = 0x8E,
    CMD_SET_BUFFER_BASE_ADDRESS = 0x8F,
    CMD_SET_PA_CONFIG = 0x95,
    CMD_SET_REGULATOR_MODE = 0x96,
    CMD_SET_DIO3_AS_TCXO_CTRL = 0x97,
    CMD_CALIBRATE_IMAGE = 0x98,
    CMD_SET_DIO2_AS_RF_SWITCH_CTRL = 0x9D,
    CMD_CLEAR_IRQ_STATUS = 0x02,
    CMD_CLEAR_DEVICE_ERRORS = 0x07,
    CMD_SET_DIO_IRQ_PARAMS = 0x08,
    CMD_WRITE_REGISTER = 0x0D,
    CMD_WRITE_BUFFER = 0x0E,
    CMD_GET_IRQ_STATUS = 0x12,
    CMD_GET_RX_BUFFER_STATUS = 0x13,
    CMD_GET_PACKET_STATUS = 0x14,
    CMD_READ_REGISTER = 0x1D,
    CMD_READ_BUFFER = 0x1E,
};

enum {
    REG_LORA_SYNC_WORD = 0x0740, /* two bytes, most significant first */
    REG_IQ_POLARITY = 0x0736,    /* errata 15.4 */
    REG_TX_MODULATION = 0x0889,  /* errata 15.1 */
    REG_TX_CLAMP = 0x08D8,       /* errata 15.2 */
};

enum {
    IRQ_TX_DONE = 1u << 0,
    IRQ_RX_DONE = 1u << 1,
    IRQ_PREAMBLE = 1u << 2,
    IRQ_HEADER_VALID = 1u << 4,
    IRQ_HEADER_ERR = 1u << 5,
    IRQ_CRC_ERR = 1u << 6,
    IRQ_TIMEOUT = 1u << 9,
};

#define IRQ_COUNTED (IRQ_PREAMBLE | IRQ_HEADER_VALID)
#define IRQ_USED                                                                                   \
    (IRQ_TX_DONE | IRQ_RX_DONE | IRQ_HEADER_ERR | IRQ_CRC_ERR | IRQ_TIMEOUT | IRQ_COUNTED)
#define STANDBY_RC 0x00
#define PACKET_TYPE_LORA 0x01
#define RX_CONTINUOUS 0xFFFFFFu
#define MIN_POWER_DBM (-9) /* the SX1262's high-power PA */
#define MAX_POWER_DBM 22
#define MIN_FREQ_HZ 150000000u /* the SX1262 tunes from 150 to 960 MHz (section 3) */
#define MAX_FREQ_HZ 960000000u

static int cmd(struct tern_sx126x *d, const uint8_t *tx, size_t len) {
    return d->bus.transfer(d->bus.ctx, tx, NULL, len);
}

#define CMD(d, ...) cmd((d), (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))

static int write_register(struct tern_sx126x *d, uint16_t addr, uint8_t value) {
    return CMD(d, CMD_WRITE_REGISTER, (uint8_t)(addr >> 8), (uint8_t)addr, value);
}

static int read_register(struct tern_sx126x *d, uint16_t addr, uint8_t *value) {
    uint8_t tx[5] = {CMD_READ_REGISTER, (uint8_t)(addr >> 8), (uint8_t)addr, 0, 0};
    uint8_t rx[5];
    int err = d->bus.transfer(d->bus.ctx, tx, rx, sizeof tx);
    *value = rx[4];
    return err;
}

/* Sets or clears bits in a register, leaving the rest as they were. */
static int update_register(struct tern_sx126x *d, uint16_t addr, uint8_t mask, bool set) {
    uint8_t v;
    int err = read_register(d, addr, &v);
    if (err != TERN_OK) {
        return err;
    }
    return write_register(d, addr, (uint8_t)(set ? v | mask : v & ~mask));
}

static int clear_irq(struct tern_sx126x *d, uint16_t mask) {
    return CMD(d, CMD_CLEAR_IRQ_STATUS, (uint8_t)(mask >> 8), (uint8_t)mask);
}

/* The TCXO supply voltages DIO3 can give, in the order of their codes (section 13.3.6). */
static const uint16_t tcxo_mv[] = {1600, 1700, 1800, 2200, 2400, 2700, 3000, 3300};

int tern_sx126x_init(struct tern_sx126x *d, const struct tern_sx126x_bus *bus,
                     const struct tern_sx126x_board *board) {
    d->bus = *bus;
    d->board = *board;
    d->configured = false;
    d->counts = (struct tern_sx126x_counts){0};
    tern_listen_init(&d->listen);

    int err = CMD(d, CMD_SET_STANDBY, STANDBY_RC);
    if (err == TERN_OK && board->dcdc) {
        err = CMD(d, CMD_SET_REGULATOR_MODE, 0x01);
    }

    if (err == TERN_OK && board->tcxo_mv != 0) {
        uint8_t code = 0xFF;
        for (uint8_t i = 0; i < sizeof tcxo_mv / sizeof tcxo_mv[0]; i++) {
            if (tcxo_mv[i] == board->tcxo_mv) {
                code = i;
            }
        }
        if (code == 0xFF) {
            return TERN_EINVAL;
        }
        /* 5 ms for the TCXO to settle, in steps of 15.625 us: 320. The chip calibrated itself at
         * power-on without its clock, which fails, so the error is cleared and it calibrates
         * again now that it has one (section 13.3.6). */
        err = CMD(d, CMD_SET_DIO3_AS_TCXO_CTRL, code, 0x00, 0x01, 0x40);
        if (err == TERN_OK) {
            err = CMD(d, CMD_CLEAR_DEVICE_ERRORS, 0x00, 0x00);
        }
        if (err == TERN_OK) {
            err = CMD(d, CMD_CALIBRATE, 0x7F);
        }
    }

    if (err == TERN_OK && board->dio2_rf_switch) {
        err = CMD(d, CMD_SET_DIO2_AS_RF_SWITCH_CTRL, 0x01);
    }
    if (err == TERN_OK) {
        /* Errata 15.2: without this the PA can be damaged by a badly matched antenna. */
        err = update_register(d, REG_TX_CLAMP, 0x1E, true);
    }
    if (err == TERN_OK) {
        err = CMD(d, CMD_SET_PACKET_TYPE, PACKET_TYPE_LORA);
    }
    if (err == TERN_OK) {
        err = CMD(d, CMD_SET_BUFFER_BASE_ADDRESS, 0x00, 0x00);
    }
    if (err == TERN_OK) {
        uint16_t m = IRQ_USED;
        err = CMD(d, CMD_SET_DIO_IRQ_PARAMS, (uint8_t)(m >> 8), (uint8_t)m, (uint8_t)(m >> 8),
                  (uint8_t)m, 0, 0, 0, 0);
    }
    return err;
}

/* The bandwidth codes of SetModulationParams (section 13.4.5). */
static int bw_code(uint32_t bw_hz, uint8_t *code) {
    switch (bw_hz) {
    case 62500:
        *code = 0x03;
        return TERN_OK;
    case 125000:
        *code = 0x04;
        return TERN_OK;
    case 250000:
        *code = 0x05;
        return TERN_OK;
    case 500000:
        *code = 0x06;
        return TERN_OK;
    default:
        return TERN_EINVAL;
    }
}

/* The two ends of the band CalibrateImage should cover, in units of 4 MHz (section 9.2.1): the
 * datasheet's pairs for the usual bands, else 4 MHz either side of the frequency. */
static void image_band(uint32_t freq_hz, uint8_t band[2]) {
    static const struct {
        uint32_t lo, hi;
        uint8_t f1, f2;
    } bands[] = {
        {430000000, 440000000, 0x6B, 0x6F}, {470000000, 510000000, 0x75, 0x81},
        {779000000, 787000000, 0xC1, 0xC5}, {863000000, 870000000, 0xD7, 0xDB},
        {902000000, 928000000, 0xE1, 0xE9},
    };
    for (size_t i = 0; i < sizeof bands / sizeof bands[0]; i++) {
        if (freq_hz >= bands[i].lo && freq_hz <= bands[i].hi) {
            band[0] = bands[i].f1;
            band[1] = bands[i].f2;
            return;
        }
    }
    uint32_t mhz4 = freq_hz / 4000000;
    band[0] = (uint8_t)(mhz4 - 1);
    band[1] = (uint8_t)(mhz4 + 1);
}

static int packet_params(struct tern_sx126x *d, uint8_t len) {
    const struct tern_lora *m = &d->cfg.mod;
    return CMD(d, CMD_SET_PACKET_PARAMS, (uint8_t)(m->preamble >> 8), (uint8_t)m->preamble,
               m->implicit_header ? 0x01 : 0x00, len, m->crc ? 0x01 : 0x00, 0x00);
}

static int sx_configure(void *ctx, const struct tern_radio_config *cfg) {
    struct tern_sx126x *d = ctx;
    const struct tern_lora *m = &cfg->mod;
    uint8_t bw, band[2];
    /* Implicit-header reception needs the frame's length programmed in advance, and the radio
     * seam has no way to give it, so only explicit headers are supported. */
    if (bw_code(m->bw_hz, &bw) != TERN_OK || cfg->tx_power_dbm < MIN_POWER_DBM ||
        cfg->tx_power_dbm > MAX_POWER_DBM || cfg->freq_hz < MIN_FREQ_HZ ||
        cfg->freq_hz > MAX_FREQ_HZ || m->implicit_header) {
        return TERN_EINVAL;
    }
    d->cfg = *cfg;
    d->configured = false;
    tern_listen_over(&d->listen); /* it is left in standby */

    /* RfFreq = freq * 2^25 / 32 MHz (section 13.4.1). */
    uint32_t rf = (uint32_t)(((uint64_t)cfg->freq_hz << 25) / 32000000u);
    uint16_t sync = tern_sync_word_sx126x(cfg->sync_word);
    image_band(cfg->freq_hz, band);

    int err = CMD(d, CMD_SET_STANDBY, STANDBY_RC);
    if (err == TERN_OK) {
        err = CMD(d, CMD_SET_PACKET_TYPE, PACKET_TYPE_LORA);
    }
    if (err == TERN_OK) {
        err = CMD(d, CMD_SET_RF_FREQUENCY, (uint8_t)(rf >> 24), (uint8_t)(rf >> 16),
                  (uint8_t)(rf >> 8), (uint8_t)rf);
    }
    if (err == TERN_OK) {
        err = CMD(d, CMD_CALIBRATE_IMAGE, band[0], band[1]);
    }
    if (err == TERN_OK) {
        /* The SX1262's high-power PA, set up for its full +22 dBm (section 13.1.14), so that the
         * power SetTxParams asks for is the power it gives, from -9 to +22 dBm. */
        err = CMD(d, CMD_SET_PA_CONFIG, 0x04, 0x07, 0x00, 0x01);
    }
    if (err == TERN_OK) {
        err = CMD(d, CMD_SET_TX_PARAMS, (uint8_t)cfg->tx_power_dbm, 0x04 /* 200 us ramp */);
    }
    if (err == TERN_OK) {
        err = CMD(d, CMD_SET_MODULATION_PARAMS, m->sf, bw, m->cr, tern_lora_ldro(m) ? 0x01 : 0x00);
    }
    if (err == TERN_OK) {
        err = packet_params(d, 255);
    }
    if (err == TERN_OK) {
        err = write_register(d, REG_LORA_SYNC_WORD, (uint8_t)(sync >> 8));
    }
    if (err == TERN_OK) {
        err = write_register(d, REG_LORA_SYNC_WORD + 1, (uint8_t)sync);
    }
    if (err == TERN_OK) {
        /* Errata 15.1: the bit is cleared at 500 kHz and set at every other bandwidth. */
        err = update_register(d, REG_TX_MODULATION, 0x04, m->bw_hz != 500000);
    }
    if (err == TERN_OK) {
        /* Errata 15.4: with the standard IQ polarity this bit must be set. */
        err = update_register(d, REG_IQ_POLARITY, 0x04, true);
    }
    d->configured = err == TERN_OK;
    return err;
}

static int sx_transmit(void *ctx, const uint8_t *frame, uint8_t len) {
    struct tern_sx126x *d = ctx;
    if (!d->configured) {
        return TERN_EINVAL;
    }
    uint8_t buf[2 + 255];
    buf[0] = CMD_WRITE_BUFFER;
    buf[1] = 0x00;
    for (uint8_t i = 0; i < len; i++) {
        buf[2 + i] = frame[i];
    }

    tern_listen_over(&d->listen);
    int err = CMD(d, CMD_SET_STANDBY, STANDBY_RC);
    if (err == TERN_OK) {
        err = packet_params(d, len);
    }
    if (err == TERN_OK) {
        err = cmd(d, buf, 2 + (size_t)len);
    }
    if (err == TERN_OK) {
        err = clear_irq(d, 0xFFFF);
    }
    if (err == TERN_OK) {
        err = CMD(d, CMD_SET_TX, 0x00, 0x00, 0x00); /* no timeout */
    }
    return err;
}

static int sx_receive(void *ctx) {
    struct tern_sx126x *d = ctx;
    if (!d->configured) {
        return TERN_EINVAL;
    }
    tern_listen_over(&d->listen);
    int err = CMD(d, CMD_SET_STANDBY, STANDBY_RC);
    if (err == TERN_OK) {
        err = packet_params(d, 255);
    }
    if (err == TERN_OK) {
        err = clear_irq(d, 0xFFFF);
    }
    if (err == TERN_OK) {
        err = CMD(d, CMD_SET_RX, (uint8_t)(RX_CONTINUOUS >> 16), (uint8_t)(RX_CONTINUOUS >> 8),
                  (uint8_t)RX_CONTINUOUS);
    }
    return err;
}

static int sx_standby(void *ctx) {
    struct tern_sx126x *d = ctx;
    tern_listen_over(&d->listen);
    return CMD(d, CMD_SET_STANDBY, 0x00);
}

/* Asks the chip what has happened, and notes what the receiver got as far as, counted whether or
 * not a frame comes of it: a frame with another network's sync word is a preamble and nothing
 * more. Those two flags are cleared here, so each is noted once. */
static int read_irq(struct tern_sx126x *d, uint16_t *irq) {
    uint8_t tx[4] = {CMD_GET_IRQ_STATUS, 0, 0, 0}, rx[4];
    int err = d->bus.transfer(d->bus.ctx, tx, rx, 4);
    if (err != TERN_OK) {
        return err;
    }
    *irq = (uint16_t)(rx[2] << 8 | rx[3]);
    if (*irq & IRQ_COUNTED) {
        tern_time now = d->bus.now(d->bus.ctx);
        if (*irq & IRQ_PREAMBLE) {
            d->counts.preambles++;
            tern_listen_preamble(&d->listen, now);
        }
        if (*irq & IRQ_HEADER_VALID) {
            d->counts.headers++;
            tern_listen_header(&d->listen, now);
        }
        err = clear_irq(d, *irq & IRQ_COUNTED);
    }
    return err;
}

/* The flags the chip ends a frame with are left for sx_poll() to collect: a frame that has ended
 * and not yet been collected is no longer being received. */
static int sx_receiving(void *ctx) {
    struct tern_sx126x *d = ctx;
    uint16_t irq;
    if (!d->configured) {
        return 0;
    }
    int err = read_irq(d, &irq);
    if (err != TERN_OK) {
        return err;
    }
    if (irq & (IRQ_RX_DONE | IRQ_HEADER_ERR | IRQ_CRC_ERR)) {
        return 0;
    }
    return tern_listen_receiving(&d->listen, &d->cfg.mod, d->bus.now(d->bus.ctx)) ? 1 : 0;
}

static int sx_poll(void *ctx, struct tern_radio_event *ev) {
    struct tern_sx126x *d = ctx;
    uint8_t tx[5] = {CMD_GET_IRQ_STATUS, 0, 0, 0, 0}, rx[5];
    uint16_t irq;

    int err = read_irq(d, &irq);
    if (err != TERN_OK) {
        return err;
    }

    if (irq & IRQ_TX_DONE) {
        *ev = (struct tern_radio_event){.kind = TERN_RADIO_TX_DONE, .at = d->bus.now(d->bus.ctx)};
        err = clear_irq(d, IRQ_TX_DONE);
        return err == TERN_OK ? 1 : err;
    }

    if (irq & (IRQ_RX_DONE | IRQ_HEADER_ERR)) {
        tern_time at = d->bus.now(d->bus.ctx);
        tern_listen_over(&d->listen);
        if (irq & (IRQ_HEADER_ERR | IRQ_CRC_ERR)) {
            d->counts.header_errors += (irq & IRQ_HEADER_ERR) != 0;
            d->counts.crc_errors += (irq & IRQ_HEADER_ERR) == 0;
            *ev = (struct tern_radio_event){.kind = TERN_RADIO_RX_ERROR, .at = at};
            err = clear_irq(d, IRQ_RX_DONE | IRQ_HEADER_ERR | IRQ_CRC_ERR);
            return err == TERN_OK ? 1 : err;
        }

        /* Where the frame is and how long (section 13.5.2), then the frame, then its signal
         * (section 13.5.3): RSSI is -raw / 2 dBm, SNR is the signed raw / 4 dB. */
        tx[0] = CMD_GET_RX_BUFFER_STATUS;
        err = d->bus.transfer(d->bus.ctx, tx, rx, 4);
        if (err != TERN_OK) {
            return err;
        }
        uint8_t len = rx[2], start = rx[3];

        uint8_t buf[3 + 255] = {CMD_READ_BUFFER, start, 0};
        uint8_t got[3 + 255];
        err = d->bus.transfer(d->bus.ctx, buf, got, 3 + (size_t)len);
        if (err != TERN_OK) {
            return err;
        }
        for (uint8_t i = 0; i < len; i++) {
            d->rx[i] = got[3 + i];
        }

        tx[0] = CMD_GET_PACKET_STATUS;
        err = d->bus.transfer(d->bus.ctx, tx, rx, 5);
        if (err != TERN_OK) {
            return err;
        }

        *ev = (struct tern_radio_event){
            .kind = TERN_RADIO_RX_DONE,
            .at = at,
            .data = d->rx,
            .len = len,
            .rssi_dbm = (int16_t)(-(int16_t)rx[2] / 2),
            .snr_cdb = (int16_t)((int8_t)rx[3] * 25),
        };
        d->counts.frames++;
        err = clear_irq(d, IRQ_RX_DONE);
        return err == TERN_OK ? 1 : err;
    }

    if (irq & IRQ_TIMEOUT) {
        err = clear_irq(d, IRQ_TIMEOUT);
        if (err != TERN_OK) {
            return err;
        }
    }
    return 0;
}

static const struct tern_radio_ops sx_ops = {
    .configure = sx_configure,
    .transmit = sx_transmit,
    .receive = sx_receive,
    .standby = sx_standby,
    .poll = sx_poll,
    .receiving = sx_receiving,
};

struct tern_radio tern_sx126x_radio(struct tern_sx126x *d) {
    return (struct tern_radio){.ops = &sx_ops, .ctx = d};
}
