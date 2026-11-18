#include "tern/address.h"
#include "tern/crypto.h"
#include "tern/route.h"

/* Apart from route.c, so that a router built alone, as the simulator builds it, needs no Ed25519:
 * the router knows nodes by their keys, and only the node's own code turns an address into one. */
uint32_t tern_route_id(const uint8_t address[TERN_ADDRESS_LEN]) {
    uint8_t key[TERN_ADDRESS_LEN];
    if (!tern_address_blind(key, address, TERN_EPOCH)) {
        return 0;
    }
    uint32_t id = tern_route_key_id(key);
    tern_wipe(key, sizeof key);
    return id;
}
