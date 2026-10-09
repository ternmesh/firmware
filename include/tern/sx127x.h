#ifndef TERN_SX127X_H
#define TERN_SX127X_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tern/listen.h"
#include "tern/radio.h"
#include "tern/time.h"

/* A driver for Semtech's SX1276 and SX1278, written from their datasheet (SX1276/77/78/79, revision
 * 7, May 2020) and errata note (revision 1), page numbers in src/sx127x.c being the datasheet's:
 * its section numbers do not match its contents page. It drives the
 * LoRa modem only and implements struct tern_radio_ops, so the core drives these chips as it does
 * the SX1262: the frames on the air are the same. The SX1277 and SX1279 are not supported yet.
 *
 * It needs only the SPI bus from the board, and is otherwise portable. The board resets the chip
 * before tern_sx127x_init(): NRESET low for more than 100 us, then let go, then 5 ms (page 117). No
 * DIO is needed: tern_radio_poll() reads the chip's flags.
 */

struct tern_sx127x_bus {
    void *ctx;
    /* Holds NSS low while it clocks len bytes out of tx and the same number in to rx (which may be
     * NULL), then raises NSS. Returns TERN_OK or an error. */
    int (*transfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
    /* The board's clock, for the time stamps on events. */
    tern_time (*now)(void *ctx);
};

enum tern_sx127x_chip {
    TERN_SX1276, /* 137 to 1020 MHz */
    TERN_SX1278, /* 137 to 525 MHz */
};

/* How the chip is wired on this board. */
struct tern_sx127x_board {
    enum tern_sx127x_chip chip;
    /* The antenna is on PA_BOOST, as on most modules: +2 to +17 dBm. Otherwise on RFO: 0 to
     * +14 dBm. */
    bool pa_boost;
    bool tcxo; /* the chip's clock is a TCXO on XTA, not a crystal */
};

/* What the receiver has seen since tern_sx127x_init(), as tern_radio_poll() and
 * tern_radio_receiving() found it. The chip has no flag for a preamble: one is counted each time
 * its modem goes from searching to having found a signal. Nor does it report a header that fails
 * its check, which it drops and goes on searching. So header_errors stays 0, and a preamble that
 * led nowhere is the difference between preambles and headers. */
struct tern_sx127x_counts {
    uint32_t preambles; /* signals the modem found, whatever followed */
    uint32_t headers;   /* of them, those with this radio's sync word and a header that checked */
    uint32_t header_errors; /* always 0: the chip does not say */
    uint32_t crc_errors;    /* frames that failed their CRC */
    uint32_t frames;        /* frames received whole */
};

struct tern_sx127x {
    struct tern_sx127x_bus bus;
    struct tern_sx127x_board board;
    struct tern_sx127x_counts counts;
    struct tern_radio_config cfg; /* as last configured */
    struct tern_listen listen;    /* what it is receiving, for tern_radio_receiving() */
    bool signal;                  /* the modem had found a signal when last asked */
    bool configured;
    uint8_t rx[255]; /* the last frame received; RX_DONE events point here */
};

/* Checks the chip is one of these (TERN_EIO if it does not answer as one), and puts it to sleep in
 * LoRa mode, set up as the board is wired. Each configuration calibrates the receiver at its
 * frequency, which takes about 10 ms. */
int tern_sx127x_init(struct tern_sx127x *d, const struct tern_sx127x_bus *bus,
                     const struct tern_sx127x_board *board);

/* Puts the chip to sleep, drawing about a microamp, for a board that is turning itself off. It
 * keeps its registers, but is configured again before it is used. */
int tern_sx127x_sleep(struct tern_sx127x *d);

/* The radio, for the core. */
struct tern_radio tern_sx127x_radio(struct tern_sx127x *d);

#endif
