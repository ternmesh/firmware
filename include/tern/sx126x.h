#ifndef TERN_SX126X_H
#define TERN_SX126X_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tern/listen.h"
#include "tern/radio.h"
#include "tern/time.h"

/* A driver for Semtech's SX1262, written from the SX1261/2 datasheet (DS.SX1261-2, revision
 * 2.1), section numbers in src/sx126x.c being its. The SX1261, whose power amplifier is the
 * low-power one, is not supported yet. It implements struct tern_radio_ops, so the core
 * drives the chip the way it drives any radio.
 *
 * It needs only the SPI bus from the board, and is otherwise portable, so it is part of the core
 * and every board with one of these chips shares it: the Heltec V3 today, nRF52840 boards later.
 * The board resets the chip (NRESET) before tern_sx126x_init(), and its transfer() waits for BUSY
 * to go low before each command. DIO1 is not needed: tern_radio_poll() asks the chip. */

struct tern_sx126x_bus {
    void *ctx;
    /* Waits for BUSY low, then holds NSS low while it clocks len bytes out of tx and the same
     * number in to rx (which may be NULL), then raises NSS. Returns TERN_OK or an error. */
    int (*transfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
    /* The board's clock, for the time stamps on events. */
    tern_time (*now)(void *ctx);
};

/* How the chip is wired on this board. */
struct tern_sx126x_board {
    /* The voltage DIO3 gives a TCXO, in millivolts (1600 to 3300), or 0 if the chip has a
     * crystal. */
    uint16_t tcxo_mv;
    bool dio2_rf_switch; /* DIO2 drives the antenna switch */
    bool dcdc;           /* the DC-DC regulator is fitted (as on most modules), not just the LDO */
};

/* What the receiver has seen since tern_sx126x_init(), as tern_radio_poll() found it. The chip
 * says once that it has seen a preamble or a header however many came between two polls, so these
 * are exact only when frames come further apart than the polls. The caller may clear them. */
struct tern_sx126x_counts {
    uint32_t preambles; /* preambles detected, whatever followed */
    uint32_t headers;   /* of them, those with this radio's sync word and a header that checked */
    uint32_t header_errors; /* headers that did not check */
    uint32_t crc_errors;    /* frames that failed their CRC */
    uint32_t frames;        /* frames received whole */
};

struct tern_sx126x {
    struct tern_sx126x_bus bus;
    struct tern_sx126x_board board;
    struct tern_sx126x_counts counts;
    struct tern_radio_config cfg; /* as last configured */
    struct tern_listen listen;    /* what it is receiving, for tern_radio_receiving() */
    bool configured;
    uint8_t rx[255]; /* the last frame received; RX_DONE events point here */
};

/* Wakes the chip into standby and sets it up for LoRa as the board is wired. */
int tern_sx126x_init(struct tern_sx126x *d, const struct tern_sx126x_bus *bus,
                     const struct tern_sx126x_board *board);

/* The radio, for the core. */
struct tern_radio tern_sx126x_radio(struct tern_sx126x *d);

#endif
