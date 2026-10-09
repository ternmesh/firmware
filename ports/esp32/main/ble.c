#include "ble.h"

#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

void ble_store_config_init(void); /* NimBLE's bond store in NVS, which it declares nowhere */

/* The draft's UUIDs, least significant byte first, as NimBLE takes them. */
#define TERN_UUID(n)                                                                               \
    BLE_UUID128_INIT(0x40, 0xff, 0x50, 0xdd, 0x41, 0x17, 0x9b, 0x88, 0x1c, 0x4c, 0x17, 0xeb, (n),  \
                     0x00, 0x28, 0x7a)

static const ble_uuid128_t service_uuid = TERN_UUID(0x01);
static const ble_uuid128_t to_node_uuid = TERN_UUID(0x02);
static const ble_uuid128_t from_node_uuid = TERN_UUID(0x03);

/* What the advertisement names the node: nothing of its address or its user's name for it. */
#define NAME "Tern"
#define EVENTS 8
/* Frames waiting for a NimBLE buffer: more than the largest sync, which is SELF, every contact,
 * message and neighbour, AIRTIME, POWER and SYNCED (link.h). */
#define WAITING 128

static QueueHandle_t events;
static uint16_t from_node_handle;
static volatile uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static volatile uint32_t gen;    /* counts connections; the present one's, set in NimBLE's task */
static volatile bool paired;     /* the client on `conn` paired with a passkey */
static volatile bool subscribed; /* and asked for notifications */
static volatile uint32_t passkey_setting;
static bool have_screen;
static uint8_t own_addr_type;

static void post(const struct ble_event *e) {
    if (xQueueSend(events, e, 0) != pdTRUE) {
        printf("bluetooth: an event was lost\n");
    }
}

static void post_kind(enum ble_event_kind kind, uint16_t mtu, uint32_t passkey) {
    struct ble_event e = {.kind = kind, .gen = gen, .mtu = mtu, .passkey = passkey};
    post(&e);
}

/* The main loop's side: the connection the link has open, and the frames waiting to go to it. */
static uint32_t open_gen; /* 0: none */
static struct {
    uint8_t len;
    uint8_t frame[TERN_COMPANION_MAX_FRAME];
} waiting[WAITING];
static size_t first, n_waiting;

/* A client's write to the node: one frame. Refused until it has paired (the characteristic asks
 * for an encrypted, authenticated link, which NimBLE enforces), and refused whole if it is longer
 * than a frame. */
static int on_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                     void *arg) {
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (conn_handle != conn || !paired) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    struct ble_event e = {.kind = BLE_FRAME, .gen = gen};
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > sizeof e.frame) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (ble_hs_mbuf_to_flat(ctxt->om, e.frame, sizeof e.frame, &len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    e.len = len;
    post(&e);
    return 0;
}

/* Notifications go only to a client that paired and subscribed. */
static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &to_node_uuid.u,
                    .access_cb = on_access,
                    .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
                             BLE_GATT_CHR_F_WRITE_AUTHEN,
                },
                {
                    .uuid = &from_node_uuid.u,
                    .access_cb = on_access,
                    .val_handle = &from_node_handle,
                    .flags = BLE_GATT_CHR_F_NOTIFY,
                },
                {0},
            },
    },
    {0},
};

static void advertise(void);

/* A passkey from 0 to 999999, uniform: the chip's generator gives true random numbers while
 * Bluetooth is on (main.c, start_bluetooth()). */
static uint32_t random_passkey(void) {
    const uint32_t keys = 1000000, limit = UINT32_MAX - UINT32_MAX % keys;
    uint32_t r;
    do {
        r = esp_random();
    } while (r >= limit);
    return r % keys;
}

static int on_gap(struct ble_gap_event *event, void *arg) {
    (void)arg;
    struct ble_gap_conn_desc desc;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            advertise();
            return 0;
        }
        conn = event->connect.conn_handle;
        paired = subscribed = false;
        gen++;
        /* Ask for the pairing at once; a client that has a bond restores it instead. */
        ble_gap_security_initiate(conn);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        if (paired) {
            post_kind(BLE_CLOSE, 0, 0);
        }
        conn = BLE_HS_CONN_HANDLE_NONE;
        paired = subscribed = false;
        advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        if (event->passkey.params.action != BLE_SM_IOACT_DISP) {
            /* Only the node shows a passkey: anything else would pair without one. */
            ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_AUTH_FAIL);
            return 0;
        }
        uint32_t key = passkey_setting;
        bool shown = key == BLE_PASSKEY_RANDOM;
        if (shown && !have_screen) {
            printf("bluetooth: no passkey set and no screen to show one: not pairing. Set one "
                   "over USB.\n");
            ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_AUTH_FAIL);
            return 0;
        }
        if (shown) {
            key = random_passkey();
        }
        struct ble_sm_io io = {.action = BLE_SM_IOACT_DISP, .passkey = key};
        ble_sm_inject_io(event->passkey.conn_handle, &io);
        if (shown) {
            post_kind(BLE_PASSKEY, 0, key);
        }
        return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        post_kind(BLE_PAIRED, 0, 0);
        if (event->enc_change.status != 0 ||
            ble_gap_conn_find(event->enc_change.conn_handle, &desc) != 0) {
            ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        /* Encrypted by a key from a pairing with a passkey, and kept: anything less is not a
         * client that may drive the node. */
        if (!desc.sec_state.encrypted || !desc.sec_state.authenticated || !desc.sec_state.bonded ||
            desc.sec_state.key_size < 16) {
            printf("bluetooth: a link without passkey pairing; dropped\n");
            ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_AUTH_FAIL);
            return 0;
        }
        if (!paired) {
            paired = true;
            post_kind(BLE_OPEN, ble_att_mtu(event->enc_change.conn_handle), 0);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        if (paired && event->mtu.conn_handle == conn) {
            post_kind(BLE_MTU, event->mtu.value, 0);
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == from_node_handle) {
            subscribed = event->subscribe.cur_notify;
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* A bonded client that lost its keys pairs again, with a passkey like any other. */
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    default:
        return 0;
    }
}

static void advertise(void) {
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    fields.name = (const uint8_t *)NAME;
    fields.name_len = sizeof NAME - 1;
    fields.name_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields) != 0) {
        printf("bluetooth: could not set the advertisement\n");
        return;
    }
    struct ble_gap_adv_params params = {.conn_mode = BLE_GAP_CONN_MODE_UND,
                                        .disc_mode = BLE_GAP_DISC_MODE_GEN};
    int rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, on_gap, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        printf("bluetooth: could not advertise (%d)\n", rc);
    }
}

static void on_sync(void) {
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        printf("bluetooth: no address to advertise from\n");
        return;
    }
    advertise();
}

static void on_reset(int reason) { printf("bluetooth: the host reset (%d)\n", reason); }

static void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool ble_start(uint32_t passkey, bool screen) {
    passkey_setting = passkey;
    have_screen = screen;
    events = xQueueCreate(EVENTS, sizeof(struct ble_event));
    if (events == NULL || nimble_port_init() != ESP_OK) {
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    /* The node shows a passkey and the client types it: LE Secure Connections, with protection
     * against a man in the middle, and the keys kept. */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_sc_only = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(services) != 0 || ble_gatts_add_svcs(services) != 0 ||
        ble_svc_gap_device_name_set(NAME) != 0) {
        nimble_port_deinit(); /* the controller off again, as ble_start() promises */
        return false;
    }
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    return true;
}

void ble_passkey(uint32_t passkey) { passkey_setting = passkey; }

/* Sends what is waiting, oldest first, until NimBLE is out of buffers; the rest goes on a later
 * call. What was queued for a connection that has since gone is dropped. */
static void pump(void) {
    if (open_gen == 0 || open_gen != gen) {
        n_waiting = 0;
        return;
    }
    while (n_waiting > 0 && paired && subscribed) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(waiting[first].frame, waiting[first].len);
        if (om == NULL) {
            return;
        }
        int rc = ble_gatts_notify_custom(conn, from_node_handle, om); /* om is taken either way */
        if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) {
            return;
        }
        if (rc != 0) {
            printf("bluetooth: a frame to the client was lost (%d)\n", rc);
        }
        first = (first + 1) % WAITING;
        n_waiting--;
    }
}

bool ble_poll(struct ble_event *e) {
    pump();
    while (events != NULL && xQueueReceive(events, e, 0)) {
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

void ble_forget(void) {
    uint16_t c = conn;
    if (c != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
    }
    ble_store_clear();
}

unsigned ble_paired(void) {
    int n = 0;
    if (ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &n) != 0 || n < 0) {
        return 0;
    }
    return (unsigned)n;
}
