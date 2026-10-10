#ifndef ESP32_BOARDS_H
#define ESP32_BOARDS_H

#include <stdbool.h>
#include <stdint.h>

#include "axp.h"

/* What makes one ESP32 board different from another: which ESP32 it is, which radio, its pins, how
 * the radio is wired, and what else it has fitted. board.c drives whichever one the build chose
 * (Kconfig, "Board"); everything above board.h is the same on every board.
 *
 * Each is read off its maker's published datasheet and schematic, cited beside it in boards.c,
 * never from another mesh project's source (CONTRIBUTING.md). Nothing here touches the hardware,
 * so tests/boards.c checks the table on a host. */

#define BOARD_NO_PIN (-1)

/* A rail of a board's power management chip, at a voltage, or off at 0. */
struct board_rail {
    enum axp_rail rail;
    uint16_t mv;
};

/* The ESP32 on the board, which the build is for: CONFIG_IDF_TARGET in boards/<name>.defaults,
 * esp32s3 where it does not say (sdkconfig.defaults). */
enum board_soc {
    BOARD_ESP32S3,
    BOARD_ESP32,
};

/* The LoRa radio, which board.c drives with tern/sx126x.h or tern/sx127x.h. */
enum board_chip {
    BOARD_SX1262,
    BOARD_SX1276, /* 137 to 1020 MHz */
    BOARD_SX1278, /* 137 to 525 MHz */
};

struct board_def {
    /* The name a client finds an image by (tern-<name>-<region>-<version>.bin), and the one it is
     * told over the companion link. Lower case, letters, digits and hyphens. */
    const char *name;
    const char *title; /* as the maker sells it */
    enum board_soc soc;

    struct {
        enum board_chip chip;
        int8_t nss, sck, mosi, miso, reset;
        int8_t busy; /* the SX1262's; an SX127x has none, BOARD_NO_PIN */
        /* The SX1262's TCXO's supply from DIO3, or 0 for a crystal. On an SX127x, which does not
         * power its TCXO, any but 0 says it has one. */
        uint16_t tcxo_mv;
        bool dio2_rf_switch; /* the SX1262's DIO2 drives the antenna switch, or the amplifier's */
        bool pa_boost;       /* the SX127x's antenna is on PA_BOOST rather than RFO */
    } lora;

    /* Low while it is pressed; it also wakes the board, so an RTC pin. BOARD_NO_PIN for a board
     * with none but RESET, which board_off() then leaves to wake it. */
    int8_t button;
    int8_t led;      /* BOARD_NO_PIN for none */
    bool led_low_on; /* lit when low, rather than high */

    /* A 128x64 SSD1306 or a controller that takes its commands (the SSD1315), on I2C. */
    struct {
        int8_t sda, scl, reset; /* sda BOARD_NO_PIN for none */
    } screen;

    /* The switched supply the screen is on, and whether high turns it on. Where it supplies more
     * than the screen (an antenna switch, the battery's divider), it is on whenever the board is,
     * with a screen or without: vext_always. */
    int8_t vext;
    bool vext_high_on;
    bool vext_always;

    /* The battery, through a divider onto an ADC pin, behind a switch on `enable` whose sense the
     * board learns (power.h). */
    struct {
        int8_t sense, enable; /* sense BOARD_NO_PIN for none; enable BOARD_NO_PIN if always on */
        uint16_t top_k, bottom_k;
    } battery;

    /* A front-end amplifier after the radio, or none (power BOARD_NO_PIN). `power` and `enable`
     * are raised once at start and lowered when the board turns off; every pin in `tx` is raised
     * while a frame is sent and lowered otherwise. `gain_db` is what the amplifier and whatever
     * stands before it add to the chip's power, taken as high as it might be, so that a power
     * asked for is never exceeded at the antenna (board_chip_dbm()). */
    struct {
        int8_t power, enable;
        int8_t tx[2];
        int8_t gain_db;
    } amp;

    /* A power management chip (axp.h) on I2C, or none (chip AXP_NONE). Its rails are set as they
     * are listed when the board starts, the radio's among them, and each turned off when the
     * board turns off; the one the ESP32 is on is not listed. Its battery is the board's, rather
     * than one on an ADC pin. The screen may share its bus. */
    struct {
        enum axp_chip chip;
        int8_t sda, scl;
        struct board_rail rails[3];
    } pmu;

    int8_t max_dbm; /* the most this board puts into its antenna, as its maker rates it */
};

extern const struct board_def board_heltec_v3;
extern const struct board_def board_heltec_v4;
extern const struct board_def board_heltec_wsl_v3;
extern const struct board_def board_heltec_tracker;
extern const struct board_def board_heltec_vme290;
extern const struct board_def board_heltec_vme213;
extern const struct board_def board_heltec_paper;
extern const struct board_def board_heltec_v2;
extern const struct board_def board_heltec_v21;
extern const struct board_def board_lilygo_t3_v161;
extern const struct board_def board_lilygo_tbeam;
extern const struct board_def board_lilygo_tbeam12;

/* Every board this port knows, ending with NULL. */
extern const struct board_def *const board_defs[];

/* The board with that name, or NULL. */
const struct board_def *board_def_named(const char *name);

/* The least and the most power, in dBm into the antenna, that this board gives. */
int8_t board_min_dbm(const struct board_def *b);
int8_t board_max_dbm(const struct board_def *b);

/* What a full cell, 4.2 V, puts on the battery's ADC pin through the board's divider, in
 * millivolts, or 0 for no battery: board.c chooses the ADC's range by it. */
#define BOARD_CELL_FULL_MV 4200
uint16_t board_battery_pin_mv(const struct board_def *b);

/* Whether the board gives dbm into its antenna. */
bool board_gives(const struct board_def *b, int dbm);

/* The least and the most the board's radio gives, in dBm at its pins: -9 to 22 for the SX1262, 2
 * to 17 on an SX127x's PA_BOOST and 0 to 14 on its RFO (tern/sx127x.h). */
int8_t board_chip_min_dbm(const struct board_def *b);
int8_t board_chip_max_dbm(const struct board_def *b);

/* What to ask the radio for, within its range, so that the antenna gets no more than antenna_dbm,
 * for a power the board gives (board_gives()). */
int8_t board_chip_dbm(const struct board_def *b, int8_t antenna_dbm);

#endif
