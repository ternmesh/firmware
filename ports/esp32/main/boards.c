#include "boards.h"

#include <stddef.h>
#include <string.h>

#define SX1262_MIN_DBM (-9) /* the SX1262's high-power amplifier, its least */
#define SX1262_MAX_DBM 22   /* and its most */
#define PA_BOOST_MIN_DBM 2  /* an SX127x's PA_BOOST (tern/sx127x.h) */
#define PA_BOOST_MAX_DBM 17
#define RFO_MAX_DBM 14 /* and its RFO, from 0 */

/* The Heltec WiFi LoRa 32 V3, from Heltec's pin map and its schematics for the V3, V3.1 and V3.2:
 * a 1.8 V TCXO on DIO3, the antenna switch on DIO2, the screen and the header's 3.3 V on a supply
 * GPIO36 turns on when low, and the battery through 390k over 100k onto GPIO1, switched by GPIO37,
 * whose sense changed with the V3.2 (power.h). */
const struct board_def board_heltec_v3 = {
    .name = "heltec-v3",
    .title = "Heltec WiFi LoRa 32 V3",
    .soc = BOARD_ESP32S3,
    .lora = {.chip = BOARD_SX1262,
             .nss = 8,
             .sck = 9,
             .mosi = 10,
             .miso = 11,
             .reset = 12,
             .busy = 13,
             .tcxo_mv = 1800,
             .dio2_rf_switch = true},
    .button = 0,
    .led = 35,
    .screen = {.sda = 17, .scl = 18, .reset = 21},
    .vext = 36,
    .vext_high_on = false,
    .battery = {.sense = 1, .enable = 37, .top_k = 390, .bottom_k = 100},
    .amp = {.power = BOARD_NO_PIN, .enable = BOARD_NO_PIN, .tx = {BOARD_NO_PIN, BOARD_NO_PIN}},
    .max_dbm = SX1262_MAX_DBM,
};

/* The Heltec WiFi LoRa 32 V4, from Heltec's datasheet (V4.3.1) and its schematics for the V4.2 and
 * the V4.3. Pin for pin a V3 (the radio, the screen, Vext, the LED, PRG and the battery), with an
 * SSD1315 screen, which takes the SSD1306's commands, and USB from the ESP32-S3's own port rather
 * than a CP2102, so its console is USB Serial/JTAG (boards/heltec-v4.defaults).
 *
 * After the SX1262 comes a 17 dB attenuator and a front-end amplifier, rated together at 28 dBm.
 * The V4.2's is a GC1109 and the V4.3's a KCT8103L, and their three control lines are wired
 * differently, though both are powered from an LDO GPIO7 enables (pulled up, so on until told) and
 * both are enabled by GPIO2:
 *
 *            CSD (enable)   CTX (transmit)   CPS (amplifier on)
 *   V4.2     GPIO2          DIO2             GPIO46
 *   V4.3     GPIO2          GPIO5            DIO2
 *
 * Either way, the line the ESP32 drives besides GPIO2 is high to send and low to receive, and on
 * each revision the other one goes only to the header. So one image raises both GPIO5 and GPIO46
 * while it sends, and runs on either.
 *
 * The GC1109's datasheet (GC1109_EN_V0.9.2, which Heltec publishes beside the V4's) gives it 30 dB
 * of small-signal gain, so with the V4.2's 17 dB attenuator the radio's power gains up to 13 dB on
 * the way to the antenna, less as the amplifier nears its limit: Heltec rates the board at 28 dBm
 * with the SX1262 at its 22. The KCT8103L's datasheet is not published, and the V4.3's attenuator
 * is about 21 dB (its 59, 280 and 59 ohms), so 13 dB holds for it unless its gain passes 34 dB. 13
 * dB is taken, and a power asked for is reached or undershot, never exceeded: the least the V4
 * gives is +4 dBm. Until a meter says better, the board's figures for its power are that bound. */
const struct board_def board_heltec_v4 = {
    .name = "heltec-v4",
    .title = "Heltec WiFi LoRa 32 V4",
    .soc = BOARD_ESP32S3,
    .lora = {.chip = BOARD_SX1262,
             .nss = 8,
             .sck = 9,
             .mosi = 10,
             .miso = 11,
             .reset = 12,
             .busy = 13,
             .tcxo_mv = 1800,
             .dio2_rf_switch = true},
    .button = 0,
    .led = 35,
    .screen = {.sda = 17, .scl = 18, .reset = 21},
    .vext = 36,
    .vext_high_on = false, /* a P-channel FET held off by a pull-up (the schematic's Q2) */
    .battery = {.sense = 1, .enable = 37, .top_k = 390, .bottom_k = 100},
    .amp = {.power = 7, .enable = 2, .tx = {5, 46}, .gain_db = 13},
    .max_dbm = 28,
};

/* Heltec's other ESP32-S3 boards with an SX1262 are wired as the V3 is where they share a part, and
 * each wires the SX1262 the same way: NSS 8, SCK 9, MOSI 10, MISO 11, RESET 12, BUSY 13, a 32 MHz
 * TCXO powered from DIO3 and a UPG2179 antenna switch on DIO2, with no amplifier. Their schematics
 * do not give the TCXO's voltage; the V3's 1.8 V is taken, and a board whose radio does not start
 * says so (docs/boards.md, bringing a board up). None of their screens is an SSD1306, so each runs
 * without one until the port has a driver for it. */
#define HELTEC_SX1262                                                                              \
    {                                                                                              \
        .chip = BOARD_SX1262, .nss = 8, .sck = 9, .mosi = 10, .miso = 11, .reset = 12, .busy = 13, \
        .tcxo_mv = 1800, .dio2_rf_switch = true                                                    \
    }
#define NO_AMP                                                                                     \
    {                                                                                              \
        .power = BOARD_NO_PIN, .enable = BOARD_NO_PIN, .tx = { BOARD_NO_PIN, BOARD_NO_PIN }        \
    }
#define NO_SCREEN                                                                                  \
    { .sda = BOARD_NO_PIN, .scl = BOARD_NO_PIN, .reset = BOARD_NO_PIN }

/* The Heltec Wireless Stick Lite V3, from its schematic (HTIT-WSL_V3_Schematic_Diagram), datasheet
 * (HTIT-WSL_V3 Rev1.1) and pin map: a V3 without its screen. PRG on GPIO0, the LED on GPIO35, and
 * the battery through 390k over 100k onto GPIO1 behind a P-channel switch GPIO37 turns on when low.
 * USB through a CP2102, so its console is UART0. Rated 21 dBm, give or take a decibel. */
const struct board_def board_heltec_wsl_v3 = {
    .name = "heltec-wsl-v3",
    .title = "Heltec Wireless Stick Lite V3",
    .soc = BOARD_ESP32S3,
    .lora = HELTEC_SX1262,
    .button = 0,
    .led = 35,
    .screen = NO_SCREEN,
    .vext = BOARD_NO_PIN, /* GPIO36, which only the header's 3.3 V is on */
    .vext_high_on = false,
    .battery = {.sense = 1, .enable = 37, .top_k = 390, .bottom_k = 100},
    .amp = NO_AMP,
    .max_dbm = SX1262_MAX_DBM,
};

/* The Heltec Wireless Tracker V1.1, from its datasheet (Wireless Tracker1.1), schematic
 * (HTIT-Tracker_V0.5), pin map and hardware update log. PRG on GPIO0, the LED on GPIO18, and the
 * battery through 390k over 100k onto GPIO1, behind a switch GPIO2 turns on when high. Its 80x160
 * colour TFT and its GNSS are on a supply GPIO3 turns on (high on the V1.1, low on the V1.0), left
 * off. USB from the ESP32-S3's own port (boards/heltec-tracker.defaults). Rated 21 dBm, give or
 * take a decibel. The V1.0's radio, button, LED and battery are on the same pins
 * (HTIT-Tracker_V0.3), so the image runs on it too. */
const struct board_def board_heltec_tracker = {
    .name = "heltec-tracker",
    .title = "Heltec Wireless Tracker",
    .soc = BOARD_ESP32S3,
    .lora = HELTEC_SX1262,
    .button = 0,
    .led = 18,
    .screen = NO_SCREEN,
    .vext = BOARD_NO_PIN,
    .vext_high_on = true,
    .battery = {.sense = 1, .enable = 2, .top_k = 390, .bottom_k = 100},
    .amp = NO_AMP,
    .max_dbm = SX1262_MAX_DBM,
};

/* The Heltec Vision Master E290 and E213, from their schematics (HT-VME290, HT-VME213) and
 * datasheets, and the HT-RA62 module's, which carries their SX1262. The two are wired alike but
 * for their e-paper panels (128x296 and 122x250), which this port does not drive yet. The user
 * button on GPIO21 (BOOT, GPIO0, is the other), the LED on GPIO45, and the battery through 390k
 * over 100k onto GPIO7, behind a switch GPIO46 turns on when high. The E213's datasheet names
 * GPIO17 for that switch, but both its schematics wire GPIO46, as the E290's do; the schematics are
 * taken, and the switch's sense is learnt either way (power.h). USB from the ESP32-S3's own port.
 * Rated 21 dBm, give or take a decibel. */
const struct board_def board_heltec_vme290 = {
    .name = "heltec-vme290",
    .title = "Heltec Vision Master E290",
    .soc = BOARD_ESP32S3,
    .lora = HELTEC_SX1262,
    .button = 21,
    .led = 45,
    .screen = NO_SCREEN,
    .vext = BOARD_NO_PIN, /* GPIO18, the panel's supply, high on */
    .vext_high_on = true,
    .battery = {.sense = 7, .enable = 46, .top_k = 390, .bottom_k = 100},
    .amp = NO_AMP,
    .max_dbm = SX1262_MAX_DBM,
};

const struct board_def board_heltec_vme213 = {
    .name = "heltec-vme213",
    .title = "Heltec Vision Master E213",
    .soc = BOARD_ESP32S3,
    .lora = HELTEC_SX1262,
    .button = 21,
    .led = 45,
    .screen = NO_SCREEN,
    .vext = BOARD_NO_PIN, /* GPIO18, the panel's supply, high on */
    .vext_high_on = true,
    .battery = {.sense = 7, .enable = 46, .top_k = 390, .bottom_k = 100},
    .amp = NO_AMP,
    .max_dbm = SX1262_MAX_DBM,
};

/* The Heltec Wireless Paper, from its schematic (Wireless_Paper_V0.4) and datasheet (Rev1.0); its
 * update log changes only the panel after it. A button on GPIO0, the LED on GPIO18, and the battery
 * through 10k over 10k onto GPIO20, behind a P-channel switch GPIO19 turns on when low: the even
 * divider puts a full cell at 2.1 V, which board.c reads in the ADC's widest range. Its 2.13-inch
 * e-paper is not driven yet. USB through a CP2102, so its console is UART0. Heltec rates no
 * power at the antenna, so the chip's most is taken. */
const struct board_def board_heltec_paper = {
    .name = "heltec-paper",
    .title = "Heltec Wireless Paper",
    .soc = BOARD_ESP32S3,
    .lora = HELTEC_SX1262,
    .button = 0,
    .led = 18,
    .screen = NO_SCREEN,
    .vext = BOARD_NO_PIN, /* GPIO45, the panel's supply, low on */
    .vext_high_on = false,
    .battery = {.sense = 20, .enable = 19, .top_k = 10, .bottom_k = 10},
    .amp = NO_AMP,
    .max_dbm = SX1262_MAX_DBM,
};

const struct board_def *const board_defs[] = {
    &board_heltec_v3,     &board_heltec_v4,     &board_heltec_wsl_v3, &board_heltec_tracker,
    &board_heltec_vme290, &board_heltec_vme213, &board_heltec_paper,  NULL};

const struct board_def *board_def_named(const char *name) {
    for (size_t i = 0; name != NULL && board_defs[i] != NULL; i++) {
        if (strcmp(board_defs[i]->name, name) == 0) {
            return board_defs[i];
        }
    }
    return NULL;
}

static int8_t gain(const struct board_def *b) {
    return b->amp.power == BOARD_NO_PIN ? 0 : b->amp.gain_db;
}

int8_t board_chip_min_dbm(const struct board_def *b) {
    if (b->lora.chip == BOARD_SX1262) {
        return SX1262_MIN_DBM;
    }
    return b->lora.pa_boost ? PA_BOOST_MIN_DBM : 0;
}

int8_t board_chip_max_dbm(const struct board_def *b) {
    if (b->lora.chip == BOARD_SX1262) {
        return SX1262_MAX_DBM;
    }
    return b->lora.pa_boost ? PA_BOOST_MAX_DBM : RFO_MAX_DBM;
}

int8_t board_min_dbm(const struct board_def *b) {
    return (int8_t)(board_chip_min_dbm(b) + gain(b));
}

int8_t board_max_dbm(const struct board_def *b) {
    int most = board_chip_max_dbm(b) + gain(b);
    return (int8_t)(b->max_dbm < most ? b->max_dbm : most);
}

uint16_t board_battery_pin_mv(const struct board_def *b) {
    if (b->battery.sense == BOARD_NO_PIN) {
        return 0;
    }
    unsigned total = (unsigned)b->battery.top_k + b->battery.bottom_k;
    return (uint16_t)(BOARD_CELL_FULL_MV * (unsigned)b->battery.bottom_k / total);
}

bool board_gives(const struct board_def *b, int dbm) {
    return dbm >= board_min_dbm(b) && dbm <= board_max_dbm(b);
}

int8_t board_chip_dbm(const struct board_def *b, int8_t antenna_dbm) {
    int chip = antenna_dbm - gain(b);
    if (chip < board_chip_min_dbm(b)) {
        chip = board_chip_min_dbm(b);
    }
    if (chip > board_chip_max_dbm(b)) {
        chip = board_chip_max_dbm(b);
    }
    return (int8_t)chip;
}
