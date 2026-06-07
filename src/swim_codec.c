#define _DEFAULT_SOURCE
#include "swim_codec.h"
#include "swim_errno.h"
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
  if (p + 1 >= q) {
    return -1;
  }
  *p = val;
  return 1;
}

int swim_encode_int16(uint16_t val, uint8_t *p, uint8_t *q) {
  if (p + 2 >= q) {
    return -1;
  }
  uint16_t net_val = htons(val);
  memcpy(p, &net_val, 2);
  return 2;
}

int swim_encode_int32(uint32_t val, uint8_t *p, uint8_t *q) {
  if (p + 4 >= q) {
    return -1;
  }
  uint32_t net_val = htonl(val);
  memcpy(p, &net_val, 4);
  return 4;
}

int swim_encode_int64(uint64_t val, uint8_t *p, uint8_t *q) {
  if (p + 8 >= q) {
    return -1;
  }
  uint64_t net_val = htobe64(val);
  memcpy(p, &net_val, 8);
  return 8;
}

int swim_encode_string(const char *str, uint8_t *p, uint8_t *q) {
  size_t len = strlen(str);
  if (p + 2 + len >= q) {
    return -1;
  }
  uint16_t net_len = htons((uint16_t)len);
  memcpy(p, &net_len, 2);
  memcpy(p + 2, str, len);
  return (int)(2 + len);
}

// Decode a compactly encoded Node ID with overflow checks
static int decode_node_id(swim_node_id_t *id, const uint8_t *buf, size_t limit,
                          size_t *offset) {
  if (*offset + 2 > limit) {
    return -1;
  }
  size_t host_len = read_uint16(&buf[*offset]);
  *offset += 2;

  // Prevent buffer overflow (sizeof host is 64)
  if (host_len >= sizeof(id->host)) {
    return -1;
  }
  if (*offset + host_len + 2 + 2 > limit) {
    return -1;
  }

  memcpy(id->host, &buf[*offset], host_len);
  id->host[host_len] = '\0';
  *offset += host_len;

  id->port = read_uint16(&buf[*offset]);
  *offset += 2;

  size_t cookie_len = read_uint16(&buf[*offset]);
  *offset += 2;

  // Prevent buffer overflow (sizeof cookie is 64)
  if (cookie_len >= sizeof(id->cookie)) {
    return -1;
  }
  if (*offset + cookie_len > limit) {
    return -1;
  }

  memcpy(id->cookie, &buf[*offset], cookie_len);
  id->cookie[cookie_len] = '\0';
  *offset += cookie_len;

  return 0;
}



int swim_codec_decode(const uint8_t *buf, size_t size, swim_message_t *msg) {
  if (!buf || !msg) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Invalid NULL arguments to swim_codec_decode");
  }

  size_t offset = 0;

  // 1. Message Type
  if (offset + 1 > size) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Buffer too short for message type");
  }
  msg->type = buf[offset++];
  if (msg->type < SWIM_MSG_PING || msg->type > SWIM_MSG_LEAVE) {
    return swim_set_error(SWIM_ERR_INVALID, "Invalid message type value");
  }

  // 2. Sequence number
  if (offset + 4 > size) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Buffer too short for sequence number");
  }
  msg->seq = read_uint32(&buf[offset]);
  offset += 4;

  // 3. Sender Node ID
  if (decode_node_id(&msg->sender, buf, size, &offset) != 0) {
    return swim_set_error(SWIM_ERR_INVALID, "Failed to decode sender node ID");
  }

  // 4. Peer Node ID
  if (msg->type == SWIM_MSG_PING_REQ || msg->type == SWIM_MSG_FWD_ACK) {
    if (decode_node_id(&msg->peer, buf, size, &offset) != 0) {
      return swim_set_error(SWIM_ERR_INVALID, "Failed to decode peer node ID");
    }
  } else {
    memset(&msg->peer, 0, sizeof(msg->peer));
  }

  // 5. Gossip Events Payload
  if (offset == size) {
    msg->event_count = 0;
    return 0;
  }

  if (offset + 2 > size) {
    return swim_set_error(SWIM_ERR_INVALID, "Buffer too short for gossip count");
  }
  uint16_t g_count = read_uint16(&buf[offset]);
  offset += 2;

  if (g_count > SWIM_MAX_EVENTS) {
    return swim_set_error(SWIM_ERR_INVALID, "Event count exceeds maximum allowed");
  }

  msg->event_count = (uint8_t)g_count;

  for (int i = 0; i < g_count; i++) {
    swim_member_t *ev = &msg->events[i];

    if (offset + 2 > size) {
      return swim_set_error(SWIM_ERR_INVALID, "Buffer too short for member length");
    }
    uint16_t member_body_len = read_uint16(&buf[offset]);
    offset += 2;

    if (offset + member_body_len > size) {
      return swim_set_error(SWIM_ERR_INVALID, "Buffer too short for member body");
    }

    size_t member_end = offset + member_body_len;

    // Decode node ID LEN
    if (offset + 2 > member_end) {
      return swim_set_error(SWIM_ERR_INVALID, "Member body too short for node ID length");
    }
    uint16_t node_id_body_len = read_uint16(&buf[offset]);
    offset += 2;

    size_t node_id_end = offset + node_id_body_len;
    if (node_id_end > member_end) {
      return swim_set_error(SWIM_ERR_INVALID, "Node ID body exceeds member body size");
    }

    // Decode host LEN
    if (offset + 2 > node_id_end) {
      return swim_set_error(SWIM_ERR_INVALID, "Node ID body too short for host length");
    }
    uint16_t host_len = read_uint16(&buf[offset]);
    offset += 2;

    if (host_len >= sizeof(ev->id.host)) {
      return swim_set_error(SWIM_ERR_INVALID, "Host name too long");
    }
    if (offset + host_len > node_id_end) {
      return swim_set_error(SWIM_ERR_INVALID, "Host characters exceed node ID body");
    }
    memcpy(ev->id.host, &buf[offset], host_len);
    ev->id.host[host_len] = '\0';
    offset += host_len;

    // Decode port
    if (offset + 2 > node_id_end) {
      return swim_set_error(SWIM_ERR_INVALID, "Node ID body too short for port");
    }
    ev->id.port = read_uint16(&buf[offset]);
    offset += 2;

    // Decode cookie LEN
    if (offset + 2 > node_id_end) {
      return swim_set_error(SWIM_ERR_INVALID, "Node ID body too short for cookie length");
    }
    uint16_t cookie_len = read_uint16(&buf[offset]);
    offset += 2;

    if (cookie_len >= sizeof(ev->id.cookie)) {
      return swim_set_error(SWIM_ERR_INVALID, "Cookie too long");
    }
    if (offset + cookie_len > node_id_end) {
      return swim_set_error(SWIM_ERR_INVALID, "Cookie characters exceed node ID body");
    }
    memcpy(ev->id.cookie, &buf[offset], cookie_len);
    ev->id.cookie[cookie_len] = '\0';
    offset += cookie_len;

    if (offset != node_id_end) {
      return swim_set_error(SWIM_ERR_INVALID, "Inconsistent node ID body parsing offset");
    }

    // Decode status
    if (offset + 1 > member_end) {
      return swim_set_error(SWIM_ERR_INVALID, "Member body too short for status");
    }
    ev->status = (swim_status_t)buf[offset];
    offset += 1;

    // Decode incarnation
    if (offset + 8 > member_end) {
      return swim_set_error(SWIM_ERR_INVALID, "Member body too short for incarnation");
    }
    ev->incarnation = read_uint64(&buf[offset]);
    offset += 8;

    if (offset != member_end) {
      return swim_set_error(SWIM_ERR_INVALID, "Inconsistent member body parsing offset");
    }

    ev->dead_at = 0;
  }

  return 0;
}
