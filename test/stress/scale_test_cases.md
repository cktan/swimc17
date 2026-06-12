# Scale Test Cases (`scale.cpp`)

All tests run a 64-node cluster on loopback ports 5001–5064.
Timing parameters are derived from `swim_opts_for(64, 4000)`
(T ≈ 540 ms, detection target ~4 s). Tests that apply packet
loss use `make_opts_lossy()`, which multiplies the suspicion
timeout by 8 to suppress false deaths.

---

## 0. `scale: cluster setup`

**Purpose:** Smoke-test that all 64 nodes start and converge.

**Steps:**
1. Start all 64 nodes seeded to nodes 1–4.
2. Verify every node sees 63 peers within 30 s.

---

## 1. `scale: staged startup, failure detection, pause/unpause`

**Purpose:** Verify failure detection via graceful leave and
packet-loss simulation, and confirm a paused node can rejoin.

**Steps:**
1. Start all 64 nodes; attach a telemetry feed to node 1.
2. Verify full convergence (all nodes see 63 peers).
3. Kill node 7 (`swim_leave`); confirm the feed reports it
   down.
4. Apply 100% outbound packet loss to node 14; confirm the
   feed reports it down.
5. Clear the loss; leave and restart node 14 seeded to all
   62 survivors; confirm the feed reports it back up.
6. Wait for all 63 surviving nodes to stabilise at 62 peers.
7. Leave node 14 again; confirm the feed reports it down.

---

## 2. `scale: 4-way partition and heal`

**Purpose:** Verify that a four-way partition produces four
isolated sub-clusters and that healing restores full
convergence.

**Steps:**
1. Start all 64 nodes; verify full convergence.
2. Partition into four groups of 16 (A: 1–16, B: 17–32,
   C: 33–48, D: 49–64) by dropping all cross-group traffic.
3. Verify each group stabilises at 15 peers.
4. Clear all filters; verify all 64 nodes see 63 peers.

---

## 3. `scale: asymmetric partition (1 vs 63)`

**Purpose:** Verify that an isolated node is declared dead
by the majority and can rejoin after the partition heals.

**Steps:**
1. Start all 64 nodes; verify full convergence.
2. Drop all traffic to/from node 10.
3. Verify the 63-node majority marks node 10 dead
   (each non-isolated node sees 62 peers).
4. Clear the filter; verify all 64 nodes see 63 peers.

---

## 4. `scale: 30% packet loss stress`

**Purpose:** Confirm no false deaths occur under sustained
30% packet loss.

**Steps:**
1. Start all 64 nodes with lossy opts; verify convergence.
2. Apply 30% outbound loss to every node.
3. Verify the cluster remains at 63 peers (no false deaths).
4. Clear loss; verify the cluster is still converged.

---

## 5. `scale: churn stress (restarting nodes)`

**Purpose:** Verify that simultaneous node deaths and
restarts are handled correctly.

**Steps:**
1. Start all 64 nodes; verify full convergence.
2. Kill nodes 50–60 simultaneously.
3. Wait for the 53 survivors to stabilise at 52 peers.
4. Restart nodes 50–60 seeded to node 61.
5. Verify all 64 nodes see 63 peers.

---

## 6. `scale: half-cluster immediate restart`

**Purpose:** Verify that restarting nodes with a higher
incarnation number causes survivors to accept fresh ALIVE
messages over stale DEAD entries.

**Steps:**
1. Start all 64 nodes; verify full convergence.
2. Kill nodes 1–32 simultaneously.
3. Immediately restart nodes 1–32 (new cookie = node index)
   seeded to node 33.
4. Verify all 64 nodes see 63 peers.

---

## 7. `scale: half-cluster staged revival`

**Purpose:** Verify that nodes restarting after survivors
have already declared them dead can still rejoin.

**Steps:**
1. Start all 64 nodes; verify full convergence.
2. Kill nodes 1–32.
3. Wait for nodes 33–64 to stabilise at 31 peers.
4. Restart nodes 1–32 seeded to node 33.
5. Verify all 64 nodes see 63 peers.

---

## 8. `scale: rolling upgrade simulation`

**Purpose:** Simulate an 8-batch rolling upgrade where each
batch converges before the next begins.

**Steps:**
1. Start all 64 nodes; verify full convergence.
2. For each batch of 8 nodes (batches 1–8):
   - Kill the batch.
   - Restart the batch seeded to node 33 (batches 1–4)
     or node 1 (batches 5–8).
   - Verify all 64 nodes see 63 peers before proceeding.

---

## 9. `scale: high latency jitter and delay stress`

**Purpose:** Confirm no false deaths occur under random
per-node packet-loss bursts (0–20%).

**Steps:**
1. Start all 64 nodes with lossy opts; verify convergence.
2. Apply random 0–20% packet loss per node (seed 42).
3. Verify the cluster remains at 63 peers.
4. Clear loss; verify the cluster is still converged.

---

## 10. `scale: bootstrap storm simulation`

**Purpose:** Verify that all 64 nodes converge when started
simultaneously against a 4-node seed list.

**Steps:**
1. Start all 64 nodes at once seeded to nodes 1–4.
2. Verify all 64 nodes see 63 peers within 60 s.
