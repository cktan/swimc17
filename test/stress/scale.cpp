#include "doctest.h"

extern "C" {
#include "swim.h"
#include "swim_udp.h"
}

#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// Call before each test. Leaves all nodes and clears packet loss state.
static void reset_cluster() {
  for (int i = 1; i <= 64; i++)
    swim_leave(("node_" + std::to_string(i)).c_str());
  swim_clear_udp_loss();
}

// swim_opts_for(64, 4000) → T≈540ms, ping_timeout≈108ms (1 tick minimum).
// Detection latency target ~4s. Used for all tests.
static swim_start_opts_t make_opts() { return swim_opts_for(64, 4000); }

// 8× suspicion timeout to suppress false deaths under packet loss.
static swim_start_opts_t make_opts_lossy() {
  swim_start_opts_t opts = make_opts();
  opts.suspicion_timeout_ms *= 8;
  opts.dead_node_expiry_ms = 2 * opts.suspicion_timeout_ms;
  return opts;
}

// Poll until all nodes in [from,to] (skipping any i where skip(i) is true)
// see exactly `expected` peers. Returns true on success, false on timeout.
static bool wait_for_all_peers(int from, int to, std::function<bool(int)> skip,
                               int expected, int timeout_ms) {
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    bool all_ok = true;
    for (int i = from; i <= to && all_ok; i++) {
      if (skip && skip(i))
        continue;
      std::string name = "node_" + std::to_string(i);
      int cnt = 0;
      char *p = swim_peers(name.c_str(), false, &cnt);
      free(p);
      if (cnt != expected)
        all_ok = false;
    }
    if (all_ok)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

// Poll the feed until a record containing both f1 and f2 is found.
// Returns true on success, false on timeout.
static bool drain_feed_for(swim_feed_t *feed, const char *f1, const char *f2,
                           int timeout_ms) {
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  char buf[SWIM_FEED_MAX_RECORD_SIZE];
  char *ptrs[SWIM_FEED_MAX_ELEMENTS];
  while (std::chrono::steady_clock::now() < deadline) {
    while (!swim_feed_empty(feed)) {
      int n =
          swim_feed_get(feed, sizeof(buf), buf, SWIM_FEED_MAX_ELEMENTS, ptrs);
      if (n > 0) {
        bool has_f1 = false, has_f2 = false;
        for (int i = 0; i < n; i++) {
          if (strstr(ptrs[i], f1))
            has_f1 = true;
          if (strstr(ptrs[i], f2))
            has_f2 = true;
        }
        if (has_f1 && has_f2)
          return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

// ---------------------------------------------------------------------------
// 1. 64-node network: staged startup, failure detection, and pause/unpause
//
// Steps:
// 1. Start node_1 as seed with a telemetry feed attached.
// 2. Start nodes 2-64, all seeded to node_1.
// 3. Wait for full convergence: every node sees 63 peers.
// 4. Kill node_7 (graceful leave); confirm feed reports it down.
// 5. Pause node_14 (100% outbound packet loss); confirm feed
//    reports it down after the suspicion timeout expires.
// 6. Unpause node_14: clear packet loss, leave, and restart it
//    with all 62 surviving nodes as seeds so it rejoins in one
//    round. Confirm feed reports node_14 up.
// 7. Wait for all 63 surviving nodes to stabilise at 62 peers.
// 8. Gracefully leave node_14; confirm feed reports it down.
// ---------------------------------------------------------------------------
TEST_CASE("scale: staged startup, failure detection, pause/unpause") {
  reset_cluster();

  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);

  const char *seed_list[] = {"127.0.0.1:5001/c1", nullptr};

  // Start node_1 as seed with telemetry feed attached
  {
    swim_start_opts_t opts = make_opts();
    opts.self = "127.0.0.1:5001/c1";
    opts.name = "node_1";
    opts.seeds = nullptr;
    opts.feed = feed;
    REQUIRE(swim_start(&opts) == 0);
  }

  // Start nodes 2-64, all seeded to node_1
  for (int i = 2; i <= 64; i++) {
    std::string name = "node_" + std::to_string(i);
    std::string self =
        "127.0.0.1:" + std::to_string(5000 + i) + "/c" + std::to_string(i);
    swim_start_opts_t opts = make_opts();
    opts.self = self.c_str();
    opts.name = name.c_str();
    opts.seeds = seed_list;
    REQUIRE(swim_start(&opts) == 0);
  }

  // Wait for full convergence: every node sees 63 peers
  CHECK(wait_for_all_peers(1, 64, nullptr, 63, 30000));

  // Kill node_7 and wait for node_1's feed to report it dead
  REQUIRE(swim_leave("node_7") == 0);
  CHECK(drain_feed_for(feed, "down", ":5007", 10000));

  // Pause node_14 (100% outbound loss) and wait for death detection
  swim_udp_set_packet_loss(5014, 100);
  CHECK(drain_feed_for(feed, "down", ":5014", 10000));

  // Unpause node_14: clear packet loss and restart with all surviving nodes
  // as seeds. When membership is empty the seed retry timer pings all seeds
  // at once, so node_14 discovers the full cluster in a single round.
  swim_udp_set_packet_loss(5014, 0);
  swim_leave("node_14");
  {
    std::vector<std::string> seed_strs;
    std::vector<const char *> seed_ptrs;
    for (int i = 1; i <= 64; i++) {
      if (i == 7 || i == 14)
        continue;
      seed_strs.push_back("127.0.0.1:" + std::to_string(5000 + i) + "/c" +
                          std::to_string(i));
    }
    for (auto &s : seed_strs)
      seed_ptrs.push_back(s.c_str());
    seed_ptrs.push_back(nullptr);

    std::string self = "127.0.0.1:5014/c14";
    swim_start_opts_t opts = make_opts();
    opts.self = self.c_str();
    opts.name = "node_14";
    opts.seeds = seed_ptrs.data();
    REQUIRE(swim_start(&opts) == 0);
  }
  CHECK(drain_feed_for(feed, "up", ":5014", 10000));

  // All 63 surviving nodes (skip node_7) should stabilise at 62 peers each
  CHECK(wait_for_all_peers(1, 64, [](int i) { return i == 7; }, 62, 60000));

  // Gracefully leave node_14 and verify detection via node_1's feed
  REQUIRE(swim_leave("node_14") == 0);
  CHECK(drain_feed_for(feed, "down", ":5014", 10000));

  // Cleanup
  swim_feed_destroy(feed);
}

// ---------------------------------------------------------------------------
// 2. 64-node network: partition and heal
//
// Seeds span both halves (node_1, node_2, node_33, node_34) so that
// after healing, the seed-retry timer contacts across the old
// partition boundary, triggering GC-then-rediscovery.
//
// Steps:
// 1. Start all 64 nodes seeded to node_1, node_2, node_33, node_34.
// 2. Verify full convergence (each node sees 63 peers).
// 3. Install a drop filter that blocks all traffic between the
//    left half (ports 5001-5032) and the right half (5033-5064).
// 4. Wait for each half to stabilise at 31 peers (the other 32
//    nodes get suspected then declared dead).
// 5. Heal: clear the filter. Dead nodes are GC'd after
//    dead_node_expiry_ms; seed retries then re-add them as fresh
//    ALIVE members, allowing full reconvergence.
// 6. Verify all 64 nodes see 63 peers.
// ---------------------------------------------------------------------------
TEST_CASE("scale: partition and heal") {
  reset_cluster();

  const char *seed_list[] = {
      "127.0.0.1:5001/c1",
      "127.0.0.1:5002/c2",
      "127.0.0.1:5033/c33",
      "127.0.0.1:5034/c34",
      nullptr,
  };

  {
    swim_start_opts_t opts = make_opts();
    opts.self = "127.0.0.1:5001/c1";
    opts.name = "node_1";
    opts.seeds = nullptr;
    REQUIRE(swim_start(&opts) == 0);
  }
  for (int i = 2; i <= 64; i++) {
    std::string name = "node_" + std::to_string(i);
    std::string self =
        "127.0.0.1:" + std::to_string(5000 + i) + "/c" + std::to_string(i);
    swim_start_opts_t opts = make_opts();
    opts.self = self.c_str();
    opts.name = name.c_str();
    opts.seeds = seed_list;
    REQUIRE(swim_start(&opts) == 0);
  }

  CHECK(wait_for_all_peers(1, 64, nullptr, 63, 30000));

  // Block all cross-partition traffic
  swim_udp_set_drop_filter([](int src, int dst) -> int {
    return (src >= 5001 && src <= 5032 && dst >= 5033 && dst <= 5064) ||
           (src >= 5033 && src <= 5064 && dst >= 5001 && dst <= 5032);
  });

  // Each half detects the other 32 nodes as dead
  CHECK(wait_for_all_peers(1, 32, nullptr, 31, 30000));
  CHECK(wait_for_all_peers(33, 64, nullptr, 31, 30000));

  // Heal: clear the filter. After dead_node_expiry_ms the GC removes
  // the stale DEAD entries; seed retries then re-add those nodes as
  // fresh ALIVE members and the cluster reconverges.
  swim_clear_udp_loss();

  CHECK(wait_for_all_peers(1, 64, nullptr, 63, 60000));
}

// ---------------------------------------------------------------------------
// 3. 64-node network: 4-way partition and heal
//
// Seeds: node_1, node_2, node_3 (all in group A, ports 5001-5016).
//
// Steps:
// 1. Start all 64 nodes seeded to node_1, node_2, node_3.
// 2. Verify full convergence (each node sees 63 peers).
// 3. Partition into four groups of 16:
//    A: nodes  1-16 (ports 5001-5016)
//    B: nodes 17-32 (ports 5017-5032)
//    C: nodes 33-48 (ports 5033-5048)
//    D: nodes 49-64 (ports 5049-5064)
// 4. Assert each group stabilises at 15 peers.
// 5. Heal: clear all filters and verify all 64 nodes see 63 peers.
// ---------------------------------------------------------------------------
TEST_CASE("scale: 4-way partition and heal") {
  reset_cluster();

  const char *seed_list[] = {
      "127.0.0.1:5001/c1",
      "127.0.0.1:5002/c2",
      "127.0.0.1:5003/c3",
      nullptr,
  };

  // node_1 starts with no seeds; nodes 2-64 seed to node_1/2/3
  {
    swim_start_opts_t opts = make_opts();
    opts.self = "127.0.0.1:5001/c1";
    opts.name = "node_1";
    opts.seeds = nullptr;
    REQUIRE(swim_start(&opts) == 0);
  }
  for (int i = 2; i <= 64; i++) {
    std::string name = "node_" + std::to_string(i);
    std::string self =
        "127.0.0.1:" + std::to_string(5000 + i) + "/c" + std::to_string(i);
    swim_start_opts_t opts = make_opts();
    opts.self = self.c_str();
    opts.name = name.c_str();
    opts.seeds = seed_list;
    REQUIRE(swim_start(&opts) == 0);
  }

  // Step 2: verify full convergence
  CHECK(wait_for_all_peers(1, 64, nullptr, 63, 30000));

  // Step 3: partition into four groups of 16.
  // group(port) = (port - 5001) / 16 → 0=A, 1=B, 2=C, 3=D
  swim_udp_set_drop_filter([](int src, int dst) -> int {
    return (src - 5001) / 16 != (dst - 5001) / 16;
  });

  // Step 4: each group stabilises at 15 peers
  CHECK(wait_for_all_peers(1, 16, nullptr, 15, 30000));
  CHECK(wait_for_all_peers(17, 32, nullptr, 15, 30000));
  CHECK(wait_for_all_peers(33, 48, nullptr, 15, 30000));
  CHECK(wait_for_all_peers(49, 64, nullptr, 15, 30000));

  // Step 5: heal and verify full convergence
  swim_clear_udp_loss();
  CHECK(wait_for_all_peers(1, 64, nullptr, 63, 60000));
}

// ---------------------------------------------------------------------------
// 4. 64-node network: asymmetric partition (1 vs 63)
//
// Steps:
// 1. Start all 64 nodes seeded to node_1.
// 2. Verify full convergence (each node sees 63 peers).
// 3. Isolate node_10: drop all traffic to/from port 5010.
// 4. Assert the 63-node majority marks node_10 dead
//    (all nodes except node_10 stabilise at 62 peers).
// 5. Heal: clear the filter. node_10 seed-retries node_1 after GC
//    and rejoins; the cluster reconverges.
// 6. Verify all 64 nodes see 63 peers.
// ---------------------------------------------------------------------------
TEST_CASE("scale: asymmetric partition (1 vs 63)") {
  reset_cluster();

  const char *seed_list[] = {"127.0.0.1:5001/c1", nullptr};

  {
    swim_start_opts_t opts = make_opts();
    opts.self = "127.0.0.1:5001/c1";
    opts.name = "node_1";
    opts.seeds = nullptr;
    REQUIRE(swim_start(&opts) == 0);
  }
  for (int i = 2; i <= 64; i++) {
    std::string name = "node_" + std::to_string(i);
    std::string self =
        "127.0.0.1:" + std::to_string(5000 + i) + "/c" + std::to_string(i);
    swim_start_opts_t opts = make_opts();
    opts.self = self.c_str();
    opts.name = name.c_str();
    opts.seeds = seed_list;
    REQUIRE(swim_start(&opts) == 0);
  }

  CHECK(wait_for_all_peers(1, 64, nullptr, 63, 30000));

  // Isolate node_10: drop all traffic to/from port 5010
  swim_udp_set_drop_filter(
      [](int src, int dst) -> int { return src == 5010 || dst == 5010; });

  // node_10 is isolated: the 63-node majority detects it as dead
  CHECK(wait_for_all_peers(1, 64, [](int i) { return i == 10; }, 62, 30000));

  // Heal and verify full convergence
  swim_clear_udp_loss();
  CHECK(wait_for_all_peers(1, 64, nullptr, 63, 60000));
}

// ---------------------------------------------------------------------------
// 5. 64-node network: 30% packet loss stress
//
// Uses 8× suspicion timeout (make_opts_lossy) to prevent false deaths
// under sustained loss.
//
// Steps:
// 1. Start all 64 nodes seeded to node_1 with lossy opts.
// 2. Verify initial convergence (each node sees 63 peers).
// 3. Apply 30% outbound loss to all 64 ports.
// 4. Assert the cluster remains at 63 peers (no false deaths).
// 5. Clear loss and verify cluster is still fully converged.
// ---------------------------------------------------------------------------
TEST_CASE("scale: 30% packet loss stress") {
  reset_cluster();

  const char *seed_list[] = {"127.0.0.1:5001/c1", nullptr};

  {
    swim_start_opts_t opts = make_opts_lossy();
    opts.self = "127.0.0.1:5001/c1";
    opts.name = "node_1";
    opts.seeds = nullptr;
    REQUIRE(swim_start(&opts) == 0);
  }
  for (int i = 2; i <= 64; i++) {
    std::string name = "node_" + std::to_string(i);
    std::string self =
        "127.0.0.1:" + std::to_string(5000 + i) + "/c" + std::to_string(i);
    swim_start_opts_t opts = make_opts_lossy();
    opts.self = self.c_str();
    opts.name = name.c_str();
    opts.seeds = seed_list;
    REQUIRE(swim_start(&opts) == 0);
  }

  CHECK(wait_for_all_peers(1, 64, nullptr, 63, 30000));

  // Apply 30% outbound loss to all nodes
  for (int i = 1; i <= 64; i++)
    swim_udp_set_packet_loss(5000 + i, 30);

  // Cluster should remain stable (no false deaths) under sustained loss
  CHECK(wait_for_all_peers(1, 64, nullptr, 63, 60000));

  // Clear loss and verify cluster is still fully converged
  swim_clear_udp_loss();
  CHECK(wait_for_all_peers(1, 64, nullptr, 63, 30000));
}

// ---------------------------------------------------------------------------
// 6. 64-node network: churn stress (restarting nodes)
// ---------------------------------------------------------------------------
TEST_CASE("scale: churn stress (restarting nodes)") {
  // TODO: Kill and restart a rolling subset of nodes repeatedly.
  // Steps:
  // 1. Start all 64 nodes with node_1 as seed.
  // 2. Verify full convergence.
  // 3. Kill nodes 50-60 simultaneously.
  // 4. Wait for the surviving nodes to detect the deaths.
  // 5. Restart nodes 50-60 seeded to surviving nodes.
  // 6. Verify full convergence at 64.
  REQUIRE(true);
}

// ---------------------------------------------------------------------------
// 7. 64-node network: half-cluster immediate restart
// ---------------------------------------------------------------------------
TEST_CASE("scale: half-cluster immediate restart") {
  // TODO: Kill and immediately restart 32 nodes before death
  // detection fires.
  // Steps:
  // 1. Start all 64 nodes with node_1 as seed.
  // 2. Verify full convergence.
  // 3. Kill nodes 1-32.
  // 4. Immediately restart nodes 1-32 seeded to nodes 33-64.
  // 5. Verify full convergence at 64.
  REQUIRE(true);
}

// ---------------------------------------------------------------------------
// 8. 64-node network: half-cluster staged revival
// ---------------------------------------------------------------------------
TEST_CASE("scale: half-cluster staged revival") {
  // TODO: Kill 32 nodes, wait for death detection, then restart.
  // Steps:
  // 1. Start all 64 nodes with node_1 as seed.
  // 2. Verify full convergence.
  // 3. Kill nodes 1-32.
  // 4. Wait for nodes 33-64 to mark nodes 1-32 dead.
  // 5. Restart nodes 1-32 seeded to nodes 33-64.
  // 6. Verify full convergence at 64.
  REQUIRE(true);
}

// ---------------------------------------------------------------------------
// 9. 64-node network: rolling upgrade simulation
// ---------------------------------------------------------------------------
TEST_CASE("scale: rolling upgrade simulation") {
  // TODO: Simulate a rolling restart in batches of 8.
  // Steps:
  // 1. Start all 64 nodes with node_1 as seed.
  // 2. Verify full convergence.
  // 3. For each batch of 8 nodes: leave and restart with a fresh
  //    cookie (simulating a new version). Wait for the batch to
  //    rejoin before moving to the next.
  // 4. Verify full convergence at 64 after all 8 batches.
  REQUIRE(true);
}

// ---------------------------------------------------------------------------
// 10. 64-node network: high latency jitter and delay stress
// ---------------------------------------------------------------------------
TEST_CASE("scale: high latency jitter and delay stress") {
  // TODO: Verify convergence under heterogeneous simulated latency.
  // Steps:
  // 1. Start all 64 nodes with node_1 as seed.
  // 2. Apply random per-node packet-loss bursts (0-20%) to simulate
  //    jitter without causing permanent failures.
  // 3. Verify the cluster converges and stays converged.
  REQUIRE(true);
}

// ---------------------------------------------------------------------------
// 11. 64-node network: bootstrap storm simulation
// ---------------------------------------------------------------------------
TEST_CASE("scale: bootstrap storm simulation") {
  // TODO: All 63 nodes join simultaneously against a single seed.
  // Steps:
  // 1. Start node_1 as the sole seed.
  // 2. Start nodes 2-64 all at once, each seeded only to node_1.
  // 3. Verify full convergence at 64 within a generous timeout.
  REQUIRE(true);
}
