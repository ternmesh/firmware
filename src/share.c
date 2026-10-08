#include "tern/share.h"

#include <string.h>

#include "tern/crypto.h"

static const char hex[] = "0123456789ABCDEF";
static const char link[] = "HTTPS://TERNMESH.ORG/A/";
static const char base32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"; /* RFC 4648, section 6 */

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
    char *b = out + sizeof link - 1;
    unsigned n = 0, bits = 0;
    memcpy(out, link, sizeof link - 1);
    /* Five bits a character, most significant first; the last carries one bit and four zeros. */
    for (size_t i = 0; i < TERN_ADDRESS_LEN; i++) {
        n = (n << 8 | address[i]) & 0xFFF;
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            *b++ = base32[n >> bits & 31];
        }
    }
    *b++ = base32[n << (5 - bits) & 31];
    *b = '\0';
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

static char upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c; }

/* The core takes nothing from the C library but memcpy and its kind, so no strchr or strlen. */
static int base32_value(char c) {
    c = upper(c);
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= '2' && c <= '7' ? c - '2' + 26 : -1;
}

/* The base32 of a link: exactly 52 characters, either case, the last four bits zero. */
static bool read_base32(const char *text, uint8_t address[TERN_ADDRESS_LEN]) {
    uint8_t got[TERN_ADDRESS_LEN];
    unsigned n = 0, bits = 0;
    size_t k = 0;
    for (size_t i = 0; i < TERN_ADDRESS_BASE32_LEN; i++) {
        int v = base32_value(text[i]); /* a NUL here, too short, is not base32 */
        if (v < 0) {
            return false;
        }
        n = (n << 5 | (unsigned)v) & 0xFFF; /* at most seven bits pending, and five more */
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            got[k++] = (uint8_t)(n >> bits);
        }
    }
    if (text[TERN_ADDRESS_BASE32_LEN] != '\0' || (n & ((1u << bits) - 1)) != 0) {
        return false; /* a spare bit set: not the one link this address has */
    }
    memcpy(address, got, TERN_ADDRESS_LEN);
    return true;
}

bool tern_address_read(const char *text, uint8_t address[TERN_ADDRESS_LEN]) {
    uint8_t got[TERN_ADDRESS_LEN];
    size_t n = 0;
    size_t i = 0;
    while (i < sizeof link - 1 && upper(text[i]) == link[i]) {
        i++;
    }
    if (i == sizeof link - 1) {
        return read_base32(text + i, address);
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
