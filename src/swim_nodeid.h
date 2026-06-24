#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Unified nodeid pool backed by an open-addressing hash table.
 * nodeid_idx_t.v is the table slot [0..NODEID_TABLE_SIZE)
 * Arrays indexed by nodeid_idx_t must be NODEID_TABLE_SIZE elements wide.
 */
#define NODEID_POOL_MAX 4000   /* max registered nodeids */
#define NODEID_TABLE_SIZE 8192 /* hash table size; nodeid_idx_t.v < this */

typedef struct {
  uint16_t v;
} nodeid_idx_t;

#define NODEID_NONE ((nodeid_idx_t){0xFFFF}) /* out-of-range sentinel */

/* True if idx is a valid (non-sentinel) index. */
static inline int nodeid_valid(nodeid_idx_t idx) { return idx.v != 0xFFFF; }

/* True if two indices refer to the same entry. */
static inline int nodeid_eq(nodeid_idx_t a, nodeid_idx_t b) {
  return a.v == b.v;
}

/* Register nodeid; idempotent. Returns NODEID_NONE if pool is full. */
nodeid_idx_t nodeid_register(const char *nodeid);

/* Return the nodeid string for idx, or NULL if idx is invalid. */
const char *nodeid_lookup(nodeid_idx_t idx);

/* Find existing nodeid without registering. Returns NODEID_NONE if absent. */
nodeid_idx_t nodeid_find(const char *nodeid);

/* Free all registered nodeids and destroy the pool mutex. Call at shutdown. */
void nodeid_destroy(void);

/* Parse a nodeid index back into its host name, port, and optional first_pos.
 */
int nodeid_split(nodeid_idx_t idx, char host[254], int *port, int *first_pos);

typedef struct {
  uint8_t bits[512]; /* 4096-bit bloom filter; k=4, n<128 → ~0.02% FPR */
} nodeid_bloom_t;

/* Clear the Bloom filter. */
void nodeid_bloom_init(nodeid_bloom_t *bf);

/* Add a nodeid string to the Bloom filter. */
void nodeid_bloom_add(nodeid_bloom_t *bf, const char *nodeid);

/* Test if a nodeid string is present in the Bloom filter. */
int nodeid_bloom_test(const nodeid_bloom_t *bf, const char *nodeid);

#ifdef __cplusplus
}
#endif
