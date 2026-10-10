#ifndef NODE_AXP_H
#define NODE_AXP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* X-Powers' power management chips, the AXP192 and the AXP2101, on I2C at 0x34: the supplies a
 * board switches through them, and the battery they measure. Written from their datasheets
 * (AXP192, revision 1.1, March 2015; AXP2101, revision 1.0, May 2021), page numbers below being
 * theirs. Only what a node needs is here: a rail on or off at a voltage, and the battery's voltage
 * and whether it charges.
 *
 * Neither datasheet gives a register that says which chip it is, so the board says (boards.h).
 * Nothing here touches the hardware, so tests/axp.c runs it on a host against a fake chip. */

#define AXP_ADDRESS 0x34

enum axp_chip {
    AXP_NONE,
    AXP192,
    AXP2101,
};

/* The rails a board may switch. Each chip has some of them: the AXP192 DCDC1, DCDC3, LDO2 and
 * LDO3; the AXP2101 DCDC1, ALDO1 to ALDO4, BLDO1 and BLDO2. */
enum axp_rail {
    AXP_RAIL_NONE,
    AXP_DCDC1,
    AXP_DCDC3,
    AXP_LDO2,
    AXP_LDO3,
    AXP_ALDO1,
    AXP_ALDO2,
    AXP_ALDO3,
    AXP_ALDO4,
    AXP_BLDO1,
    AXP_BLDO2,
};

struct axp_bus {
    void *ctx;
    /* Reads len registers from reg on; true if the chip answered. */
    bool (*read)(void *ctx, uint8_t reg, uint8_t *buf, size_t len);
    /* Writes one register; true if the chip answered. */
    bool (*write)(void *ctx, uint8_t reg, uint8_t value);
};

struct axp {
    struct axp_bus bus;
    enum axp_chip chip;
};

/* Whether the chip answers, and its battery's voltage measured from now on. */
bool axp_init(struct axp *a, const struct axp_bus *bus, enum axp_chip chip);

/* Sets a rail to mv and turns it on, or turns it off for 0. False for a rail the chip has not got
 * or a voltage it does not give exactly, without touching it, or if the chip did not answer. */
bool axp_set_rail(struct axp *a, enum axp_rail rail, uint16_t mv);

/* Whether the chip gives mv on that rail (0, off, it always does). */
bool axp_rail_ok(enum axp_chip chip, enum axp_rail rail, uint16_t mv);

/* The battery's voltage in millivolts, 0 for none fitted, and whether it is charging. False if the
 * chip did not answer. */
bool axp_battery(struct axp *a, uint16_t *mv, bool *charging);

#endif
