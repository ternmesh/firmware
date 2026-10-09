#include "boards.h"

#include <stddef.h>
#include <string.h>

#define CHIP_MIN_DBM (-9) /* the SX1262's high-power amplifier, its least */
#define CHIP_MAX_DBM 22   /* and its most */

/* The Heltec WiFi LoRa 32 V3, from Heltec's pin map and its schematics for the V3, V3.1 and V3.2:
 * a 1.8 V TCXO on DIO3, the antenna switch on DIO2, the screen and the header's 3.3 V on a supply
 * GPIO36 turns on when low, and the battery through 390k over 100k onto GPIO1, switched by GPIO37,
 * whose sense changed with the V3.2 (power.h). */
const struct board_def board_heltec_v3 = {
    .name = "heltec-v3",
    .title = "Heltec WiFi LoRa 32 V3",
    .lora = {.nss = 8,
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
    .max_dbm = CHIP_MAX_DBM,
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
    .lora = {.nss = 8,
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

const struct board_def *const board_defs[] = {&board_heltec_v3, &board_heltec_v4, NULL};

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

int8_t board_min_dbm(const struct board_def *b) { return (int8_t)(CHIP_MIN_DBM + gain(b)); }

int8_t board_max_dbm(const struct board_def *b) {
    int most = CHIP_MAX_DBM + gain(b);
    return (int8_t)(b->max_dbm < most ? b->max_dbm : most);
}

bool board_gives(const struct board_def *b, int dbm) {
    return dbm >= board_min_dbm(b) && dbm <= board_max_dbm(b);
}

int8_t board_chip_dbm(const struct board_def *b, int8_t antenna_dbm) {
    int chip = antenna_dbm - gain(b);
    if (chip < CHIP_MIN_DBM) {
        chip = CHIP_MIN_DBM;
    }
    if (chip > CHIP_MAX_DBM) {
        chip = CHIP_MAX_DBM;
    }
    return (int8_t)chip;
}
