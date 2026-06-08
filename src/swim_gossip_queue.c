/*
 * swim_gossip_queue.c — Gossip dissemination priority queue.
 *
 * Tracks pending membership events to be piggybacked on
 * outgoing messages. Events are keyed by node ID; a new
 * event supersedes an existing one when its incarnation is
 * higher, or when the incarnation is equal but its priority
 * is higher (DEAD > SUSPECT > ALIVE). Each entry carries a
 * transmit count; entries are pruned after being packed
 * ceil(log2(N)) * 3 * multiplier times, where N is the
 * current cluster size.
 *
 * The queue is a dynamically-grown flat array. There is no
 * heap data structure: qsort is called on the array before
 * every pack_ex, sorting by (priority, transmit_count,
 * node_id). This is simple and fast enough for the small
 * queue sizes typical in SWIM (bounded by MTU / event size).
 */
#include "swim_gossip_queue.h"
#include "swim_errno.h"
#include "swim_codec.h"
#include <stdlib.h>

typedef struct gossip_entry_t gossip_entry_t;
struct gossip_entry_t {
  swim_member_t event;
  uint32_t transmit_count;
  uint32_t multiplier;
};

// A vector of entries
struct swim_gossip_queue_t {
  gossip_entry_t *entries;
  int count;
  int capacity;
};


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

// Create a new, empty gossip queue. Free it with swim_gossip_queue_destroy().
swim_gossip_queue_t *swim_gossip_queue_create(void) {
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

// Destroy the gossip queue and free all associated memory.
void swim_gossip_queue_destroy(swim_gossip_queue_t *q) {
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

// Enqueue a gossip event. Supersession rules: higher incarnation wins; on
// equal incarnations, higher priority (DEAD > SUSPECT > ALIVE) wins.
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

// Pack events into [p, q) in wire format. Writes as many events as fit.
// Returns bytes written, or -1 on error.
int swim_gossip_queue_pack(swim_gossip_queue_t *queue, uint32_t cluster_size,
                              uint8_t *p, uint8_t *q) {
  if (!queue || !p || q < p) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Invalid arguments to swim_gossip_queue_pack");
  }

  if (queue->count == 0) {
    return 0;
  }

  uint8_t *start = p;

  // Sort entries to establish priority order (DEAD > SUSPECT > ALIVE) and
  // lowest transmit count
  qsort(queue->entries, queue->count, sizeof(gossip_entry_t), compare_entries);

  bool keep[queue->count];
  memset(keep, 1, queue->count);

  uint32_t limit = get_transmit_limit(cluster_size);

  // First loop: pack members until the buffer budget or event cap is exhausted
  int packed = 0;
  for (int i = 0; i < queue->count; i++) {
    if (packed >= SWIM_MAX_EVENTS)
      break;
    gossip_entry_t *entry = &queue->entries[i];

    int n = swim_encode_membership(&entry->event, p, q);
    if (n < 0) {
      // Out of buffer space: stop packing
      break;
    }
    p += n;
    packed++;

    // Increment transmit count and check pruning threshold
    entry->transmit_count++;
    keep[i] = (entry->transmit_count < limit * entry->multiplier);
  }

  // Second loop: Squeeze out all pruned (not-keep) entries
  int write_idx = 0;
  for (int i = 0; i < queue->count; i++) {
    if (keep[i]) {
      if (write_idx != i) {
        queue->entries[write_idx] = queue->entries[i];
      }
      write_idx++;
    }
  }

  queue->count = write_idx;

  return (int)(p - start);
}

// Return the number of events currently in the gossip queue.
int swim_gossip_queue_size(const swim_gossip_queue_t *q) {
  return q ? q->count : 0;
}

// Copy up to max_len queued events (in priority order) into out_events.
// For testing and debugging. Returns count copied, or -1 on error.
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

  // make a copy so we can qsort()
  // TODO: is there any harm in just sorting in place?
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
