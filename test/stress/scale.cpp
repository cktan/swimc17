#include "doctest.h"

extern "C" {
#include "swim.h"
#include "swim_udp.h"
}

#include <chrono>
#include <cstring>
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

// Poll until all nodes in [from,to] (skipping skip1 and skip2, 0 = no skip)
// see exactly `expected` peers. Returns true on success, false on timeout.
static bool wait_for_all_peers(int from, int to, int skip1, int skip2,
                               int expected, int timeout_ms) {
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    bool all_ok = true;
    for (int i = from; i <= to && all_ok; i++) {
      if (i == skip1 || i == skip2)
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
  CHECK(wait_for_all_peers(1, 64, 0, 0, 63, 30000));

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
  CHECK(wait_for_all_peers(1, 64, 7, 0, 62, 60000));

  // Gracefully leave node_14 and verify detection via node_1's feed
  REQUIRE(swim_leave("node_14") == 0);
  CHECK(drain_feed_for(feed, "down", ":5014", 10000));

  // Cleanup
  swim_feed_destroy(feed);
}
