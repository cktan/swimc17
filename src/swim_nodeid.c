/* Copyright (c) 2026, CK Tan.
 * https://github.com/cktan/swimc17/blob/main/LICENSE
 */
#define _POSIX_C_SOURCE 200809L
#include "swim_nodeid.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Sentinel 0xFFFF must not be a valid slot index. */
_Static_assert(
    SWIM_NODEID_TABLE_SIZE <= 0xFFFF,
    "SWIM_NODEID_TABLE_SIZE must be <= 0xFFFF to keep sentinel valid");

/*
 * g_table[slot] holds the registered nodeid string; NULL means empty.
 * All slots 0..SWIM_NODEID_TABLE_SIZE-1 are usable.
 * Each pointer is written once under g_mu with a release store,
 * so lock-free readers can safely load with acquire.
 */
static char *_Atomic g_table[SWIM_NODEID_TABLE_SIZE];
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

/* Map hash to probe slot in 0..SWIM_NODEID_TABLE_SIZE-1 via bitmask. */
static inline uint16_t hash_slot(uint32_t h) {
  return (uint16_t)(h & (SWIM_NODEID_TABLE_SIZE - 1));
}

/* Advance probe slot, wrapping within 0..SWIM_NODEID_TABLE_SIZE-1. */
static inline uint16_t next_slot(uint16_t slot) {
  return (uint16_t)((slot + 1) & (SWIM_NODEID_TABLE_SIZE - 1));
}

/*
 * Parse "host:port[/cookie]", where host may be a bracketed IPv6 literal
 * ("[::1]:7000"). On success points *host at the host (brackets excluded,
 * length *host_len < 254), sets *port (1..65535), and points *cookie at the
 * cookie ("" if absent, length < 64). Returns 0, or -1 if malformed.
 */
static int parse_nodeid(const char *s, const char **host, size_t *host_len,
                        int *port, const char **cookie) {
  const char *h = s;
  const char *p;
  size_t hl;
  if (*s == '[') {
    const char *rb = strchr(s, ']');
    if (!rb || rb[1] != ':')
      return -1;
    h = s + 1;
    hl = (size_t)(rb - h);
    p = rb + 2;
  } else {
    const char *colon = strchr(s, ':');
    if (!colon)
      return -1;
    hl = (size_t)(colon - s);
    p = colon + 1;
  }
  if (hl == 0 || hl >= 254)
    return -1;

  int v = 0;
  const char *q = p;
  for (; *q >= '0' && *q <= '9'; q++) {
    v = v * 10 + (*q - '0');
    if (v > 65535)
      return -1;
  }
  if (q == p || v == 0)
    return -1;
  if (*q == '/')
    q++;
  else if (*q)
    return -1;
  if (strlen(q) >= 64)
    return -1;

  *host = h;
  *host_len = hl;
  *port = v;
  *cookie = q;
  return 0;
}

/* Lock-free probe; returns SWIM_NODEID_NONE if absent. */
static swim_nodeid_idx_t nodeid_probe(const char *nodeid) {
  uint16_t slot = hash_slot(nodeid_hash(nodeid));
  for (uint16_t i = 0; i < SWIM_NODEID_TABLE_SIZE; i++) {
    char *p = atomic_load_explicit(&g_table[slot], memory_order_acquire);
    if (!p)
      return SWIM_NODEID_NONE;
    if (strcmp(p, nodeid) == 0)
      return (swim_nodeid_idx_t){slot};
    slot = next_slot(slot);
  }
  return SWIM_NODEID_NONE;
}

/* Register nodeid; idempotent. Returns SWIM_NODEID_NONE if malformed or pool
 * is full. Fast path: lock-free probe first; only locks when the entry is
 * absent. */
static swim_nodeid_idx_t swim_nodeid_register(const char *nodeid) {
  swim_nodeid_idx_t existing = nodeid_probe(nodeid);
  if (nodeid_valid(existing))
    return existing;

  /* Only well-formed ids may take one of the pool's limited slots. */
  const char *h, *c;
  size_t hl;
  int port;
  if (parse_nodeid(nodeid, &h, &hl, &port, &c) != 0)
    return SWIM_NODEID_NONE;

  pthread_mutex_lock(&g_mu); /* acquire registration lock */
  if (atomic_load_explicit(&g_count, memory_order_relaxed) >=
      SWIM_NODEID_POOL_MAX) {
    goto none;
  }
  uint16_t slot = hash_slot(nodeid_hash(nodeid));
  for (uint16_t i = 0; i < SWIM_NODEID_TABLE_SIZE; i++) {
    char *p = atomic_load_explicit(&g_table[slot], memory_order_relaxed);
    if (!p) {
      char *s = strdup(nodeid);
      if (!s)
        goto none;
      /* Publish with release so lock-free readers see the complete string. */
      atomic_store_explicit(&g_table[slot], s, memory_order_release);
      atomic_fetch_add_explicit(&g_count, 1, memory_order_relaxed);
      pthread_mutex_unlock(&g_mu); /* release registration lock */
      return (swim_nodeid_idx_t){slot};
    }
    if (strcmp(p, nodeid) == 0) {
      pthread_mutex_unlock(&g_mu); /* release registration lock */
      return (swim_nodeid_idx_t){slot};
    }
    slot = next_slot(slot);
  }

none:
  pthread_mutex_unlock(&g_mu); /* release registration lock */
  return SWIM_NODEID_NONE;
}

/* Find nodeid, registering it if absent. Returns SWIM_NODEID_NONE if malformed
 * or pool full.
 */
swim_nodeid_idx_t swim_nodeid_find(const char *nodeid) {
  return swim_nodeid_register(nodeid);
}

/* Find nodeid without registering it; lock-free. */
swim_nodeid_idx_t swim_nodeid_get(const char *nodeid) {
  return nodeid_probe(nodeid);
}

/* Return nodeid string for idx; lock-free. */
const char *swim_nodeid_lookup(swim_nodeid_idx_t idx) {
  if (idx.v >= SWIM_NODEID_TABLE_SIZE)
    return NULL;
  return atomic_load_explicit(&g_table[idx.v], memory_order_acquire);
}

/* Free all registered nodeids and destroy the pool mutex. Call at shutdown. */
void swim_nodeid_destroy(void) {
  pthread_mutex_lock(&g_mu); /* acquire registration lock */
  for (uint16_t i = 0; i < SWIM_NODEID_TABLE_SIZE; i++) {
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

/* Parse a nodeid index back into its host, port, and optional cookie. */
int swim_nodeid_split(swim_nodeid_idx_t idx, char host[254], int *port,
                      char cookie[64]) {
  const char *node_str = swim_nodeid_lookup(idx);
  if (!node_str)
    return -1;

  const char *h, *c;
  size_t host_len;
  int p;
  if (parse_nodeid(node_str, &h, &host_len, &p, &c) != 0)
    return -1;

  memcpy(host, h, host_len);
  host[host_len] = '\0';
  if (port)
    *port = p;
  if (cookie)
    strcpy(cookie, c); /* parse_nodeid bounds it below 64 bytes */
  return 0;
}
