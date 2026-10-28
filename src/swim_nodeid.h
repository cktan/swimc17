/* Copyright (c) 2026, CK Tan.
 * https://github.com/cktan/swimc17/blob/main/LICENSE
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Unified nodeid pool backed by an open-addressing hash table.
 * swim_nodeid_idx_t.v is the table slot [0..SWIM_NODEID_TABLE_SIZE)
 * Arrays indexed by swim_nodeid_idx_t must be SWIM_NODEID_TABLE_SIZE elements
 * wide.
 */
#define SWIM_NODEID_POOL_MAX 4000 /* max registered nodeids */
#define SWIM_NODEID_TABLE_SIZE                                                 \
  8192 /* hash table size; swim_nodeid_idx_t.v < this */

typedef struct {
  uint16_t v;
} swim_nodeid_idx_t;

#define SWIM_NODEID_NONE                                                       \
  ((swim_nodeid_idx_t){0xFFFF}) /* out-of-range sentinel */

/* True if idx is a valid (non-sentinel) index. */
static inline int nodeid_valid(swim_nodeid_idx_t idx) {
  return idx.v != 0xFFFF;
}

/* True if two indices refer to the same entry. */
static inline int nodeid_eq(swim_nodeid_idx_t a, swim_nodeid_idx_t b) {
  return a.v == b.v;
}

/* Return the nodeid string for idx, or NULL if idx is invalid. */
const char *swim_nodeid_lookup(swim_nodeid_idx_t idx);

/* Find nodeid, registering it if absent. Returns SWIM_NODEID_NONE if nodeid
 * is not "host:port[/cookie]" (host may be a bracketed IPv6 literal, port is
 * 1..65535, cookie is under 64 bytes) or if the pool is full. Equivalent
 * spellings ("10.0.0.1:07000/", "[10.0.0.1]:7000") resolve to one identity,
 * stored in canonical form ("10.0.0.1:7000"); see canonicalize().
 */
swim_nodeid_idx_t swim_nodeid_find(const char *nodeid);

/* Find nodeid (any spelling) without registering it. Returns
 * SWIM_NODEID_NONE if absent or malformed. */
swim_nodeid_idx_t swim_nodeid_get(const char *nodeid);

/* Return 0 if nodeid is well-formed (see swim_nodeid_find), -1 if not. */
int swim_nodeid_check(const char *nodeid);

/* Free all registered nodeids and destroy the pool mutex. Call at shutdown. */
void swim_nodeid_destroy(void);

/* Parse a nodeid index back into its host, port, and optional cookie. */
int swim_nodeid_split(swim_nodeid_idx_t idx, char host[254], int *port,
                      char cookie[64]);

#ifdef __cplusplus
}
#endif
