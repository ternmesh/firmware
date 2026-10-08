#ifndef TERN_RADIO_H
#define TERN_RADIO_H

#include <stdint.h>

#include "tern/lora.h"
#include "tern/time.h"

/* The seam between the protocol core and a board's radio.
 *
 * A port implements struct tern_radio_ops for its chip and hands the core a struct tern_radio.
 * The core calls through the tern_radio_*() functions below, never the ops directly, so that what
 * every port is asked to do has been checked once, here.
 *
 * The core never runs in interrupt context. A port's interrupt handler records what happened and
 * returns; the core collects it later with tern_radio_poll(). That keeps the core free of locks
 * and makes the same code run unchanged on a host, where there are no interrupts at all. */

struct tern_radio_config {
    struct tern_lora mod;
    uint32_t freq_hz;
    int8_t tx_power_dbm;
    /* The sync word in its one-byte form, as SX127x radios take it. Ports for other chips convert
     * it (tern_sync_word_sx126x()). Tern's own is TERN_SYNC_WORD (tern/region.h). */
    uint8_t sync_word;
};

enum tern_radio_event_kind {
    TERN_RADIO_TX_DONE = 1, /* the frame given to tern_radio_transmit() has left the antenna */
    TERN_RADIO_RX_DONE,     /* a frame arrived with a good CRC; the rx fields are set */
    TERN_RADIO_RX_ERROR,    /* a frame arrived but failed its CRC or header check */
};

struct tern_radio_event {
    enum tern_radio_event_kind kind;
    tern_time at; /* when it happened, on the port's clock: the end of the frame */
    /* RX_DONE only. data points into the port's buffer and stays valid until the next call to
     * tern_radio_poll(). */
    const uint8_t *data;
    uint8_t len;
    int16_t rssi_dbm;
    int16_t snr_cdb; /* centibels, so -7.25 dB is -725 */
};

struct tern_radio_ops {
    /* Set modulation, frequency, power and sync word, and leave the radio in standby. */
    int (*configure)(void *ctx, const struct tern_radio_config *cfg);
    /* Start sending len bytes (1..255) and return; TX_DONE follows. */
    int (*transmit)(void *ctx, const uint8_t *frame, uint8_t len);
    /* Listen continuously until told otherwise. */
    int (*receive)(void *ctx);
    /* Stop transmitting or receiving and wait, ready to do either. */
    int (*standby)(void *ctx);
    /* Copy out the oldest event not yet collected: 1 if there was one, 0 if not, or an error. */
    int (*poll)(void *ctx, struct tern_radio_event *ev);
    /* Whether the radio is receiving a frame, as tern/listen.h has it, asked of the chip now: 1 if
     * so, 0 if not, or an error. NULL for a radio that cannot tell. */
    int (*receiving)(void *ctx);
};

struct tern_radio {
    const struct tern_radio_ops *ops;
    void *ctx; /* the port's own state, passed back to every op */
};

/* Each returns TERN_OK or a negative enum tern_err. Arguments the core can check, it checks before
 * the port sees them, and answers TERN_EINVAL without calling it. */
int tern_radio_configure(struct tern_radio *r, const struct tern_radio_config *cfg);
int tern_radio_transmit(struct tern_radio *r, const uint8_t *frame, uint32_t len);
int tern_radio_receive(struct tern_radio *r);
int tern_radio_standby(struct tern_radio *r);
/* 1 and *ev filled if an event was waiting, 0 if none, or a negative enum tern_err. */
int tern_radio_poll(struct tern_radio *r, struct tern_radio_event *ev);

/* 1 if the radio is receiving a frame, when the core must not start to send one; 0 if it is not,
 * or cannot tell; or a negative enum tern_err. A port asks just before each frame. */
int tern_radio_receiving(struct tern_radio *r);

/* The SX126x family takes a two-byte sync word: the one-byte form 0xXY becomes 0xX4Y4. So
 * Meshtastic's 0x2B is 0x24B4 and MeshCore's 0x12 is 0x1424. */
uint16_t tern_sync_word_sx126x(uint8_t sync_word);

#endif
