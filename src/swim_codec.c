#include "swim_codec.h"
#include "swim_errno.h"
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
  p[2] = (val >> 8)  & 0xFF;
  p[3] = val & 0xFF;
}

static inline uint32_t read_uint32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) |
         ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8)  |
         (uint32_t)p[3];
}

static inline void write_uint64(uint8_t *p, uint64_t val) {
  p[0] = (val >> 56) & 0xFF;
  p[1] = (val >> 48) & 0xFF;
  p[2] = (val >> 40) & 0xFF;
  p[3] = (val >> 32) & 0xFF;
  p[4] = (val >> 24) & 0xFF;
  p[5] = (val >> 16) & 0xFF;
  p[6] = (val >> 8)  & 0xFF;
  p[7] = val & 0xFF;
}

static inline uint64_t read_uint64(const uint8_t *p) {
  return ((uint64_t)p[0] << 56) |
         ((uint64_t)p[1] << 48) |
         ((uint64_t)p[2] << 40) |
         ((uint64_t)p[3] << 32) |
         ((uint64_t)p[4] << 24) |
         ((uint64_t)p[5] << 16) |
         ((uint64_t)p[6] << 8)  |
         (uint64_t)p[7];
}

// Compactly encode a Node ID: [host_len (1B)] [host (NB)] [port (2B)] [cookie_len (1B)] [cookie (MB)]
static int encode_node_id(const swim_node_id_t *id, uint8_t *buf, size_t limit, size_t *offset) {
  size_t host_len = strlen(id->host);
  size_t cookie_len = strlen(id->cookie);

  if (*offset + 1 + host_len + 2 + 1 + cookie_len > limit) {
    return -1;
  }

  buf[*offset] = (uint8_t)host_len;
  (*offset)++;
  memcpy(&buf[*offset], id->host, host_len);
  *offset += host_len;

  write_uint16(&buf[*offset], id->port);
  *offset += 2;

  buf[*offset] = (uint8_t)cookie_len;
  (*offset)++;
  memcpy(&buf[*offset], id->cookie, cookie_len);
  *offset += cookie_len;

  return 0;
}

// Decode a compactly encoded Node ID with overflow checks
static int decode_node_id(swim_node_id_t *id, const uint8_t *buf, size_t limit, size_t *offset) {
  if (*offset + 1 > limit) {
    return -1;
  }
  size_t host_len = buf[*offset];
  (*offset)++;

  // Prevent buffer overflow (sizeof host is 64)
  if (host_len >= sizeof(id->host)) {
    return -1;
  }
  if (*offset + host_len + 2 + 1 > limit) {
    return -1;
  }

  memcpy(id->host, &buf[*offset], host_len);
  id->host[host_len] = '\0';
  *offset += host_len;

  id->port = read_uint16(&buf[*offset]);
  *offset += 2;

  size_t cookie_len = buf[*offset];
  (*offset)++;

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

int swim_codec_encode(const swim_message_t *msg, uint8_t *buf, size_t size) {
  if (!msg || !buf || size == 0) {
    return swim_set_error(SWIM_ERR_INVALID, "Invalid NULL or zero size arguments to swim_codec_encode");
  }

  if (msg->event_count > SWIM_MAX_EVENTS) {
    return swim_set_error(SWIM_ERR_INVALID, "Event count exceeds maximum allowed");
  }

  size_t offset = 0;

  // 1. Message Type (1 byte)
  if (offset + 1 > size) return -1;
  buf[offset++] = msg->type;

  // 2. Sequence number (4 bytes)
  if (offset + 4 > size) return -1;
  write_uint32(&buf[offset], msg->seq);
  offset += 4;

  // 3. Event count (1 byte)
  if (offset + 1 > size) return -1;
  buf[offset++] = msg->event_count;

  // 4. Sender Node ID
  if (encode_node_id(&msg->sender, buf, size, &offset) != 0) {
    return -1;
  }

  // 5. Peer Node ID (target for ping_req, source for fwd_ack)
  if (msg->type == SWIM_MSG_PING_REQ || msg->type == SWIM_MSG_FWD_ACK) {
    if (encode_node_id(&msg->peer, buf, size, &offset) != 0) {
      return -1;
    }
  }

  // 6. Events Payload
  for (int i = 0; i < msg->event_count; i++) {
    const swim_member_t *ev = &msg->events[i];

    // Event Status (1 byte)
    if (offset + 1 > size) return -1;
    buf[offset++] = (uint8_t)ev->status;

    // Incarnation (8 bytes)
    if (offset + 8 > size) return -1;
    write_uint64(&buf[offset], ev->incarnation);
    offset += 8;

    // Subject Node ID
    if (encode_node_id(&ev->id, buf, size, &offset) != 0) {
      return -1;
    }
  }

  return (int)offset;
}

int swim_codec_decode(const uint8_t *buf, size_t size, swim_message_t *msg) {
  if (!buf || !msg) {
    return swim_set_error(SWIM_ERR_INVALID, "Invalid NULL arguments to swim_codec_decode");
  }

  size_t offset = 0;

  // 1. Message Type
  if (offset + 1 > size) {
    return swim_set_error(SWIM_ERR_INVALID, "Buffer too short for message type");
  }
  msg->type = buf[offset++];
  if (msg->type < SWIM_MSG_PING || msg->type > SWIM_MSG_FWD_ACK) {
    return swim_set_error(SWIM_ERR_INVALID, "Invalid message type value");
  }

  // 2. Sequence number
  if (offset + 4 > size) {
    return swim_set_error(SWIM_ERR_INVALID, "Buffer too short for sequence number");
  }
  msg->seq = read_uint32(&buf[offset]);
  offset += 4;

  // 3. Event count
  if (offset + 1 > size) {
    return swim_set_error(SWIM_ERR_INVALID, "Buffer too short for event count");
  }
  msg->event_count = buf[offset++];
  if (msg->event_count > SWIM_MAX_EVENTS) {
    return swim_set_error(SWIM_ERR_INVALID, "Event count exceeds maximum allowed");
  }

  // 4. Sender Node ID
  if (decode_node_id(&msg->sender, buf, size, &offset) != 0) {
    return swim_set_error(SWIM_ERR_INVALID, "Failed to decode sender node ID");
  }

  // 5. Peer Node ID
  if (msg->type == SWIM_MSG_PING_REQ || msg->type == SWIM_MSG_FWD_ACK) {
    if (decode_node_id(&msg->peer, buf, size, &offset) != 0) {
      return swim_set_error(SWIM_ERR_INVALID, "Failed to decode peer node ID");
    }
  } else {
    // Clear for clean consistency
    memset(&msg->peer, 0, sizeof(msg->peer));
  }

  // 6. Events Payload
  for (int i = 0; i < msg->event_count; i++) {
    swim_member_t *ev = &msg->events[i];

    // Status
    if (offset + 1 > size) {
      return swim_set_error(SWIM_ERR_INVALID, "Buffer too short for event status");
    }
    ev->status = (swim_status_t)buf[offset++];
    if (ev->status != SWIM_STATUS_ALIVE &&
        ev->status != SWIM_STATUS_SUSPECT &&
        ev->status != SWIM_STATUS_DEAD) {
      return swim_set_error(SWIM_ERR_INVALID, "Invalid event status value");
    }

    // Incarnation
    if (offset + 8 > size) {
      return swim_set_error(SWIM_ERR_INVALID, "Buffer too short for event incarnation");
    }
    ev->incarnation = read_uint64(&buf[offset]);
    offset += 8;

    // Node ID
    if (decode_node_id(&ev->id, buf, size, &offset) != 0) {
      return swim_set_error(SWIM_ERR_INVALID, "Failed to decode event node ID");
    }

    ev->dead_at = 0;
  }

  return 0;
}
