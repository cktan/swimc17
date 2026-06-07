#define _DEFAULT_SOURCE
#include "swim_codec.h"
#include "swim_errno.h"
#include "swim_gossip_queue.h"
#include <arpa/inet.h>
#include <endian.h>
#include <string.h>

// Helpers for endianness-safe network-byte-order packing/unpacking
static inline void write_uint16(uint8_t *p, uint16_t val) {
  p[0] = (val >> 8) & 0xFF;
  p[1] = val & 0xFF;
}

static inline uint16_t read_uint16(const uint8_t *p) {
  return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static inline void write_uint32(uint8_t *p, uint32_t val) {
  p[0] = (val >> 24) & 0xFF;
  p[1] = (val >> 16) & 0xFF;
  p[2] = (val >> 8) & 0xFF;
  p[3] = val & 0xFF;
}

static inline uint32_t read_uint32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void write_uint64(uint8_t *p, uint64_t val) {
  p[0] = (val >> 56) & 0xFF;
  p[1] = (val >> 48) & 0xFF;
  p[2] = (val >> 40) & 0xFF;
  p[3] = (val >> 32) & 0xFF;
  p[4] = (val >> 24) & 0xFF;
  p[5] = (val >> 16) & 0xFF;
  p[6] = (val >> 8) & 0xFF;
  p[7] = val & 0xFF;
}

static inline uint64_t read_uint64(const uint8_t *p) {
  return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
         ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
         ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
         ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}

// Compactly encode a Node ID: [host_len (2B)] [host (NB)] [port (2B)]
// [cookie_len (2B)] [cookie (MB)]
int swim_encode_node_id(const swim_node_id_t *id, uint8_t *p, uint8_t *q) {
  uint8_t *start = p;
  uint16_t port = id->port;
  const char *cookie = id->cookie;
  int n;

  n = swim_encode_string(id->host, p, q);
  if (n < 0) { return -1; }
  p += n;
  n = swim_encode_int16(port, p, q);
  if (n < 0) { return -1; }
  p += n;
  n = swim_encode_string(cookie, p, q);
  if (n < 0) { return -1; }
  p += n;

  return (int)(p - start);
}

int swim_encode_membership(const swim_member_t *m, uint8_t *p, uint8_t *q) {
  uint8_t *start = p;
  int n;

  n = swim_encode_node_id(&m->id, p, q);
  if (n < 0) { return -1; }
  p += n;

  n = swim_encode_int8((uint8_t)m->status, p, q);
  if (n < 0) { return -1; }
  p += n;

  n = swim_encode_int64(m->incarnation, p, q);
  if (n < 0) { return -1; }
  p += n;

  return (int)(p - start);
}

int swim_encode_int8(uint8_t val, uint8_t *p, uint8_t *q) {
  if (p + 1 > q) {
    return -1;
  }
  *p = val;
  return 1;
}

int swim_encode_int16(uint16_t val, uint8_t *p, uint8_t *q) {
  if (p + 2 > q) {
    return -1;
  }
  uint16_t net_val = htons(val);
  memcpy(p, &net_val, 2);
  return 2;
}

int swim_encode_int32(uint32_t val, uint8_t *p, uint8_t *q) {
  if (p + 4 > q) {
    return -1;
  }
  uint32_t net_val = htonl(val);
  memcpy(p, &net_val, 4);
  return 4;
}

int swim_encode_int64(uint64_t val, uint8_t *p, uint8_t *q) {
  if (p + 8 > q) {
    return -1;
  }
  uint64_t net_val = htobe64(val);
  memcpy(p, &net_val, 8);
  return 8;
}

int swim_encode_string(const char *str, uint8_t *p, uint8_t *q) {
  size_t len = strlen(str);
  if (p + 2 + len > q) {
    return -1;
  }
  uint16_t net_len = htons((uint16_t)len);
  memcpy(p, &net_len, 2);
  memcpy(p + 2, str, len);
  return (int)(2 + len);
}





int swim_encode_message(uint8_t type, const swim_node_id_t *sender, uint32_t seq,
                        const swim_node_id_t *peer, struct swim_gossip_queue_t *q,
                        uint32_t active_members, uint8_t *buf, int bufsz) {
  if (!sender || !buf || bufsz <= 0) {
    return -1;
  }

  uint8_t *p = buf;
  uint8_t *end = buf + bufsz;
  int n;

  // 1. Message Type (1 byte)
  n = swim_encode_int8(type, p, end);
  if (n < 0) { return -1; }
  p += n;

  // 2. Sequence number (4 bytes)
  n = swim_encode_int32(seq, p, end);
  if (n < 0) { return -1; }
  p += n;

  // 3. Sender Node ID
  n = swim_encode_node_id(sender, p, end);
  if (n < 0) { return -1; }
  p += n;

  // 4. Peer Node ID (target for ping_req, source for fwd_ack)
  if (type == SWIM_MSG_PING_REQ || type == SWIM_MSG_FWD_ACK) {
    if (!peer) {
      return -1;
    }
    n = swim_encode_node_id(peer, p, end);
    if (n < 0) { return -1; }
    p += n;
  }

  // Fill gossip into the remaining packet room (DESIGN §8). With no queue
  // (e.g. leave messages) the packet simply ends after the header; the
  // decoder treats a missing gossip section as zero events.
  int gossip_bytes = 0;
  if (q) {
    gossip_bytes = swim_gossip_queue_pack_ex(q, active_members, p, end);
    if (gossip_bytes < 0) {
      return -1;
    }
  }

  p += gossip_bytes;
  return (int)(p - buf);
}

int swim_decode_int8(uint8_t *val, const uint8_t *p, const uint8_t *q) {
  if (p + 1 > q) {
    return -1;
  }
  *val = *p;
  return 1;
}

int swim_decode_int16(uint16_t *val, const uint8_t *p, const uint8_t *q) {
  if (p + 2 > q) {
    return -1;
  }
  uint16_t net_val;
  memcpy(&net_val, p, 2);
  *val = ntohs(net_val);
  return 2;
}

int swim_decode_int32(uint32_t *val, const uint8_t *p, const uint8_t *q) {
  if (p + 4 > q) {
    return -1;
  }
  uint32_t net_val;
  memcpy(&net_val, p, 4);
  *val = ntohl(net_val);
  return 4;
}

int swim_decode_int64(uint64_t *val, const uint8_t *p, const uint8_t *q) {
  if (p + 8 > q) {
    return -1;
  }
  uint64_t net_val;
  memcpy(&net_val, p, 8);
  *val = be64toh(net_val);
  return 8;
}

int swim_decode_string(char *str, size_t max_len, const uint8_t *p, const uint8_t *q) {
  if (p + 2 > q) {
    return -1;
  }
  uint16_t net_len;
  memcpy(&net_len, p, 2);
  size_t len = ntohs(net_len);
  if (p + 2 + len > q) {
    return -1;
  }
  if (len >= max_len) {
    return -1;
  }
  memcpy(str, p + 2, len);
  str[len] = '\0';
  return (int)(2 + len);
}

int swim_decode_node_id(swim_node_id_t *id, const uint8_t *p, const uint8_t *q) {
  const uint8_t *start = p;
  int n;

  n = swim_decode_string(id->host, sizeof(id->host), p, q);
  if (n < 0) { return -1; }
  p += n;

  uint16_t port;
  n = swim_decode_int16(&port, p, q);
  if (n < 0) { return -1; }
  id->port = port;
  p += n;

  n = swim_decode_string(id->cookie, sizeof(id->cookie), p, q);
  if (n < 0) { return -1; }
  p += n;

  return (int)(p - start);
}

int swim_decode_membership(swim_member_t *m, const uint8_t *p, const uint8_t *q) {
  const uint8_t *start = p;
  int n;

  n = swim_decode_node_id(&m->id, p, q);
  if (n < 0) { return -1; }
  p += n;

  uint8_t status;
  n = swim_decode_int8(&status, p, q);
  if (n < 0) { return -1; }
  m->status = (swim_status_t)status;
  p += n;

  n = swim_decode_int64(&m->incarnation, p, q);
  if (n < 0) { return -1; }
  p += n;

  m->dead_at = 0;

  return (int)(p - start);
}

int swim_decode_message(const uint8_t *buf, size_t size, swim_message_t *msg) {
  if (!buf || !msg) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Invalid NULL arguments to swim_decode_message");
  }

  const uint8_t *p = buf;
  const uint8_t *end = buf + size;
  int n;

  // 1. Message Type
  n = swim_decode_int8(&msg->type, p, end);
  if (n < 0) {
    return swim_set_error(SWIM_ERR_INVALID, "Buffer too short for message type");
  }
  if (msg->type < SWIM_MSG_PING || msg->type > SWIM_MSG_LEAVE) {
    return swim_set_error(SWIM_ERR_INVALID, "Invalid message type value");
  }
  p += n;

  // 2. Sequence number
  n = swim_decode_int32(&msg->seq, p, end);
  if (n < 0) {
    return swim_set_error(SWIM_ERR_INVALID, "Buffer too short for sequence number");
  }
  p += n;

  // 3. Sender Node ID
  n = swim_decode_node_id(&msg->sender, p, end);
  if (n < 0) {
    return swim_set_error(SWIM_ERR_INVALID, "Failed to decode sender node ID");
  }
  p += n;

  // 4. Peer Node ID
  if (msg->type == SWIM_MSG_PING_REQ || msg->type == SWIM_MSG_FWD_ACK) {
    n = swim_decode_node_id(&msg->peer, p, end);
    if (n < 0) {
      return swim_set_error(SWIM_ERR_INVALID, "Failed to decode peer node ID");
    }
    p += n;
  } else {
    memset(&msg->peer, 0, sizeof(msg->peer));
  }

  // 5. Gossip Events Payload: a list of members filling the rest of the
  // packet. There is no count; the list runs until the end of the buffer.
  int count = 0;
  while (p < end) {
    if (count >= SWIM_MAX_EVENTS) {
      return swim_set_error(SWIM_ERR_INVALID, "Event count exceeds maximum allowed");
    }
    n = swim_decode_membership(&msg->events[count], p, end);
    if (n < 0) {
      return swim_set_error(SWIM_ERR_INVALID, "Failed to decode gossip event");
    }
    p += n;
    count++;
  }

  msg->event_count = (uint8_t)count;
  return 0;
}
