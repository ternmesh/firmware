#include "platform.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "bootloader_random.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "tern/companion.h"

/* The node's platform on an ESP32 (../../node/platform.h): NVS for what it saves, the chip's
 * generator, ESP-IDF's two firmware slots, and FreeRTOS. */

void app_main(void) { node_main(); }

/* --- Storage: one NVS namespace --------------------------------------------------------------- */

#define NAMESPACE "tern"

const char *plat_error_name(int code) { return esp_err_to_name(code); }

bool plat_store_load(const char *key, void *buf, size_t len) {
    nvs_handle_t h;
    size_t got = len;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_get_blob(h, key, buf, &got) == ESP_OK && got == len;
    nvs_close(h);
    return ok;
}

bool plat_store_save(const char *key, const void *buf, size_t len) {
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_blob(h, key, buf, len) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool plat_store_has(const char *key) {
    nvs_handle_t h;
    size_t len = 0;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_get_blob(h, key, NULL, &len) == ESP_OK;
    nvs_close(h);
    return ok;
}

size_t plat_store_read(const char *key, void *buf, size_t cap) {
    nvs_handle_t h;
    size_t len = cap;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    bool ok = nvs_get_blob(h, key, buf, &len) == ESP_OK;
    nvs_close(h);
    return ok ? len : 0;
}

/* Erases the whole of the flash's NVS partition, as the Reset page asked: the identity, the
 * sessions, contacts, groups, messages, settings and message ids, and the Bluetooth bonds NimBLE
 * keeps there too. Erased, not marked deleted, so the old identity's keys cannot be read back out
 * of the flash. `keep` is put back: the time on the air, so the region's limit on transmitting
 * does not start again because the board did. `e` is what nvs_flash_init() gave; returns what it
 * gives after. */
static esp_err_t erase_storage(esp_err_t e, const char *keep) {
    uint8_t kept[64];
    size_t kept_len = sizeof kept;
    bool had = false;
    if (e == ESP_OK && keep != NULL) {
        nvs_handle_t h;
        if (nvs_open(NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
            had = nvs_get_blob(h, keep, kept, &kept_len) == ESP_OK;
            nvs_close(h);
        }
    }
    if (e == ESP_OK) {
        (void)nvs_flash_deinit();
    }
    e = nvs_flash_erase();
    if (e == ESP_OK) {
        e = nvs_flash_init();
    }
    if (e == ESP_OK && had && !plat_store_save(keep, kept, kept_len)) {
        e = ESP_FAIL; /* not starting with it forgotten */
    }
    return e;
}

enum plat_store_start plat_store_start(bool erase, const char *keep, int *code) {
    esp_err_t e = nvs_flash_init();
    if (erase) {
        e = erase_storage(e, keep);
        *code = e;
        if (e != ESP_OK) {
            return PLAT_STORE_ERASE_FAILED;
        }
    }
    *code = e;
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        return PLAT_STORE_NEEDS_ERASE;
    }
    return e == ESP_OK ? PLAT_STORE_OK : PLAT_STORE_FAILED;
}

/* The messages share the flash's storage with the identity, the sessions and everything else
 * the board must be able to save, and must never be why one of those cannot be. The storage has
 * 630 entries of 32 bytes to use. A board with nothing but a session and a contact uses 180; one
 * with all eight sessions, its groups and three phones bonded would use about 420, and writing
 * its largest record again wants 35 more. So the messages have MESSAGE_ENTRIES between them
 * whatever else is there, and one more is saved only if it fits in that, with MESSAGE_SPARE
 * still free besides. One that is not saved is held until the board restarts. */
#define MESSAGE_ENTRIES 160
#define MESSAGE_SPARE 40

/* What each place's message takes in flash: an entry for every 32 bytes, two that say what it is,
 * and one for its word. */
static uint16_t message_entries[LINK_MESSAGES];

static uint16_t entries_for(size_t len) { return len == 0 ? 0 : (uint16_t)((len + 31) / 32 + 3); }

size_t plat_message_load(size_t place, uint8_t *buf, size_t cap) {
    nvs_handle_t h;
    char key[8];
    size_t len = cap;
    snprintf(key, sizeof key, "m%02u", (unsigned)place);
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    bool ok = nvs_get_blob(h, key, buf, &len) == ESP_OK;
    nvs_close(h);
    message_entries[place] = entries_for(ok ? len : 0);
    return ok ? len : 0;
}

enum link_saved plat_message_save(size_t place, const uint8_t *buf, size_t len) {
    nvs_handle_t h;
    nvs_stats_t stats;
    char key[8];
    snprintf(key, sizeof key, "m%02u", (unsigned)place);
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return LINK_NOT_SAVED;
    }
    unsigned others = 0;
    for (size_t i = 0; i < LINK_MESSAGES; i++) {
        others += i == place ? 0 : message_entries[i];
    }
    esp_err_t e;
    bool full = false; /* by the board's own count, which taking older messages out mends */
    if (len == 0) {
        e = nvs_erase_key(h, key);
        if (e == ESP_ERR_NVS_NOT_FOUND) {
            e = ESP_OK;
        }
    } else if (nvs_get_stats(NULL, &stats) != ESP_OK) {
        e = ESP_FAIL;
    } else if (others + entries_for(len) > MESSAGE_ENTRIES ||
               stats.available_entries < MESSAGE_SPARE + entries_for(len)) {
        e = ESP_ERR_NVS_NOT_ENOUGH_SPACE;
        full = true;
    } else {
        e = nvs_set_blob(h, key, buf, len);
    }
    bool ok = e == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) {
        message_entries[place] = entries_for(len);
    }
    return ok ? LINK_SAVED : full ? LINK_NO_ROOM : LINK_NOT_SAVED;
}

/* Only with MESSAGE_SPARE entries still free besides. */
bool plat_store_save_if_room(const char *key, const void *buf, size_t len) {
    nvs_handle_t h;
    nvs_stats_t stats;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t e;
    if (len == 0) {
        e = nvs_erase_key(h, key);
        if (e == ESP_ERR_NVS_NOT_FOUND) {
            e = ESP_OK;
        }
    } else if (nvs_get_stats(NULL, &stats) != ESP_OK ||
               stats.available_entries < MESSAGE_SPARE + entries_for(len)) {
        e = ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    } else {
        e = nvs_set_blob(h, key, buf, len);
    }
    bool ok = e == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool plat_state_load(size_t place, uint64_t *state) {
    nvs_handle_t h;
    char key[8];
    snprintf(key, sizeof key, "s%02u", (unsigned)place);
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_get_u64(h, key, state) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool plat_state_save(size_t place, uint64_t state) {
    nvs_handle_t h;
    char key[8];
    snprintf(key, sizeof key, "s%02u", (unsigned)place);
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_u64(h, key, state) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool plat_store_usage(unsigned *used, unsigned *total, unsigned *spare) {
    nvs_stats_t stored;
    if (nvs_get_stats(NULL, &stored) != ESP_OK) {
        return false;
    }
    *used = (unsigned)stored.used_entries;
    *total = (unsigned)stored.total_entries;
    *spare = (unsigned)stored.available_entries;
    return true;
}

/* Asked for on the Reset page, and done as the board next starts, before anything has opened the
 * flash, so nothing is using it: kept through esp_restart(), and lost with the asking if the power
 * goes first. */
static RTC_NOINIT_ATTR uint32_t erase_asked;
#define ERASE_ASKED 0x45524153u /* "ERAS"; anything else, as at power on, is not asking */

void plat_erase_and_restart(void) {
    erase_asked = ERASE_ASKED;
    esp_restart();
}

bool plat_erase_asked(void) {
    bool asked = erase_asked == ERASE_ASKED;
    erase_asked = 0; /* once: a board that fails while erasing starts as it is the next time */
    return asked;
}

/* --- Random numbers --------------------------------------------------------------------------- */

/* The ESP32's generator, which is a true one while its entropy source is on. Espressif's
 * documentation for it (ESP-IDF, "Random Number Generation", ESP32-S3) says it gives true random
 * numbers while Wi-Fi or Bluetooth is on, or while the noise source bootloader_random_enable()
 * turns on is on; that the noise source must be turned off before Bluetooth is used; and that it
 * shares the SAR ADC, which nothing else may use while it is on. So it is turned on once the node's
 * first readings of the battery are done (plat_entropy_start()), before the identity and the
 * router's seed are made from it, and off as Bluetooth starts; nothing is made from the generator
 * between the two, and it is on again if Bluetooth does not start. Either way, every key made after
 * this, a first contact's included, draws on a true source. */
bool plat_random(uint8_t *buf, size_t len) {
    esp_fill_random(buf, len);
    return true;
}

uint32_t plat_random32(void) {
    uint32_t r;
    (void)plat_random((uint8_t *)&r, sizeof r);
    return r;
}

void plat_entropy_start(void) { bootloader_random_enable(); }

void plat_bluetooth_starting(bool starting) {
    if (starting) {
        bootloader_random_disable();
    } else {
        bootloader_random_enable();
    }
}

/* --- Updates ---------------------------------------------------------------------------------- */

/* The flash holds two slots for the firmware (partitions.csv), and an update is written to the
 * one not running, as it arrives, erasing as it goes so that no request waits for a whole slot's
 * erase. A board on the old layout, one slot and no otadata, has nowhere to write one: its INFO
 * names no board, and it is updated over USB (README.md). */
static const esp_partition_t *update_slot;
static esp_ota_handle_t update_handle;
static bool update_open;

uint32_t plat_update_room(void) {
    const esp_partition_t *spare = esp_ota_get_next_update_partition(NULL);
    return spare != NULL ? spare->size : 0;
}

void plat_update_abandon(void) {
    if (update_open) {
        esp_ota_abort(update_handle);
        update_open = false;
    }
}

bool plat_update_begin(uint32_t size) {
    plat_update_abandon();
    update_slot = esp_ota_get_next_update_partition(NULL);
    if (update_slot == NULL || size > update_slot->size ||
        esp_ota_begin(update_slot, OTA_WITH_SEQUENTIAL_WRITES, &update_handle) != ESP_OK) {
        return false;
    }
    update_open = true;
    return true;
}

bool plat_update_write(const uint8_t *data, size_t len) {
    if (!update_open || esp_ota_write(update_handle, data, len) != ESP_OK) {
        plat_update_abandon();
        return false;
    }
    return true;
}

/* It runs if ESP-IDF finds it an image for this chip, its own checksum and hash right, and it is
 * this firmware's, for this board, and not another project's: the project is named for the board
 * (../CMakeLists.txt). */
uint8_t plat_update_finish(char *version, size_t version_len) {
    if (!update_open) {
        return TERN_C_ERR_NOT_THERE;
    }
    update_open = false;
    esp_app_desc_t desc;
    if (esp_ota_end(update_handle) != ESP_OK ||
        esp_ota_get_partition_description(update_slot, &desc) != ESP_OK ||
        strncmp(desc.project_name, esp_app_get_description()->project_name,
                sizeof desc.project_name) != 0) {
        return TERN_C_ERR_NOT_AN_IMAGE;
    }
    if (esp_ota_set_boot_partition(update_slot) != ESP_OK) {
        return TERN_C_ERR_NOT_NOW;
    }
    snprintf(version, version_len, "%s", desc.version);
    return 0;
}

/* ESP-IDF's bootloader goes back to the image before if one an update gave restarts unconfirmed
 * (partitions.csv). */
bool plat_update_on_trial(void) {
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

void plat_update_confirm(void) { (void)esp_ota_mark_app_valid_cancel_rollback(); }

void plat_update_reject(void) {
    (void)esp_ota_mark_app_invalid_rollback_and_reboot();
    esp_restart(); /* not reached unless there was nothing to go back to */
}

/* --- The system ------------------------------------------------------------------------------- */

void plat_restart(void) { esp_restart(); }

void plat_sleep_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

void plat_yield(void) { vTaskDelay(1); }

size_t plat_stack_unused(void) { return uxTaskGetStackHighWaterMark(NULL); }
