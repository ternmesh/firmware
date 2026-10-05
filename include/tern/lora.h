#ifndef TERN_LORA_H
#define TERN_LORA_H

#include <stdbool.h>
#include <stdint.h>

#include "tern/time.h"

/* LoRa modulation and time on air.
 *
 * Time on air is what the airtime budget charges and what the routing metric is measured in, so
 * the firmware and the simulator must agree on it to the nanosecond. The formula is Semtech's, as
 * published for the SX126x and SX127x (application note AN1200.13 and the SX1261/2 datasheet), and
 * the arithmetic is the simulator's (ternmesh/sim, tsim/lora.h); the tests check the same values.
 * Only SF7 to SF12 are accepted: SF5 and SF6 use a different preamble and payload rule on the
 * SX126x, and nothing in the design uses them yet. */

enum tern_ldro {
    TERN_LDRO_AUTO = 0, /* on when a symbol lasts 16 ms or longer, as the radio drivers do */
    TERN_LDRO_OFF,
    TERN_LDRO_ON,
};

struct tern_lora {
    uint8_t sf;        /* spreading factor, 7..12 */
    uint32_t bw_hz;    /* bandwidth: 62500, 125000, 250000 or 500000 (any value > 0 computes) */
    uint8_t cr;        /* coding rate 4/(4+cr), cr = 1..4 */
    uint16_t preamble; /* programmed preamble length in symbols */
    bool implicit_header;
    bool crc;
    enum tern_ldro ldro;
};

/* SF and BW with CR 4/5, an 8-symbol preamble, explicit header, CRC on and LDRO automatic. These
 * are a starting point, not Tern's on-air parameters, which the specification will set. */
struct tern_lora tern_lora_default(uint8_t sf, uint32_t bw_hz);

/* Whether the parameters are ones the time-on-air formula covers. */
bool tern_lora_valid(const struct tern_lora *m);

/* Whether low data rate optimisation is in effect for these parameters. */
bool tern_lora_ldro(const struct tern_lora *m);

/* The duration of one symbol, 2^SF / BW, rounded to the nearest nanosecond. */
tern_time tern_lora_symbol(const struct tern_lora *m);

/* The number of symbols after the preamble: header, payload and CRC. */
uint32_t tern_lora_payload_symbols(const struct tern_lora *m, uint32_t payload_len);

/* Time on air for a frame carrying payload_len bytes, rounded to the nearest nanosecond.
 * Returns -1 for parameters tern_lora_valid() rejects or a payload over 255 bytes. */
tern_time tern_lora_airtime(const struct tern_lora *m, uint32_t payload_len);

#endif
