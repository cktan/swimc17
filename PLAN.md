# Unit Test Gap Fill Plan

## Goals

Fill gaps identified in unit test coverage. One gap at a time,
full plan-act-validate cycle each.

## Gaps (priority order)

### 1. `swim_membership_peers` — ZERO coverage
**File:** `test/unit/swim_membership_test.cpp`
**Cases to add:**
- Empty membership → count=0, `*out=NULL`
- Alive-only members → correct packed strings, correct count
- Mixed alive+dead, `include_dead=false` → dead excluded
- Mixed alive+dead, `include_dead=true` → dead included
- NULL args → returns -1, sets `SWIM_ERR_INVALID`

**Verify:** `make test` passes

---

### 2. `swim_feed_wait` timeout path
**File:** `test/unit/swim_feed_test.cpp`
**Cases to add:**
- Finite `timeout_ms` on empty feed → returns `1` within
  reasonable wall time
- `swim_feed_wait(NULL, 100)` → returns `-1`,
  `swim_errno() == SWIM_ERR_INVALID`

**Verify:** `make test` passes

---

### 3. `swim_node_id_format` error paths
**File:** `test/unit/swim_node_id_test.cpp`
**Cases to add:**
- NULL `id` → returns non-zero
- NULL `buf` → returns non-zero
- `size==0` → returns non-zero
- Buffer too small → returns non-zero
- IPv6 host with cookie → `[host]:port/cookie` format
- IPv6 host without cookie → `[host]:port` format

**Verify:** `make test` passes

---

### 4. `swim_node_id_parse` edge cases
**File:** `test/unit/swim_node_id_test.cpp`
**Cases to add:**
- `NULL id` → returns non-zero
- `NULL str` → returns non-zero
- Port `0` → rejected
- Cookie too long (256+ chars) → rejected
- Host too long (256+ chars) → rejected

**Verify:** `make test` passes

---

### 5. `swim_gossip_queue` invalid-arg paths
**File:** `test/unit/swim_gossip_queue_test.cpp`
**Cases to add:**
- `swim_gossip_queue_enqueue(NULL, ...)` → -1
- `swim_gossip_queue_enqueue(q, ..., NULL, ...)` → -1
- `swim_gossip_queue_enqueue(q, ..., 0)` (multiplier < 1) → -1
- `swim_gossip_queue_pack(NULL, ...)` → -1
- `swim_gossip_queue_pack(q, ..., NULL, q)` → -1
- `swim_gossip_queue_peek(NULL, ...)` → -1
- `swim_gossip_queue_peek(q, NULL, ...)` → -1

**Verify:** `make test` passes

---

### 6. `swim_codec` — `SWIM_MSG_FWD_ACK`
**File:** `test/unit/swim_codec_test.cpp`
**Cases to add:**
- Pack `SWIM_MSG_FWD_ACK` with sender+peer → unpack roundtrip,
  verify type, seq, sender, peer

**Verify:** `make test` passes

---

### 7. `swim_udp` utility functions
**File:** `test/unit/swim_udp_test.cpp`
**Cases to add:**
- `swim_udp_fd` returns valid fd (>= 0) on live socket
- `swim_udp_set_packet_loss` + send → packet dropped at
  configured rate (probabilistic; use 100% loss)
- `swim_clear_udp_loss` removes loss setting
- `swim_udp_set_drop_filter` with custom filter fn

**Verify:** `make test` passes

---

## Current status

- [x] 1. swim_membership_peers
- [x] 2. swim_feed_wait timeout
- [x] 3. swim_node_id_format errors
- [x] 4. swim_node_id_parse edge cases
- [x] 5. swim_gossip_queue invalid args
- [x] 6. swim_codec FWD_ACK
- [x] 7. swim_udp utilities
