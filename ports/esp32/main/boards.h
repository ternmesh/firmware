#ifndef ESP32_BOARDS_H
#define ESP32_BOARDS_H

#include <stdbool.h>
#include <stdint.h>

/* What makes one ESP32 board with an SX1262 different from another: its pins, how its radio is
 * wired, and what else it has fitted. board.c drives whichever one the build chose (Kconfig,
 * "Board"); everything above board.h is the same on every board.
 *
 * Each is read off its maker's published datasheet and schematic, cited beside it in boards.c,
 * never from another mesh project's source (CONTRIBUTING.md). Nothing here touches the hardware,
 * so tests/boards.c checks the table on a host. */

#define BOARD_NO_PIN (-1)

struct board_def {
    /* The name a client finds an image by (tern-<name>-<region>-<version>.bin), and the one it is
     * told over the companion link. Lower case, letters, digits and hyphens. */
    const char *name;
    const char *title; /* as the maker sells it */

    struct {
        int8_t nss, sck, mosi, miso, reset, busy;
        uint16_t tcxo_mv;    /* the TCXO's supply from DIO3, or 0 for a crystal */
        bool dio2_rf_switch; /* DIO2 drives the antenna switch, or the amplifier's */
    } lora;

    int8_t button; /* low while it is pressed; it also wakes the board, so an RTC pin */
    int8_t led;    /* lit when high, or BOARD_NO_PIN */

    /* A 128x64 SSD1306 or a controller that takes its commands (the SSD1315), on I2C. */
    struct {
        int8_t sda, scl, reset; /* sda BOARD_NO_PIN for none */
    } screen;

    /* The switched supply the screen is on, and whether high turns it on. */
    int8_t vext;
    bool vext_high_on;

    /* The battery, through a divider onto an ADC pin, behind a switch on `enable` whose sense the
     * board learns (power.h). */
    struct {
        int8_t sense, enable; /* sense BOARD_NO_PIN for none; enable BOARD_NO_PIN if always on */
        uint16_t top_k, bottom_k;
    } battery;

    /* A front-end amplifier after the SX1262, or none (power BOARD_NO_PIN). `power` and `enable`
     * are raised once at start and lowered when the board turns off; every pin in `tx` is raised
     * while a frame is sent and lowered otherwise. `gain_db` is what the amplifier and whatever
     * stands before it add to the chip's power, taken as high as it might be, so that a power
     * asked for is never exceeded at the antenna (board_chip_dbm()). */
    struct {
        int8_t power, enable;
        int8_t tx[2];
        int8_t gain_db;
    } amp;

    int8_t max_dbm; /* the most this board puts into its antenna, as its maker rates it */
};

extern const struct board_def board_heltec_v3;
extern const struct board_def board_heltec_v4;
extern const struct board_def board_heltec_wsl_v3;
extern const struct board_def board_heltec_tracker;
extern const struct board_def board_heltec_vme290;
extern const struct board_def board_heltec_vme213;
extern const struct board_def board_heltec_paper;

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

/* What to ask the SX1262 for, -9 to 22 dBm, so that the antenna gets no more than antenna_dbm, for
 * a power the board gives (board_gives()). */
int8_t board_chip_dbm(const struct board_def *b, int8_t antenna_dbm);

#endif
