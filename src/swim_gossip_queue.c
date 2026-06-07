#include "swim_gossip_queue.h"
#include "swim_errno.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static inline void write_uint16(uint8_t *p, uint16_t val) {
  p[0] = (val >> 8) & 0xFF;
  p[1] = val & 0xFF;
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

typedef struct {
  swim_member_t event;
  uint32_t transmit_count;
  uint32_t multiplier;
} gossip_entry_t;

struct swim_gossip_queue_t {
  gossip_entry_t *entries;
  int count;
  int capacity;
};

// Calculate size on the wire for a node ID
static inline size_t event_wire_size(const swim_node_id_t *id) {
  // 1 byte type + 8 bytes incarnation + 1 byte host length + host bytes + 2
  // bytes port + 1 byte cookie length + cookie bytes
  return 1 + 8 + 1 + strlen(id->host) + 2 + 1 + strlen(id->cookie);
}

// Convert status to priority value: DEAD (0) > SUSPECT (1) > ALIVE (2)
static inline int get_priority(swim_status_t status) {
  if (status == SWIM_STATUS_DEAD) {
    return 0;
  }
  if (status == SWIM_STATUS_SUSPECT) {
    return 1;
  }
  return 2;
}

// Compare two gossip entries for qsort
static int compare_entries(const void *a, const void *b) {
  const gossip_entry_t *ea = (const gossip_entry_t *)a;
  const gossip_entry_t *eb = (const gossip_entry_t *)b;

  // 1. Priority (lower value is higher priority: DEAD=0, SUSPECT=1, ALIVE=2)
  int pa = get_priority(ea->event.status);
  int pb = get_priority(eb->event.status);
  if (pa != pb) {
    return (pa < pb) ? -1 : 1;
  }

  // 2. Transmit count (lower is packed first)
  if (ea->transmit_count != eb->transmit_count) {
    return (ea->transmit_count < eb->transmit_count) ? -1 : 1;
  }

  // 3. Stable tie-breaker: Node ID comparison
  return swim_node_id_compare(&ea->event.id, &eb->event.id);
}

// Integer-only ceil(log2(n)) limit multiplier helper
static inline uint32_t get_transmit_limit(uint32_t cluster_size) {
  if (cluster_size == 0) {
    return 1;
  }
  uint32_t n = cluster_size + 1;
  uint32_t val = 0;
  while ((1U << val) < n) {
    val++;
  }
  return val * 3;
}

swim_gossip_queue_t *swim_gossip_queue_init(void) {
  swim_gossip_queue_t *q = calloc(1, sizeof(*q));
  if (!q) {
    swim_set_error(SWIM_ERR_NOMEM,
                   "Failed to allocate swim_gossip_queue_t container");
    return NULL;
  }
  q->entries = NULL;
  q->count = 0;
  q->capacity = 0;
  return q;
}

void swim_gossip_queue_final(swim_gossip_queue_t *q) {
  if (!q)
    return;
  free(q->entries);
  free(q);
}

static int find_entry(const swim_gossip_queue_t *q, const swim_node_id_t *id) {
  for (int i = 0; i < q->count; i++) {
    if (swim_node_id_compare(&q->entries[i].event.id, id) == 0) {
      return i;
    }
  }
  return -1;
}

int swim_gossip_queue_enqueue(swim_gossip_queue_t *q, swim_status_t status,
                              const swim_node_id_t *id, uint64_t incarnation,
                              uint32_t multiplier) {
  if (!q || !id || multiplier < 1) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Invalid arguments to swim_gossip_queue_enqueue");
  }

  int idx = find_entry(q, id);
  if (idx == -1) {
    // Brand new event, insert at end
    if (q->count == q->capacity) {
      int new_capacity = q->capacity == 0 ? 8 : q->capacity * 2;
      gossip_entry_t *new_entries =
          realloc(q->entries, new_capacity * sizeof(gossip_entry_t));
      if (!new_entries) {
        return swim_set_error(SWIM_ERR_NOMEM,
                              "Failed to reallocate gossip queue entries");
      }
      q->entries = new_entries;
      q->capacity = new_capacity;
    }

    gossip_entry_t *entry = &q->entries[q->count];
    entry->event.id = *id;
    entry->event.status = status;
    entry->event.incarnation = incarnation;
    entry->event.dead_at = 0;
    entry->transmit_count = 0;
    entry->multiplier = multiplier;
    q->count++;
    return 0;
  }

  // Existing entry found: apply supersession rules
  gossip_entry_t *existing = &q->entries[idx];
  if (incarnation > existing->event.incarnation) {
    existing->event.status = status;
    existing->event.incarnation = incarnation;
    existing->transmit_count = 0;
    existing->multiplier = multiplier;
  } else if (incarnation == existing->event.incarnation) {
    int incoming_prio = get_priority(status);
    int existing_prio = get_priority(existing->event.status);
    if (incoming_prio <
        existing_prio) { // Lower priority value is higher priority: DEAD=0 >
                         // SUSPECT=1 > ALIVE=2
      existing->event.status = status;
      existing->transmit_count = 0;
      if (multiplier > existing->multiplier) {
        existing->multiplier = multiplier;
      }
    }
  }
  // Otherwise, ignore (stale incarnation, or same incarnation but lower/equal
  // priority)
  return 0;
}

int swim_gossip_queue_pack_ex(swim_gossip_queue_t *q, uint32_t cluster_size,
                              char* buf, int bufsz) {
  if (!q || !buf || bufsz < 0) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Invalid arguments to swim_gossip_queue_pack_ex");
  }

  if (bufsz < 2) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Buffer too small for swim_gossip_queue_pack_ex header");
  }

  if (q->count == 0) {
    write_uint16((uint8_t *)buf, 0);
    return 2;
  }

  // Sort entries to establish priority order (DEAD > SUSPECT > ALIVE) and
  // lowest transmit count
  qsort(q->entries, q->count, sizeof(gossip_entry_t), compare_entries);

  bool keep[q->count];
  memset(keep, 1, q->count);

  uint32_t limit = get_transmit_limit(cluster_size);
  int packed_count = 0;
  int offset = 2;

  // First loop: Pack entries until buffer budget is exhausted
  for (int i = 0; i < q->count; i++) {
    gossip_entry_t *entry = &q->entries[i];
    const swim_member_t *ev = &entry->event;

    size_t host_len = strlen(ev->id.host);
    size_t cookie_len = strlen(ev->id.cookie);

    // Calculate node ID body size:
    // host:encoded-string -> 2 + host_len
    // port:int2 -> 2
    // cookie:string -> 2 + cookie_len
    size_t node_id_body_size = 2 + host_len + 2 + 2 + cookie_len;
    size_t node_id_total_size = 2 + node_id_body_size;

    // Calculate member body size:
    // {encoded-node_id} -> node_id_total_size
    // {status:int1} -> 1
    // {incarnation:int64} -> 8
    size_t member_body_size = node_id_total_size + 1 + 8;
    size_t member_total_size = 2 + member_body_size;

    if (offset + member_total_size > (size_t)bufsz) {
      // Out of buffer space: stop packing
      break;
    }

    // Pack the member
    uint8_t *p = (uint8_t *)buf + offset;

    // member LEN
    write_uint16(p, (uint16_t)member_body_size);
    p += 2;

    // node_id LEN
    write_uint16(p, (uint16_t)node_id_body_size);
    p += 2;

    // host LEN
    write_uint16(p, (uint16_t)host_len);
    p += 2;

    // host chars
    memcpy(p, ev->id.host, host_len);
    p += host_len;

    // port
    write_uint16(p, ev->id.port);
    p += 2;

    // cookie LEN
    write_uint16(p, (uint16_t)cookie_len);
    p += 2;

    // cookie chars
    memcpy(p, ev->id.cookie, cookie_len);
    p += cookie_len;

    // status
    *p = (uint8_t)ev->status;
    p += 1;

    // incarnation
    write_uint64(p, ev->incarnation);
    p += 8;

    offset += member_total_size;
    packed_count++;

    // Increment transmit count and check pruning threshold
    entry->transmit_count++;
    keep[i] = (entry->transmit_count < limit * entry->multiplier); 
  }

  // Second loop: Squeeze out all pruned (not-keep) entries
  int write_idx = 0;
  for (int i = 0; i < q->count; i++) {
    if (keep[i]) {
      if (write_idx != i) {
        q->entries[write_idx] = q->entries[i];
      }
      write_idx++;
    }
  }

  q->count = write_idx;

  write_uint16((uint8_t *)buf, (uint16_t)packed_count);
  return offset;
}


int swim_gossip_queue_size(const swim_gossip_queue_t *q) {
  return q ? q->count : 0;
}

int swim_gossip_queue_peek(const swim_gossip_queue_t *q,
                           swim_member_t *out_events, int max_len) {
  if (!q || !out_events || max_len < 0) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Invalid arguments to swim_gossip_queue_peek");
  }

  int n = q->count;
  if (n == 0) {
    return 0;
  }

  gossip_entry_t *temp = malloc(n * sizeof(gossip_entry_t));
  if (!temp) {
    return swim_set_error(SWIM_ERR_NOMEM,
                          "Failed to allocate temporary copy for peek");
  }
  memcpy(temp, q->entries, n * sizeof(gossip_entry_t));
  qsort(temp, n, sizeof(gossip_entry_t), compare_entries);

  int copied = 0;
  for (int i = 0; i < n && copied < max_len; i++) {
    out_events[copied] = temp[i].event;
    copied++;
  }

  free(temp);
  return copied;
}
