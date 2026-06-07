#ifndef SWIM_CODEC_H
#define SWIM_CODEC_H

#include "swim_node_id.h"
#include "swim_membership.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWIM_MSG_PING     1
#define SWIM_MSG_ACK      2
#define SWIM_MSG_PING_REQ 3
#define SWIM_MSG_FWD_ACK  4

#define SWIM_MAX_PACKET_SIZE 1400
#define SWIM_MAX_EVENTS      64

typedef struct {
  uint8_t type;
  swim_node_id_t sender;
  uint32_t seq;
  swim_node_id_t peer; // target for ping_req, source for fwd_ack (ignored otherwise)
  uint8_t event_count;
  swim_member_t events[SWIM_MAX_EVENTS];
} swim_message_t;

/**
 * Encode a message structure into a binary payload.
 *
 * @param msg  The message structure to encode.
 * @param buf  The output buffer.
 * @param size The size of the output buffer.
 * @return The number of bytes written on success, or -1 on error (e.g. buffer too small).
 */
int swim_codec_encode(const swim_message_t *msg, uint8_t *buf, size_t size);

/**
 * Decode a binary payload into a message structure.
 *
 * @param buf  The input buffer.
 * @param size The size of the input buffer.
 * @param msg  The message structure to populate.
 * @return 0 on success, or -1 on decoding/parsing failure.
 */
int swim_codec_decode(const uint8_t *buf, size_t size, swim_message_t *msg);

#ifdef __cplusplus
}
#endif

#endif // SWIM_CODEC_H
