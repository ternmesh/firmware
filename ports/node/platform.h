#ifndef NODE_PLATFORM_H
#define NODE_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link.h"

/* What the node (node.c) needs of the platform it runs on, beyond the board's hardware (board.h)
 * and Bluetooth (ble.h): somewhere to keep what it saves, random numbers, a way to take new
 * firmware, and to restart and wait. Each port implements these, in ESP-IDF's terms on an ESP32
 * (ports/esp32/main/platform.c) and in its own on another chip.
 *
 * The node's settings are the build's Kconfig symbols, CONFIG_TERN_*, which every port defines:
 * ESP-IDF and Zephyr both build with Kconfig. */

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h" /* ESP-IDF, which does not include its configuration everywhere itself */
#endif
#endif

/* --- Storage ---------------------------------------------------------------------------------- */

/* Records the node keeps across restarts, by name: the identity, the sessions, contacts, groups,
 * settings, the time on the air. A record is read back whole, the length it was saved at.
 *
 * Starts the store. If `erase`, the Reset page asked for the board to be erased: everything the
 * store holds goes, Bluetooth's bonds with it where the platform keeps them there, but the record
 * named `keep`, which is put back. Returns PLAT_STORE_OK, or why not, with the platform's own
 * error in *code. */
enum plat_store_start {
    PLAT_STORE_OK,
    PLAT_STORE_NEEDS_ERASE, /* the store is full or a newer format: only erasing it would start it,
                               and that would lose the board's identity */
    PLAT_STORE_FAILED,
    PLAT_STORE_ERASE_FAILED,
};
enum plat_store_start plat_store_start(bool erase, const char *keep, int *code);

/* The platform's name for one of its errors, for the console. */
const char *plat_error_name(int code);

bool plat_store_load(const char *key, void *buf, size_t len); /* only a record of exactly len */
bool plat_store_save(const char *key, const void *buf, size_t len);
bool plat_store_has(const char *key);

/* The link's messages, place by place (link.h): a store shares its room with the records above,
 * and a message must never be why one of those cannot be saved, so the platform, which knows how
 * its store counts room, decides whether one fits. len 0 takes the place's message out. */
size_t plat_message_load(size_t place, uint8_t *buf, size_t cap);
enum link_saved plat_message_save(size_t place, const uint8_t *buf, size_t len);

/* Each message's state (link.h), a word a place. */
bool plat_state_load(size_t place, uint64_t *state);
bool plat_state_save(size_t place, uint64_t state);

/* How full the store is, in its own units, for 'status': false if it cannot say. */
bool plat_store_usage(unsigned *used, unsigned *total, unsigned *spare);

/* Restarts the board to be erased: plat_store_start() is then given erase. Nothing is saved
 * first. */
__attribute__((noreturn)) void plat_erase_and_restart(void);

/* Whether this start is one plat_erase_and_restart() asked for. Once: it reads false after. */
bool plat_erase_asked(void);

/* --- Random numbers --------------------------------------------------------------------------- */

/* From a true source: the node's keys are made from these. False if there are none. */
bool plat_random(uint8_t *buf, size_t len);
uint32_t plat_random32(void);

/* Bluetooth is starting (true) or did not (false). On a chip whose generator draws on its radio
 * while Bluetooth is on, and on a noise source otherwise, as the ESP32's does, the noise source is
 * turned off and on again here; elsewhere this does nothing. */
void plat_bluetooth_starting(bool starting);

/* --- Firmware updates ------------------------------------------------------------------------- */

/* An update over the companion link (draft/companion.md): the image is written, in order, to
 * where the firmware not running is kept, and run at the next start; if that start does not get as
 * far as plat_update_confirm(), the platform goes back to the firmware before. */
uint32_t plat_update_room(void);       /* the most an image may be, or 0 if there is nowhere */
bool plat_update_begin(uint32_t size); /* false if there is nowhere for one that size */
bool plat_update_write(const uint8_t *data, size_t len);
void plat_update_abandon(void);
/* The image is whole and its digest right: 0 if it is firmware for this board and will run at the
 * next start, its version in `version`, or TERN_C_ERR_NOT_AN_IMAGE or TERN_C_ERR_NOT_NOW. */
uint8_t plat_update_finish(char *version, size_t version_len);

/* Whether this is the first start of an image an update gave, not yet known to work. */
bool plat_update_on_trial(void);
void plat_update_confirm(void); /* it works: keep it */
/* It cannot start: back to the firmware before, now. */
__attribute__((noreturn)) void plat_update_reject(void);

/* --- The system ------------------------------------------------------------------------------- */

__attribute__((noreturn)) void plat_restart(void);
void plat_sleep_ms(uint32_t ms);
void plat_yield(void); /* lets the platform's other tasks run, once a turn of the loop */
size_t
plat_stack_unused(void); /* the bytes of the node's stack never used so far, for 'selftest' */

/* The node: what the platform runs once it has started. Never returns. */
__attribute__((noreturn)) void node_main(void);

#endif
