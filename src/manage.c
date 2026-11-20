#include "tern/manage.h"

bool tern_manage_request_ok(const uint8_t *frame, size_t len) {
    (void)frame;
    return len >= 1 && len <= TERN_MANAGE_REQUEST_MAX;
}

bool tern_manage_answer_ok(const uint8_t *content, size_t len) {
    if (len == 0 || len > TERN_MANAGE_ANSWER_MAX) {
        return false;
    }
    for (size_t at = 0; at < len; at += 1 + content[at]) {
        if (content[at] == 0 || at + 1 + content[at] > len) {
            return false;
        }
    }
    return true;
}

bool tern_manage_allowed(uint8_t type, uint8_t setting) {
    switch (type) {
    case TERN_C_HELLO:
    case TERN_C_PING:
    case TERN_C_SET_TIME:
    case TERN_C_SYNC:
    case TERN_C_SAVE_CONTACT:
    case TERN_C_REMOVE_CONTACT:
    case TERN_C_END_SESSION:
    case TERN_C_OWNER:
    case TERN_C_RESTART:
        return true;
    case TERN_C_SET:
        /* A new region is a node its owner can no longer hear; the passkey is for those beside
         * it, who have a link of their own. */
        return setting != TERN_C_SET_REGION && setting != TERN_C_SET_PASSKEY;
    default:
        return false;
    }
}
