#ifndef SWIM_PROTOCOL_INTERNAL_H
#define SWIM_PROTOCOL_INTERNAL_H

#include "swim.h"
#include "swim_membership.h"

/*
 * Internal/QA-only interface. Not part of the public API exposed in
 * swim.h.
 */

/**
 * Query the current cluster membership list (with full member records).
 * Exposed for QA/testing; the public API is swim_peers().
 *
 * @param inst         The instance handle returned by swim_start.
 * @param out_list     Output buffer for members.
 * @param max_len      Size of the out_list buffer.
 * @param include_dead Whether to include dead nodes in the list.
 * @return The number of members copied, or -1 on error.
 */
SWIM_EXTERN int swim_members(swim_t *inst, swim_member_t *out_list, int max_len,
                             bool include_dead);

#endif // SWIM_PROTOCOL_INTERNAL_H
