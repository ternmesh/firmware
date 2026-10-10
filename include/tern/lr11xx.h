#ifndef TERN_LR11XX_H
#define TERN_LR11XX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tern/listen.h"
#include "tern/radio.h"
#include "tern/time.h"

/* A driver for Semtech's LR1110 and LR1121, from the LR1121 user manual (UM.LR1121.W.APP, revision
 * 1.2), page numbers in src/lr11xx.c being its, and Semtech's own driver for the family (SWDR001)
 * where the LR1110 differs. It drives the sub-GHz LoRa modem only, not the GNSS and Wi-Fi scanners
 * the LR1110 also carries, and implements struct tern_radio_ops, so the core drives these chips as
 * it does the SX1262: the frames on the air are the same.
 *
 * It needs only the SPI bus from the board, and is otherwise portable. The board resets the chip
 * before tern_lr11xx_init(): NRESET low for more than 100 us, then let go, then about 240 ms while
 * it starts its firmware (page 22). DIO9 is not needed: tern_radio_poll() asks the chip.
 *
 * Unlike the SX126x, a command that answers does so in a second transaction: the command, then
 * NSS high, then, once BUSY is low, as many zero bytes as the answer needs, the first of which
 * comes back as the chip's status (page 25). */

struct tern_lr11xx_bus {
    void *ctx;
    /* Waits for BUSY low, then holds NSS low while it clocks len bytes out of tx and the same
     * number in to rx (which may be NULL), then raises NSS. Returns TERN_OK or an error. */
    int (*transfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
    /* The board's clock, for the time stamps on events. */
    tern_time (*now)(void *ctx);
};

/* Which of the chip's DIO5, DIO6, DIO7, DIO8 and DIO10 drive the antenna switch, as bits 0 to 4, in
 * each of its modes (SetDioAsRfSwitch, page 38). */
#define TERN_LR11XX_RFSW_DIO5 0x01u
#define TERN_LR11XX_RFSW_DIO6 0x02u
#define TERN_LR11XX_RFSW_DIO7 0x04u
#define TERN_LR11XX_RFSW_DIO8 0x08u
#define TERN_LR11XX_RFSW_DIO10 0x10u

struct tern_lr11xx_rf_switch {
    uint8_t enable; /* the DIOs the chip drives at all; the rest are left alone */
    uint8_t standby, rx, tx, tx_hp;
};

/* How the chip is wired on this board. */
struct tern_lr11xx_board {
    /* The voltage the chip gives its TCXO on VTCXO, in millivolts (1600 to 3300), or 0 if it has a
     * crystal. */
    uint16_t tcxo_mv;
    bool dcdc; /* the DC-DC regulator's inductor is fitted, not just the LDO */
    struct tern_lr11xx_rf_switch rf_switch;
    /* The antenna is reached from the high-power amplifier, so -9 to +22 dBm. Otherwise only from
     * the low-power one: -17 to +14 dBm. Above +14 dBm the high-power one is used. */
    bool hp_pa;
};

/* What the receiver has seen since tern_lr11xx_init(), as for the SX126x (tern/sx126x.h). */
struct tern_lr11xx_counts {
    uint32_t preambles;
    uint32_t headers;
    uint32_t header_errors;
    uint32_t crc_errors;
    uint32_t frames;
};

struct tern_lr11xx {
    struct tern_lr11xx_bus bus;
    struct tern_lr11xx_board board;
    struct tern_lr11xx_counts counts;
    struct tern_radio_config cfg; /* as last configured */
    struct tern_listen listen;    /* what it is receiving, for tern_radio_receiving() */
    uint16_t fw;                  /* the chip's firmware, as GetVersion gave it */
    bool configured;
    uint8_t rx[255]; /* the last frame received; RX_DONE events point here */
};

/* Checks the chip is an LR1110 or LR1121 running its firmware (TERN_EIO if it answers as neither,
 * or is in its bootloader), and sets it up for LoRa as the board is wired, in standby. */
int tern_lr11xx_init(struct tern_lr11xx *d, const struct tern_lr11xx_bus *bus,
                     const struct tern_lr11xx_board *board);

/* Puts the chip to sleep, keeping nothing, for a board that is turning itself off. A falling edge
 * on NSS wakes it, but it is reset and initialised again before it is used. */
int tern_lr11xx_sleep(struct tern_lr11xx *d);

/* The radio, for the core. */
struct tern_radio tern_lr11xx_radio(struct tern_lr11xx *d);

#endif
