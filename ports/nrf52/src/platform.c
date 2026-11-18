#include "platform.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <hal/nrf_power.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

#include "crumb.h"
#include "tern/companion.h"

/* The node's platform on an nRF52840 under Zephyr (../../node/platform.h): Zephyr's settings on
 * NVS in the flash's storage partition for what it saves, the chip's true generator, and Zephyr's
 * kernel. Firmware updates over the companion link are not here yet: the board is updated by
 * copying a .uf2 file onto it (README.md), and says so by naming no board in INFO. */

int main(void) { node_main(); }

/* --- Storage: Zephyr's settings, under "tern/" ----------------------------------------------- */

#define PREFIX "tern/"
#define KEY_MAX 32

/* Zephyr's errors are negative errnos. */
const char *plat_error_name(int code) {
    switch (-code) {
    case 0:
        return "ok";
    case ENOSPC:
        return "no room";
    case EIO:
        return "a flash error";
    case EINVAL:
        return "not valid";
    case ENOENT:
        return "not there";
    default:
        return "an error";
    }
}

static void full_key(char out[KEY_MAX], const char *key) {
    snprintf(out, KEY_MAX, PREFIX "%s", key);
}

/* What settings_load_subtree_direct() found under one key: the bytes, if they fit. */
struct found {
    void *buf;
    size_t cap;
    ssize_t len; /* -1 for none, or the length of the record */
};

static int on_found(const char *rest, size_t len, settings_read_cb read, void *read_arg,
                    void *param) {
    struct found *f = param;
    if (rest != NULL && rest[0] != '\0') {
        return 0; /* a key below this one, not this one */
    }
    f->len = (ssize_t)len;
    if (len <= f->cap && f->buf != NULL && read(read_arg, f->buf, len) != (ssize_t)len) {
        f->len = -1;
    }
    return 1; /* found: no more */
}

/* The record under `key`: its length, with up to `cap` bytes of it in buf, or -1 for none. */
static ssize_t find(const char *key, void *buf, size_t cap) {
    char k[KEY_MAX];
    struct found f = {.buf = buf, .cap = cap, .len = -1};
    full_key(k, key);
    if (settings_load_subtree_direct(k, on_found, &f) != 0) {
        return -1;
    }
    return f.len;
}

bool plat_store_load(const char *key, void *buf, size_t len) {
    return find(key, buf, len) == (ssize_t)len;
}

bool plat_store_save(const char *key, const void *buf, size_t len) {
    char k[KEY_MAX];
    full_key(k, key);
    return settings_save_one(k, buf, len) == 0;
}

bool plat_store_has(const char *key) { return find(key, NULL, 0) >= 0; }

static struct nvs_fs *store_fs(void) {
    void *storage = NULL;
    return settings_storage_get(&storage) == 0 ? storage : NULL;
}

/* A record carried across the restart an erase ends with, in RAM a warm restart keeps: the
 * settings cannot be used again until they start afresh on the erased flash. If the bootloader
 * cleared the RAM on the way, it is lost, and the board starts with none. */
static __noinit struct {
    uint32_t magic;
    char key[16];
    uint8_t len;
    uint8_t data[64];
    uint32_t check;
} carried;
#define CARRIED 0x4b454550u /* "KEEP" */

static uint32_t carried_check(void) {
    uint32_t h = 2166136261u; /* FNV-1a over what is carried */
    const uint8_t *p = (const uint8_t *)&carried;
    for (size_t i = offsetof(__typeof__(carried), key); i < offsetof(__typeof__(carried), check);
         i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

/* Erased whole, Bluetooth's bonds with it, so the old identity's keys cannot be read back out of
 * the flash: the storage partition wiped, and the board restarted, to start its settings afresh.
 * `keep` goes with it, in RAM. Returns only if the erase failed. */
static int erase_storage(const char *keep) {
    ssize_t len = keep != NULL ? find(keep, carried.data, sizeof carried.data) : -1;
    carried.magic = 0;
    if (len > 0 && (size_t)len <= sizeof carried.data && strlen(keep) < sizeof carried.key) {
        strcpy(carried.key, keep);
        carried.len = (uint8_t)len;
        carried.check = carried_check();
        carried.magic = CARRIED;
    }
    const struct flash_area *fa;
    int err = flash_area_open(PARTITION_ID(storage_partition), &fa);
    if (err != 0) {
        return err;
    }
    err = flash_area_erase(fa, 0, fa->fa_size);
    flash_area_close(fa);
    if (err != 0) {
        return err;
    }
    printf("erased, as the Reset page asked: restarting as a new board\n");
    k_msleep(100);
    sys_reboot(SYS_REBOOT_WARM);
    return -EIO;
}

static int store_ready = 1; /* 0 once started, or settings_subsys_init()'s error */

/* Set before restarting to start with the storage wiped, and cleared once it has: so a board whose
 * storage still will not start after that says so, rather than wiping it at every start. */
static __noinit uint32_t wiped;
#define WIPED 0x57495045u /* "WIPE" */

/* Whether the storage partition holds a LittleFS volume, which is how Meshtastic and MeshCore keep
 * their files on these boards. Its superblock has the name "littlefs" in a block: eight bytes in
 * for LittleFS 2, and forty for LittleFS 1 (a 16-byte directory header, then 24 of the
 * superblock's own before the name). Adafruit's nRF52 core, which Meshtastic builds on, uses
 * LittleFS 1 with 128-byte blocks, from 0xED000: 4 KB into this partition, so every 128 bytes is
 * looked at. */
static bool holds_littlefs(void) {
    static const off_t name_at[] = {8, 40};
    const struct flash_area *fa;
    bool found = false;
    if (flash_area_open(PARTITION_ID(storage_partition), &fa) != 0) {
        return false;
    }
    for (off_t at = 0; !found && at + 128 <= (off_t)fa->fa_size; at += 128) {
        for (size_t i = 0; !found && i < sizeof name_at / sizeof name_at[0]; i++) {
            char name[8];
            found = flash_area_read(fa, at + name_at[i], name, sizeof name) == 0 &&
                    memcmp(name, "littlefs", sizeof name) == 0;
        }
    }
    flash_area_close(fa);
    return found;
}

/* On a board that ran Meshtastic or MeshCore before, the storage partition holds that firmware's
 * files: wiped, and the board started again with it new. Only then, and only if the storage would
 * not start or holds no identity of this firmware's, so that nothing of Tern's is ever wiped for an
 * error; and only once, so a board whose storage still will not start says so. Returns only if it
 * did not wipe. */
static void wipe_foreign(int err) {
    const struct flash_area *fa;
    if (wiped == WIPED) {
        return; /* wiped once already: not again */
    }
    printf("the storage holds another firmware's files (%s): wiping them, and starting again as a "
           "new board\n",
           err != 0 ? plat_error_name(err) : "no identity of this firmware's");
    if (flash_area_open(PARTITION_ID(storage_partition), &fa) != 0) {
        return;
    }
    err = flash_area_erase(fa, 0, fa->fa_size);
    flash_area_close(fa);
    if (err == 0) {
        wiped = WIPED;
        k_msleep(100);
        sys_reboot(SYS_REBOOT_WARM);
    }
}

static int store_open(void) {
    if (store_ready != 1) {
        return store_ready;
    }
    bool foreign = holds_littlefs();
    store_ready = settings_subsys_init();
    if (foreign && (store_ready != 0 || find("identity", NULL, 0) < 0)) {
        wipe_foreign(store_ready);
    }
    wiped = 0;
    /* What an erase carried across its restart, put back. If it cannot be, the board does not
     * start: the time on the air is what holds it to the region's limit, so it is not started
     * without it. The record stays in RAM for the next start to try again. */
    if (store_ready == 0 && carried.magic == CARRIED && carried.check == carried_check() &&
        carried.len <= sizeof carried.data) {
        carried.key[sizeof carried.key - 1] = '\0';
        if (!plat_store_save(carried.key, carried.data, carried.len)) {
            store_ready = -EIO;
            return store_ready;
        }
    }
    carried.magic = 0;
    return store_ready;
}

enum plat_store_start plat_store_start(bool erase, const char *keep, int *code) {
    *code = store_open();
    if (*code != 0) {
        return PLAT_STORE_FAILED;
    }
    if (erase) {
        *code = erase_storage(keep);
        return PLAT_STORE_ERASE_FAILED;
    }
    return PLAT_STORE_OK;
}

/* The Reset page's asking, kept in the store itself: a record the erase takes with everything
 * else. Unlike the ESP32's, it outlasts the power going before the restart, and the board is
 * erased when it next starts. */
#define ERASE_KEY "erase"

void plat_erase_and_restart(void) {
    uint8_t yes = 1;
    if (store_open() == 0) {
        (void)plat_store_save(ERASE_KEY, &yes, sizeof yes);
    }
    sys_reboot(SYS_REBOOT_COLD);
    for (;;) {
    }
}

bool plat_erase_asked(void) { return store_open() == 0 && plat_store_has(ERASE_KEY); }

/* The messages share the storage with everything else the board must be able to save, and must
 * never be why one of those cannot be. The partition is 32 KB, of which NVS keeps one 4 KB sector
 * free to move records into. So the messages have MESSAGE_BYTES between them, by what each costs
 * in flash, and one more is saved only if that leaves SPARE_BYTES free besides. One that is not
 * saved is held until the board restarts. */
#define MESSAGE_BYTES 6144u
#define SPARE_BYTES 8192u
#define RECORD_COST 16 /* NVS's entry for a record, and the name the settings give it */

static uint16_t message_cost[LINK_MESSAGES];

static uint16_t cost_of(size_t len) {
    return len == 0 ? 0 : (uint16_t)((len + 7) / 8 * 8 + 2 * RECORD_COST);
}

static void message_key(char out[8], char kind, size_t place) {
    snprintf(out, 8, "%c%02u", kind, (unsigned)place);
}

size_t plat_message_load(size_t place, uint8_t *buf, size_t cap) {
    char key[8];
    message_key(key, 'm', place);
    ssize_t len = find(key, buf, cap);
    bool ok = len > 0 && (size_t)len <= cap;
    message_cost[place] = cost_of(ok ? (size_t)len : 0);
    return ok ? (size_t)len : 0;
}

enum link_saved plat_message_save(size_t place, const uint8_t *buf, size_t len) {
    char key[8], k[KEY_MAX];
    message_key(key, 'm', place);
    full_key(k, key);
    if (len == 0) {
        int err = settings_delete(k);
        if (err != 0 && err != -ENOENT) {
            return LINK_NOT_SAVED;
        }
        message_cost[place] = 0;
        return LINK_SAVED;
    }
    unsigned others = 0;
    for (size_t i = 0; i < LINK_MESSAGES; i++) {
        others += i == place ? 0 : message_cost[i];
    }
    struct nvs_fs *fs = store_fs();
    ssize_t free_bytes = fs != NULL ? nvs_calc_free_space(fs) : -1;
    if (free_bytes < 0) {
        return LINK_NOT_SAVED;
    }
    if (others + cost_of(len) > MESSAGE_BYTES || (size_t)free_bytes < SPARE_BYTES + cost_of(len)) {
        return LINK_NO_ROOM;
    }
    if (settings_save_one(k, buf, len) != 0) {
        return LINK_NOT_SAVED;
    }
    message_cost[place] = cost_of(len);
    return LINK_SAVED;
}

size_t plat_store_read(const char *key, void *buf, size_t cap) {
    ssize_t len = find(key, buf, cap);
    return len > 0 && (size_t)len <= cap ? (size_t)len : 0;
}

/* Only with SPARE_BYTES still free besides, as for a message. */
bool plat_store_save_if_room(const char *key, const void *buf, size_t len) {
    char k[KEY_MAX];
    full_key(k, key);
    if (len == 0) {
        int err = settings_delete(k);
        return err == 0 || err == -ENOENT;
    }
    struct nvs_fs *fs = store_fs();
    ssize_t free_bytes = fs != NULL ? nvs_calc_free_space(fs) : -1;
    if (free_bytes < 0 || (size_t)free_bytes < SPARE_BYTES + cost_of(len)) {
        return false;
    }
    return settings_save_one(k, buf, len) == 0;
}

bool plat_state_load(size_t place, uint64_t *state) {
    char key[8];
    message_key(key, 's', place);
    return plat_store_load(key, state, sizeof *state);
}

bool plat_state_save(size_t place, uint64_t state) {
    char key[8];
    message_key(key, 's', place);
    return plat_store_save(key, &state, sizeof state);
}

/* In bytes. */
bool plat_store_usage(unsigned *used, unsigned *total, unsigned *spare) {
    struct nvs_fs *fs = store_fs();
    ssize_t free_bytes = fs != NULL ? nvs_calc_free_space(fs) : -1;
    if (free_bytes < 0) {
        return false;
    }
    *total = (unsigned)(fs->sector_size * (fs->sector_count - 1));
    *spare = (unsigned)free_bytes;
    *used = *total > *spare ? *total - *spare : 0;
    return true;
}

/* --- Random numbers -------------------------------------------------------------------------- */

/* The nRF52840's RNG peripheral, a true generator, through Zephyr's entropy driver. */
bool plat_random(uint8_t *buf, size_t len) { return sys_csrand_get(buf, len) == 0; }

uint32_t plat_random32(void) {
    uint32_t r = 0;
    (void)plat_random((uint8_t *)&r, sizeof r);
    return r;
}

void plat_entropy_start(void) {}

void plat_bluetooth_starting(void) {}

void plat_bluetooth_started(bool started) { (void)started; }

size_t plat_heap_free(void) { return 0; } /* its table is static (prj.conf) */

/* --- Updates: not over the link yet ----------------------------------------------------------- */

/* The application's flash has room for one image and not two, so an update is not written beside
 * the running one: the Adafruit bootloader these boards come with, which keeps Nordic's SoftDevice
 * below the application for itself, takes it over Bluetooth instead (Nordic's legacy DFU), started
 * by the value its OTA mode is asked for with in GPREGRET. */
#define DFU_MAGIC_OTA_RESET 0xA8

/* Whether the bootloader takes an update over Bluetooth that a client can count on finishing: the
 * OTAFIX build of the Adafruit bootloader, which erases as the image arrives and, if a transfer
 * breaks, waits for another over Bluetooth. Its version, as its drive's INFO_UF2.TXT gives it, is
 * in its flash. The stock bootloader advertises the same service, but a T1000-E's left its START
 * unanswered, erasing the application, and came back from that in USB only: a node on one says it
 * cannot be updated, and a client offers USB. */
static bool bootloader_takes_bluetooth(void) {
#if DT_NODE_EXISTS(DT_NODELABEL(boot_partition))
    static const char mark[] = "OTAFIX";
    const uint8_t *at = (const uint8_t *)DT_REG_ADDR(DT_NODELABEL(boot_partition));
    size_t n = DT_REG_SIZE(DT_NODELABEL(boot_partition));
    for (size_t i = 0; i + sizeof mark - 1 <= n; i++) {
        if (memcmp(&at[i], mark, sizeof mark - 1) == 0) {
            return true;
        }
    }
#endif
    return false;
}

uint8_t plat_update_how(void) {
    static int how = -1;
    if (how < 0) {
        how = bootloader_takes_bluetooth() ? TERN_C_UPDATES_BOOTLOADER : TERN_C_UPDATES_NONE;
    }
    return (uint8_t)how;
}

void plat_update_by_bootloader(void) {
    nrf_power_gpregret_set(NRF_POWER, 0, DFU_MAGIC_OTA_RESET);
    sys_reboot(SYS_REBOOT_COLD);
    for (;;) {
    }
}

uint32_t plat_update_room(void) { return 0; }
bool plat_update_begin(uint32_t size) {
    (void)size;
    return false;
}
bool plat_update_write(const uint8_t *data, size_t len) {
    (void)data;
    (void)len;
    return false;
}
void plat_update_abandon(void) {}
uint8_t plat_update_finish(char *version, size_t version_len) {
    (void)version;
    (void)version_len;
    return TERN_C_ERR_NOT_NOW;
}
bool plat_update_on_trial(void) { return false; }
void plat_update_confirm(void) {}
void plat_update_reject(void) { plat_restart(); }

/* RESETREAS, read once and cleared, since the chip adds to it at each reset until it is (the
 * bootloader leaves it: Adafruit's src/main.c says the application is to clear it). */
static uint32_t reset_reason(void) {
    static uint32_t why;
    static bool read;
    if (!read) {
        why = nrf_power_resetreas_get(NRF_POWER);
        nrf_power_resetreas_clear(NRF_POWER, why);
        read = true;
    }
    return why;
}

/* The reset pin, and waking from System OFF by the button, are by hand, and so is power coming on,
 * which leaves RESETREAS empty. A reset the chip asked for itself (every restart here, the
 * bootloader's included), its watchdog and a lockup are not. If a bootloader cleared it on its
 * way, every start would read as power coming on: the window would open more often, not less. */
bool plat_started_by_hand(void) {
    uint32_t itself = NRF_POWER_RESETREAS_SREQ_MASK | NRF_POWER_RESETREAS_DOG_MASK |
                      NRF_POWER_RESETREAS_LOCKUP_MASK;
    return (reset_reason() & itself) == 0;
}

bool plat_started_by_watchdog(void) { return (reset_reason() & NRF_POWER_RESETREAS_DOG_MASK) != 0; }

/* --- The watchdog ---------------------------------------------------------------------------- */

/* The chip's WDT, counting while the CPU sleeps too (a loop blocked on a semaphore sleeps), but not
 * while a debugger has it halted. Once started it runs until a reset other than a soft one: on
 * through sys_reboot() and the bootloader, which feeds it while it waits for an update and between
 * the pages it erases (Adafruit's bootloader.c, wait_for_events()). Starting it again after a
 * restart only takes the channel the last start took: the timeout cannot change while it runs. */
static const struct device *const dog = DEVICE_DT_GET(DT_NODELABEL(wdt0));
static int dog_channel = -1;

void plat_watchdog_start(void) {
    if (dog_channel >= 0 || !device_is_ready(dog)) {
        return;
    }
    const struct wdt_timeout_cfg cfg = {.window = {.min = 0, .max = PLAT_WATCHDOG_S * 1000u},
                                        .flags = WDT_FLAG_RESET_SOC};
    int channel = wdt_install_timeout(dog, &cfg);
    if (channel < 0 || wdt_setup(dog, WDT_OPT_PAUSE_HALTED_BY_DBG) != 0) {
        printf("the watchdog did not start (%d)\n", channel);
        return;
    }
    dog_channel = channel;
}

void plat_watchdog_feed(void) {
    if (dog_channel >= 0) {
        (void)wdt_feed(dog, dog_channel);
    }
}

/* Kept in RAM Zephyr does not clear at a start, as `wiped` is. */
static __noinit struct crumb crumb;
struct crumb *plat_crumb(void) { return &crumb; }

/* --- The system ------------------------------------------------------------------------------ */

void plat_restart(void) {
    sys_reboot(SYS_REBOOT_COLD);
    for (;;) {
    }
}

void plat_sleep_ms(uint32_t ms) { k_msleep((int32_t)ms); }

void plat_yield(void) { k_msleep(1); }

size_t plat_stack_unused(void) {
    size_t unused = 0;
    (void)k_thread_stack_space_get(k_current_get(), &unused);
    return unused;
}
