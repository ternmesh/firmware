#include "crumb.h"

#include <string.h>

#define CRUMB_MAGIC 0x43524D42u /* "CRMB" */

static uint32_t crumb_check(const struct crumb *c) {
    return c->magic ^ 0x5A17C0DEu ^ ((uint32_t)c->restarts << 8) ^ c->last_step ^
           (c->last_pass * 2654435761u);
}

bool crumb_boot(struct crumb *c, bool watchdog) {
    bool sound = c->magic == CRUMB_MAGIC && c->check == crumb_check(c) && c->step < CRUMB_STEPS &&
                 c->last_step < CRUMB_STEPS;
    if (!sound) {
        memset(c, 0, sizeof *c);
        c->magic = CRUMB_MAGIC;
    }
    bool report = false;
    if (watchdog && sound && c->step != CRUMB_NONE) {
        c->last_step = c->step;
        c->last_pass = c->pass;
        if (c->restarts < UINT16_MAX) {
            c->restarts++;
        }
        report = true;
    }
    c->step = CRUMB_STARTING;
    c->pass = 0;
    c->check = crumb_check(c);
    return report;
}

const char *crumb_name(uint8_t step) {
    static const char *const names[CRUMB_STEPS] = {
        [CRUMB_NONE] = "none",
        [CRUMB_STARTING] = "starting",
        [CRUMB_RADIO] = "poll_radio",
        [CRUMB_BEACON] = "poll_beacon",
        [CRUMB_CONTACT] = "poll_contact",
        [CRUMB_OUTGOING] = "poll_outgoing",
        [CRUMB_FORWARD] = "poll_forward",
        [CRUMB_CARD] = "poll_card",
        [CRUMB_FLOOD] = "poll_flood",
        [CRUMB_ROUTE] = "poll_route",
        [CRUMB_WRITERS] = "poll_writers",
        [CRUMB_CONSOLE] = "poll_console",
        [CRUMB_BLE] = "poll_ble",
        [CRUMB_BUTTON] = "poll_button",
        [CRUMB_SCREEN] = "poll_screen",
        [CRUMB_BATTERY] = "poll_battery",
        [CRUMB_RECEIVER] = "poll_receiver",
        [CRUMB_RESTART] = "restart_if_due",
        [CRUMB_TX_CHECK] = "the transmit deadline",
        [CRUMB_YIELD] = "poll_led/yield",
        [CRUMB_HALTED] = "halted",
    };
    return step < CRUMB_STEPS ? names[step] : "?";
}
