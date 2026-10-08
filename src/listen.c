#include "tern/listen.h"

void tern_listen_init(struct tern_listen *l) { *l = (struct tern_listen){.found = -1}; }

void tern_listen_preamble(struct tern_listen *l, tern_time now) {
    l->found = now;
    l->header = false;
}

void tern_listen_header(struct tern_listen *l, tern_time now) {
    if (l->found < 0) {
        l->found = now;
    }
    l->header = true;
}

void tern_listen_over(struct tern_listen *l) { l->found = -1; }

bool tern_listen_receiving(const struct tern_listen *l, const struct tern_lora *m, tern_time now) {
    if (l->found < 0) {
        return false;
    }
    tern_time wait = l->header
                         ? tern_lora_airtime(m, 255)
                         : (tern_time)(m->preamble + TERN_LISTEN_HEAD_WAIT) * tern_lora_symbol(m);
    return now - l->found < wait;
}
