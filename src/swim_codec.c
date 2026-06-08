/*
 * swim_codec.c — Wire protocol encoder and decoder.
 *
 * Serializes and deserializes SWIM messages (PING, ACK,
 * PING_REQ, FWD_ACK, LEAVE) and their components (node IDs,
 * membership events) into a compact binary format in network
 * byte order.
 *
 * Message layout: [type (1B)] [seq (4B)] [sender node ID]
 * [peer node ID, PING_REQ/FWD_ACK only] [gossip payload].
 * The gossip payload is a variable-length list of encoded
 * swim_member_t records that fills whatever space remains in
 * the packet. There is no explicit event count — the decoder
 * reads membership records until it reaches the buffer
 * boundary.
 *
 * All multi-byte integers are big-endian. Strings are
 * length-prefixed: [len (2B)] [bytes]. Encoding helpers
 * return the number of bytes written, or -1 if the buffer
 * is exhausted; callers chain these p += n checks to build
 * up the packet incrementally.
 */
#define _DEFAULT_SOURCE

#include "swim_codec.h"
#include "swim_errno.h"
#include "swim_gossip_queue.h"
#include <arpa/inet.h>
#include <endian.h>
#include <string.h>

static int encode_int8(uint8_t val, uint8_t *p, uint8_t *q) {
  if (p + 1 > q) return -1;
  *p = val;
  return 1;
}

static int encode_int16(uint16_t val, uint8_t *p, uint8_t *q) {
  if (p + 2 > q) return -1;
  uint16_t net_val = htons(val);
  memcpy(p, &net_val, 2);
  return 2;
}

static int encode_int32(uint32_t val, uint8_t *p, uint8_t *q) {
  if (p + 4 > q) return -1;
  uint32_t net_val = htonl(val);
  memcpy(p, &net_val, 4);
  return 4;
}

static int encode_int64(uint64_t val, uint8_t *p, uint8_t *q) {
  if (p + 8 > q) return -1;
  uint64_t net_val = htobe64(val);
  memcpy(p, &net_val, 8);
  return 8;
}

static int encode_string(const char *str, uint8_t *p, uint8_t *q) {
  size_t len = strlen(str);
  if (p + 2 + len > q) return -1;
  uint16_t net_len = htons((uint16_t)len);
  memcpy(p, &net_len, 2);
  memcpy(p + 2, str, len);
  return (int)(2 + len);
}

static int decode_int8(uint8_t *val, const uint8_t *p, const uint8_t *q) {
  if (p + 1 > q) return -1;
  *val = *p;
  return 1;
}

static int decode_int16(uint16_t *val, const uint8_t *p, const uint8_t *q) {
  if (p + 2 > q) return -1;
  uint16_t net_val;
  memcpy(&net_val, p, 2);
  *val = ntohs(net_val);
  return 2;
}

static int decode_int32(uint32_t *val, const uint8_t *p, const uint8_t *q) {
  if (p + 4 > q) return -1;
  uint32_t net_val;
  memcpy(&net_val, p, 4);
  *val = ntohl(net_val);
  return 4;
}

static int decode_int64(uint64_t *val, const uint8_t *p, const uint8_t *q) {
  if (p + 8 > q) return -1;
  uint64_t net_val;
  memcpy(&net_val, p, 8);
  *val = be64toh(net_val);
  return 8;
}

static int decode_string(char *str, size_t max_len, const uint8_t *p,
                         const uint8_t *q) {
  if (p + 2 > q) return -1;
  uint16_t net_len;
  memcpy(&net_len, p, 2);
  size_t len = ntohs(net_len);
  if (p + 2 + len > q) return -1;
  if (len >= max_len) return -1;
  memcpy(str, p + 2, len);
  str[len] = '\0';
  return (int)(2 + len);
}

// Node ID wire format: [host_len (2B)] [host (NB)] [port (2B)]
// [cookie_len (2B)] [cookie (MB)]
static int encode_node_id(const swim_node_id_t *id, uint8_t *p, uint8_t *q) {
  uint8_t *start = p;
  int n;

  n = encode_string(id->host, p, q);
  if (n < 0) return -1;
  p += n;
  n = encode_int16(id->port, p, q);
  if (n < 0) return -1;
  p += n;
  n = encode_string(id->cookie, p, q);
  if (n < 0) return -1;
  p += n;

  return (int)(p - start);
}

static int decode_node_id(swim_node_id_t *id, const uint8_t *p,
                           const uint8_t *q) {
  const uint8_t *start = p;
  int n;

  n = decode_string(id->host, sizeof(id->host), p, q);
  if (n < 0) return -1;
  p += n;

  uint16_t port;
  n = decode_int16(&port, p, q);
  if (n < 0) return -1;
  id->port = port;
  p += n;

  n = decode_string(id->cookie, sizeof(id->cookie), p, q);
  if (n < 0) return -1;
  p += n;

  return (int)(p - start);
}

// Encode a membership event: [node_id] [status (1B)] [incarnation (8B)].
// Returns bytes written, or -1 if buffer is too small (no error set — callers
// use -1 as a stopping signal when packing gossip into a packet).
int swim_encode_membership(const swim_member_t *m, uint8_t *p, uint8_t *q) {
  if (!m || !p || !q) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Invalid arguments to swim_encode_membership");
  }
  uint8_t *start = p;
  int n;

  n = encode_node_id(&m->id, p, q);
  if (n < 0) return -1;
  p += n;

  n = encode_int8((uint8_t)m->status, p, q);
  if (n < 0) return -1;
  p += n;

  n = encode_int64(m->incarnation, p, q);
  if (n < 0) return -1;
  p += n;

  return (int)(p - start);
}

static int decode_membership(swim_member_t *m, const uint8_t *p,
                              const uint8_t *q) {
  const uint8_t *start = p;
  int n;

  n = decode_node_id(&m->id, p, q);
  if (n < 0) return -1;
  p += n;

  uint8_t status;
  n = decode_int8(&status, p, q);
  if (n < 0) return -1;
  m->status = (swim_status_t)status;
  p += n;

  n = decode_int64(&m->incarnation, p, q);
  if (n < 0) return -1;
  p += n;

  m->dead_at = 0;

  return (int)(p - start);
}

int swim_encode_message(uint8_t type, const swim_node_id_t *sender,
                        uint32_t seq, const swim_node_id_t *peer,
                        struct swim_gossip_queue_t *q, uint32_t active_members,
                        uint8_t *buf, int bufsz) {
  if (!sender || !buf || bufsz <= 0) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Invalid arguments to swim_encode_message");
  }

  uint8_t *p = buf;
  uint8_t *end = buf + bufsz;
  int n;

  n = encode_int8(type, p, end);
  if (n < 0) {
    return swim_set_error(SWIM_ERR_INVALID, "Buffer too small for message");
  }
  p += n;

  n = encode_int32(seq, p, end);
  if (n < 0) {
    return swim_set_error(SWIM_ERR_INVALID, "Buffer too small for message");
  }
  p += n;

  n = encode_node_id(sender, p, end);
  if (n < 0) {
    return swim_set_error(SWIM_ERR_INVALID, "Buffer too small for message");
  }
  p += n;

  if (type == SWIM_MSG_PING_REQ || type == SWIM_MSG_FWD_ACK) {
    if (!peer) {
      return swim_set_error(SWIM_ERR_INVALID,
                            "Peer node ID required for PING_REQ/FWD_ACK");
    }
    n = encode_node_id(peer, p, end);
    if (n < 0) {
      return swim_set_error(SWIM_ERR_INVALID, "Buffer too small for message");
    }
    p += n;
  }

  // Fill gossip into the remaining packet room (DESIGN §8). With no queue
  // (e.g. leave messages) the packet simply ends after the header; the
  // decoder treats a missing gossip section as zero events.
  int gossip_bytes = 0;
  if (q) {
    gossip_bytes = swim_gossip_queue_pack(q, active_members, p, end);
    if (gossip_bytes < 0) {
      return -1;
    }
  }

  p += gossip_bytes;
  return (int)(p - buf);
}

int swim_decode_message(const uint8_t *buf, size_t size, swim_message_t *msg) {
  if (!buf || !msg) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Invalid NULL arguments to swim_decode_message");
  }

  const uint8_t *p = buf;
  const uint8_t *end = buf + size;
  int n;

  n = decode_int8(&msg->type, p, end);
  if (n < 0) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Buffer too short for message type");
  }
  if (msg->type < SWIM_MSG_PING || msg->type > SWIM_MSG_LEAVE) {
    return swim_set_error(SWIM_ERR_INVALID, "Invalid message type value");
  }
  p += n;

  n = decode_int32(&msg->seq, p, end);
  if (n < 0) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Buffer too short for sequence number");
  }
  p += n;

  n = decode_node_id(&msg->sender, p, end);
  if (n < 0) {
    return swim_set_error(SWIM_ERR_INVALID, "Failed to decode sender node ID");
  }
  p += n;

  if (msg->type == SWIM_MSG_PING_REQ || msg->type == SWIM_MSG_FWD_ACK) {
    n = decode_node_id(&msg->peer, p, end);
    if (n < 0) {
      return swim_set_error(SWIM_ERR_INVALID, "Failed to decode peer node ID");
    }
    p += n;
  } else {
    memset(&msg->peer, 0, sizeof(msg->peer));
  }

  int count = 0;
  while (p < end) {
    if (count >= SWIM_MAX_EVENTS) {
      return swim_set_error(SWIM_ERR_INVALID,
                            "Event count exceeds maximum allowed");
    }
    n = decode_membership(&msg->gossip[count], p, end);
    if (n < 0) {
      return swim_set_error(SWIM_ERR_INVALID, "Failed to decode gossip event");
    }
    p += n;
    count++;
  }

  msg->gossip_count = (uint8_t)count;
  return 0;
}
