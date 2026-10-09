#include "ble.h"

#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>

/* The companion link over Bluetooth LE on Zephyr's host and the nRF52840's controller: the same
 * service, pairing and frames as the ESP32's over NimBLE (../../esp32/main/ble.c), which
 * draft/companion.md, "Bluetooth LE", defines. Zephyr reports from its own threads; what it
 * reports is queued for the loop's ble_poll(), so link.c is only ever called from the loop. */

/* The draft's UUIDs, least significant byte first. */
#define TERN_UUID(n)                                                                               \
    BT_UUID_INIT_128(0x40, 0xff, 0x50, 0xdd, 0x41, 0x17, 0x9b, 0x88, 0x1c, 0x4c, 0x17, 0xeb, (n),  \
                     0x00, 0x28, 0x7a)

static const struct bt_uuid_128 service_uuid = TERN_UUID(0x01);
static const struct bt_uuid_128 to_node_uuid = TERN_UUID(0x02);
static const struct bt_uuid_128 from_node_uuid = TERN_UUID(0x03);

#define EVENTS 8
/* Frames waiting for a buffer: more than the largest sync (link.h). */
#define WAITING 128

K_MSGQ_DEFINE(events, sizeof(struct ble_event), EVENTS, 4);

static struct bt_conn *volatile conn;
static volatile uint32_t gen;    /* counts connections; the present one's */
static volatile bool paired;     /* the client on `conn` paired with a passkey */
static volatile bool subscribed; /* and asked for notifications */
static volatile uint32_t passkey_setting;
static volatile uint32_t shown_passkey; /* this pairing's, if the node chose it */
static bool have_screen;

static void post(const struct ble_event *e) {
    if (k_msgq_put(&events, e, K_NO_WAIT) != 0) {
        printf("bluetooth: an event was lost\n");
    }
}

static void post_kind(enum ble_event_kind kind, uint16_t mtu, uint32_t passkey) {
    struct ble_event e = {.kind = kind, .gen = gen, .mtu = mtu, .passkey = passkey};
    post(&e);
}

/* The loop's side: the connection the link has open, and the frames waiting to go to it. */
static uint32_t open_gen; /* 0: none */
static struct {
    uint8_t len;
    uint8_t frame[TERN_COMPANION_MAX_FRAME];
} waiting[WAITING];
static size_t first, n_waiting;

/* A client's write to the node: one frame. The characteristic asks for an encrypted link made by
 * LE Secure Connections with a passkey, which Zephyr enforces; it is refused whole if it is longer
 * than a frame, or comes in parts. */
static ssize_t on_write(struct bt_conn *c, const struct bt_gatt_attr *attr, const void *buf,
                        uint16_t len, uint16_t offset, uint8_t flags) {
    (void)attr;
    (void)flags;
    if (c != conn || !paired) {
        return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
    }
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    struct ble_event e = {.kind = BLE_FRAME, .gen = gen};
    if (len > sizeof e.frame) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    memcpy(e.frame, buf, len);
    e.len = len;
    post(&e);
    return len;
}

static void on_ccc(const struct bt_gatt_attr *attr, uint16_t value) {
    (void)attr;
    subscribed = value == BT_GATT_CCC_NOTIFY;
}

BT_GATT_SERVICE_DEFINE(tern_service, BT_GATT_PRIMARY_SERVICE(&service_uuid),
                       BT_GATT_CHARACTERISTIC(&to_node_uuid.uuid, BT_GATT_CHRC_WRITE,
                                              BT_GATT_PERM_WRITE_LESC, NULL, on_write, NULL),
                       BT_GATT_CHARACTERISTIC(&from_node_uuid.uuid, BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_NONE, NULL, NULL, NULL),
                       BT_GATT_CCC(on_ccc, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_LESC));

/* The value attribute of from_node: the service's declaration, to_node's two, then its own. */
#define FROM_NODE_ATTR (&tern_service.attrs[4])

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_UUID128_ALL, service_uuid.val, sizeof service_uuid.val),
};
/* What the scan response names the node: nothing of its address or its user's name for it. */
static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof CONFIG_BT_DEVICE_NAME - 1),
};

static void advertise(void) {
    int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err != 0 && err != -EALREADY) {
        printf("bluetooth: could not advertise (%d)\n", err);
    }
}

static void on_connected(struct bt_conn *c, uint8_t err) {
    if (err != 0) {
        return;
    }
    conn = bt_conn_ref(c);
    paired = subscribed = false;
    gen++;
    /* Ask for the pairing at once; a client that has a bond restores it instead. */
    if (bt_conn_set_security(c, BT_SECURITY_L4) != 0) {
        bt_conn_disconnect(c, BT_HCI_ERR_AUTH_FAIL);
    }
}

static void on_disconnected(struct bt_conn *c, uint8_t reason) {
    (void)reason;
    if (c != conn) {
        return;
    }
    if (paired) {
        post_kind(BLE_CLOSE, 0, 0);
    }
    paired = subscribed = false;
    bt_conn_unref(conn);
    conn = NULL;
}

/* The controller frees its connection after the disconnection is reported: advertising again then,
 * with the one connection there is to give. */
static void on_recycled(void) { advertise(); }

static void on_security(struct bt_conn *c, bt_security_t level, enum bt_security_err err) {
    if (c != conn) {
        return;
    }
    post_kind(BLE_PAIRED, 0, 0);
    struct bt_conn_info info;
    /* Encrypted by a key from LE Secure Connections with a passkey, at full length: anything less
     * is not a client that may drive the node. */
    if (err != BT_SECURITY_ERR_SUCCESS || level < BT_SECURITY_L4 ||
        bt_conn_get_info(c, &info) != 0 || info.security.enc_key_size < 16) {
        printf("bluetooth: a link without passkey pairing; dropped\n");
        bt_conn_disconnect(c, BT_HCI_ERR_AUTH_FAIL);
        return;
    }
    if (!paired) {
        paired = true;
        post_kind(BLE_OPEN, bt_gatt_get_mtu(c), 0);
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = on_connected,
    .disconnected = on_disconnected,
    .recycled = on_recycled,
    .security_changed = on_security,
};

static void on_mtu(struct bt_conn *c, uint16_t tx, uint16_t rx) {
    (void)rx;
    if (c == conn && paired) {
        post_kind(BLE_MTU, tx, 0);
    }
}

static struct bt_gatt_cb gatt_callbacks = {.att_mtu_updated = on_mtu};

/* A passkey from 0 to 999999, uniform, from the chip's true generator. */
static uint32_t random_passkey(void) {
    const uint32_t keys = 1000000, limit = UINT32_MAX - UINT32_MAX % keys;
    uint32_t r;
    do {
        sys_csrand_get(&r, sizeof r);
    } while (r >= limit);
    return r % keys;
}

/* The passkey for this pairing: the one set, or a new one to show on the screen. */
static uint32_t app_passkey(struct bt_conn *c) {
    (void)c;
    uint32_t key = passkey_setting;
    shown_passkey = BLE_PASSKEY_RANDOM;
    if (key == BLE_PASSKEY_RANDOM) {
        key = random_passkey();
        shown_passkey = key;
    }
    return key;
}

/* Only the node shows a passkey, and the client types it: Zephyr pairs as a display with no
 * keyboard, which with LE Secure Connections only is protection against a man in the middle. */
static void passkey_display(struct bt_conn *c, unsigned int passkey) {
    if (shown_passkey == BLE_PASSKEY_RANDOM) {
        return; /* the one set, which the user knows */
    }
    if (!have_screen) {
        printf("bluetooth: no passkey set and no screen to show one: not pairing. Set one over "
               "USB.\n");
        bt_conn_auth_cancel(c);
        return;
    }
    post_kind(BLE_PASSKEY, 0, passkey);
}

static void auth_cancel(struct bt_conn *c) { (void)c; }

static const struct bt_conn_auth_cb auth_callbacks = {
    .app_passkey = app_passkey,
    .passkey_display = passkey_display,
    .cancel = auth_cancel,
};

bool ble_start(uint32_t passkey, bool screen) {
    passkey_setting = passkey;
    have_screen = screen;
    if (bt_enable(NULL) != 0) {
        return false;
    }
    (void)settings_load_subtree("bt");
    if (bt_conn_auth_cb_register(&auth_callbacks) != 0) {
        bt_disable();
        return false;
    }
    bt_gatt_cb_register(&gatt_callbacks);
    advertise();
    return true;
}

void ble_passkey(uint32_t passkey) { passkey_setting = passkey; }

/* Sends what is waiting, oldest first, until Zephyr is out of buffers; the rest goes on a later
 * call. What was queued for a connection that has since gone is dropped. */
static void pump(void) {
    if (open_gen == 0 || open_gen != gen) {
        n_waiting = 0;
        return;
    }
    struct bt_conn *c = conn;
    while (n_waiting > 0 && c != NULL && paired && subscribed) {
        int err = bt_gatt_notify(c, FROM_NODE_ATTR, waiting[first].frame, waiting[first].len);
        if (err == -ENOMEM || err == -EAGAIN || err == -ENOBUFS) {
            return;
        }
        if (err != 0) {
            printf("bluetooth: a frame to the client was lost (%d)\n", err);
        }
        first = (first + 1) % WAITING;
        n_waiting--;
    }
}

bool ble_poll(struct ble_event *e) {
    pump();
    while (k_msgq_get(&events, e, K_NO_WAIT) == 0) {
        if (e->kind == BLE_OPEN) {
            open_gen = e->gen;
            n_waiting = 0;
            return true;
        }
        if (e->gen != open_gen) {
            /* From a connection the link no longer has: a frame written just before it went, or
             * a passkey for one that is pairing, which is the present connection's. */
            if (e->kind != BLE_PASSKEY && e->kind != BLE_PAIRED) {
                continue;
            }
            return true;
        }
        if (e->kind == BLE_CLOSE) {
            open_gen = 0;
            n_waiting = 0;
        }
        return true;
    }
    return false;
}

void ble_send(const uint8_t *frame, size_t len) {
    if (open_gen == 0 || len > TERN_COMPANION_MAX_FRAME) {
        return;
    }
    if (n_waiting == WAITING) {
        printf("bluetooth: too many frames waiting; one to the client was lost\n");
        return;
    }
    size_t i = (first + n_waiting) % WAITING;
    memcpy(waiting[i].frame, frame, len);
    waiting[i].len = (uint8_t)len;
    n_waiting++;
    pump();
}

/* bt_unpair() with any address forgets every bond, and drops the client connected. */
void ble_forget(void) { (void)bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY); }

static void count_bond(const struct bt_bond_info *info, void *n) {
    (void)info;
    (*(unsigned *)n)++;
}

unsigned ble_paired(void) {
    unsigned n = 0;
    bt_foreach_bond(BT_ID_DEFAULT, count_bond, &n);
    return n;
}
