#ifndef TERN_LISTEN_H
#define TERN_LISTEN_H

#include <stdbool.h>

#include "tern/lora.h"
#include "tern/time.h"

/* Listening first: specification draft 0, draft/forwarding.md in ternmesh/spec.
 *
 * A node must not start to send a frame while its radio is receiving one. This keeps what the
 * radio has said - a preamble found, a header found, a frame ended - and answers whether, by the
 * specification's rules, the radio is receiving now. A radio's driver tells it what the chip
 * reports and a port asks before each frame; it is the same for every chip.
 *
 * It does not make the channel clear. A radio does not find a frame too weak for it, or one that
 * began while it was sending, and takes several symbols to find any. */

/* A preamble with no header found this many symbols after the preamble's own length is noise, or
 * a frame with another network's sync word: the 4.25 symbols that end a preamble and the 8 that
 * hold a header, rounded up. */
#define TERN_LISTEN_HEAD_WAIT 13

struct tern_listen {
    tern_time found; /* when the radio found the preamble it is on, or -1 if it is on none */
    bool header;     /* and whether it has found a header since */
};

void tern_listen_init(struct tern_listen *l);

/* The radio found a preamble: one found while a header is waited for starts the wait again. */
void tern_listen_preamble(struct tern_listen *l, tern_time now);

/* The radio found a header: with no preamble before it, it counts as both. */
void tern_listen_header(struct tern_listen *l, tern_time now);

/* The radio said the frame ended, whole or not, or the node began to send. */
void tern_listen_over(struct tern_listen *l);

/* Whether the radio is receiving, on modulation m: from the preamble until the frame is over, but
 * no longer than a header takes to come if none has, nor than the longest frame is on the air. */
bool tern_listen_receiving(const struct tern_listen *l, const struct tern_lora *m, tern_time now);

#endif
