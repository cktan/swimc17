/*
 * swim_membership.c — Sorted node membership list.
 *
 * Maintains the set of known peers and their SWIM states
 * (ALIVE, SUSPECT, DEAD). Nodes are kept in a dynamically-
 * grown array sorted by node ID, giving O(log n) lookup via
 * binary search and O(n) insertion via memmove.
 *
 * State transitions follow SWIM+Suspicion precedence rules:
 * higher incarnation always wins; at equal incarnation,
 * DEAD > SUSPECT > ALIVE. DEAD is terminal for a given
 * incarnation — a dead node can only be revived by an ALIVE
 * event with a strictly higher incarnation number. Unknown
 * nodes are accepted only as ALIVE (not SUSPECT or DEAD).
 *
 * swim_membership_gc() reclaims dead entries older than a
 * configurable expiry window by compacting the array in a
 * single pass.
 */
#include "swim_membership.h"
#include "swim_errno.h"

#include <stdlib.h>
#include <string.h>

struct swim_membership_t {
  swim_member_t *members;
  int count;
  int capacity;
};

// Internal binary search helper.
// Returns true if found and sets *out_idx. If not found, returns false and sets
// *out_idx to the insertion point.
static bool find_node(const swim_membership_t *m, const swim_node_id_t *id,
                      int *out_idx) {
  int low = 0;
  int high = m->count - 1;
  while (low <= high) {
    int mid = low + (high - low) / 2;
    int r = swim_node_id_compare(&m->members[mid].id, id);
    if (r == 0) {
      *out_idx = mid;
      return true;
    }
    if (r < 0) {
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }
  *out_idx = low;
  return false;
}

// Create an empty membership list. Free it with swim_membership_destroy().
swim_membership_t *swim_membership_create(void) {
  swim_membership_t *m = calloc(1, sizeof(*m));
  if (!m) {
    swim_set_error(SWIM_ERR_NOMEM,
                   "Failed to allocate swim_membership_t container");
    return NULL;
  }
  m->members = NULL;
  m->count = 0;
  m->capacity = 0;
  return m;
}

// Destroy the membership list and free all associated memory.
void swim_membership_destroy(swim_membership_t *m) {
  if (!m)
    return;
  free(m->members);
  free(m);
}

// Add a new node as ALIVE. Delegates to swim_membership_apply_event.
int swim_membership_add(swim_membership_t *m, const swim_node_id_t *id,
                        uint64_t incarnation) {
  return swim_membership_apply_event(m, SWIM_STATUS_ALIVE, id, incarnation, 0);
}

// Force a node to ALIVE, bypassing normal incarnation precedence rules.
int swim_membership_set_alive(swim_membership_t *m, const swim_node_id_t *id,
                              uint64_t incarnation) {
  if (!m || !id) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Invalid NULL argument to swim_membership_set_alive");
  }
  int idx;
  if (find_node(m, id, &idx)) {
    m->members[idx].status = SWIM_STATUS_ALIVE;
    m->members[idx].incarnation = incarnation;
    m->members[idx].dead_at = 0;
    return 0;
  }
  // Not found, add a new one
  return swim_membership_apply_event(m, SWIM_STATUS_ALIVE, id, incarnation, 0);
}

// Look up a node by ID. Returns pointer to member entry, or NULL if not found.
const swim_member_t *swim_membership_get(const swim_membership_t *m,
                                         const swim_node_id_t *id) {
  if (!m || !id)
    return NULL;
  int idx;
  if (find_node(m, id, &idx)) {
    return &m->members[idx];
  }
  return NULL;
}

// Apply a gossip event (ALIVE/SUSPECT/DEAD) following SWIM+Suspension
// precedence and incarnation rules. Returns 0 if state changed, 1 if ignored
// (stale), -1 on error.
int swim_membership_apply_event(swim_membership_t *m, swim_status_t status,
                                const swim_node_id_t *id, uint64_t incarnation,
                                uint64_t now_ms) {
  if (!m || !id) {
    return swim_set_error(
        SWIM_ERR_INVALID,
        "Invalid NULL argument to swim_membership_apply_event");
  }

  int idx;
  if (find_node(m, id, &idx)) {
    swim_member_t *member = &m->members[idx];

    // Higher incarnation always wins; at equal incarnation, DEAD > SUSPECT > ALIVE.
    // Node is already known, apply precedence rules:
    if (member->status == SWIM_STATUS_DEAD) {
      // DEAD is terminal for a given incarnation. Revive only if inc is
      // strictly higher and the incoming status is ALIVE.
      if (incarnation > member->incarnation && status == SWIM_STATUS_ALIVE) {
        member->status = SWIM_STATUS_ALIVE;
        member->incarnation = incarnation;
        member->dead_at = 0;
        return 0; // State updated
      }
      return 1; // Ignored
    }

    // Node is ALIVE or SUSPECT:
    if (incarnation > member->incarnation) {
      member->status = status;
      member->incarnation = incarnation;
      member->dead_at = (status == SWIM_STATUS_DEAD) ? now_ms : 0;
      return 0; // State updated
    } else if (incarnation == member->incarnation) {
      // Same incarnation, check state precedence: DEAD > SUSPECT > ALIVE
      if (status == SWIM_STATUS_DEAD) {
        member->status = SWIM_STATUS_DEAD;
        member->dead_at = now_ms;
        return 0; // State updated
      }
      if (member->status == SWIM_STATUS_ALIVE &&
          status == SWIM_STATUS_SUSPECT) {
        member->status = SWIM_STATUS_SUSPECT;
        return 0; // State updated
      }
      return 1; // Ignored
    } else {
      // Stale incarnation (event_inc < member->incarnation)
      return 1; // Ignored
    }
  } else {
    // Node is unknown:
    // Only accept ALIVE events for unknown nodes (new joins).
    if (status != SWIM_STATUS_ALIVE) {
      return 1; // Ignored
    }

    // Grow array capacity if full
    if (m->count == m->capacity) {
      int new_capacity = m->capacity == 0 ? 16 : m->capacity * 2;
      swim_member_t *new_members =
          realloc(m->members, new_capacity * sizeof(swim_member_t));
      if (!new_members) {
        return swim_set_error(SWIM_ERR_NOMEM,
                              "Failed to reallocate membership list");
      }
      m->members = new_members;
      m->capacity = new_capacity;
    }

    // Shift subsequent elements to the right to maintain sorted order
    memmove(&m->members[idx + 1], &m->members[idx],
            (m->count - idx) * sizeof(swim_member_t));

    swim_member_t *member = &m->members[idx];
    member->id = *id;
    member->status = SWIM_STATUS_ALIVE;
    member->incarnation = incarnation;
    member->dead_at = 0;
    m->count++;

    return 0; // Node added
  }
}

// Remove nodes that have been dead for longer than expiry_ms.
void swim_membership_gc(swim_membership_t *m, uint64_t expiry_ms,
                        uint64_t now_ms) {
  if (!m)
    return;

  int write_idx = 0;
  for (int read_idx = 0; read_idx < m->count; read_idx++) {
    swim_member_t *member = &m->members[read_idx];
    if (member->status == SWIM_STATUS_DEAD &&
        (now_ms - member->dead_at >= expiry_ms)) {
      // GC/prune this node
      continue;
    }
    if (write_idx != read_idx) {
      m->members[write_idx] = m->members[read_idx];
    }
    write_idx++;
  }
  m->count = write_idx;
}

// Return the count of active (non-dead: ALIVE or SUSPECT) members.
int swim_membership_count(const swim_membership_t *m) {
  if (!m)
    return 0;
  int active = 0;
  for (int i = 0; i < m->count; i++) {
    if (m->members[i].status != SWIM_STATUS_DEAD) {
      active++;
    }
  }
  return active;
}

// Copy up to max_len members into out_list. include_dead controls whether dead
// nodes are included. Returns count copied, or -1 on error.
int swim_membership_list(const swim_membership_t *m, swim_member_t *out_list,
                         int max_len, bool include_dead) {
  if (!m || !out_list || max_len < 0) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Invalid arguments to swim_membership_list");
  }

  int copied = 0;
  for (int i = 0; i < m->count && copied < max_len; i++) {
    const swim_member_t *member = &m->members[i];
    if (!include_dead && member->status == SWIM_STATUS_DEAD) {
      continue;
    }
    out_list[copied] = *member;
    copied++;
  }
  return copied;
}

static inline size_t align1024(size_t n) { return (n + 1023) & ~(size_t)1023; }

// Build a packed string buffer of formatted peer IDs. Each string is
// NUL-terminated and consecutive in the returned buffer; *count is set to the
// number of strings. Caller must free() the result. Returns NULL on error.
char *swim_membership_peers(const swim_membership_t *m, bool include_dead,
                            int *count) {
  if (!m || !count) {
    swim_set_error(SWIM_ERR_INVALID,
                   "Invalid arguments to swim_membership_peers");
    return NULL;
  }

  char *buf = NULL;
  size_t used = 0;
  int n = 0;

  for (int i = 0; i < m->count; i++) {
    const swim_member_t *member = &m->members[i];
    if (!include_dead && member->status == SWIM_STATUS_DEAD)
      continue;
    char tmp[384];
    swim_node_id_format(&member->id, tmp, sizeof(tmp));
    size_t len = strlen(tmp) + 1;
    // Allocate in 1 KiB chunks to amortize realloc overhead.
    char *b = realloc(buf, align1024(used + len));
    if (!b) {
      free(buf);
      swim_set_error(SWIM_ERR_NOMEM, "Failed to allocate peers buffer");
      return NULL;
    }
    buf = b;
    memcpy(buf + used, tmp, len);
    used += len;
    n++;
  }

  if (!buf) {
    buf = malloc(1); // zero peers: valid pointer so caller can always free()
    if (!buf) {
      swim_set_error(SWIM_ERR_NOMEM, "Failed to allocate peers buffer");
      return NULL;
    }
  }
  *count = n;
  return buf;
}
