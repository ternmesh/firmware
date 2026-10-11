#ifndef TERN_REGION_H
#define TERN_REGION_H

#include <stdint.h>

#include "tern/lora.h"
#include "tern/radio.h"

/* Radio settings: specification draft 0, draft/phy.md in ternmesh/spec.
 *
 * The settings every Tern frame is sent with, and for each region a profile: the one channel and
 * modulation a node there uses, and what the region's rules allow it. Every value is provisional,
 * as the specification's are: from published rules and sources, not yet from a bench. */

#define TERN_SYNC_WORD 0x5E /* 0x54E4 on the SX126x: tern_sync_word_sx126x() */
#define TERN_PREAMBLE 16

/* duty_ppm for a region that does not limit how much a node transmits. */
#define TERN_DUTY_UNLIMITED 1000000u
/* max_conducted_dbm for a region that limits only what is radiated. */
#define TERN_POWER_UNLIMITED INT8_MAX
/* min_eirp_dbm for a region that sets no least power. */
#define TERN_POWER_NO_FLOOR INT8_MIN

enum tern_region_id {
    TERN_REGION_US915 = 1,
    TERN_REGION_EU868,
    TERN_REGION_AU915,
    TERN_REGION_NZ915,
    /* A node keeps its region by this number: a new one goes last, and none is renumbered. */
    TERN_REGION_CL915,
    TERN_REGION_MY919,
    TERN_REGION_TH920,
    TERN_REGION_RU868,
    TERN_REGION_KZ863,
    TERN_REGION_END, /* one past the last */
};

struct tern_region {
    const char *name;
    uint32_t freq_hz;
    uint32_t bw_hz;
    uint8_t sf;
    int8_t max_eirp_dbm;      /* radiated, antenna gain included */
    int8_t max_conducted_dbm; /* into the antenna, or TERN_POWER_UNLIMITED */
    /* The least it may radiate, or TERN_POWER_NO_FLOOR: where the limit on transmitting holds
     * only from some power up. */
    int8_t min_eirp_dbm;
    /* A node may transmit for at most duty_ppm millionths of any duty_window_s seconds
     * (tern/duty.h). */
    uint32_t duty_ppm;
    uint32_t duty_window_s;
};

/* A region's profile, or NULL if id is not a region. */
const struct tern_region *tern_region(enum tern_region_id id);

/* The modulation of a region's frames: its SF and bandwidth, with the settings every Tern frame
 * uses. */
struct tern_lora tern_region_lora(const struct tern_region *r);

/* Fills in the radio configuration for a region, sending at tx_power_dbm into an antenna of
 * antenna_dbi. Returns TERN_OK, or TERN_EINVAL, leaving cfg alone, if that is more power than
 * the region allows, or less than it requires. */
int tern_region_radio(const struct tern_region *r, int8_t tx_power_dbm, int8_t antenna_dbi,
                      struct tern_radio_config *cfg);

/* A node's lowest power, into the antenna: the least that power control sends any frame at. It
 * is radio_min_dbm, or the region's floor less the antenna's gain, whichever is more. */
int8_t tern_region_lowest_dbm(const struct tern_region *r, int8_t radio_min_dbm,
                              int8_t antenna_dbi);

#endif
