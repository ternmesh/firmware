#ifndef TERN_DUTY_H
#define TERN_DUTY_H

#include <stdbool.h>
#include <stdint.h>

#include "tern/time.h"

/* The regulator's limit on transmitting: specification draft 0, draft/phy.md in ternmesh/spec.
 *
 * Where a region limits it, the time on air of all the frames a node begins to send in any period
 * of the window's length must stay within a share of that period: 10% of any hour, in the EU's
 * sub-band. This keeps the account. It is the same hard limit for every node, and not the airtime
 * budget, which shares the channel out between nodes and is yet to be specified.
 *
 * The window is kept as 61 slices, each a sixtieth of it, and a frame is counted against the
 * slice it begins in until that slice is more than a whole window old. So the account never
 * shows less than was sent in the last window, and can show up to a slice more: a node is held
 * to the limit, and may be held up to a sixtieth of the window longer than it strictly need be.
 *
 * A node that restarts loses the account, and usually its clock. A port should save what
 * tern_duty_used() shows before each frame goes, and charge it all again when it starts, as if
 * sent that moment. */

#define TERN_DUTY_SLICES 61

struct tern_duty {
    tern_time slice;                  /* a sixtieth of the window */
    tern_time limit;                  /* time on air allowed in any window, or -1 for no limit */
    int64_t newest;                   /* the number of the latest slice the account has reached */
    tern_time used[TERN_DUTY_SLICES]; /* slice n's is at n % TERN_DUTY_SLICES */
};

/* Starts an empty account allowing ppm millionths of every window_s seconds. 1000000 or more is
 * no limit. */
void tern_duty_init(struct tern_duty *d, uint32_t ppm, uint32_t window_s);

/* The time on air counted against the window that ends now. */
tern_time tern_duty_used(struct tern_duty *d, tern_time now);

/* Whether a frame of this time on air may begin now. */
bool tern_duty_allows(struct tern_duty *d, tern_time now, tern_time airtime);

/* Counts a frame that begins now. Call it for every frame sent, allowed or not. */
void tern_duty_charge(struct tern_duty *d, tern_time now, tern_time airtime);

#endif
