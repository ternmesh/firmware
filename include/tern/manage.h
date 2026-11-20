#ifndef TERN_MANAGE_H
#define TERN_MANAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tern/companion.h"
#include "tern/time.h"

/* Managing a node over the mesh (draft/managing.md in ternmesh/spec): what goes in the messages for
 * the node that carry an owner's companion requests and the managed node's answers, and what a
 * managed node acts on. The requests themselves are the companion protocol's (tern/companion.h),
 * and what a node holds and does is the node's own. */

#define TERN_MANAGE_REQUEST 0x05 /* the first byte of a message for the node that is a request */
#define TERN_MANAGE_ANSWER 0x06  /* and of one that is an answer */
#define TERN_MANAGE_REQUEST_MAX TERN_COMPANION_MANAGE_MAX
#define TERN_MANAGE_ANSWER_MAX TERN_COMPANION_MANAGED_MAX
#define TERN_MANAGE_TRIAL TERN_S(900) /* a SET over the mesh goes back unless confirmed in this */

/* Whether a request's content, after its kind, is one to act on: one frame, at most
 * TERN_MANAGE_REQUEST_MAX bytes. */
bool tern_manage_request_ok(const uint8_t *frame, size_t len);

/* Whether an answer's content, after its kind, is one: one or more frames, each after a length
 * that is not 0, the last ending at its end, at most TERN_MANAGE_ANSWER_MAX bytes in all. */
bool tern_manage_answer_ok(const uint8_t *content, size_t len);

/* Whether a managed node acts on a request of `type` over the mesh, `setting` being SET's: else it
 * answers ERROR 13. */
bool tern_manage_allowed(uint8_t type, uint8_t setting);

#endif
