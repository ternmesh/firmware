#include "tern/lr11xx.h"

#include "tern/err.h"

/* Opcodes from the user manual's command tables, two bytes, most significant first. */
enum {
    CMD_GET_VERSION = 0x0101,
    CMD_WRITE_BUFFER8 = 0x0109,
    CMD_READ_BUFFER8 = 0x010A,
    CMD_CLEAR_ERRORS = 0x010E,
    CMD_CALIBRATE = 0x010F,
    CMD_SET_REG_MODE = 0x0110,
    CMD_CALIB_IMAGE = 0x0111,
    CMD_SET_DIO_AS_RF_SWITCH = 0x0112,
    CMD_SET_DIO_IRQ_PARAMS = 0x0113,
    CMD_CLEAR_IRQ = 0x0114,
    CMD_SET_TCXO_MODE = 0x0117,
    CMD_SET_SLEEP = 0x011B,
    CMD_SET_STANDBY = 0x011C,
    CMD_GET_RX_BUFFER_STATUS = 0x0203,
    CMD_GET_PACKET_STATUS = 0x0204,
    CMD_SET_LORA_PUBLIC_NETWORK = 0x0208,
    CMD_SET_RX = 0x0209,
    CMD_SET_TX = 0x020A,
    CMD_SET_RF_FREQUENCY = 0x020B,
    CMD_SET_PACKET_TYPE = 0x020E,
    CMD_SET_MODULATION_PARAMS = 0x020F,
    CMD_SET_PACKET_PARAMS = 0x0210,
    CMD_SET_TX_PARAMS = 0x0211,
    CMD_SET_PA_CONFIG = 0x0215,
    CMD_SET_LORA_SYNC_WORD = 0x022B,
};

/* The IRQ flags (page 36), 32 bits, as GetStatus gives them. */
enum {
    IRQ_TX_DONE = 1u << 2,
    IRQ_RX_DONE = 1u << 3,
    IRQ_PREAMBLE = 1u << 4,
    IRQ_HEADER_VALID = 1u << 5,
    IRQ_HEADER_ERR = 1u << 6,
    IRQ_CRC_ERR = 1u << 7,
    IRQ_TIMEOUT = 1u << 10,
};

#define IRQ_COUNTED (IRQ_PREAMBLE | IRQ_HEADER_VALID)
#define IRQ_USED                                                                                   \
    (IRQ_TX_DONE | IRQ_RX_DONE | IRQ_HEADER_ERR | IRQ_CRC_ERR | IRQ_TIMEOUT | IRQ_COUNTED)
#define IRQ_ALL 0xFFFFFFFFu

/* GetVersion's use case (page 21): which chip, or its bootloader. */
#define USE_CASE_LR1110 0x01
#define USE_CASE_LR1120 0x02
#define USE_CASE_LR1121 0x03
/* The LR1110 takes SetLoRaSyncWord from this firmware on (SWDR001, lr11xx_radio.h); before it,
 * only the public and private words, by SetLoRaPublicNetwork. */
#define LR1110_SYNC_WORD_FW 0x0303
#define SYNC_PRIVATE 0x12
#define SYNC_PUBLIC 0x34

#define STANDBY_RC 0x00
#define PACKET_TYPE_LORA 0x02
#define RX_CONTINUOUS 0xFFFFFFu
#define CALIBRATE_ALL 0x3F /* LF RC, HF RC, PLL, ADC, image and PLL TX (page 16) */
/* 5 ms for the TCXO to settle, in steps of 30.52 us (page 47): 164. */
#define TCXO_DELAY 164u
#define RAMP_48_US 0x02 /* the ramp time the manual recommends (page 99) */
#define LP_MIN_DBM (-17)
#define LP_MAX_DBM 14
#define HP_MAX_DBM 22
#define MIN_FREQ_HZ 150000000u /* the sub-GHz path */
#define MAX_FREQ_HZ 960000000u

static int cmd(struct tern_lr11xx *d, const uint8_t *tx, size_t len) {
    return d->bus.transfer(d->bus.ctx, tx, NULL, len);
}

#define OP(op) (uint8_t)((op) >> 8), (uint8_t)(op)
#define CMD(d, op, ...)                                                                            \
    cmd((d), (const uint8_t[]){OP(op), __VA_ARGS__}, sizeof((const uint8_t[]){OP(op), __VA_ARGS__}))
#define CMD0(d, op) cmd((d), (const uint8_t[]){OP(op)}, 2)
#define BE32(v) (uint8_t)((v) >> 24), (uint8_t)((v) >> 16), (uint8_t)((v) >> 8), (uint8_t)(v)
#define BE24(v) (uint8_t)((v) >> 16), (uint8_t)((v) >> 8), (uint8_t)(v)

/* A command that answers: the command, then n bytes of answer, read after the status byte that
 * comes first (page 25). Only zeros go out while it is read, lest the chip take them for a
 * command. */
static int ask(struct tern_lr11xx *d, const uint8_t *tx, size_t len, uint8_t *answer, size_t n) {
    uint8_t zeros[1 + 255] = {0}, got[1 + 255];
    int err = cmd(d, tx, len);
    if (err == TERN_OK) {
        err = d->bus.transfer(d->bus.ctx, zeros, got, 1 + n);
    }
    for (size_t i = 0; err == TERN_OK && i < n; i++) {
        answer[i] = got[1 + i];
    }
    return err;
}

static int clear_irq(struct tern_lr11xx *d, uint32_t mask) {
    return CMD(d, CMD_CLEAR_IRQ, BE32(mask));
}

/* The TCXO supply voltages the chip can give, in the order of their codes (page 47). */
static const uint16_t tcxo_mv[] = {1600, 1700, 1800, 2200, 2400, 2700, 3000, 3300};

int tern_lr11xx_init(struct tern_lr11xx *d, const struct tern_lr11xx_bus *bus,
                     const struct tern_lr11xx_board *board) {
    d->bus = *bus;
    d->board = *board;
    d->configured = false;
    d->counts = (struct tern_lr11xx_counts){0};
    tern_listen_init(&d->listen);

    uint8_t tcxo = 0xFF;
    for (uint8_t i = 0; i < sizeof tcxo_mv / sizeof tcxo_mv[0]; i++) {
        if (tcxo_mv[i] == board->tcxo_mv) {
            tcxo = i;
        }
    }
    if (board->tcxo_mv != 0 && tcxo == 0xFF) {
        return TERN_EINVAL;
    }

    /* Hardware version, use case, firmware major and minor. */
    uint8_t v[4];
    int err = ask(d, (const uint8_t[]){OP(CMD_GET_VERSION)}, 2, v, sizeof v);
    if (err != TERN_OK) {
        return err;
    }
    if (v[1] != USE_CASE_LR1110 && v[1] != USE_CASE_LR1120 && v[1] != USE_CASE_LR1121) {
        return TERN_EIO;
    }
    d->fw = (uint16_t)(v[1] == USE_CASE_LR1110 ? (v[2] << 8 | v[3]) : 0xFFFF);

    /* Each of these is taken only in standby on the RC oscillator, where the chip starts. */
    err = CMD(d, CMD_SET_STANDBY, STANDBY_RC);
    if (err == TERN_OK && board->dcdc) {
        err = CMD(d, CMD_SET_REG_MODE, 0x01);
    }
    if (err == TERN_OK) {
        const struct tern_lr11xx_rf_switch *s = &board->rf_switch;
        /* The LR1110 reads two more bytes, for its GNSS and Wi-Fi scanners, which stay 0. */
        err = CMD(d, CMD_SET_DIO_AS_RF_SWITCH, s->enable, s->standby, s->rx, s->tx, s->tx_hp, 0x00,
                  0x00, 0x00);
    }
    if (err == TERN_OK && board->tcxo_mv != 0) {
        /* The chip calibrated itself at power-on without its clock, which fails, so the errors are
         * cleared and it calibrates again now that it has one (page 47). */
        err = CMD(d, CMD_SET_TCXO_MODE, tcxo, BE24(TCXO_DELAY));
        if (err == TERN_OK) {
            err = CMD0(d, CMD_CLEAR_ERRORS);
        }
        if (err == TERN_OK) {
            err = CMD(d, CMD_CALIBRATE, CALIBRATE_ALL);
        }
    }
    if (err == TERN_OK) {
        err = CMD(d, CMD_SET_PACKET_TYPE, PACKET_TYPE_LORA);
    }
    if (err == TERN_OK) {
        err = CMD(d, CMD_SET_DIO_IRQ_PARAMS, BE32((uint32_t)IRQ_USED), BE32(0u));
    }
    if (err == TERN_OK) {
        err = clear_irq(d, IRQ_ALL);
    }
    return err;
}

/* The bandwidth codes of SetModulationParams (page 67). */
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

/* The two ends of the band CalibImage should cover, in units of 4 MHz (page 15): the manual's
 * pairs for the usual bands, else a step either side of the frequency. */
static void image_band(uint32_t freq_hz, uint8_t band[2]) {
    static const struct {
        uint32_t lo, hi;
        uint8_t f1, f2;
    } bands[] = {
        {430000000, 440000000, 0x6B, 0x6E}, {470000000, 510000000, 0x75, 0x81},
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

static int packet_params(struct tern_lr11xx *d, uint8_t len) {
    const struct tern_lora *m = &d->cfg.mod;
    return CMD(d, CMD_SET_PACKET_PARAMS, (uint8_t)(m->preamble >> 8), (uint8_t)m->preamble,
               m->implicit_header ? 0x01 : 0x00, len, m->crc ? 0x01 : 0x00, 0x00);
}

/* The LR1110 before firmware 0x0303 has only the two words LoRaWAN uses. */
static int sync_word(struct tern_lr11xx *d, uint8_t word) {
    if (d->fw >= LR1110_SYNC_WORD_FW) {
        return CMD(d, CMD_SET_LORA_SYNC_WORD, word);
    }
    if (word != SYNC_PRIVATE && word != SYNC_PUBLIC) {
        return TERN_EINVAL;
    }
    return CMD(d, CMD_SET_LORA_PUBLIC_NETWORK, word == SYNC_PUBLIC ? 0x01 : 0x00);
}

static int lr_configure(void *ctx, const struct tern_radio_config *cfg) {
    struct tern_lr11xx *d = ctx;
    const struct tern_lora *m = &cfg->mod;
    int8_t most = d->board.hp_pa ? HP_MAX_DBM : LP_MAX_DBM;
    uint8_t bw, band[2];
    /* Implicit headers are not supported, as for the SX126x (src/sx126x.c). */
    if (bw_code(m->bw_hz, &bw) != TERN_OK || cfg->tx_power_dbm < LP_MIN_DBM ||
        cfg->tx_power_dbm > most || cfg->freq_hz < MIN_FREQ_HZ || cfg->freq_hz > MAX_FREQ_HZ ||
        m->implicit_header) {
        return TERN_EINVAL;
    }
    if (d->fw < LR1110_SYNC_WORD_FW && cfg->sync_word != SYNC_PRIVATE &&
        cfg->sync_word != SYNC_PUBLIC) {
        return TERN_EINVAL;
    }
    d->cfg = *cfg;
    d->configured = false;
    tern_listen_over(&d->listen); /* it is left in standby */
    image_band(cfg->freq_hz, band);

    /* The low-power amplifier to +14 dBm, from the chip's regulator, at the duty cycle that
     * gives +14 dBm at a power of 14; above it the high-power one, from the battery, set up for its
     * full +22 dBm (page 88 and 89), so that the power SetTxParams asks for is the power it
     * gives. */
    bool hp = cfg->tx_power_dbm > LP_MAX_DBM;

    /* The order the manual gives (page 62): the packet type, then its modulation and packet, then
     * the amplifier and the power. */
    int err = CMD(d, CMD_SET_STANDBY, STANDBY_RC);
    if (err == TERN_OK) {
        err = CMD(d, CMD_SET_PACKET_TYPE, PACKET_TYPE_LORA);
    }
    if (err == TERN_OK) {
        err = CMD(d, CMD_SET_RF_FREQUENCY, BE32(cfg->freq_hz));
    }
    if (err == TERN_OK) {
        err = CMD(d, CMD_CALIB_IMAGE, band[0], band[1]);
    }
    if (err == TERN_OK) {
        err = CMD(d, CMD_SET_MODULATION_PARAMS, m->sf, bw, m->cr, tern_lora_ldro(m) ? 0x01 : 0x00);
    }
    if (err == TERN_OK) {
        err = packet_params(d, 255);
    }
    if (err == TERN_OK) {
        err = hp ? CMD(d, CMD_SET_PA_CONFIG, 0x01, 0x01, 0x04, 0x07)
                 : CMD(d, CMD_SET_PA_CONFIG, 0x00, 0x00, 0x04, 0x00);
    }
    if (err == TERN_OK) {
        err = CMD(d, CMD_SET_TX_PARAMS, (uint8_t)cfg->tx_power_dbm, RAMP_48_US);
    }
    if (err == TERN_OK) {
        err = sync_word(d, cfg->sync_word);
    }
    d->configured = err == TERN_OK;
    return err;
}

static int lr_transmit(void *ctx, const uint8_t *frame, uint8_t len) {
    struct tern_lr11xx *d = ctx;
    if (!d->configured) {
        return TERN_EINVAL;
    }
    uint8_t buf[2 + 255] = {OP(CMD_WRITE_BUFFER8)};
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
        err = clear_irq(d, IRQ_ALL);
    }
    if (err == TERN_OK) {
        err = CMD(d, CMD_SET_TX, BE24(0u)); /* no timeout */
    }
    return err;
}

static int lr_receive(void *ctx) {
    struct tern_lr11xx *d = ctx;
    if (!d->configured) {
        return TERN_EINVAL;
    }
    tern_listen_over(&d->listen);
    int err = CMD(d, CMD_SET_STANDBY, STANDBY_RC);
    if (err == TERN_OK) {
        err = packet_params(d, 255);
    }
    if (err == TERN_OK) {
        err = clear_irq(d, IRQ_ALL);
    }
    if (err == TERN_OK) {
        err = CMD(d, CMD_SET_RX, BE24(RX_CONTINUOUS));
    }
    return err;
}

static int lr_standby(void *ctx) {
    struct tern_lr11xx *d = ctx;
    tern_listen_over(&d->listen);
    return CMD(d, CMD_SET_STANDBY, STANDBY_RC);
}

/* A cold sleep, keeping nothing and with no timer to wake on: the least the chip draws (page 17).
 * Since nothing is kept, the LR1110's erratum on waking with retention does not arise. */
int tern_lr11xx_sleep(struct tern_lr11xx *d) {
    int err = lr_standby(d);
    if (err == TERN_OK) {
        err = CMD(d, CMD_SET_SLEEP, 0x00, BE32(0u));
    }
    d->configured = false;
    return err;
}

/* Asks the chip what has happened: GetStatus as a read of six bytes with no command, the two
 * status bytes, then the IRQ flags, most significant first (page 27). Then notes what the
 * receiver got as far as, as the SX126x driver does, clearing those two flags. */
static int read_irq(struct tern_lr11xx *d, uint32_t *irq) {
    uint8_t zeros[6] = {0}, rx[6];
    int err = d->bus.transfer(d->bus.ctx, zeros, rx, sizeof zeros);
    if (err != TERN_OK) {
        return err;
    }
    *irq = (uint32_t)rx[2] << 24 | (uint32_t)rx[3] << 16 | (uint32_t)rx[4] << 8 | rx[5];
    if (*irq & IRQ_COUNTED) {
        tern_time now = d->bus.now(d->bus.ctx);
        if (*irq & IRQ_PREAMBLE) {
            d->counts.preambles++;
            if (d->configured) {
                tern_listen_preamble(&d->listen, now);
            }
        }
        if (*irq & IRQ_HEADER_VALID) {
            d->counts.headers++;
            if (d->configured) {
                tern_listen_header(&d->listen, &d->cfg.mod, now);
            }
        }
        err = clear_irq(d, *irq & IRQ_COUNTED);
    }
    return err;
}

static int lr_receiving(void *ctx) {
    struct tern_lr11xx *d = ctx;
    uint32_t irq;
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

static int lr_poll(void *ctx, struct tern_radio_event *ev) {
    struct tern_lr11xx *d = ctx;
    uint32_t irq;
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

        /* How long the frame is and where (page 57), then the frame (page 33), then its signal
         * (page 70): RSSI is -raw / 2 dBm, SNR the signed raw / 4 dB. */
        uint8_t where[2], signal[3];
        err = ask(d, (const uint8_t[]){OP(CMD_GET_RX_BUFFER_STATUS)}, 2, where, sizeof where);
        if (err != TERN_OK) {
            return err;
        }
        uint8_t len = where[0];
        err = ask(d, (const uint8_t[]){OP(CMD_READ_BUFFER8), where[1], len}, 4, d->rx, len);
        if (err == TERN_OK) {
            err = ask(d, (const uint8_t[]){OP(CMD_GET_PACKET_STATUS)}, 2, signal, sizeof signal);
        }
        if (err != TERN_OK) {
            return err;
        }

        *ev = (struct tern_radio_event){
            .kind = TERN_RADIO_RX_DONE,
            .at = at,
            .data = d->rx,
            .len = len,
            .rssi_dbm = (int16_t)(-(int16_t)signal[0] / 2),
            .snr_cdb = (int16_t)((int8_t)signal[1] * 25),
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

static const struct tern_radio_ops lr_ops = {
    .configure = lr_configure,
    .transmit = lr_transmit,
    .receive = lr_receive,
    .standby = lr_standby,
    .poll = lr_poll,
    .receiving = lr_receiving,
};

struct tern_radio tern_lr11xx_radio(struct tern_lr11xx *d) {
    return (struct tern_radio){.ops = &lr_ops, .ctx = d};
}
