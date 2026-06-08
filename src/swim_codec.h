#ifndef SWIM_CODEC_H
#define SWIM_CODEC_H

#include "swim_membership.h"
#include "swim.h"

#define SWIM_MSG_PING 1
#define SWIM_MSG_ACK 2
#define SWIM_MSG_PING_REQ 3
#define SWIM_MSG_FWD_ACK 4
#define SWIM_MSG_LEAVE 5

#define SWIM_MAX_PACKET_SIZE 1400
#define SWIM_MAX_EVENTS 64

typedef struct swim_message_t swim_message_t;
struct swim_message_t {
  uint8_t type;
  swim_node_id_t sender;
  uint32_t seq;
  swim_node_id_t
      peer; // target for ping_req, source for fwd_ack (ignored otherwise)
  uint8_t event_count;
  swim_member_t events[SWIM_MAX_EVENTS];
};

/**
 * Compactly encode a Node ID: [host_len (2B)] [host (NB)] [port (2B)]
 * [cookie_len (2B)] [cookie (MB)]
 *
 * @param id     The node ID to encode.
 * @param p      Pointer to start of the first free byte.
 * @param q      Pointer to the end of the buffer.
 * @return Number of bytes encoded, or -1 on overflow/error.
 */
SWIM_EXTERN int swim_encode_node_id(const swim_node_id_t *id, uint8_t *p,
                                    uint8_t *q);

/**
 * Decode a Node ID from the binary format.
 *
 * @param id     Pointer to the node ID to populate.
 * @param p      Pointer to start of the first byte to read.
 * @param q      Pointer to the end of the buffer (one past the end).
 * @return Number of bytes decoded, or -1 on error/overflow.
 */
SWIM_EXTERN int swim_decode_node_id(swim_node_id_t *id, const uint8_t *p,
                                    const uint8_t *q);

/**
 * Compactly encode a membership event/member: [node_id] [status (1B)]
 * [incarnation (8B)]
 *
 * @param m      The member info to encode.
 * @param p      Pointer to start of the first free byte.
 * @param q      Pointer to the end of the buffer.
 * @return Number of bytes encoded, or -1 on overflow/error.
 */
SWIM_EXTERN int swim_encode_membership(const swim_member_t *m, uint8_t *p,
                                       uint8_t *q);

/**
 * Decode a membership event/member from the binary format.
 *
 * @param m      Pointer to the member info to populate.
 * @param p      Pointer to start of the first byte to read.
 * @param q      Pointer to the end of the buffer (one past the end).
 * @return Number of bytes decoded, or -1 on error/overflow.
 */
SWIM_EXTERN int swim_decode_membership(swim_member_t *m, const uint8_t *p,
                                       const uint8_t *q);

/**
 * Encode an 8-bit unsigned integer into the buffer.
 *
 * @param val    The value to encode.
 * @param p      Pointer to start of the first free byte.
 * @param q      Pointer to the end of the buffer.
 * @return Number of bytes encoded (1), or -1 on overflow.
 */
SWIM_EXTERN int swim_encode_int8(uint8_t val, uint8_t *p, uint8_t *q);

/**
 * Decode an 8-bit unsigned integer from the buffer.
 *
 * @param val    Pointer to the value to populate.
 * @param p      Pointer to start of the first byte to read.
 * @param q      Pointer to the end of the buffer (one past the end).
 * @return Number of bytes decoded (1), or -1 on overflow.
 */
SWIM_EXTERN int swim_decode_int8(uint8_t *val, const uint8_t *p,
                                 const uint8_t *q);

/**
 * Encode a 16-bit unsigned integer in big-endian into the buffer.
 *
 * @param val    The value to encode.
 * @param p      Pointer to start of the first free byte.
 * @param q      Pointer to the end of the buffer.
 * @return Number of bytes encoded (2), or -1 on overflow.
 */
SWIM_EXTERN int swim_encode_int16(uint16_t val, uint8_t *p, uint8_t *q);

/**
 * Decode a 16-bit unsigned integer from the buffer.
 *
 * @param val    Pointer to the value to populate.
 * @param p      Pointer to start of the first byte to read.
 * @param q      Pointer to the end of the buffer (one past the end).
 * @return Number of bytes decoded (2), or -1 on overflow.
 */
SWIM_EXTERN int swim_decode_int16(uint16_t *val, const uint8_t *p,
                                  const uint8_t *q);

/**
 * Encode a 32-bit unsigned integer in big-endian into the buffer.
 *
 * @param val    The value to encode.
 * @param p      Pointer to start of the first free byte.
 * @param q      Pointer to the end of the buffer.
 * @return Number of bytes encoded (4), or -1 on overflow.
 */
SWIM_EXTERN int swim_encode_int32(uint32_t val, uint8_t *p, uint8_t *q);

/**
 * Decode a 32-bit unsigned integer from the buffer.
 *
 * @param val    Pointer to the value to populate.
 * @param p      Pointer to start of the first byte to read.
 * @param q      Pointer to the end of the buffer (one past the end).
 * @return Number of bytes decoded (4), or -1 on overflow.
 */
SWIM_EXTERN int swim_decode_int32(uint32_t *val, const uint8_t *p,
                                  const uint8_t *q);

/**
 * Encode a 64-bit unsigned integer in big-endian into the buffer.
 *
 * @param val    The value to encode.
 * @param p      Pointer to start of the first free byte.
 * @param q      Pointer to the end of the buffer.
 * @return Number of bytes encoded (8), or -1 on overflow.
 */
SWIM_EXTERN int swim_encode_int64(uint64_t val, uint8_t *p, uint8_t *q);

/**
 * Decode a 64-bit unsigned integer from the buffer.
 *
 * @param val    Pointer to the value to populate.
 * @param p      Pointer to start of the first byte to read.
 * @param q      Pointer to the end of the buffer (one past the end).
 * @return Number of bytes decoded (8), or -1 on overflow.
 */
SWIM_EXTERN int swim_decode_int64(uint64_t *val, const uint8_t *p,
                                  const uint8_t *q);

/**
 * Encode a null-terminated string into the buffer (raw characters, no length
 * prefix).
 *
 * @param str    The string to encode.
 * @param p      Pointer to start of the first free byte.
 * @param q      Pointer to the end of the buffer.
 * @return Number of bytes encoded (strlen(str)), or -1 on overflow.
 */
SWIM_EXTERN int swim_encode_string(const char *str, uint8_t *p, uint8_t *q);

/**
 * Decode a length-prefixed string from the buffer.
 *
 * @param str      Destination string buffer.
 * @param max_len  Capacity of destination string buffer.
 * @param p        Pointer to start of the first byte to read.
 * @param q        Pointer to the end of the buffer (one past the end).
 * @return Number of bytes decoded, or -1 on error/overflow.
 */
SWIM_EXTERN int swim_decode_string(char *str, size_t max_len, const uint8_t *p,
                                   const uint8_t *q);

struct swim_gossip_queue_t;

/**
 * Encode a message into the binary format.
 *
 * @param type           Message type.
 * @param sender         Sender Node ID.
 * @param seq            Sequence number.
 * @param peer           Peer Node ID (if applicable, otherwise NULL).
 * @param q              Gossip queue.
 * @param active_members Number of active members.
 * @param buf            Output buffer.
 * @param bufsz          Buffer size.
 * @return Number of bytes written on success, or -1 on error.
 */
SWIM_EXTERN int swim_encode_message(uint8_t type, const swim_node_id_t *sender,
                                    uint32_t seq, const swim_node_id_t *peer,
                                    struct swim_gossip_queue_t *q,
                                    uint32_t active_members, uint8_t *buf,
                                    int bufsz);

/**
 * Decode a binary payload into a message structure.
 *
 * @param buf  The input buffer.
 * @param size The size of the input buffer.
 * @param msg  The message structure to populate.
 * @return 0 on success, or -1 on decoding/parsing failure.
 */
SWIM_EXTERN int swim_decode_message(const uint8_t *buf, size_t size,
                                    swim_message_t *msg);

#endif // SWIM_CODEC_H
