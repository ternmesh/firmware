#ifndef HELTEC_V3_BLE_H
#define HELTEC_V3_BLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tern/companion.h"

/* The companion link over Bluetooth LE (draft/companion.md, "Bluetooth LE"): one GATT service,
 * a characteristic a client writes frames to and one the node notifies them on, behind LE Secure
 * Connections passkey pairing.
 *
 * NimBLE runs in a task of its own. Everything it reports comes to main.c's loop through
 * ble_poll(), so link.c is only ever called from that loop, as it is for the USB port. A client
 * is a connection for the link once its link is encrypted by a pairing with a passkey, never
 * before: until then nothing is read from it or sent to it. */

#define BLE_PASSKEY_RANDOM 0xFFFFFFFFu /* a new passkey for each pairing, shown on the screen */

enum ble_event_kind {
    BLE_OPEN,    /* a paired client: `mtu` is its ATT MTU */
    BLE_MTU,     /* its ATT MTU changed to `mtu` */
    BLE_FRAME,   /* a frame it wrote */
    BLE_CLOSE,   /* it went */
    BLE_PASSKEY, /* pairing: show `passkey` until BLE_PAIRED or BLE_CLOSE */
    BLE_PAIRED,  /* pairing finished, whether or not it succeeded */
};

struct ble_event {
    enum ble_event_kind kind;
    uint32_t gen; /* which connection, counted from 1: ble_poll() drops a past one's */
    uint16_t mtu;
    uint32_t passkey;
    size_t len;
    uint8_t frame[TERN_COMPANION_MAX_FRAME];
};

/* Starts the controller and the host, and advertises. `passkey` is as for ble_passkey(); a
 * passkey that is random needs `screen`, or the node does not pair. False if Bluetooth did not
 * start, and then the controller, and the radio with it, is off. */
bool ble_start(uint32_t passkey, bool screen);

/* The passkey for pairings from now: 0 to 999999, or BLE_PASSKEY_RANDOM. */
void ble_passkey(uint32_t passkey);

/* The next thing NimBLE reported about the present connection, if there is one. It also sends
 * what ble_send() queued, as far as NimBLE has room, so call it often. */
bool ble_poll(struct ble_event *e);

/* One frame to the client, as one notification: queued, in order, until NimBLE has a buffer for
 * it, since a sync sends far more frames at once than NimBLE has buffers. */
void ble_send(const uint8_t *frame, size_t len);

/* Forgets every bonded client, and drops the one connected. */
void ble_forget(void);

#endif
