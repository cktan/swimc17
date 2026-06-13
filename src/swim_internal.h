/* Copyright (c) 2026, CK Tan.
 * https://github.com/cktan/swimc17/blob/main/LICENSE
 */
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

/**
 * Wrap a pre-encoded message with an auth header for testing.
 * Writes [tval_be4][hval_be8][msg] into out.
 *
 * @param name   Shared cluster secret (same as swim_start_opts_t.name).
 * @param msg    Encoded message bytes (from swim_pack_message).
 * @param msglen Length of msg.
 * @param out    Output buffer; must be at least msglen + 12 bytes.
 * @param outsz  Size of out.
 * @return msglen + 12 on success, -1 if out is too small.
 */
SWIM_EXTERN int swim_pack_authed(const char *name, const uint8_t *msg,
                                 int msglen, uint8_t *out, int outsz);

#endif // SWIM_PROTOCOL_INTERNAL_H
