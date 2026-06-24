#define _POSIX_C_SOURCE 200809L
#include "nodeid.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Sentinel 0xFFFF must not be a valid slot index. */
_Static_assert(NODEID_TABLE_SIZE <= 0xFFFF,
               "NODEID_TABLE_SIZE must be <= 0xFFFF to keep sentinel valid");

/*
 * g_table[slot] holds the registered nodeid string; NULL means empty.
 * All slots 0..NODEID_TABLE_SIZE-1 are usable.
 * Each pointer is written once under g_mu with a release store,
 * so lock-free readers can safely load with acquire.
 */
static char *_Atomic g_table[NODEID_TABLE_SIZE];
static _Atomic uint16_t g_count = 0;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

/* FNV-1a 32-bit string hash. */
static uint32_t nodeid_hash(const char *s) {
  uint32_t h = 2166136261u;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    h ^= (uint32_t)*p;
    h *= 16777619u;
  }
  return h;
}

/* Map hash to probe slot in 0..NODEID_TABLE_SIZE-1 via bitmask. */
static inline uint16_t hash_slot(uint32_t h) {
  return (uint16_t)(h & (NODEID_TABLE_SIZE - 1));
}

/* Advance probe slot, wrapping within 0..NODEID_TABLE_SIZE-1. */
static inline uint16_t next_slot(uint16_t slot) {
  return (uint16_t)((slot + 1) & (NODEID_TABLE_SIZE - 1));
}

/* Find existing nodeid without registering; lock-free. */
nodeid_idx_t nodeid_find(const char *nodeid) {
  uint16_t slot = hash_slot(nodeid_hash(nodeid));
  for (uint16_t i = 0; i < NODEID_TABLE_SIZE; i++) {
    char *p = atomic_load_explicit(&g_table[slot], memory_order_acquire);
    if (!p)
      return NODEID_NONE;
    if (strcmp(p, nodeid) == 0)
      return (nodeid_idx_t){slot};
    slot = next_slot(slot);
  }
  return NODEID_NONE;
}

/* Register nodeid; idempotent. Returns NODEID_NONE if pool is full. */
nodeid_idx_t nodeid_register(const char *nodeid) {
  pthread_mutex_lock(&g_mu); /* acquire registration lock */
  if (atomic_load_explicit(&g_count, memory_order_relaxed) >= NODEID_POOL_MAX) {
    goto none;
  }
  uint16_t slot = hash_slot(nodeid_hash(nodeid));
  for (uint16_t i = 0; i < NODEID_TABLE_SIZE; i++) {
    char *p = atomic_load_explicit(&g_table[slot], memory_order_relaxed);
    if (!p) {
      char *s = strdup(nodeid);
      if (!s)
        goto none;
      /* Publish with release so lock-free readers see the complete string. */
      atomic_store_explicit(&g_table[slot], s, memory_order_release);
      atomic_fetch_add_explicit(&g_count, 1, memory_order_relaxed);
      pthread_mutex_unlock(&g_mu); /* release registration lock */
      return (nodeid_idx_t){slot};
    }
    if (strcmp(p, nodeid) == 0) {
      pthread_mutex_unlock(&g_mu); /* release registration lock */
      return (nodeid_idx_t){slot};
    }
    slot = next_slot(slot);
  }

none:
  pthread_mutex_unlock(&g_mu); /* release registration lock */
  return NODEID_NONE;
}

/* Return nodeid string for idx; lock-free. */
const char *nodeid_lookup(nodeid_idx_t idx) {
  if (idx.v >= NODEID_TABLE_SIZE)
    return NULL;
  return atomic_load_explicit(&g_table[idx.v], memory_order_acquire);
}

/* Free all registered nodeids and destroy the pool mutex. Call at shutdown. */
void nodeid_destroy(void) {
  pthread_mutex_lock(&g_mu); /* acquire registration lock */
  for (uint16_t i = 0; i < NODEID_TABLE_SIZE; i++) {
    char *p = atomic_load_explicit(&g_table[i], memory_order_relaxed);
    if (p) {
      free(p);
      atomic_store_explicit(&g_table[i], NULL, memory_order_relaxed);
    }
  }
  atomic_store_explicit(&g_count, 0, memory_order_relaxed);
  pthread_mutex_unlock(&g_mu);  /* release before destroy */
  pthread_mutex_destroy(&g_mu); /* destroy pool mutex */
}

/* Parse a nodeid index back into its host name, port, and optional first_pos.
 */
int nodeid_split(nodeid_idx_t idx, char host[254], int *port, int *first_pos) {
  const char *node_str = nodeid_lookup(idx);
  if (!node_str)
    return -1;

  const char *colon = strchr(node_str, ':');
  if (!colon)
    return -1;

  size_t host_len = (size_t)(colon - node_str);
  if (host_len >= 254)
    return -1;

  memcpy(host, node_str, host_len);
  host[host_len] = '\0';

  const char *slash = strchr(colon + 1, '/');
  if (port)
    *port = atoi(colon + 1);

  if (first_pos) {
    if (slash)
      *first_pos = atoi(slash + 1);
    else
      *first_pos = 0;
  }

  return 0;
}

/* Helper FNV-1a hash with custom basis/seed. */
static uint32_t bloom_hash(const char *s, uint32_t basis) {
  uint32_t h = basis;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    h ^= (uint32_t)*p;
    h *= 16777619u;
  }
  return h;
}

/* Clear the Bloom filter. */
void nodeid_bloom_init(nodeid_bloom_t *bf) {
  memset(bf->bits, 0, sizeof(bf->bits));
}

/* Add a nodeid string to the Bloom filter. */
void nodeid_bloom_add(nodeid_bloom_t *bf, const char *nodeid) {
  uint32_t h1 = bloom_hash(nodeid, 2166136261u);
  uint32_t h2 = bloom_hash(nodeid, 0u);
  uint32_t h3 = bloom_hash(nodeid, 123456789u);
  uint32_t h4 = bloom_hash(nodeid, 987654321u);

  bf->bits[(h1 % 4096) / 8] |= (uint8_t)(1 << (h1 % 8));
  bf->bits[(h2 % 4096) / 8] |= (uint8_t)(1 << (h2 % 8));
  bf->bits[(h3 % 4096) / 8] |= (uint8_t)(1 << (h3 % 8));
  bf->bits[(h4 % 4096) / 8] |= (uint8_t)(1 << (h4 % 8));
}

/* Test if a nodeid string is present in the Bloom filter. */
int nodeid_bloom_test(const nodeid_bloom_t *bf, const char *nodeid) {
  uint32_t h1 = bloom_hash(nodeid, 2166136261u);
  uint32_t h2 = bloom_hash(nodeid, 0u);
  uint32_t h3 = bloom_hash(nodeid, 123456789u);
  uint32_t h4 = bloom_hash(nodeid, 987654321u);

  if (!(bf->bits[(h1 % 4096) / 8] & (1 << (h1 % 8))))
    return 0;
  if (!(bf->bits[(h2 % 4096) / 8] & (1 << (h2 % 8))))
    return 0;
  if (!(bf->bits[(h3 % 4096) / 8] & (1 << (h3 % 8))))
    return 0;
  if (!(bf->bits[(h4 % 4096) / 8] & (1 << (h4 % 8))))
    return 0;
  return 1;
}
