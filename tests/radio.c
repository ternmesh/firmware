#include "tern/radio.h"

#include <string.h>

#include "check.h"
#include "tern/err.h"

/* A port that does nothing but count what it was asked to do and hand back events the test
 * queues. It is the shape every real port takes, without the hardware. */
struct fake {
    int configures, transmits, receives, standbys;
    struct tern_radio_config last_cfg;
    uint8_t last_frame[255];
    uint8_t last_len;
    struct tern_radio_event queue[4];
    int queued, next;
};

static int fake_configure(void *ctx, const struct tern_radio_config *cfg) {
    struct fake *f = ctx;
    f->configures++;
    f->last_cfg = *cfg;
    return TERN_OK;
}

static int fake_transmit(void *ctx, const uint8_t *frame, uint8_t len) {
    struct fake *f = ctx;
    f->transmits++;
    memcpy(f->last_frame, frame, len);
    f->last_len = len;
    return TERN_OK;
}

static int fake_receive(void *ctx) {
    ((struct fake *)ctx)->receives++;
    return TERN_OK;
}

static int fake_standby(void *ctx) {
    ((struct fake *)ctx)->standbys++;
    return TERN_OK;
}

static int fake_poll(void *ctx, struct tern_radio_event *ev) {
    struct fake *f = ctx;
    if (f->next == f->queued) {
        return 0;
    }
    *ev = f->queue[f->next++];
    return 1;
}

static const struct tern_radio_ops fake_ops = {
    .configure = fake_configure,
    .transmit = fake_transmit,
    .receive = fake_receive,
    .standby = fake_standby,
    .poll = fake_poll,
};

static struct tern_radio_config good_config(void) {
    return (struct tern_radio_config){
        .mod = tern_lora_default(9, 500000),
        .freq_hz = 915000000,
        .tx_power_dbm = 14,
        .sync_word = 0x5E,
    };
}

static void configure_passes_good_settings_through(void) {
    struct fake f = {0};
    struct tern_radio r = {&fake_ops, &f};
    struct tern_radio_config cfg = good_config();
    CHECK_EQ_I64(tern_radio_configure(&r, &cfg), TERN_OK);
    CHECK_EQ_I64(f.configures, 1);
    CHECK_EQ_I64(f.last_cfg.freq_hz, 915000000);
    CHECK_EQ_I64(f.last_cfg.mod.sf, 9);
}

static void configure_rejects_bad_settings_before_the_port(void) {
    struct fake f = {0};
    struct tern_radio r = {&fake_ops, &f};
    struct tern_radio_config cfg = good_config();
    cfg.mod.sf = 13;
    CHECK_EQ_I64(tern_radio_configure(&r, &cfg), TERN_EINVAL);
    cfg = good_config();
    cfg.mod.bw_hz = 0;
    CHECK_EQ_I64(tern_radio_configure(&r, &cfg), TERN_EINVAL);
    cfg = good_config();
    cfg.freq_hz = 0;
    CHECK_EQ_I64(tern_radio_configure(&r, &cfg), TERN_EINVAL);
    CHECK_EQ_I64(tern_radio_configure(&r, NULL), TERN_EINVAL);
    CHECK_EQ_I64(f.configures, 0);
}

static void transmit_takes_one_to_255_bytes(void) {
    struct fake f = {0};
    struct tern_radio r = {&fake_ops, &f};
    uint8_t frame[256] = {0xA5, 0x5A};
    CHECK_EQ_I64(tern_radio_transmit(&r, frame, 0), TERN_EINVAL);
    CHECK_EQ_I64(tern_radio_transmit(&r, frame, 256), TERN_EINVAL);
    CHECK_EQ_I64(tern_radio_transmit(&r, NULL, 2), TERN_EINVAL);
    CHECK_EQ_I64(f.transmits, 0);
    CHECK_EQ_I64(tern_radio_transmit(&r, frame, 2), TERN_OK);
    CHECK_EQ_I64(tern_radio_transmit(&r, frame, 255), TERN_OK);
    CHECK_EQ_I64(f.transmits, 2);
    CHECK_EQ_I64(f.last_len, 255);
    CHECK_EQ_I64(f.last_frame[0], 0xA5);
}

static void poll_hands_back_events_in_order(void) {
    struct fake f = {0};
    struct tern_radio r = {&fake_ops, &f};
    static const uint8_t payload[] = {1, 2, 3};
    f.queue[0] = (struct tern_radio_event){.kind = TERN_RADIO_TX_DONE, .at = TERN_MS(10)};
    f.queue[1] = (struct tern_radio_event){.kind = TERN_RADIO_RX_DONE,
                                           .at = TERN_MS(25),
                                           .data = payload,
                                           .len = 3,
                                           .rssi_dbm = -112,
                                           .snr_cdb = -725};
    f.queued = 2;

    struct tern_radio_event ev;
    CHECK_EQ_I64(tern_radio_poll(&r, &ev), 1);
    CHECK_EQ_I64(ev.kind, TERN_RADIO_TX_DONE);
    CHECK_EQ_I64(tern_radio_poll(&r, &ev), 1);
    CHECK_EQ_I64(ev.kind, TERN_RADIO_RX_DONE);
    CHECK_EQ_I64(ev.len, 3);
    CHECK_EQ_I64(ev.data[2], 3);
    CHECK_EQ_I64(ev.snr_cdb, -725);
    CHECK_EQ_I64(tern_radio_poll(&r, &ev), 0);
    CHECK_EQ_I64(tern_radio_poll(&r, NULL), TERN_EINVAL);
}

static void receive_and_standby_reach_the_port(void) {
    struct fake f = {0};
    struct tern_radio r = {&fake_ops, &f};
    CHECK_EQ_I64(tern_radio_receive(&r), TERN_OK);
    CHECK_EQ_I64(tern_radio_standby(&r), TERN_OK);
    CHECK_EQ_I64(f.receives, 1);
    CHECK_EQ_I64(f.standbys, 1);
}

static int fake_receiving(void *ctx) {
    (void)ctx;
    return 1;
}

/* A port that cannot tell whether it is receiving never holds a frame back. */
static void receiving_is_asked_of_a_port_that_can_tell(void) {
    struct fake f = {0};
    struct tern_radio r = {&fake_ops, &f};
    CHECK_EQ_I64(tern_radio_receiving(&r), 0);
    struct tern_radio_ops can = fake_ops;
    can.receiving = fake_receiving;
    struct tern_radio r2 = {&can, &f};
    CHECK_EQ_I64(tern_radio_receiving(&r2), 1);
}

/* The values in use, as verified from source for MSH-28 (Research 02), and the two proposed for
 * Tern. */
static void sync_word_sx126x_encoding(void) {
    CHECK_EQ_I64(tern_sync_word_sx126x(0x2B), 0x24B4); /* Meshtastic */
    CHECK_EQ_I64(tern_sync_word_sx126x(0x12), 0x1424); /* MeshCore, RNode, LoRaWAN private */
    CHECK_EQ_I64(tern_sync_word_sx126x(0x34), 0x3444); /* LoRaWAN public */
    CHECK_EQ_I64(tern_sync_word_sx126x(0x5E), 0x54E4); /* proposed for Tern */
    CHECK_EQ_I64(tern_sync_word_sx126x(0x67), 0x6474); /* proposed alternate */
}

int main(void) {
    RUN(configure_passes_good_settings_through);
    RUN(configure_rejects_bad_settings_before_the_port);
    RUN(transmit_takes_one_to_255_bytes);
    RUN(poll_hands_back_events_in_order);
    RUN(receive_and_standby_reach_the_port);
    RUN(receiving_is_asked_of_a_port_that_can_tell);
    RUN(sync_word_sx126x_encoding);
    return CHECK_DONE();
}
