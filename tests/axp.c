#include "axp.h"

#include <string.h>

#include "check.h"

/* The AXP192 and AXP2101 driver (ports/node/axp.c), against a chip that is only its registers. */

struct fake {
    uint8_t reg[256];
    bool gone; /* answers nothing */
    int writes;
};

static bool fake_read(void *ctx, uint8_t reg, uint8_t *buf, size_t len) {
    struct fake *f = ctx;
    if (f->gone) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        buf[i] = f->reg[(uint8_t)(reg + i)];
    }
    return true;
}

static bool fake_write(void *ctx, uint8_t reg, uint8_t value) {
    struct fake *f = ctx;
    if (f->gone) {
        return false;
    }
    f->reg[reg] = value;
    f->writes++;
    return true;
}

static struct axp start(struct fake *f, enum axp_chip chip) {
    struct axp a;
    struct axp_bus bus = {f, fake_read, fake_write};
    CHECK(axp_init(&a, &bus, chip));
    return a;
}

/* The AXP192's LDO2 at 3.3 V is code 0xF in 0x28's high half, its LDO3 untouched in the low; and
 * DC-DC1 at 3.3 V is (3300 - 700) / 25 = 104 = 0x68 (datasheet p.37 and p.38). */
static void axp192_rails_are_set_then_switched(void) {
    struct fake f = {0};
    f.reg[0x28] = 0x0A; /* LDO3 at 2.8 V */
    f.reg[0x12] = 0x02; /* DC-DC3, the processor's, on */
    struct axp a = start(&f, AXP192);
    CHECK(axp_set_rail(&a, AXP_LDO2, 3300));
    CHECK_EQ_I64(f.reg[0x28], 0xFA);
    CHECK_EQ_I64(f.reg[0x12], 0x06);
    CHECK(axp_set_rail(&a, AXP_DCDC1, 3300));
    CHECK_EQ_I64(f.reg[0x26], 0x68);
    CHECK_EQ_I64(f.reg[0x12], 0x07);
    CHECK(axp_set_rail(&a, AXP_LDO3, 0));
    CHECK_EQ_I64(f.reg[0x12], 0x07); /* already off */
    CHECK_EQ_I64(f.reg[0x28], 0xFA);
    CHECK(axp_set_rail(&a, AXP_LDO2, 0));
    CHECK_EQ_I64(f.reg[0x12], 0x03); /* DC-DC3 left on */
}

/* The AXP2101's ALDO2 at 3.3 V is (3300 - 500) / 100 = 28 = 0x1C in 0x93, on in 0x90 b1. */
static void axp2101_rails_are_set_then_switched(void) {
    struct fake f = {0};
    f.reg[0x80] = 0x01; /* DCDC1, the processor's, on */
    struct axp a = start(&f, AXP2101);
    CHECK(axp_set_rail(&a, AXP_ALDO2, 3300));
    CHECK_EQ_I64(f.reg[0x93], 0x1C);
    CHECK_EQ_I64(f.reg[0x90], 0x02);
    CHECK(axp_set_rail(&a, AXP_BLDO1, 1800));
    CHECK_EQ_I64(f.reg[0x96], 13);
    CHECK_EQ_I64(f.reg[0x90], 0x12);
    CHECK(axp_set_rail(&a, AXP_DCDC1, 3300));
    CHECK_EQ_I64(f.reg[0x82], 18);
    CHECK_EQ_I64(f.reg[0x80], 0x01);
    CHECK(axp_set_rail(&a, AXP_ALDO2, 0));
    CHECK_EQ_I64(f.reg[0x90], 0x10);
}

/* A rail the chip has not got, or a voltage it does not give exactly, is refused untouched. */
static void what_a_chip_cannot_give_is_refused(void) {
    struct fake f = {0};
    struct axp a = start(&f, AXP192);
    int writes = f.writes;
    CHECK(!axp_set_rail(&a, AXP_ALDO2, 3300)); /* the AXP2101's */
    CHECK(!axp_set_rail(&a, AXP_LDO2, 1700));  /* below 1.8 V */
    CHECK(!axp_set_rail(&a, AXP_LDO2, 3350));  /* not a step */
    CHECK(!axp_set_rail(&a, AXP_DCDC1, 3510)); /* above 3.5 V */
    CHECK(!axp_set_rail(&a, AXP_RAIL_NONE, 0));
    CHECK_EQ_I64(f.writes, writes);
    CHECK(axp_rail_ok(AXP192, AXP_DCDC1, 3325)); /* 25 mV steps */
    CHECK(!axp_rail_ok(AXP2101, AXP_DCDC1, 3325));
    CHECK(!axp_rail_ok(AXP2101, AXP_LDO2, 0));
}

/* The battery's voltage is read on, whatever the chip was left measuring. */
static void the_battery_is_measured_from_the_start(void) {
    struct fake f = {0};
    start(&f, AXP192);
    CHECK_EQ_I64(f.reg[0x82] & 0x80, 0x80);
    struct fake g = {0};
    start(&g, AXP2101);
    CHECK_EQ_I64(g.reg[0x30] & 0x01, 0x01);
}

/* AXP192: 12 bits at 1.1 mV, 0x78 the high eight; 3.9 V is code 3545 = 0xDD9. */
static void axp192_reads_the_battery(void) {
    struct fake f = {0};
    struct axp a = start(&f, AXP192);
    uint16_t mv = 1;
    bool charging = true;
    CHECK(axp_battery(&a, &mv, &charging));
    CHECK_EQ_I64(mv, 0); /* none fitted */
    CHECK(!charging);
    f.reg[0x01] = 0x20; /* present */
    f.reg[0x78] = 0xDD;
    f.reg[0x79] = 0x09;
    CHECK(axp_battery(&a, &mv, &charging));
    CHECK_EQ_I64(mv, 3899);
    CHECK(!charging);
    f.reg[0x01] = 0x60;
    CHECK(axp_battery(&a, &mv, &charging));
    CHECK(charging);
}

/* AXP2101: 14 bits at 1 mV, the high six in 0x34, whose top two are something else. */
static void axp2101_reads_the_battery(void) {
    struct fake f = {0};
    struct axp a = start(&f, AXP2101);
    uint16_t mv = 1;
    bool charging = true;
    CHECK(axp_battery(&a, &mv, &charging));
    CHECK_EQ_I64(mv, 0);
    f.reg[0x00] = 0x08; /* present */
    f.reg[0x34] = 0xC0 | (3912 >> 8);
    f.reg[0x35] = 3912 & 0xFF;
    f.reg[0x01] = 0x40; /* discharging */
    CHECK(axp_battery(&a, &mv, &charging));
    CHECK_EQ_I64(mv, 3912);
    CHECK(!charging);
    f.reg[0x01] = 0x20; /* charging */
    CHECK(axp_battery(&a, &mv, &charging));
    CHECK(charging);
}

static void a_chip_that_does_not_answer_says_so(void) {
    struct fake f = {.gone = true};
    struct axp a;
    struct axp_bus bus = {&f, fake_read, fake_write};
    CHECK(!axp_init(&a, &bus, AXP192));
    CHECK(!axp_set_rail(&a, AXP_LDO2, 3300));
    uint16_t mv;
    bool charging;
    CHECK(!axp_battery(&a, &mv, &charging));
    CHECK(!axp_init(&a, &bus, AXP_NONE));
}

int main(void) {
    RUN(axp192_rails_are_set_then_switched);
    RUN(axp2101_rails_are_set_then_switched);
    RUN(what_a_chip_cannot_give_is_refused);
    RUN(the_battery_is_measured_from_the_start);
    RUN(axp192_reads_the_battery);
    RUN(axp2101_reads_the_battery);
    RUN(a_chip_that_does_not_answer_says_so);
    return CHECK_DONE();
}
