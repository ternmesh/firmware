#include "tern/radio.h"

#include <stddef.h>

#include "tern/err.h"

int tern_radio_configure(struct tern_radio *r, const struct tern_radio_config *cfg) {
    if (cfg == NULL || !tern_lora_valid(&cfg->mod) || cfg->freq_hz == 0) {
        return TERN_EINVAL;
    }
    return r->ops->configure(r->ctx, cfg);
}

int tern_radio_transmit(struct tern_radio *r, const uint8_t *frame, uint32_t len) {
    if (frame == NULL || len == 0 || len > 255) {
        return TERN_EINVAL;
    }
    return r->ops->transmit(r->ctx, frame, (uint8_t)len);
}

int tern_radio_receive(struct tern_radio *r) { return r->ops->receive(r->ctx); }

int tern_radio_standby(struct tern_radio *r) { return r->ops->standby(r->ctx); }

int tern_radio_poll(struct tern_radio *r, struct tern_radio_event *ev) {
    if (ev == NULL) {
        return TERN_EINVAL;
    }
    return r->ops->poll(r->ctx, ev);
}

uint16_t tern_sync_word_sx126x(uint8_t sync_word) {
    return (uint16_t)(((sync_word & 0xF0u) << 8) | 0x0400u | ((sync_word & 0x0Fu) << 4) | 0x04u);
}
