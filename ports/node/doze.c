#include "doze.h"

#include "tern/sleep.h"

void doze_init(struct doze *d, tern_time now) { *d = (struct doze){.started = now}; }

bool doze_should(const struct doze *d, tern_time now, bool may, tern_time wake_at,
                 tern_time other_due, bool busy, tern_time *until) {
    tern_time next = other_due < wake_at ? other_due : wake_at;
    if (d->dozing || !may || busy || d->failures >= DOZE_FAILURES || next - now < DOZE_MIN) {
        return false;
    }
    *until = next - DOZE_LEAD;
    return true;
}

bool doze_wake_now(const struct doze *d, tern_time now, tern_time due, bool sleeping) {
    return d->dozing && (!sleeping || now >= d->until || due - now <= DOZE_LEAD);
}

void doze_begin(struct doze *d, tern_time now, tern_time until) {
    d->dozing = true;
    d->since = now;
    d->until = until;
    d->dozes++;
}

void doze_end(struct doze *d, tern_time now) {
    if (d->dozing) {
        d->off += now - d->since;
        d->dozing = false;
    }
}

tern_time doze_off(const struct doze *d, tern_time now) {
    return d->off + (d->dozing ? now - d->since : 0);
}

uint32_t doze_on_permille(const struct doze *d, tern_time now) {
    tern_time all = now - d->started, off = doze_off(d, now);
    if (all <= 0) {
        return 1000;
    }
    return (uint32_t)((all - off) * 1000 / all);
}

uint32_t doze_slice_ms(const struct doze *d, tern_time now) {
    tern_time left = d->until - now;
    if (!d->dozing || left <= 0) {
        return 1;
    }
    if (left >= TERN_MS(DOZE_SLICE_MS)) {
        return DOZE_SLICE_MS;
    }
    return left / TERN_MS(1) > 0 ? (uint32_t)(left / TERN_MS(1)) : 1;
}

/* A whole number of at most nine digits, and where it ended; false for none. */
static bool number(const char **p, uint32_t *out) {
    uint32_t n = 0;
    int digits = 0;
    while (**p == ' ') {
        (*p)++;
    }
    while (**p >= '0' && **p <= '9') {
        if (++digits > 9) {
            return false;
        }
        n = n * 10 + (uint32_t)(**p - '0');
        (*p)++;
    }
    *out = n;
    return digits > 0;
}

bool doze_parse(const char *args, uint16_t *wake, uint8_t *listen) {
    uint32_t w, l = TERN_SLEEP_LISTEN_MIN_S;
    const char *p = args;
    if (!number(&p, &w)) {
        return false;
    }
    while (*p == ' ') {
        p++;
    }
    if (*p != '\0' && !number(&p, &l)) {
        return false;
    }
    while (*p == ' ') {
        p++;
    }
    /* WAKE_MAX is under 32768 s, so a wake is its seconds as they are (a promise's encoding). */
    if (*p != '\0' || w > 0x7FFF || !tern_sleep_wake_ok((uint16_t)w) ||
        l < TERN_SLEEP_LISTEN_MIN_S || l > 255) {
        return false;
    }
    *wake = (uint16_t)w;
    *listen = (uint8_t)l;
    return true;
}

uint32_t doze_setting(uint16_t wake, uint8_t listen) {
    return (uint32_t)wake | (uint32_t)listen << 16;
}

void doze_setting_read(uint32_t word, uint16_t *wake, uint8_t *listen) {
    uint16_t w = (uint16_t)word;
    uint8_t l = (uint8_t)(word >> 16);
    /* Flash never written reads as all ones: neither a wake nor anything after it. */
    if (word == 0xFFFFFFFFu) {
        w = 0;
        l = 0;
    }
    *wake = w != 0 && tern_sleep_wake_ok(w) ? w : 0;
    *listen = l >= TERN_SLEEP_LISTEN_MIN_S ? l : 0;
}
