#include "tern/share.h"

#include <string.h>

#include "tern/crypto.h"

static const char hex[] = "0123456789ABCDEF";
static const char scheme[] = "TERN:";

void tern_address_text(const uint8_t address[TERN_ADDRESS_LEN],
                       char out[TERN_ADDRESS_TEXT_LEN + 1]) {
    for (size_t i = 0; i < TERN_ADDRESS_LEN; i++) {
        out[2 * i] = hex[address[i] >> 4];
        out[2 * i + 1] = hex[address[i] & 0x0F];
    }
    out[TERN_ADDRESS_TEXT_LEN] = '\0';
}

void tern_address_link(const uint8_t address[TERN_ADDRESS_LEN],
                       char out[TERN_ADDRESS_LINK_LEN + 1]) {
    memcpy(out, scheme, sizeof scheme - 1);
    tern_address_text(address, out + sizeof scheme - 1);
}

void tern_short_code(const uint8_t address[TERN_ADDRESS_LEN], char out[TERN_SHORT_CODE_LEN + 1]) {
    static const char label[] = "tern short code";
    struct tern_sha256 c;
    uint8_t h[TERN_SHA256_LEN];
    tern_sha256_init(&c);
    tern_sha256_update(&c, (const uint8_t *)label, sizeof label - 1);
    tern_sha256_update(&c, address, TERN_ADDRESS_LEN);
    tern_sha256_final(&c, h);
    uint64_t n = 0;
    for (int i = 0; i < 8; i++) {
        n = n << 8 | h[i];
    }
    tern_short_code_text(n % 1000000000000ULL, out);
}

void tern_short_code_text(uint64_t n, char out[TERN_SHORT_CODE_LEN + 1]) {
    /* Twelve digits from the right, a space before each group of four but the first. */
    for (int k = TERN_SHORT_CODE_LEN - 1; k >= 0; k--) {
        if (k == 4 || k == 9) {
            out[k] = ' ';
            continue;
        }
        out[k] = (char)('0' + n % 10);
        n /= 10;
    }
    out[TERN_SHORT_CODE_LEN] = '\0';
}

static int digit(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

bool tern_address_read(const char *text, uint8_t address[TERN_ADDRESS_LEN]) {
    uint8_t got[TERN_ADDRESS_LEN];
    size_t n = 0;
    bool linked = true;
    for (size_t i = 0; i < sizeof scheme - 1; i++) {
        char c = text[i];
        char upper = c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
        if (upper != scheme[i]) {
            linked = false;
            break;
        }
    }
    if (linked) {
        text += sizeof scheme - 1;
    }
    for (; *text != '\0'; text++) {
        if (*text == ' ') {
            continue;
        }
        int v = digit(*text);
        if (v < 0 || n == TERN_ADDRESS_TEXT_LEN) {
            return false;
        }
        got[n / 2] = (uint8_t)(n % 2 ? got[n / 2] << 4 | v : v);
        n++;
    }
    if (n != TERN_ADDRESS_TEXT_LEN) {
        return false;
    }
    memcpy(address, got, TERN_ADDRESS_LEN);
    return true;
}
