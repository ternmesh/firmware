#ifndef TERN_TIME_H
#define TERN_TIME_H

#include <stdint.h>

/* Time, in nanoseconds, from a clock the port chooses (usually boot).
 *
 * The same representation as the simulator's, so durations computed here and there compare
 * directly. Nanoseconds because the shortest interval that matters is a fraction of a LoRa symbol
 * (256 us at SF7/500 kHz); int64_t still covers 292 years. */
typedef int64_t tern_time;

#define TERN_NS(x) ((tern_time)(x))
#define TERN_US(x) ((tern_time)(x) * 1000)
#define TERN_MS(x) ((tern_time)(x) * 1000000)
#define TERN_S(x) ((tern_time)(x) * 1000000000)

#endif
