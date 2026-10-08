#ifndef TERN_SHARE_H
#define TERN_SHARE_H

#include <stdbool.h>
#include <stdint.h>

#include "tern/address.h"

/* Sharing an address off the air: specification draft 0, draft/sharing.md in ternmesh/spec.
 *
 * An address is written as sixty-four upper-case hex digits, the text form; as a link, and in a QR
 * code, as "TERN:" and the text form; and checked by two people with a short code, twelve digits
 * made from a hash of it, the same in every implementation. None of these goes over the air. */

#define TERN_ADDRESS_TEXT_LEN (2 * TERN_ADDRESS_LEN)      /* 64 */
#define TERN_ADDRESS_LINK_LEN (5 + TERN_ADDRESS_TEXT_LEN) /* 69 */
#define TERN_SHORT_CODE_LEN 14                            /* "5358 3737 3382" */

/* The text form, NUL-terminated. */
void tern_address_text(const uint8_t address[TERN_ADDRESS_LEN],
                       char out[TERN_ADDRESS_TEXT_LEN + 1]);

/* The link, NUL-terminated: what a QR code of the address holds. */
void tern_address_link(const uint8_t address[TERN_ADDRESS_LEN],
                       char out[TERN_ADDRESS_LINK_LEN + 1]);

/* The short code, NUL-terminated: SHA-256("tern short code" || address), its first eight bytes
 * big-endian, mod 10^12, as three groups of four digits. */
void tern_short_code(const uint8_t address[TERN_ADDRESS_LEN], char out[TERN_SHORT_CODE_LEN + 1]);

/* A short code's value, below 10^12, as it is shown: twelve digits, leading zeros kept, in three
 * groups of four. tern_short_code() is this, of the address's value. */
void tern_short_code_text(uint64_t value, char out[TERN_SHORT_CODE_LEN + 1]);

/* Reads an address as a person gives one: the link, its scheme in any case, or the text form
 * alone; the digits in either case, with spaces anywhere among them. False, writing nothing, for
 * anything else. Whether the address is valid is not checked here: a caller that keeps it as a
 * contact checks that (tern_address_valid()). */
bool tern_address_read(const char *text, uint8_t address[TERN_ADDRESS_LEN]);

#endif
