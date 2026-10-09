#include "platform.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

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

/* The storage partition holds what NVS cannot read: on a board that ran other firmware before,
 * Meshtastic's among them, that firmware's files. Nothing in it is this firmware's, since anything
 * this firmware wrote would read, so it is wiped and the board starts again with it new. */
static int wipe_foreign(int err) {
    const struct flash_area *fa;
    if (wiped == WIPED) {
        wiped = 0;
        return err; /* wiped once already: not again */
    }
    printf("the storage holds what this firmware cannot read (%s), as after other firmware: "
           "wiping it, and starting again as a new board\n",
           plat_error_name(err));
    if (flash_area_open(PARTITION_ID(storage_partition), &fa) != 0 ||
        flash_area_erase(fa, 0, fa->fa_size) != 0) {
        return err;
    }
    flash_area_close(fa);
    wiped = WIPED;
    k_msleep(100);
    sys_reboot(SYS_REBOOT_WARM);
    return err;
}

static int store_open(void) {
    if (store_ready == 1) {
        store_ready = settings_subsys_init();
        if (store_ready != 0) {
            store_ready = wipe_foreign(store_ready);
        }
        wiped = 0;
        /* What an erase carried across its restart, put back. */
        if (store_ready == 0 && carried.magic == CARRIED && carried.check == carried_check() &&
            carried.len <= sizeof carried.data) {
            carried.key[sizeof carried.key - 1] = '\0';
            (void)plat_store_save(carried.key, carried.data, carried.len);
        }
        carried.magic = 0;
    }
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

void plat_bluetooth_starting(bool starting) { (void)starting; }

/* --- Updates: not over the link yet ----------------------------------------------------------- */

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
