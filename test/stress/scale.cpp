#include "doctest.h"

extern "C" {
#include "swim.h"
#include "swim_codec.h"
#include "swim_errno.h"
#include "swim_gossip_queue.h"
#include "swim_internal.h"
#include "swim_udp.h"
}

#include <cstring>
#include <unistd.h>
#include <vector>
#include <thread>
#include <chrono>

// Global observation storage for test scaffolding
static std::vector<std::string> g_observations;

// Helper utilities (shared across tests)
static void clear_obs() {
  g_observations.clear();
}
static bool obs_contains(const char *needle) {
  for (const auto &obs : g_observations) {
    if (obs.find(needle) != std::string::npos) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// 1. 64-node network: staged startup, failure detection, and pause/unpause
// ---------------------------------------------------------------------------
TEST_CASE("scale: staged startup, failure detection, pause/unpause") {
  clear_obs();

  // 1. Seed node (node_1)
  const char *seed_name = "node_1";
  swim_start_opts_t seed_opts = swim_opts_for(64, 15000);
  seed_opts.self = "127.0.0.1:5001/seed_cookie";
  seed_opts.name = seed_name;
  seed_opts.seeds = nullptr; // no seeds for seed node
  REQUIRE(swim_start(&seed_opts) == 0);

  // 2. Spawn odd‑indexed nodes (1,3,…,63) without explicit seeds
  std::vector<std::string> odd_names;
  for (int i = 1; i <= 63; i += 2) {
    std::string name = "node_" + std::to_string(i);
    std::string self = "127.0.0.1:" + std::to_string(5000 + i) + "/c" + std::to_string(i);
    swim_start_opts_t opts = swim_opts_for(64, 15000);
    opts.self = self.c_str();
    opts.name = name.c_str();
    opts.seeds = nullptr; // start without explicit seed
    REQUIRE(swim_start(&opts) == 0);
    odd_names.push_back(name);
  }

  // 3. Verify convergence of odd nodes (should see 31 peers + seed => 32 total)
  for (const auto &nm : odd_names) {
    int cnt = 0;
    char *peers = swim_peers(nm.c_str(), false, &cnt);
    REQUIRE(peers != nullptr);
    // odd nodes + seed = 32 total nodes, each sees 31 others
    REQUIRE(cnt == 31);
    free(peers);
  }

  // 4. Spawn even‑indexed nodes (2,4,…,64) with the seed
  std::vector<std::string> even_names;
  for (int i = 2; i <= 64; i += 2) {
    std::string name = "node_" + std::to_string(i);
    std::string self = "127.0.0.1:" + std::to_string(5000 + i) + "/c" + std::to_string(i);
    swim_start_opts_t opts = swim_opts_for(64, 15000);
    opts.self = self.c_str();
    opts.name = name.c_str();
    const char *seeds[] = {"127.0.0.1:5001/seed_cookie", nullptr};
    opts.seeds = seeds;
    REQUIRE(swim_start(&opts) == 0);
    even_names.push_back(name);
  }

  // 5. Verify convergence of the full 64‑node cluster
  for (int i = 1; i <= 64; ++i) {
    std::string name = "node_" + std::to_string(i);
    int cnt = 0;
    char *peers = swim_peers(name.c_str(), false, &cnt);
    REQUIRE(peers != nullptr);
    // full cluster size is 64 → each node sees 63 others
    REQUIRE(cnt == 63);
    free(peers);
  }

  // 6. Subscribe a collector process via telemetry feed (attach to seed)
  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);
  // Re‑start seed with feed attached
  swim_leave(seed_name);
  seed_opts.feed = feed;
  REQUIRE(swim_start(&seed_opts) == 0);

  // Helper to drain the feed into g_observations
  auto drain_feed = [&]() {
    char buf[SWIM_FEED_MAX_RECORD_SIZE];
    const char *ptr[SWIM_FEED_MAX_ELEMENTS];
    while (!swim_feed_empty(feed)) {
      int n = swim_feed_get(feed, sizeof(buf), buf, SWIM_FEED_MAX_ELEMENTS, (char **)ptr);
      if (n > 0) {
        std::string record(ptr[0], strlen(ptr[0]));
        for (int i = 1; i < n; ++i) record += " | " + std::string(ptr[i], strlen(ptr[i]));
        g_observations.push_back(record);
      }
    }
  };

  // 7. Kill node_7 and assert death detection
  REQUIRE(swim_leave("node_7") == 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(5000));
  drain_feed();
  CHECK(obs_contains(":node_down"));
  CHECK(obs_contains("node_7"));

  // 8. Pause node_14 (packet loss = 1.0) and assert it is marked dead
  extern int swim_set_packet_loss(const char *name, double loss);
  REQUIRE(swim_set_packet_loss("node_14", 1.0) == 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(5000));
  drain_feed();
  CHECK(obs_contains(":node_down"));
  CHECK(obs_contains("node_14"));

  // 9. Unpause node_14 (packet loss = 0.0) and confirm it rejoins
  REQUIRE(swim_set_packet_loss("node_14", 0.0) == 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(5000));
  // Verify full convergence again
  for (int i = 1; i <= 64; ++i) {
    if (i == 14) continue; // skip the node itself during check
    std::string name = "node_" + std::to_string(i);
    int cnt = 0;
    char *peers = swim_peers(name.c_str(), false, &cnt);
    REQUIRE(peers != nullptr);
    REQUIRE(cnt == 63);
    free(peers);
  }

  // 10. Graceful leave of node_14 and assert death detection again
  REQUIRE(swim_leave("node_14") == 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(5000));
  drain_feed();
  CHECK(obs_contains(":node_down"));
  CHECK(obs_contains("node_14"));

  // Clean up remaining nodes
  for (int i = 1; i <= 64; ++i) {
    std::string name = "node_" + std::to_string(i);
    swim_leave(name.c_str());
  }
  swim_feed_destroy(feed);
}


// ---------------------------------------------------------------------------
// 2. 64-node network: partition and heal
// ---------------------------------------------------------------------------
TEST_CASE("scale: partition and heal") {
  // TODO: Implement partition of 64-node cluster into two groups and heal.
  // Steps:
  // 1. Start all nodes with common seed.
  // 2. Verify initial convergence.
  // 3. Partition nodes 1‑32 vs 33‑64.
  // 4. Assert each group only sees its own members.
  // 5. Clear partitions and verify full convergence.
  REQUIRE(true);
}

// ---------------------------------------------------------------------------
// 3. 64-node network: 4-way partition and gradual heal
// ---------------------------------------------------------------------------
TEST_CASE("scale: 4-way partition and gradual heal") {
  // TODO: Implement 4-way partition (four groups of 16) and staged healing.
  // Steps:
  // 1. Start nodes with two seeds (m_node_1, m_node_33).
  // 2. Verify convergence.
  // 3. Partition into four groups.
  // 4. Assert each group sees 15 peers.
  // 5. Heal groups A+B and C+D, verify convergence to 32 each.
  // 6. Fully heal and verify 64.
  REQUIRE(true);
}

// ---------------------------------------------------------------------------
// 4. 64-node network: asymmetric partition (1 vs 63)
// ---------------------------------------------------------------------------
TEST_CASE("scale: asymmetric partition (1 vs 63)") {
  // TODO: Implement asymmetric partition with isolated seed node.
  // Steps:
  // 1. Start all nodes with seed.
  // 2. Verify convergence.
  // 3. Partition isolated node (ap_node_1) from remaining 63.
  // 4. Assert isolated node sees no peers; majority sees it as dead.
  // 5. Heal and verify full convergence.
  REQUIRE(true);
}

// ---------------------------------------------------------------------------
// 5. 64-node network: 30% packet loss stress
// ---------------------------------------------------------------------------
TEST_CASE("scale: 30% packet loss stress") {
  // TODO: Implement stress test with 30% packet loss on all nodes.
  // Steps:
  // 1. Start all nodes with increased suspicion timeout.
  // 2. Verify initial convergence.
  // 3. Apply 30% packet loss.
  // 4. Wait for convergence under loss and assert cluster remains converged.
  REQUIRE(true);
}

// ---------------------------------------------------------------------------
// 6. 64-node network: churn stress (restarting nodes)
// ---------------------------------------------------------------------------
TEST_CASE("scale: churn stress (restarting nodes)") {
  // TODO: Implement churn by killing nodes 50‑60 simultaneously and restarting.
  // Steps:
  // 1. Start all nodes with seed.
  // 2. Verify convergence.
  // 3. Kill nodes 50‑60.
  // 4. Allow suspicion detection.
  // 5. Restart killed nodes.
  // 6. Verify cluster re‑converges to 64.
  REQUIRE(true);
}

// ---------------------------------------------------------------------------
// 7. 64-node network: half‑cluster restart immediately (immediate revival)
// ---------------------------------------------------------------------------
TEST_CASE("scale: half‑cluster immediate restart") {
  // TODO: Implement immediate revival of first 32 nodes after kill.
  // Steps:
  // 1. Start all nodes.
  // 2. Verify convergence.
  // 3. Kill first 32 nodes.
  // 4. Immediately restart those nodes.
  // 5. Verify full convergence.
  REQUIRE(true);
}

// ---------------------------------------------------------------------------
// 8. 64-node network: half‑cluster restart after death detection (staged revival)
// ---------------------------------------------------------------------------
TEST_CASE("scale: half‑cluster staged revival") {
  // TODO: Implement staged restart after surviving half detects deaths.
  // Steps:
  // 1. Start all nodes.
  // 2. Verify convergence.
  // 3. Kill first 32 nodes.
  // 4. Wait for remaining nodes to mark them dead.
  // 5. Restart killed nodes.
  // 6. Verify re‑convergence.
  REQUIRE(true);
}

// ---------------------------------------------------------------------------
// 9. 64-node network: rolling upgrade simulation
// ---------------------------------------------------------------------------
TEST_CASE("scale: rolling upgrade simulation") {
  // TODO: Implement rolling upgrade in batches of 8.
  // Steps:
  // 1. Start all nodes with two seeds.
  // 2. Verify convergence.
  // 3. For each batch of 8: kill, restart with incarnation 2, pause.
  // 4. Verify full convergence after all batches.
  REQUIRE(true);
}

// ---------------------------------------------------------------------------
// 10. 64-node network: high latency jitter and delay stress
// ---------------------------------------------------------------------------
TEST_CASE("scale: high latency jitter and delay stress") {
  // TODO: Implement heterogeneous latency (0‑24 ms) on all nodes.
  // Steps:
  // 1. Start all nodes with seed and assigned delays.
  // 2. Verify convergence despite delays.
  REQUIRE(true);
}

// ---------------------------------------------------------------------------
// 11. 64-node network: bootstrap storm simulation
// ---------------------------------------------------------------------------
TEST_CASE("scale: bootstrap storm simulation") {
  // TODO: Implement bootstrap storm where many nodes join rapid after a single seed.
  // Steps:
  // 1. Start seed node.
  // 2. Sequentially spawn remaining 63 nodes using the seed.
  // 3. Verify convergence of all 64 nodes.
  REQUIRE(true);
}

// End of scale test suite scaffold.
