#include "axp.h"

/* The AXP192 (datasheet revision 1.1). */
#define A192_STATUS 0x00  /* p.35 */
#define A192_MODE 0x01    /* p.35: b6 charging, b5 battery present */
#define A192_OUTPUTS 0x12 /* p.36: b0 DC-DC1, b1 DC-DC3, b2 LDO2, b3 LDO3 */
#define A192_DCDC1_V 0x26 /* p.37: 0.7 to 3.5 V, 25 mV a step */
#define A192_DCDC3_V 0x27 /* p.38: as DC-DC1 */
#define A192_LDO23_V 0x28 /* p.38: LDO2 in b7:4, LDO3 in b3:0, 1.8 to 3.3 V, 100 mV a step */
#define A192_ADC1 0x82    /* p.43: b7 the battery's voltage */
#define A192_VBAT 0x78    /* p.34: 0x78 its high eight bits, 0x79 its low four; 1.1 mV each */

/* The AXP2101 (datasheet revision 1.0). */
#define A2101_STATUS1 0x00 /* p.31: b3 battery present */
#define A2101_STATUS2 0x01 /* p.31: b6:5 the battery's current, 01 charging */
#define A2101_ADC 0x30     /* p.39: b0 the battery's voltage */
#define A2101_VBAT 0x34    /* p.40: 0x34 b5:0 its high six bits, 0x35 its low eight; 1 mV each */
#define A2101_DCDC_ON 0x80 /* p.50: b0 DCDC1 */
#define A2101_DCDC1_V 0x82 /* p.51: 1.5 to 3.4 V, 100 mV a step */
#define A2101_LDO_ON 0x90  /* p.53: b0 to b3 ALDO1 to ALDO4, b4 BLDO1, b5 BLDO2 */
#define A2101_ALDO1_V                                                                              \
    0x92 /* p.54: ALDO1 to ALDO4 and then BLDO1 and BLDO2 at 0x96 and 0x97, each                   \
            0.5 to 3.5 V, 100 mV a step */

/* Where each rail is switched and set, and what it gives. The datasheet prints 0x90's b4 as
 * "aldo1" a second time; between ALDO4 and BLDO2 it can only be BLDO1. */
struct rail {
    enum axp_chip chip;
    enum axp_rail rail;
    uint8_t on_reg, on_bit;
    uint8_t v_reg, v_shift, v_mask; /* the field the voltage's code is in */
    uint16_t min_mv, max_mv, step_mv;
};

static const struct rail rails[] = {
    {AXP192, AXP_DCDC1, A192_OUTPUTS, 0, A192_DCDC1_V, 0, 0x7F, 700, 3500, 25},
    {AXP192, AXP_DCDC3, A192_OUTPUTS, 1, A192_DCDC3_V, 0, 0x7F, 700, 3500, 25},
    {AXP192, AXP_LDO2, A192_OUTPUTS, 2, A192_LDO23_V, 4, 0x0F, 1800, 3300, 100},
    {AXP192, AXP_LDO3, A192_OUTPUTS, 3, A192_LDO23_V, 0, 0x0F, 1800, 3300, 100},
    {AXP2101, AXP_DCDC1, A2101_DCDC_ON, 0, A2101_DCDC1_V, 0, 0x1F, 1500, 3400, 100},
    {AXP2101, AXP_ALDO1, A2101_LDO_ON, 0, A2101_ALDO1_V + 0, 0, 0x1F, 500, 3500, 100},
    {AXP2101, AXP_ALDO2, A2101_LDO_ON, 1, A2101_ALDO1_V + 1, 0, 0x1F, 500, 3500, 100},
    {AXP2101, AXP_ALDO3, A2101_LDO_ON, 2, A2101_ALDO1_V + 2, 0, 0x1F, 500, 3500, 100},
    {AXP2101, AXP_ALDO4, A2101_LDO_ON, 3, A2101_ALDO1_V + 3, 0, 0x1F, 500, 3500, 100},
    {AXP2101, AXP_BLDO1, A2101_LDO_ON, 4, A2101_ALDO1_V + 4, 0, 0x1F, 500, 3500, 100},
    {AXP2101, AXP_BLDO2, A2101_LDO_ON, 5, A2101_ALDO1_V + 5, 0, 0x1F, 500, 3500, 100},
};

static const struct rail *find(enum axp_chip chip, enum axp_rail rail) {
    for (size_t i = 0; i < sizeof rails / sizeof rails[0]; i++) {
        if (rails[i].chip == chip && rails[i].rail == rail) {
            return &rails[i];
        }
    }
    return NULL;
}

static bool reg_read(struct axp *a, uint8_t reg, uint8_t *v) {
    return a->bus.read(a->bus.ctx, reg, v, 1);
}

/* Sets the bits of mask in a register to those of value, leaving the rest as they are. */
static bool reg_update(struct axp *a, uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t v;
    return reg_read(a, reg, &v) && a->bus.write(a->bus.ctx, reg, (uint8_t)((v & ~mask) | value));
}

bool axp_init(struct axp *a, const struct axp_bus *bus, enum axp_chip chip) {
    a->bus = *bus;
    a->chip = chip;
    uint8_t status;
    if (chip == AXP192) {
        return reg_read(a, A192_STATUS, &status) && reg_update(a, A192_ADC1, 0x80, 0x80);
    }
    if (chip == AXP2101) {
        return reg_read(a, A2101_STATUS1, &status) && reg_update(a, A2101_ADC, 0x01, 0x01);
    }
    return false;
}

bool axp_rail_ok(enum axp_chip chip, enum axp_rail rail, uint16_t mv) {
    const struct rail *r = find(chip, rail);
    return r != NULL &&
           (mv == 0 || (mv >= r->min_mv && mv <= r->max_mv && (mv - r->min_mv) % r->step_mv == 0));
}

bool axp_set_rail(struct axp *a, enum axp_rail rail, uint16_t mv) {
    if (!axp_rail_ok(a->chip, rail, mv)) {
        return false;
    }
    const struct rail *r = find(a->chip, rail);
    uint8_t on = (uint8_t)(1u << r->on_bit);
    if (mv == 0) {
        return reg_update(a, r->on_reg, on, 0);
    }
    /* The voltage first, so the rail never comes on at the one it had. */
    uint8_t code = (uint8_t)((mv - r->min_mv) / r->step_mv);
    return reg_update(a, r->v_reg, (uint8_t)(r->v_mask << r->v_shift),
                      (uint8_t)(code << r->v_shift)) &&
           reg_update(a, r->on_reg, on, on);
}

bool axp_battery(struct axp *a, uint16_t *mv, bool *charging) {
    uint8_t s[2], v[2];
    if (a->chip == AXP192) {
        if (!reg_read(a, A192_MODE, &s[0]) || !a->bus.read(a->bus.ctx, A192_VBAT, v, 2)) {
            return false;
        }
        bool present = (s[0] & 0x20) != 0;
        uint32_t code = ((uint32_t)v[0] << 4) | (v[1] & 0x0F);
        *mv = present ? (uint16_t)(code * 11 / 10) : 0;
        *charging = present && (s[0] & 0x40) != 0;
        return true;
    }
    if (a->chip == AXP2101) {
        /* The high byte first, as the datasheet asks (p.28). */
        if (!a->bus.read(a->bus.ctx, A2101_STATUS1, s, 2) ||
            !a->bus.read(a->bus.ctx, A2101_VBAT, v, 2)) {
            return false;
        }
        bool present = (s[0] & 0x08) != 0;
        *mv = present ? (uint16_t)(((v[0] & 0x3Fu) << 8) | v[1]) : 0;
        *charging = present && ((s[1] >> 5) & 0x03) == 0x01;
        return true;
    }
    return false;
}
