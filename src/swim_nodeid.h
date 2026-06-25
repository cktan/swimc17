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

/* Find nodeid, registering it if absent. Returns SWIM_NODEID_NONE if pool full.
 */
swim_nodeid_idx_t swim_nodeid_find(const char *nodeid);

/* Free all registered nodeids and destroy the pool mutex. Call at shutdown. */
void swim_nodeid_destroy(void);

/* Parse a nodeid index back into its host, port, and optional cookie. */
int swim_nodeid_split(swim_nodeid_idx_t idx, char host[254], int *port,
                      char cookie[64]);

typedef struct {
  uint8_t bits[512]; /* 4096-bit bloom filter; k=4, n<128 → ~0.02% FPR */
} swim_nodeid_bloom_t;

/* Clear the Bloom filter. */
void swim_nodeid_bloom_init(swim_nodeid_bloom_t *bf);

/* Add a nodeid string to the Bloom filter. */
void swim_nodeid_bloom_add(swim_nodeid_bloom_t *bf, const char *nodeid);

/* Test if a nodeid string is present in the Bloom filter. */
int swim_nodeid_bloom_test(const swim_nodeid_bloom_t *bf, const char *nodeid);

#ifdef __cplusplus
}
#endif
