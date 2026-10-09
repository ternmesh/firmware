#include "tern/sx127x.h"

#include "tern/err.h"

/* Registers of the LoRa modem, from the datasheet's register table (section 6.4). */
enum {
    REG_FIFO = 0x00,
    REG_OP_MODE = 0x01,
    REG_FRF_MSB = 0x06,
    REG_FRF_MID = 0x07,
    REG_FRF_LSB = 0x08,
    REG_PA_CONFIG = 0x09,
    REG_OCP = 0x0B,
    REG_LNA = 0x0C,
    REG_FIFO_ADDR_PTR = 0x0D,
    REG_FIFO_TX_BASE = 0x0E,
    REG_FIFO_RX_BASE = 0x0F,
    REG_FIFO_RX_CURRENT = 0x10,
    REG_IRQ_FLAGS_MASK = 0x11,
    REG_IRQ_FLAGS = 0x12,
    REG_RX_NB_BYTES = 0x13,
    REG_MODEM_STAT = 0x18,
    REG_PKT_SNR = 0x19,
    REG_PKT_RSSI = 0x1A,
    REG_MODEM_CONFIG_1 = 0x1D,
    REG_MODEM_CONFIG_2 = 0x1E,
    REG_PREAMBLE_MSB = 0x20,
    REG_PREAMBLE_LSB = 0x21,
    REG_PAYLOAD_LENGTH = 0x22,
    REG_MAX_PAYLOAD_LENGTH = 0x23,
    REG_MODEM_CONFIG_3 = 0x26,
    REG_DETECT_OPTIMIZE = 0x31,
    REG_DETECTION_THRESHOLD = 0x37,
    REG_SYNC_WORD = 0x39,
    REG_VERSION = 0x42,
    REG_TCXO = 0x4B,
    REG_PA_DAC = 0x4D,
};

enum {
    MODE_LORA = 0x80, /* LongRangeMode, changed only in sleep */
    MODE_SLEEP = 0x00,
    MODE_STANDBY = 0x01,
    MODE_TX = 0x03,
    MODE_RX_CONTINUOUS = 0x05,
};

enum {
    IRQ_RX_TIMEOUT = 1u << 7,
    IRQ_RX_DONE = 1u << 6,
    IRQ_CRC_ERR = 1u << 5,
    IRQ_VALID_HEADER = 1u << 4,
    IRQ_TX_DONE = 1u << 3,
};

/* RegModemStat: what the modem is doing now. Any of these is a signal it has found. */
enum {
    STAT_HEADER_INFO_VALID = 1u << 3,
    STAT_RX_ONGOING = 1u << 2,
    STAT_SIGNAL_SYNCHRONIZED = 1u << 1,
    STAT_SIGNAL_DETECTED = 1u << 0,
};
#define STAT_SIGNAL                                                                                \
    (STAT_HEADER_INFO_VALID | STAT_RX_ONGOING | STAT_SIGNAL_SYNCHRONIZED | STAT_SIGNAL_DETECTED)

#define VERSION 0x12 /* RegVersion on every chip of the family */
#define WRITE 0x80   /* the address byte's wnr bit (section 4.3) */
#define MIN_FREQ_HZ 137000000u
#define SX1276_MAX_FREQ_HZ 1020000000u
#define SX1278_MAX_FREQ_HZ 525000000u
#define LF_MAX_HZ 525000000u /* below this the RFI_LF port: band 2 and 3 */

static int write_register(struct tern_sx127x *d, uint8_t addr, uint8_t value) {
    const uint8_t tx[2] = {(uint8_t)(WRITE | addr), value};
    return d->bus.transfer(d->bus.ctx, tx, NULL, 2);
}

static int read_register(struct tern_sx127x *d, uint8_t addr, uint8_t *value) {
    const uint8_t tx[2] = {addr, 0};
    uint8_t rx[2] = {0, 0};
    int err = d->bus.transfer(d->bus.ctx, tx, rx, 2);
    *value = rx[1];
    return err;
}

/* Sets the bits of mask to those of value, leaving the rest as they were. */
static int update_register(struct tern_sx127x *d, uint8_t addr, uint8_t mask, uint8_t value) {
    uint8_t v;
    int err = read_register(d, addr, &v);
    if (err != TERN_OK) {
        return err;
    }
    return write_register(d, addr, (uint8_t)((v & ~mask) | (value & mask)));
}

/* What the receiver was on is forgotten: it has been told to do something else. */
static void drop(struct tern_sx127x *d) {
    tern_listen_over(&d->listen);
    d->signal = false;
}

static int set_mode(struct tern_sx127x *d, uint8_t mode) {
    return write_register(d, REG_OP_MODE, (uint8_t)(MODE_LORA | mode));
}

/* Writing a flag's bit clears it (section 4.1.2.4). */
static int clear_irq(struct tern_sx127x *d, uint8_t mask) {
    return write_register(d, REG_IRQ_FLAGS, mask);
}

int tern_sx127x_init(struct tern_sx127x *d, const struct tern_sx127x_bus *bus,
                     const struct tern_sx127x_board *board) {
    d->bus = *bus;
    d->board = *board;
    d->configured = false;
    d->counts = (struct tern_sx127x_counts){0};
    tern_listen_init(&d->listen);
    d->signal = false;

    uint8_t version = 0;
    int err = read_register(d, REG_VERSION, &version);
    if (err != TERN_OK) {
        return err;
    }
    if (version != VERSION) {
        return TERN_EIO;
    }
    /* The chip starts in the FSK modem's standby. LongRangeMode is changed only in sleep, so to
     * sleep first, then to the LoRa modem's (section 4.1.3). */
    err = write_register(d, REG_OP_MODE, MODE_SLEEP);
    if (err == TERN_OK) {
        err = set_mode(d, MODE_SLEEP);
    }
    if (err == TERN_OK && board->tcxo) {
        err = update_register(d, REG_TCXO, 0x10, 0x10); /* TcxoInputOn, set in sleep */
    }
    if (err == TERN_OK) {
        err = write_register(d, REG_IRQ_FLAGS_MASK, 0x00);
    }
    return err;
}

/* RegPaConfig, RegPaDac and RegOcp for a power, or TERN_EINVAL if the board does not give it
 * (section 5.4). On PA_BOOST, Pout = 2 + OutputPower up to +17 dBm; above, with PaDac at 0x87,
 * Pout = 5 + OutputPower to +20 dBm, which draws more than the default current limit of 100 mA
 * allows, so the limit is raised to 140 mA. On RFO with MaxPower at 7, Pout = OutputPower. */
struct pa {
    uint8_t config, dac, ocp;
};

static int pa_for(const struct tern_sx127x_board *b, int dbm, struct pa *pa) {
    if (b->pa_boost) {
        if (dbm < 2 || dbm > 20) {
            return TERN_EINVAL;
        }
        if (dbm <= 17) {
            *pa = (struct pa){.config = (uint8_t)(0xF0 | (dbm - 2)), .dac = 0x84, .ocp = 0x2B};
        } else {
            *pa = (struct pa){.config = (uint8_t)(0xF0 | (dbm - 5)), .dac = 0x87, .ocp = 0x31};
        }
        return TERN_OK;
    }
    if (dbm < 0 || dbm > 15) {
        return TERN_EINVAL;
    }
    *pa = (struct pa){.config = (uint8_t)(0x70 | dbm), .dac = 0x84, .ocp = 0x2B};
    return TERN_OK;
}

/* The bandwidth codes of RegModemConfig1 (section 6.4). */
static int bw_code(uint32_t bw_hz, uint8_t *code) {
    switch (bw_hz) {
    case 62500:
        *code = 0x6;
        return TERN_OK;
    case 125000:
        *code = 0x7;
        return TERN_OK;
    case 250000:
        *code = 0x8;
        return TERN_OK;
    case 500000:
        *code = 0x9;
        return TERN_OK;
    default:
        return TERN_EINVAL;
    }
}

static int sx_configure(void *ctx, const struct tern_radio_config *cfg) {
    struct tern_sx127x *d = ctx;
    const struct tern_lora *m = &cfg->mod;
    uint32_t max_hz = d->board.chip == TERN_SX1278 ? SX1278_MAX_FREQ_HZ : SX1276_MAX_FREQ_HZ;
    uint8_t bw;
    struct pa pa;
    /* Implicit headers need the frame's length in advance, which the seam does not give, as for
     * the SX1262. */
    if (bw_code(m->bw_hz, &bw) != TERN_OK || pa_for(&d->board, cfg->tx_power_dbm, &pa) != TERN_OK ||
        cfg->freq_hz < MIN_FREQ_HZ || cfg->freq_hz > max_hz || m->implicit_header) {
        return TERN_EINVAL;
    }
    d->cfg = *cfg;
    d->configured = false;
    drop(d); /* it is left in standby */

    /* Frf = freq * 2^19 / 32 MHz (section 4.1.4). */
    uint32_t frf = (uint32_t)(((uint64_t)cfg->freq_hz << 19) / 32000000u);

    int err = set_mode(d, MODE_SLEEP);
    const uint8_t regs[][2] = {
        {REG_FRF_MSB, (uint8_t)(frf >> 16)},
        {REG_FRF_MID, (uint8_t)(frf >> 8)},
        {REG_FRF_LSB, (uint8_t)frf},
        {REG_PA_CONFIG, pa.config},
        {REG_PA_DAC, pa.dac},
        {REG_OCP, pa.ocp},
        /* The LNA at its highest gain, its current boosted on the HF port; the AGC sets the gain
         * from there (RegModemConfig3). */
        {REG_LNA, cfg->freq_hz > LF_MAX_HZ ? 0x23 : 0x20},
        {REG_MODEM_CONFIG_1, (uint8_t)(bw << 4 | m->cr << 1)}, /* explicit header */
        {REG_MODEM_CONFIG_2, (uint8_t)(m->sf << 4 | (m->crc ? 0x04 : 0x00))},
        {REG_MODEM_CONFIG_3, (uint8_t)((tern_lora_ldro(m) ? 0x08 : 0x00) | 0x04)}, /* AGC on */
        {REG_PREAMBLE_MSB, (uint8_t)(m->preamble >> 8)},
        {REG_PREAMBLE_LSB, (uint8_t)m->preamble},
        {REG_MAX_PAYLOAD_LENGTH, 0xFF},
        {REG_SYNC_WORD, cfg->sync_word},
        {REG_DETECTION_THRESHOLD, 0x0A}, /* SF7 to SF12 */
        {REG_FIFO_TX_BASE, 0x00},        /* the whole FIFO, either way: never both at once */
        {REG_FIFO_RX_BASE, 0x00},
    };
    for (size_t i = 0; err == TERN_OK && i < sizeof regs / sizeof regs[0]; i++) {
        err = write_register(d, regs[i][0], regs[i][1]);
    }
    if (err == TERN_OK) {
        err = update_register(d, REG_DETECT_OPTIMIZE, 0x07, 0x03); /* SF7 to SF12 */
    }
    if (err == TERN_OK) {
        err = set_mode(d, MODE_STANDBY);
    }
    d->configured = err == TERN_OK;
    return err;
}

static int sx_transmit(void *ctx, const uint8_t *frame, uint8_t len) {
    struct tern_sx127x *d = ctx;
    if (!d->configured) {
        return TERN_EINVAL;
    }
    uint8_t buf[1 + 255];
    buf[0] = WRITE | REG_FIFO;
    for (uint8_t i = 0; i < len; i++) {
        buf[1 + i] = frame[i];
    }

    /* The FIFO is written only in standby, from the pointer, which each byte moves on (section
     * 4.1.2.3). */
    drop(d);
    int err = set_mode(d, MODE_STANDBY);
    if (err == TERN_OK) {
        err = write_register(d, REG_FIFO_ADDR_PTR, 0x00);
    }
    if (err == TERN_OK) {
        err = d->bus.transfer(d->bus.ctx, buf, NULL, 1 + (size_t)len);
    }
    if (err == TERN_OK) {
        err = write_register(d, REG_PAYLOAD_LENGTH, len);
    }
    if (err == TERN_OK) {
        err = clear_irq(d, 0xFF);
    }
    if (err == TERN_OK) {
        err = set_mode(d, MODE_TX); /* back to standby by itself once it has gone */
    }
    return err;
}

static int sx_receive(void *ctx) {
    struct tern_sx127x *d = ctx;
    if (!d->configured) {
        return TERN_EINVAL;
    }
    drop(d);
    int err = set_mode(d, MODE_STANDBY);
    if (err == TERN_OK) {
        err = write_register(d, REG_FIFO_ADDR_PTR, 0x00);
    }
    if (err == TERN_OK) {
        err = clear_irq(d, 0xFF);
    }
    if (err == TERN_OK) {
        err = set_mode(d, MODE_RX_CONTINUOUS);
    }
    return err;
}

static int sx_standby(void *ctx) {
    struct tern_sx127x *d = ctx;
    drop(d);
    return set_mode(d, MODE_STANDBY);
}

int tern_sx127x_sleep(struct tern_sx127x *d) {
    drop(d);
    d->configured = false;
    return set_mode(d, MODE_SLEEP);
}

/* Reads the chip's flags and what its modem is doing, and notes what the receiver got as far as,
 * whether or not a frame comes of it. The chip has no flag for a preamble, so one is taken to
 * start when the modem is seen to have found a signal it had not when last asked, and the frame to
 * be over when it is seen to have let it go with no header found. ValidHeader is cleared here, so
 * each is noted once; the flags a frame ends with are left for sx_poll(). */
static int observe(struct tern_sx127x *d, uint8_t *irq) {
    uint8_t stat;
    int err = read_register(d, REG_IRQ_FLAGS, irq);
    if (err == TERN_OK) {
        err = read_register(d, REG_MODEM_STAT, &stat);
    }
    if (err != TERN_OK) {
        return err;
    }
    tern_time now = d->bus.now(d->bus.ctx);
    bool signal = (stat & STAT_SIGNAL) != 0;
    if (signal && !d->signal) {
        d->counts.preambles++;
        if (d->configured) {
            tern_listen_preamble(&d->listen, now);
        }
    } else if (!signal && !(*irq & IRQ_VALID_HEADER)) {
        tern_listen_over(&d->listen);
    }
    d->signal = signal;
    if (*irq & IRQ_VALID_HEADER) {
        d->counts.headers++;
        if (d->configured) {
            tern_listen_header(&d->listen, &d->cfg.mod, now);
        }
        err = clear_irq(d, IRQ_VALID_HEADER);
    }
    return err;
}

/* A frame that has ended and not yet been collected is no longer being received. */
static int sx_receiving(void *ctx) {
    struct tern_sx127x *d = ctx;
    uint8_t irq;
    if (!d->configured) {
        return 0;
    }
    int err = observe(d, &irq);
    if (err != TERN_OK) {
        return err;
    }
    if (irq & IRQ_RX_DONE) {
        return 0;
    }
    return tern_listen_receiving(&d->listen, &d->cfg.mod, d->bus.now(d->bus.ctx)) ? 1 : 0;
}

/* A packet's RSSI from RegPktRssiValue and RegPktSnrValue (section 5.5.5): -157 dBm plus the raw
 * value on the HF port, -164 on the LF port; above the noise, scaled by 16/15, and below it, with
 * the SNR added, since the signal is then under what the RSSI measures. */
static int16_t packet_rssi(const struct tern_sx127x *d, uint8_t raw, int8_t snr_raw) {
    int base = d->cfg.freq_hz > LF_MAX_HZ ? -157 : -164;
    if (snr_raw < 0) {
        return (int16_t)(base + raw + snr_raw / 4);
    }
    return (int16_t)(base + raw * 16 / 15);
}

static int sx_poll(void *ctx, struct tern_radio_event *ev) {
    struct tern_sx127x *d = ctx;
    uint8_t irq;
    int err = observe(d, &irq);
    if (err != TERN_OK) {
        return err;
    }

    if (irq & IRQ_TX_DONE) {
        *ev = (struct tern_radio_event){.kind = TERN_RADIO_TX_DONE, .at = d->bus.now(d->bus.ctx)};
        err = clear_irq(d, IRQ_TX_DONE);
        return err == TERN_OK ? 1 : err;
    }

    if (irq & IRQ_RX_DONE) {
        tern_time at = d->bus.now(d->bus.ctx);
        tern_listen_over(&d->listen);
        if (irq & IRQ_CRC_ERR) {
            d->counts.crc_errors++;
            *ev = (struct tern_radio_event){.kind = TERN_RADIO_RX_ERROR, .at = at};
            err = clear_irq(d, IRQ_RX_DONE | IRQ_CRC_ERR);
            return err == TERN_OK ? 1 : err;
        }

        /* How long the frame is and where it starts, then the frame from there (section
         * 4.1.2.3), then its signal: SNR is the signed raw / 4 dB. */
        uint8_t len, start, snr, rssi;
        err = read_register(d, REG_RX_NB_BYTES, &len);
        if (err == TERN_OK) {
            err = read_register(d, REG_FIFO_RX_CURRENT, &start);
        }
        if (err == TERN_OK) {
            err = write_register(d, REG_FIFO_ADDR_PTR, start);
        }
        uint8_t tx[1 + 255] = {REG_FIFO}, got[1 + 255];
        if (err == TERN_OK) {
            err = d->bus.transfer(d->bus.ctx, tx, got, 1 + (size_t)len);
        }
        if (err == TERN_OK) {
            err = read_register(d, REG_PKT_SNR, &snr);
        }
        if (err == TERN_OK) {
            err = read_register(d, REG_PKT_RSSI, &rssi);
        }
        if (err != TERN_OK) {
            return err;
        }
        for (uint8_t i = 0; i < len; i++) {
            d->rx[i] = got[1 + i];
        }
        *ev = (struct tern_radio_event){
            .kind = TERN_RADIO_RX_DONE,
            .at = at,
            .data = d->rx,
            .len = len,
            .rssi_dbm = packet_rssi(d, rssi, (int8_t)snr),
            .snr_cdb = (int16_t)((int8_t)snr * 25),
        };
        d->counts.frames++;
        err = clear_irq(d, IRQ_RX_DONE);
        return err == TERN_OK ? 1 : err;
    }

    if (irq & IRQ_RX_TIMEOUT) {
        err = clear_irq(d, IRQ_RX_TIMEOUT);
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

struct tern_radio tern_sx127x_radio(struct tern_sx127x *d) {
    return (struct tern_radio){.ops = &sx_ops, .ctx = d};
}
