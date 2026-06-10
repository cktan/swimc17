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

  const char *seed_list[] = {"127.0.0.1:5001/", nullptr};

  // Start node_1 as seed with telemetry feed attached
  {
    swim_start_opts_t opts = make_opts();
    opts.self = "127.0.0.1:5001/";
    opts.name = "node_1";
    opts.seeds = nullptr;
    opts.feed = feed;
    REQUIRE(swim_start(&opts) == 0);
  }

  // Start nodes 2-64, all seeded to node_1
  for (int i = 2; i <= 64; i++) {
    std::string name = "node_" + std::to_string(i);
    std::string self =
      "127.0.0.1:" + std::to_string(5000 + i) + "/";
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
      seed_strs.push_back("127.0.0.1:" + std::to_string(5000 + i) + "/");
    }
    for (auto &s : seed_strs)
      seed_ptrs.push_back(s.c_str());
    seed_ptrs.push_back(nullptr);

    std::string self = "127.0.0.1:5014/";
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
// 2. 64-node network: 4-way partition and heal
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
      "127.0.0.1:5001/",
      "127.0.0.1:5002/",
      "127.0.0.1:5003/",
      nullptr,
  };

  // node_1 starts with no seeds; nodes 2-64 seed to node_1/2/3
  {
    swim_start_opts_t opts = make_opts();
    opts.self = "127.0.0.1:5001/";
    opts.name = "node_1";
    opts.seeds = nullptr;
    REQUIRE(swim_start(&opts) == 0);
  }
  for (int i = 2; i <= 64; i++) {
    std::string name = "node_" + std::to_string(i);
    std::string self =
      "127.0.0.1:" + std::to_string(5000 + i) + "/";
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
// 3. 64-node network: asymmetric partition (1 vs 63)
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

  const char *seed_list[] = {"127.0.0.1:5001/", nullptr};

  {
    swim_start_opts_t opts = make_opts();
    opts.self = "127.0.0.1:5001/";
    opts.name = "node_1";
    opts.seeds = nullptr;
    REQUIRE(swim_start(&opts) == 0);
  }
  for (int i = 2; i <= 64; i++) {
    std::string name = "node_" + std::to_string(i);
    std::string self =
      "127.0.0.1:" + std::to_string(5000 + i) + "/";
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
// 4. 64-node network: 30% packet loss stress
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

  const char *seed_list[] = {"127.0.0.1:5001/", nullptr};

  {
    swim_start_opts_t opts = make_opts_lossy();
    opts.self = "127.0.0.1:5001/";
    opts.name = "node_1";
    opts.seeds = nullptr;
    REQUIRE(swim_start(&opts) == 0);
  }
  for (int i = 2; i <= 64; i++) {
    std::string name = "node_" + std::to_string(i);
    std::string self =
      "127.0.0.1:" + std::to_string(5000 + i) + "/";
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
// 5. 64-node network: churn stress (restarting nodes)
//
// Steps:
// 1. Start all 64 nodes seeded to node_1.
// 2. Verify full convergence (each node sees 63 peers).
// 3. Kill nodes 50-60 simultaneously (graceful leave).
// 4. Wait for the 53 survivors to stabilise at 52 peers.
// 5. Restart nodes 50-60 seeded to a surviving node.
// 6. Verify all 64 nodes see 63 peers.
// ---------------------------------------------------------------------------
TEST_CASE("scale: churn stress (restarting nodes)") {
  reset_cluster();

  const char *seed_list[] = {"127.0.0.1:5001/", nullptr};

  {
    swim_start_opts_t opts = make_opts();
    opts.self = "127.0.0.1:5001/";
    opts.name = "node_1";
    opts.seeds = nullptr;
    REQUIRE(swim_start(&opts) == 0);
  }
  for (int i = 2; i <= 64; i++) {
    std::string name = "node_" + std::to_string(i);
    std::string self =
      "127.0.0.1:" + std::to_string(5000 + i) + "/";
    swim_start_opts_t opts = make_opts();
    opts.self = self.c_str();
    opts.name = name.c_str();
    opts.seeds = seed_list;
    REQUIRE(swim_start(&opts) == 0);
  }

  CHECK(wait_for_all_peers(1, 64, nullptr, 63, 30000));

  // Kill nodes 50-60 simultaneously
  for (int i = 50; i <= 60; i++)
    swim_leave(("node_" + std::to_string(i)).c_str());

  // Wait for the surviving 53 nodes to detect the 11 deaths
  CHECK(wait_for_all_peers(1, 64, [](int i) { return i >= 50 && i <= 60; },
                           52, 30000));

  // Restart nodes 50-60, seeded to a surviving node
  const char *restart_seeds[] = {"127.0.0.1:5061/", nullptr};
  for (int i = 50; i <= 60; i++) {
    std::string name = "node_" + std::to_string(i);
    std::string self =
      "127.0.0.1:" + std::to_string(5000 + i) + "/";
    swim_start_opts_t opts = make_opts();
    opts.self = self.c_str();
    opts.name = name.c_str();
    opts.seeds = restart_seeds;
    REQUIRE(swim_start(&opts) == 0);
  }

  CHECK(wait_for_all_peers(1, 64, nullptr, 63, 30000));
}

// ---------------------------------------------------------------------------
// 6. 64-node network: half-cluster immediate restart
//
// Restarting without waiting for death detection exercises the
// incarnation-number override path: survivors accept the fresh
// ALIVE messages over their stale DEAD entries.
//
// Steps:
// 1. Start all 64 nodes seeded to node_1.
// 2. Verify full convergence (each node sees 63 peers).
// 3. Kill nodes 1-32 simultaneously (including the seed).
// 4. Immediately restart nodes 1-32 seeded to surviving
//    nodes 33-64.
// 5. Verify all 64 nodes see 63 peers.
// ---------------------------------------------------------------------------
TEST_CASE("scale: half-cluster immediate restart") {
  reset_cluster();

  const char *seed_list[] = {"127.0.0.1:5001/", nullptr};

  {
    swim_start_opts_t opts = make_opts();
    opts.self = "127.0.0.1:5001/";
    opts.name = "node_1";
    opts.seeds = nullptr;
    REQUIRE(swim_start(&opts) == 0);
  }
  for (int i = 2; i <= 64; i++) {
    std::string name = "node_" + std::to_string(i);
    std::string self =
      "127.0.0.1:" + std::to_string(5000 + i) + "/";
    swim_start_opts_t opts = make_opts();
    opts.self = self.c_str();
    opts.name = name.c_str();
    opts.seeds = seed_list;
    REQUIRE(swim_start(&opts) == 0);
  }

  CHECK(wait_for_all_peers(1, 64, nullptr, 63, 30000));

  // Kill nodes 1-32 simultaneously (including the seed)
  for (int i = 1; i <= 32; i++)
    swim_leave(("node_" + std::to_string(i)).c_str());

  // Immediately restart nodes 1-32 seeded to surviving nodes 33-64.
  // Restarting increments the incarnation number, so surviving nodes
  // will accept the new ALIVE messages over the old dead entries.
  const char *restart_seeds[] = {"127.0.0.1:5033/", nullptr};
  for (int i = 1; i <= 32; i++) {
    std::string name = "node_" + std::to_string(i);
    std::string self =
        "127.0.0.1:" + std::to_string(5000 + i) + "/" + std::to_string(i);
    swim_start_opts_t opts = make_opts();
    opts.self = self.c_str();
    opts.name = name.c_str();
    opts.seeds = restart_seeds;
    REQUIRE(swim_start(&opts) == 0);
  }

  // Allow time for old incarnation entries to be GC'd and the full
  // cluster to reconverge to 64 nodes.
  CHECK(wait_for_all_peers(1, 64, nullptr, 63, 60000));
}

// ---------------------------------------------------------------------------
// 7. 64-node network: half-cluster staged revival
//
// Steps:
// 1. Start all 64 nodes seeded to node_1.
// 2. Verify full convergence (each node sees 63 peers).
// 3. Kill nodes 1-32.
// 4. Wait for nodes 33-64 to mark nodes 1-32 dead
//    (each survivor stabilises at 31 peers).
// 5. Restart nodes 1-32 seeded to surviving nodes 33-64.
// 6. Verify all 64 nodes see 63 peers.
// ---------------------------------------------------------------------------
TEST_CASE("scale: half-cluster staged revival") {
  // TODO
  REQUIRE(true);
}

// ---------------------------------------------------------------------------
// 8. 64-node network: rolling upgrade simulation
//
// Steps:
// 1. Start all 64 nodes seeded to node_1.
// 2. Verify full convergence (each node sees 63 peers).
// 3. For each batch of 8 nodes: leave and restart with a
//    fresh cookie (simulating a new version); wait for the
//    batch to rejoin before moving to the next.
// 4. Verify all 64 nodes see 63 peers after all 8 batches.
// ---------------------------------------------------------------------------
TEST_CASE("scale: rolling upgrade simulation") {
  // TODO
  REQUIRE(true);
}

// ---------------------------------------------------------------------------
// 9. 64-node network: high latency jitter and delay stress
//
// Steps:
// 1. Start all 64 nodes seeded to node_1 with lossy opts.
// 2. Verify initial convergence (each node sees 63 peers).
// 3. Apply random per-node packet-loss bursts (0-20%) to
//    simulate jitter without triggering false deaths.
// 4. Verify the cluster remains at 63 peers throughout.
// ---------------------------------------------------------------------------
TEST_CASE("scale: high latency jitter and delay stress") {
  // TODO
  REQUIRE(true);
}

// ---------------------------------------------------------------------------
// 10. 64-node network: bootstrap storm simulation
//
// Steps:
// 1. Start node_1 as the sole seed.
// 2. Start nodes 2-64 all at once, each seeded only to node_1.
// 3. Verify all 64 nodes see 63 peers within a generous timeout.
// ---------------------------------------------------------------------------
TEST_CASE("scale: bootstrap storm simulation") {
  // TODO
  REQUIRE(true);
}
