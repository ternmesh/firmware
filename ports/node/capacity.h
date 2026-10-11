#ifndef NODE_CAPACITY_H
#define NODE_CAPACITY_H

/* How much of each thing the node holds, as the build's Kconfig sets it (Kconfig.tern), or the
 * smallest board's where nothing does: a host's tests, and the classic ESP32, whose static RAM is
 * full. Each port's builds raise what its chip has room for (sdkconfig.defaults.esp32s3,
 * ports/nrf52/prj.conf).
 *
 * Every one of these is RAM, and the first three are flash too: what each costs of both is in
 * Kconfig.tern's help. */

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h" /* ESP-IDF, which does not include its configuration everywhere itself */
#endif
#endif

#ifdef CONFIG_TERN_SESSIONS
#define NODE_SESSIONS CONFIG_TERN_SESSIONS
#else
#define NODE_SESSIONS 8
#endif

#ifdef CONFIG_TERN_CONTACTS
#define NODE_CONTACTS CONFIG_TERN_CONTACTS
#else
#define NODE_CONTACTS 16
#endif

#ifdef CONFIG_TERN_GROUPS
#define NODE_GROUPS CONFIG_TERN_GROUPS
#else
#define NODE_GROUPS 4
#endif

#ifdef CONFIG_TERN_CARDS
#define NODE_CARDS CONFIG_TERN_CARDS
#else
#define NODE_CARDS 32
#endif

/* The least of each a build may set: what every earlier build held, so that what one saved is
 * never more than a later one loads. */
_Static_assert(NODE_SESSIONS >= 8 && NODE_SESSIONS <= 32, "8 to 32 sessions");
_Static_assert(NODE_CONTACTS >= 16 && NODE_CONTACTS <= 128, "16 to 128 contacts");
_Static_assert(NODE_GROUPS >= 4 && NODE_GROUPS <= 16, "4 to 16 groups");
_Static_assert(NODE_CARDS >= 8 && NODE_CARDS <= 128, "8 to 128 cards");

#endif
