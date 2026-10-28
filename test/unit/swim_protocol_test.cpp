#include "doctest.h"

extern "C" {
#include "swim.h"
#include "swim_codec.h"
#include "swim_errno.h"
#include "swim_gossip_queue.h"
#include "swim_internal.h"
#include "swim_nodeid.h"
#include "swim_udp.h"
}

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

// --- Telemetry feed capture helpers ---
std::vector<std::string> g_obs;
pthread_mutex_t g_obs_mutex = PTHREAD_MUTEX_INITIALIZER;

void poll_feed_events(swim_feed_t *feed) {
  char buf[4096];
  char *ptr[10];
  int n;
  while ((n = swim_feed_get(feed, sizeof(buf), buf, 10, ptr)) > 0) {
    std::string s;
    for (int i = 0; i < n; i++) {
      if (i > 0)
        s += " ";
      s += ptr[i];
    }
    pthread_mutex_lock(&g_obs_mutex);
    g_obs.push_back(s);
    pthread_mutex_unlock(&g_obs_mutex);
  }
}

void clear_obs() {
  pthread_mutex_lock(&g_obs_mutex);
  g_obs.clear();
  pthread_mutex_unlock(&g_obs_mutex);
}

bool obs_contains(const char *needle) {
  pthread_mutex_lock(&g_obs_mutex);
  bool found = false;
  for (const auto &s : g_obs) {
    if (s.find(needle) != std::string::npos) {
      found = true;
      break;
    }
  }
  pthread_mutex_unlock(&g_obs_mutex);
  return found;
}

size_t obs_count(const char *needle) {
  pthread_mutex_lock(&g_obs_mutex);
  size_t n = 0;
  for (const auto &s : g_obs) {
    if (s.find(needle) != std::string::npos)
      n++;
  }
  pthread_mutex_unlock(&g_obs_mutex);
  return n;
}

size_t obs_size() {
  pthread_mutex_lock(&g_obs_mutex);
  size_t sz = g_obs.size();
  pthread_mutex_unlock(&g_obs_mutex);
  return sz;
}

} // namespace

TEST_CASE("protocol: single node startup and leave") {
  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.name = "single_node";

  swim_t *inst = swim_start("127.0.0.1:20001", &opts);
  REQUIRE(inst != nullptr);

  // Query members (should be empty because self is not in the list)
  char *p;
  int count = swim_peers(inst, true, &p);
  CHECK(count == 0);
  free(p);

  int rc = swim_leave(inst);
  CHECK(rc == 0);
  swim_set_error(SWIM_OK, nullptr);
}

TEST_CASE("protocol: multi-node auto-discovery") {
  // Node 1 setup (seed: Node 2)
  const char *seeds1[] = {"127.0.0.1:20102/c2", nullptr};
  swim_start_opts_t opts1;
  memset(&opts1, 0, sizeof(opts1));
  opts1.name = "multi_test";
  opts1.seeds = seeds1;
  opts1.protocol_period_ms = 400;
  opts1.ping_timeout_ms = 100;
  opts1.seed_retry_interval_ms = 400;

  // Node 2 setup (seed: Node 3)
  const char *seeds2[] = {"127.0.0.1:20103/c3", nullptr};
  swim_start_opts_t opts2;
  memset(&opts2, 0, sizeof(opts2));
  opts2.name = "multi_test";
  opts2.seeds = seeds2;
  opts2.protocol_period_ms = 400;
  opts2.ping_timeout_ms = 100;
  opts2.seed_retry_interval_ms = 400;

  // Node 3 setup (seed: Node 1)
  const char *seeds3[] = {"127.0.0.1:20101/c1", nullptr};
  swim_start_opts_t opts3;
  memset(&opts3, 0, sizeof(opts3));
  opts3.name = "multi_test";
  opts3.seeds = seeds3;
  opts3.protocol_period_ms = 400;
  opts3.ping_timeout_ms = 100;
  opts3.seed_retry_interval_ms = 400;

  swim_t *n1 = swim_start("127.0.0.1:20101/c1", &opts1);
  swim_t *n2 = swim_start("127.0.0.1:20102/c2", &opts2);
  swim_t *n3 = swim_start("127.0.0.1:20103/c3", &opts3);
  REQUIRE(n1 != nullptr);
  REQUIRE(n2 != nullptr);
  REQUIRE(n3 != nullptr);

  // Wait for periodic probes and seed discovery to execute (approx 1.5 seconds)
  usleep(1500000);

  // Check Node 1 membership
  char *p1, *p2, *p3;
  int count1 = swim_peers(n1, false, &p1);
  free(p1);
  CHECK(count1 >= 2); // should have discovered Node 2 and Node 3

  // Check Node 2 membership
  int count2 = swim_peers(n2, false, &p2);
  free(p2);
  CHECK(count2 >= 2);

  // Check Node 3 membership
  int count3 = swim_peers(n3, false, &p3);
  free(p3);
  CHECK(count3 >= 2);

  // Tear down all
  CHECK(swim_leave(n1) == 0);
  CHECK(swim_leave(n2) == 0);
  CHECK(swim_leave(n3) == 0);
  swim_set_error(SWIM_OK, nullptr);
}

TEST_CASE("protocol: failure detection and liveness hint") {
  clear_obs();

  swim_nodeid_idx_t mock_id = swim_nodeid_find("127.0.0.1:20202/mock");
  REQUIRE(nodeid_valid(mock_id));

  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);

  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.name = "failure_node";
  opts.protocol_period_ms = 400;
  opts.ping_timeout_ms = 100;
  opts.suspicion_timeout_ms = 600;
  opts.feed = feed;

  swim_t *inst = swim_start("127.0.0.1:20201/c1", &opts);
  REQUIRE(inst != nullptr);

  // Simulate a join from a mock node by sending a raw PING packet to opts.port
  swim_udp_t *mock_udp = swim_udp_create("127.0.0.1", 20202);
  REQUIRE(mock_udp != nullptr);

  // Construct authenticated PING packet from mock
  uint8_t msg_buf[256];
  int msglen = swim_pack_message(SWIM_MSG_PING, mock_id, 1, SWIM_NODEID_NONE,
                                 nullptr, 0, msg_buf, sizeof(msg_buf));
  REQUIRE(msglen > 0);
  uint8_t buf[256 + 12];
  int len = swim_pack_authed("failure_node", msg_buf, msglen, buf, sizeof(buf));
  REQUIRE(len == msglen + 12);

  // Send packet to node
  int rc = swim_udp_send(mock_udp, "127.0.0.1", 20201, buf, len);
  REQUIRE(rc == 0);

  // Wait a moment for packet processing
  usleep(150000);

  // Verify Node 1 discovered mock node
  swim_member_t members[5];
  int count = swim_members(inst, members, 5, false);
  REQUIRE(count == 1);
  CHECK(nodeid_eq(members[0].id, swim_nodeid_find("127.0.0.1:20202/mock")));
  CHECK(members[0].status == SWIM_STATUS_ALIVE);

  // Verify feed event
  poll_feed_events(feed);
  CHECK(obs_contains("node up 127.0.0.1:20202/mock"));

  // Now silently crash the mock node by closing its UDP socket
  swim_udp_destroy(mock_udp);

  // Wait for failure detector to execute:
  // - Direct probe will fail (ping_timeout = 100ms)
  // - Indirect probe will fail (ping_timeout = 100ms)
  // - Node will declare SUSPECT (400ms protocol period + timeouts ~ 600ms)
  usleep(700000);

  // Verify node is SUSPECT
  count = swim_members(inst, members, 5, false);
  REQUIRE(count == 1);
  CHECK(members[0].status == SWIM_STATUS_SUSPECT);

  poll_feed_events(feed);
  CHECK(obs_contains("node suspect 127.0.0.1:20202/mock"));

  // Provide a liveness hint to revive the suspected node
  rc = swim_hint_alive(inst, "127.0.0.1:20202/mock");
  CHECK(rc == 0);

  // Verify it is ALIVE again
  count = swim_members(inst, members, 5, false);
  REQUIRE(count == 1);
  CHECK(members[0].status == SWIM_STATUS_ALIVE);

  poll_feed_events(feed);
  // hint_alive emits another node-up
  CHECK(obs_contains("node up 127.0.0.1:20202/mock"));

  // Now wait long enough without hints to let it time out and die
  usleep(1500000);

  // Verify it is DEAD (not in active list since include_dead = false)
  count = swim_members(inst, members, 5, false);
  CHECK(count == 0);

  // Check with include_dead = true
  count = swim_members(inst, members, 5, true);
  REQUIRE(count == 1);
  CHECK(members[0].status == SWIM_STATUS_DEAD);

  poll_feed_events(feed);
  CHECK(obs_contains("node down 127.0.0.1:20202/mock"));

  swim_leave(inst);
  swim_feed_destroy(feed);
  swim_set_error(SWIM_OK, nullptr);
}

// Regression for H1: relay-table entries for ping_req targets that never ack
// must be reclaimed, otherwise the table fills to 32 and the node permanently
// stops relaying. We flood the node with 32 ping_reqs for dead targets, let
// them expire, then verify a fresh ping_req still produces a relay ping.
TEST_CASE("protocol: relay table does not permanently fill") {
  swim_nodeid_idx_t requester_id = swim_nodeid_find("127.0.0.1:20302/r");
  swim_nodeid_idx_t target_id = swim_nodeid_find("127.0.0.1:20303/t");
  REQUIRE(nodeid_valid(requester_id));
  REQUIRE(nodeid_valid(target_id));

  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.name = "relay_node";
  opts.protocol_period_ms = 1000; // keep the node's own probing out of the way
  opts.ping_timeout_ms = 100;     // relay entries expire after ~1 tick

  swim_t *inst = swim_start("127.0.0.1:20301/c1", &opts);
  REQUIRE(inst != nullptr);

  swim_udp_t *requester = swim_udp_create("127.0.0.1", 20302);
  REQUIRE(requester != nullptr);
  swim_udp_t *target = swim_udp_create("127.0.0.1", 20303);
  REQUIRE(target != nullptr);

  auto send_ping_req = [&](swim_nodeid_idx_t tgt, uint32_t seq) {
    uint8_t msg_buf[256];
    int msglen = swim_pack_message(SWIM_MSG_PING_REQ, requester_id, seq, tgt,
                                   nullptr, 0, msg_buf, sizeof(msg_buf));
    REQUIRE(msglen > 0);
    uint8_t buf[256 + 12];
    int len = swim_pack_authed("relay_node", msg_buf, msglen, buf, sizeof(buf));
    REQUIRE(len == msglen + 12);
    REQUIRE(swim_udp_send(requester, "127.0.0.1", 20301, buf, len) == 0);
  };

  // Flood the relay table (capacity 32) with ping_reqs for dead targets that
  // will never ack.
  for (uint32_t i = 0; i < 32; i++) {
    char s[64];
    snprintf(s, sizeof(s), "127.0.0.1:%u/d", 21000 + i);
    swim_nodeid_idx_t dead = swim_nodeid_find(s);
    REQUIRE(nodeid_valid(dead));
    send_ping_req(dead, 100 + i);
    usleep(5000);
  }

  // Drain any relay pings the node sent to the dead targets so they don't
  // linger and confuse the assertion below.
  usleep(200000);

  // Let the flooded entries expire (ping_timeout 100ms => ~1-2 ticks).
  usleep(400000);

  // A fresh ping_req for a live target must now produce a relay ping; without
  // GC of expired entries the table is still full and nothing is sent.
  send_ping_req(target_id, 9999);

  bool relay_ping_seen = false;
  for (int attempt = 0; attempt < 50 && !relay_ping_seen; attempt++) {
    uint8_t buf[256 + 12];
    int n = swim_udp_recv(target, buf, sizeof(buf));
    if (n > 12) {
      swim_message_t in;
      if (swim_unpack_message(buf + 12, n - 12, &in) == 0 &&
          in.type == SWIM_MSG_PING && in.seq == 9999 &&
          nodeid_eq(in.sender, swim_nodeid_find("127.0.0.1:20301/c1"))) {
        relay_ping_seen = true;
      }
    } else {
      usleep(10000);
    }
  }
  CHECK(relay_ping_seen);

  swim_udp_destroy(requester);
  swim_udp_destroy(target);
  swim_leave(inst);
  swim_set_error(SWIM_OK, nullptr);
}

// Regression for H3 lifetime: concurrent swim_hint_alive calls while the
// instance is live must not race with internal state. Hammer hint_alive from
// several threads while the main thread is active, then join before leaving.
// Meaningful under ASan/TSan.
static std::atomic<bool> g_race_stop{false};
struct RaceArg {
  swim_t *inst;
  const char *peer;
};
static void *hint_spammer(void *a) {
  RaceArg *r = (RaceArg *)a;
  while (!g_race_stop.load()) {
    swim_hint_alive(r->inst, r->peer);
  }
  return nullptr;
}

TEST_CASE("protocol: concurrent swim_hint_alive and swim_leave are memory-safe "
          "(H3)") {
  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.name = "h3_race";
  opts.protocol_period_ms = 100;
  opts.ping_timeout_ms = 50;

  swim_t *inst = swim_start("127.0.0.1:20403/c1", &opts);
  REQUIRE(inst != nullptr);

  RaceArg arg;
  arg.inst = inst;
  arg.peer = "127.0.0.1:20404/p";

  g_race_stop = false;
  pthread_t th[4];
  for (int i = 0; i < 4; i++) {
    REQUIRE(pthread_create(&th[i], nullptr, hint_spammer, &arg) == 0);
  }

  usleep(100000); // let the spammers hammer the live instance
  g_race_stop = true;
  for (int i = 0; i < 4; i++) {
    pthread_join(th[i], nullptr);
  }
  CHECK(swim_leave(inst) == 0);
  swim_set_error(SWIM_OK, nullptr);
}

TEST_CASE("protocol: gossip byte budget does not exceed MTU (M1)") {
  swim_nodeid_idx_t mock_id = swim_nodeid_find("127.0.0.1:20502/mock");
  REQUIRE(nodeid_valid(mock_id));

  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.name = "m1_budget";
  opts.protocol_period_ms = 10000; // slow
  opts.ping_timeout_ms = 1000;

  swim_t *inst = swim_start("127.0.0.1:20501/c1", &opts);
  REQUIRE(inst != nullptr);

  swim_udp_t *mock_udp = swim_udp_create("127.0.0.1", 20502);
  REQUIRE(mock_udp != nullptr);

  // Send 5 PINGs, each containing 6 large gossip events, to populate
  // the node's gossip queue with 30 large events.
  for (uint32_t p = 0; p < 5; p++) {
    swim_gossip_queue_t *test_q = swim_gossip_queue_create();
    for (int i = 0; i < 6; i++) {
      int idx = p * 6 + i;
      char ev_str[512];
      snprintf(ev_str, sizeof(ev_str),
               "node-with-a-very-long-name-to-make-it-large-and-fill-up-the-"
               "packet-budget-limit-domain-%d.com:%d/cookie-cookie-cookie-"
               "cookie-%d",
               idx, 30000 + idx, idx);
      swim_nodeid_idx_t ev_id = swim_nodeid_find(ev_str);
      swim_gossip_queue_enqueue(test_q, SWIM_STATUS_ALIVE, ev_id, 100 + idx, 1);
    }

    uint8_t msg_buf[1024];
    int msglen =
        swim_pack_message(SWIM_MSG_PING, mock_id, 100 + p, SWIM_NODEID_NONE,
                          test_q, 1, msg_buf, sizeof(msg_buf));
    swim_gossip_queue_destroy(test_q);
    REQUIRE(msglen > 0);
    uint8_t send_buf[1024 + 12];
    int len = swim_pack_authed("m1_budget", msg_buf, msglen, send_buf,
                               sizeof(send_buf));
    REQUIRE(len == msglen + 12);
    REQUIRE(swim_udp_send(mock_udp, "127.0.0.1", 20501, send_buf, len) == 0);

    // Wait and drain the reply ACK for this PING so they don't pile up.
    bool ack_seen = false;
    uint8_t recv_buf[2048];
    for (int attempt = 0; attempt < 50 && !ack_seen; attempt++) {
      int n = swim_udp_recv(mock_udp, recv_buf, sizeof(recv_buf));
      if (n > 12) {
        swim_message_t in;
        if (swim_unpack_message(recv_buf + 12, n - 12, &in) == 0 &&
            in.type == SWIM_MSG_ACK && in.seq == 100 + p) {
          ack_seen = true;
          // For the last ACK (p == 4), verify the packet sizes and counts.
          if (p == 4) {
            CHECK(n <= SWIM_MAX_PACKET_SIZE);
            CHECK(in.gossip_count > 0);
            CHECK(in.gossip_count < 30);
          }
        }
      } else {
        usleep(10000);
      }
    }
    REQUIRE(ack_seen == true);
  }

  swim_udp_destroy(mock_udp);
  swim_leave(inst);
  swim_set_error(SWIM_OK, nullptr);
}

TEST_CASE("protocol: pack and unpack message helper roundtrip") {
  swim_gossip_queue_t *q = swim_gossip_queue_create();
  REQUIRE(q != nullptr);
  swim_membership_t *m = swim_membership_create();
  REQUIRE(m != nullptr);

  swim_nodeid_idx_t sender = swim_nodeid_find("127.0.0.1:8001/sender_cookie");
  swim_nodeid_idx_t peer = swim_nodeid_find("127.0.0.1:8002/peer_cookie");
  REQUIRE(nodeid_valid(sender));
  REQUIRE(nodeid_valid(peer));

  // Enqueue a gossip event
  swim_nodeid_idx_t gossip_node =
      swim_nodeid_find("127.0.0.1:9001/gossip_cookie");
  REQUIRE(nodeid_valid(gossip_node));
  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, gossip_node, 100,
                                    1) == 0);

  uint8_t buf[2048];
  int bytes = swim_pack_message(SWIM_MSG_PING_REQ, sender, 12345, peer, q,
                                swim_membership_count(m), buf, sizeof(buf));
  REQUIRE(bytes > 0);

  swim_message_t msg;
  memset(&msg, 0, sizeof(msg));
  int rc = swim_unpack_message(buf, bytes, &msg);
  REQUIRE(rc == 0);

  CHECK(msg.type == SWIM_MSG_PING_REQ);
  CHECK(msg.seq == 12345);
  CHECK(nodeid_eq(msg.sender, sender));
  CHECK(nodeid_eq(msg.peer, peer));
  CHECK(msg.gossip_count == 1);
  CHECK(msg.gossip[0].status == SWIM_STATUS_ALIVE);
  CHECK(msg.gossip[0].incarnation == 100);
  CHECK(nodeid_eq(msg.gossip[0].id, gossip_node));

  // Check error handling with buffer too small
  int err_bytes = swim_pack_message(SWIM_MSG_PING_REQ, sender, 12345, peer, q,
                                    swim_membership_count(m), buf, 10);
  CHECK(err_bytes == -1);

  swim_membership_destroy(m);
  swim_gossip_queue_destroy(q);
}

TEST_CASE("protocol: pack and unpack leave message roundtrip") {
  swim_gossip_queue_t *q = swim_gossip_queue_create();
  REQUIRE(q != nullptr);
  swim_membership_t *m = swim_membership_create();
  REQUIRE(m != nullptr);

  swim_nodeid_idx_t sender = swim_nodeid_find("127.0.0.1:8001/sender_cookie");
  REQUIRE(nodeid_valid(sender));

  // Enqueue a gossip event (should be ignored for LEAVE)
  swim_nodeid_idx_t gossip_node =
      swim_nodeid_find("127.0.0.1:9001/gossip_cookie");
  REQUIRE(nodeid_valid(gossip_node));
  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, gossip_node, 100,
                                    1) == 0);

  uint8_t buf[2048];
  // Pass NULL gossip queue
  int bytes = swim_pack_message(SWIM_MSG_LEAVE, sender, 12345, SWIM_NODEID_NONE,
                                nullptr, 0, buf, sizeof(buf));
  REQUIRE(bytes > 0);

  swim_message_t msg;
  memset(&msg, 0, sizeof(msg));
  int rc = swim_unpack_message(buf, bytes, &msg);
  REQUIRE(rc == 0);

  CHECK(msg.type == SWIM_MSG_LEAVE);
  CHECK(msg.seq == 12345);
  CHECK(nodeid_eq(msg.sender, sender));
  CHECK(msg.gossip_count == 0);

  swim_membership_destroy(m);
  swim_gossip_queue_destroy(q);
}

// L3: the observer receives membership transitions and the cluster-size gauge.
TEST_CASE("protocol: observer telemetry — transitions, cluster size, escaping "
          "(L3)") {
  clear_obs();

  // Mock peer with a cookie containing non-alphanumeric bytes (space, '!').
  swim_nodeid_idx_t mock_id = swim_nodeid_find("127.0.0.1:20502/a b!");
  REQUIRE(nodeid_valid(mock_id));

  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);

  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.name = "obs_node";
  // First probe lands after the quiet window checked below.
  opts.protocol_period_ms = 1000;
  opts.feed = feed;

  swim_t *inst = swim_start("127.0.0.1:20501/c1", &opts);
  REQUIRE(inst != nullptr);

  swim_udp_t *mock_udp = swim_udp_create("127.0.0.1", 20502);
  REQUIRE(mock_udp != nullptr);

  uint8_t msg_buf[256];
  int msglen = swim_pack_message(SWIM_MSG_PING, mock_id, 1, SWIM_NODEID_NONE,
                                 nullptr, 0, msg_buf, sizeof(msg_buf));
  REQUIRE(msglen > 0);
  uint8_t buf[256 + 12];
  int len = swim_pack_authed("obs_node", msg_buf, msglen, buf, sizeof(buf));
  REQUIRE(len == msglen + 12);
  REQUIRE(swim_udp_send(mock_udp, "127.0.0.1", 20501, buf, len) == 0);

  usleep(400000); // packet processing + a few ticks for the cluster-size emit

  // node up transition, with the cookie unescaped
  poll_feed_events(feed);
  CHECK(obs_contains("node up 127.0.0.1:20502/a b!"));
  // cluster-size gauge moved from 0 to 1.
  CHECK(obs_contains("cluster size 1"));

  clear_obs();
  swim_udp_destroy(mock_udp);
  usleep(300000);
  poll_feed_events(feed);
  CHECK(obs_size() == 0);

  swim_leave(inst);
  swim_feed_destroy(feed);
  swim_set_error(SWIM_OK, nullptr);
}

// L3: a clean direct probe round-trip produces a ping rtt feed event.
TEST_CASE("protocol: observer reports direct ping RTT (L3)") {
  clear_obs();

  swim_feed_t *feed_a = swim_feed_create();
  REQUIRE(feed_a != nullptr);

  const char *seedsA[] = {"127.0.0.1:20512/b1", nullptr};
  swim_start_opts_t a;
  memset(&a, 0, sizeof(a));
  a.name = "rtt_test";
  a.seeds = seedsA;
  a.protocol_period_ms = 200;
  a.ping_timeout_ms = 100;
  a.seed_retry_interval_ms = 200;
  a.feed = feed_a;

  const char *seedsB[] = {"127.0.0.1:20511/a1", nullptr};
  swim_start_opts_t b;
  memset(&b, 0, sizeof(b));
  b.name = "rtt_test";
  b.seeds = seedsB;
  b.protocol_period_ms = 200;
  b.ping_timeout_ms = 100;
  b.seed_retry_interval_ms = 200;

  swim_t *na = swim_start("127.0.0.1:20511/a1", &a);
  swim_t *nb = swim_start("127.0.0.1:20512/b1", &b);
  REQUIRE(na != nullptr);
  REQUIRE(nb != nullptr);

  // Let the nodes discover each other and run several probe cycles, so A sends
  // a direct ping to B and B acks it.
  usleep(1200000);
  poll_feed_events(feed_a);

  CHECK(obs_contains("ping rtt 127.0.0.1:20512"));

  CHECK(swim_leave(na) == 0);
  CHECK(swim_leave(nb) == 0);
  swim_feed_destroy(feed_a);
  swim_set_error(SWIM_OK, nullptr);
}

// L3: a feed delivers a node-up event when a mock node joins.
TEST_CASE("protocol: feed delivers node-up event (L3)") {
  clear_obs();

  swim_nodeid_idx_t mock_id = swim_nodeid_find("127.0.0.1:20602/mock");
  REQUIRE(nodeid_valid(mock_id));

  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);

  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.name = "feed_test";
  opts.protocol_period_ms = 200;
  opts.feed = feed;

  swim_t *inst = swim_start("127.0.0.1:20601/c1", &opts);
  REQUIRE(inst != nullptr);

  swim_udp_t *mock_udp = swim_udp_create("127.0.0.1", 20602);
  REQUIRE(mock_udp != nullptr);

  uint8_t msg_buf[256];
  int msglen = swim_pack_message(SWIM_MSG_PING, mock_id, 1, SWIM_NODEID_NONE,
                                 nullptr, 0, msg_buf, sizeof(msg_buf));
  REQUIRE(msglen > 0);
  uint8_t auth_buf[256 + 12];
  int authlen = swim_pack_authed("feed_test", msg_buf, msglen, auth_buf,
                                 sizeof(auth_buf));
  REQUIRE(authlen == msglen + 12);
  REQUIRE(swim_udp_send(mock_udp, "127.0.0.1", 20601, auth_buf, authlen) == 0);

  usleep(300000);

  poll_feed_events(feed);
  CHECK(obs_contains("node up 127.0.0.1:20602/mock"));

  swim_udp_destroy(mock_udp);
  swim_leave(inst);
  swim_feed_destroy(feed);
  swim_set_error(SWIM_OK, nullptr);
}

// Wait up to ~1s for a packet on `udp` and return the incarnation carried by
// its alive(node) gossip, or 0 if the packet has none.
static uint64_t recv_alive_inc(swim_udp_t *udp, const char *node) {
  swim_nodeid_idx_t id = swim_nodeid_find(node);
  for (int attempt = 0; attempt < 100; attempt++) {
    uint8_t buf[SWIM_MAX_PACKET_SIZE];
    int n = swim_udp_recv(udp, buf, sizeof(buf));
    if (n <= 12) {
      usleep(10000);
      continue;
    }
    swim_message_t in;
    if (swim_unpack_message(buf + 12, n - 12, &in) != 0)
      return 0;
    for (int i = 0; i < in.gossip_count; i++) {
      if (nodeid_eq(in.gossip[i].id, id) &&
          in.gossip[i].status == SWIM_STATUS_ALIVE)
        return in.gossip[i].incarnation;
    }
    return 0;
  }
  return 0;
}

// A peer that learned of X only by direct contact records X at incarnation 0
// and may later gossip suspect(X, 0). X must refute it even though 0 is below
// its own incarnation; otherwise nothing overrides the rumor and X is declared
// dead.
TEST_CASE("protocol: stale suspicion about self is refuted") {
  const char *x_str = "127.0.0.1:20701/x";
  swim_nodeid_idx_t x_id = swim_nodeid_find(x_str);
  swim_nodeid_idx_t y_id = swim_nodeid_find("127.0.0.1:20702/y");

  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.name = "refute_test";
  opts.protocol_period_ms = 10000; // keep X's own probes off Y's socket

  swim_t *inst = swim_start(x_str, &opts);
  REQUIRE(inst != nullptr);
  swim_udp_t *y = swim_udp_create("127.0.0.1", 20702);
  REQUIRE(y != nullptr);

  auto ping_x = [&](swim_gossip_queue_t *q, uint32_t seq) {
    uint8_t msg_buf[512];
    int msglen = swim_pack_message(SWIM_MSG_PING, y_id, seq, SWIM_NODEID_NONE,
                                   q, 1, msg_buf, sizeof(msg_buf));
    REQUIRE(msglen > 0);
    uint8_t buf[512 + 12];
    int len =
        swim_pack_authed("refute_test", msg_buf, msglen, buf, sizeof(buf));
    REQUIRE(swim_udp_send(y, "127.0.0.1", 20701, buf, len) == 0);
  };

  // Drain X's startup self-announcement out of its gossip queue.
  uint64_t inc = 0;
  for (uint32_t seq = 1; seq <= 4; seq++) {
    ping_x(nullptr, seq);
    inc = recv_alive_inc(y, x_str);
  }
  REQUIRE(inc == 0);

  // Y suspects X at the stale incarnation 0.
  swim_gossip_queue_t *q = swim_gossip_queue_create();
  REQUIRE(q != nullptr);
  swim_gossip_queue_enqueue(q, SWIM_STATUS_SUSPECT, x_id, 0, 1);
  ping_x(q, 5);
  CHECK(recv_alive_inc(y, x_str) > 0);

  swim_gossip_queue_destroy(q);
  swim_udp_destroy(y);
  swim_leave(inst);
  swim_set_error(SWIM_OK, nullptr);
}

// A node starting alone pings all its seeds. Its self-announcement must reach
// every other seed, even when the shared seed list names the node itself
// first.
TEST_CASE("protocol: self-announcement reaches all seeds") {
  const char *x_str = "127.0.0.1:20711/x";
  const char *seeds[] = {x_str,
                         "127.0.0.1:20712/a",
                         "127.0.0.1:20713/b",
                         "127.0.0.1:20714/c",
                         "127.0.0.1:20715/d",
                         "127.0.0.1:20716/e",
                         nullptr};
  const int nmocks = 5;

  swim_udp_t *mocks[nmocks];
  for (int i = 0; i < nmocks; i++) {
    mocks[i] = swim_udp_create("127.0.0.1", (uint16_t)(20712 + i));
    REQUIRE(mocks[i] != nullptr);
  }

  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.name = "announce_test";
  opts.seeds = seeds;

  swim_t *inst = swim_start(x_str, &opts);
  REQUIRE(inst != nullptr);

  for (int i = 0; i < nmocks; i++)
    CHECK(recv_alive_inc(mocks[i], x_str) > 0);

  swim_leave(inst);
  for (int i = 0; i < nmocks; i++)
    swim_udp_destroy(mocks[i]);
  swim_set_error(SWIM_OK, nullptr);
}

// A ping timeout under one 100 ms tick must still give relays a window to
// forward the target's ack; it used to round to zero ticks, so relay entries
// expired immediately and indirect probes could never succeed.
TEST_CASE("protocol: sub-tick ping timeout still relays acks") {
  swim_nodeid_idx_t requester_id = swim_nodeid_find("127.0.0.1:20722/r");
  swim_nodeid_idx_t target_id = swim_nodeid_find("127.0.0.1:20723/t");

  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.name = "subtick_test";
  opts.protocol_period_ms = 10000; // keep the node's own probing out of the way
  opts.ping_timeout_ms = 50;

  swim_t *inst = swim_start("127.0.0.1:20721/c1", &opts);
  REQUIRE(inst != nullptr);
  swim_udp_t *requester = swim_udp_create("127.0.0.1", 20722);
  REQUIRE(requester != nullptr);
  swim_udp_t *target = swim_udp_create("127.0.0.1", 20723);
  REQUIRE(target != nullptr);

  auto send_msg = [&](swim_udp_t *from, uint8_t type, swim_nodeid_idx_t sender,
                      uint32_t seq, swim_nodeid_idx_t peer) {
    uint8_t msg_buf[256];
    int msglen = swim_pack_message(type, sender, seq, peer, nullptr, 0, msg_buf,
                                   sizeof(msg_buf));
    REQUIRE(msglen > 0);
    uint8_t buf[256 + 12];
    int len =
        swim_pack_authed("subtick_test", msg_buf, msglen, buf, sizeof(buf));
    REQUIRE(swim_udp_send(from, "127.0.0.1", 20721, buf, len) == 0);
  };
  auto recv_msg = [](swim_udp_t *udp, uint8_t type, swim_message_t *in) {
    for (int attempt = 0; attempt < 100; attempt++) {
      uint8_t buf[SWIM_MAX_PACKET_SIZE];
      int n = swim_udp_recv(udp, buf, sizeof(buf));
      if (n > 12 && swim_unpack_message(buf + 12, n - 12, in) == 0 &&
          in->type == type)
        return true;
      if (n <= 12)
        usleep(10000);
    }
    return false;
  };

  send_msg(requester, SWIM_MSG_PING_REQ, requester_id, 777, target_id);

  // The target answers the relay ping immediately.
  swim_message_t in;
  REQUIRE(recv_msg(target, SWIM_MSG_PING, &in));
  REQUIRE(in.seq == 777);
  send_msg(target, SWIM_MSG_ACK, target_id, 777, SWIM_NODEID_NONE);

  REQUIRE(recv_msg(requester, SWIM_MSG_FWD_ACK, &in));
  CHECK(in.seq == 777);
  CHECK(nodeid_eq(in.peer, target_id));

  swim_udp_destroy(requester);
  swim_udp_destroy(target);
  swim_leave(inst);
  swim_set_error(SWIM_OK, nullptr);
}

// With 2 x ping_timeout >= protocol_period, the next probe used to overwrite
// the pending one before its indirect phase timed out, so a dead peer was
// never suspected.
TEST_CASE("protocol: short period still detects failure") {
  swim_nodeid_idx_t mock_id = swim_nodeid_find("127.0.0.1:20732/mock");

  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.name = "short_period";
  opts.protocol_period_ms = 200; // ping_timeout_ms keeps its 200 ms default

  swim_t *inst = swim_start("127.0.0.1:20731/c1", &opts);
  REQUIRE(inst != nullptr);

  // The mock joins with one ping, then goes silent.
  swim_udp_t *mock = swim_udp_create("127.0.0.1", 20732);
  REQUIRE(mock != nullptr);
  uint8_t msg_buf[256];
  int msglen = swim_pack_message(SWIM_MSG_PING, mock_id, 1, SWIM_NODEID_NONE,
                                 nullptr, 0, msg_buf, sizeof(msg_buf));
  REQUIRE(msglen > 0);
  uint8_t buf[256 + 12];
  int len = swim_pack_authed("short_period", msg_buf, msglen, buf, sizeof(buf));
  REQUIRE(swim_udp_send(mock, "127.0.0.1", 20731, buf, len) == 0);
  swim_udp_destroy(mock);

  bool suspected = false;
  for (int i = 0; i < 30 && !suspected; i++) {
    usleep(100000);
    swim_member_t members[4];
    int count = swim_members(inst, members, 4, true);
    suspected = count == 1 && members[0].status != SWIM_STATUS_ALIVE;
  }
  CHECK(suspected);

  swim_leave(inst);
  swim_set_error(SWIM_OK, nullptr);
}

// A second suspect(X) at a higher incarnation used to arm a second suspicion
// timer without cancelling the first, so the older timer declared X dead
// before X had its full suspicion timeout.
TEST_CASE("protocol: re-suspicion restarts the suspicion timeout") {
  swim_nodeid_idx_t y_id = swim_nodeid_find("127.0.0.1:20742/y");
  swim_nodeid_idx_t x_id = swim_nodeid_find("127.0.0.1:20743/x");

  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.name = "resuspect";
  opts.protocol_period_ms = 10000; // keep the node's own probing out of the way
  opts.suspicion_timeout_ms = 1000;

  swim_t *inst = swim_start("127.0.0.1:20741/c1", &opts);
  REQUIRE(inst != nullptr);
  swim_udp_t *y = swim_udp_create("127.0.0.1", 20742);
  REQUIRE(y != nullptr);

  auto gossip = [&](swim_status_t status, uint64_t inc, uint32_t seq) {
    swim_gossip_queue_t *q = swim_gossip_queue_create();
    REQUIRE(q != nullptr);
    swim_gossip_queue_enqueue(q, status, x_id, inc, 1);
    uint8_t msg_buf[512];
    int msglen = swim_pack_message(SWIM_MSG_PING, y_id, seq, SWIM_NODEID_NONE,
                                   q, 1, msg_buf, sizeof(msg_buf));
    swim_gossip_queue_destroy(q);
    REQUIRE(msglen > 0);
    uint8_t buf[512 + 12];
    int len = swim_pack_authed("resuspect", msg_buf, msglen, buf, sizeof(buf));
    REQUIRE(swim_udp_send(y, "127.0.0.1", 20741, buf, len) == 0);
  };
  auto x_status = [&]() {
    swim_member_t members[4];
    int count = swim_members(inst, members, 4, true);
    for (int i = 0; i < count; i++)
      if (nodeid_eq(members[i].id, x_id))
        return members[i].status;
    return (swim_status_t)0;
  };

  gossip(SWIM_STATUS_ALIVE, 5, 1);
  gossip(SWIM_STATUS_SUSPECT, 5, 2); // first timer: due at ~1000 ms
  usleep(600000);
  gossip(SWIM_STATUS_SUSPECT, 6, 3); // restarts: due at ~1600 ms
  usleep(600000);
  CHECK(x_status() == SWIM_STATUS_SUSPECT); // ~1200 ms
  usleep(700000);
  CHECK(x_status() == SWIM_STATUS_DEAD); // ~1900 ms

  swim_udp_destroy(y);
  swim_leave(inst);
  swim_set_error(SWIM_OK, nullptr);
}

// Group names that share their first 16 bytes must still isolate clusters;
// the auth key used to be just those 16 bytes, so these two merged.
TEST_CASE("protocol: group names differing after 16 bytes stay isolated") {
  const char *seeds_a[] = {"127.0.0.1:20772/b", nullptr};
  swim_start_opts_t a;
  memset(&a, 0, sizeof(a));
  a.name = "prod-cluster-east-1";
  a.seeds = seeds_a;
  a.seed_retry_interval_ms = 200;

  const char *seeds_b[] = {"127.0.0.1:20771/a", nullptr};
  swim_start_opts_t b = a;
  b.name = "prod-cluster-east-2";
  b.seeds = seeds_b;

  swim_t *na = swim_start("127.0.0.1:20771/a", &a);
  swim_t *nb = swim_start("127.0.0.1:20772/b", &b);
  REQUIRE(na != nullptr);
  REQUIRE(nb != nullptr);

  usleep(800000); // several seed retries
  swim_member_t members[4];
  CHECK(swim_members(na, members, 4, true) == 0);
  CHECK(swim_members(nb, members, 4, true) == 0);

  swim_leave(na);
  swim_leave(nb);
  swim_set_error(SWIM_OK, nullptr);
}

// A LEAVE is not a sign of life: a leave from a node we never knew must not
// add it (it used to emit "node up" then "node down").
TEST_CASE("protocol: leave from an unknown node is ignored") {
  clear_obs();
  swim_nodeid_idx_t z_id = swim_nodeid_find("127.0.0.1:20782/z");

  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);
  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.name = "leave_unknown";
  opts.protocol_period_ms = 10000;
  opts.feed = feed;
  swim_t *inst = swim_start("127.0.0.1:20781/c1", &opts);
  REQUIRE(inst != nullptr);
  swim_udp_t *z = swim_udp_create("127.0.0.1", 20782);
  REQUIRE(z != nullptr);

  uint8_t msg_buf[256];
  int msglen = swim_pack_message(SWIM_MSG_LEAVE, z_id, 1, SWIM_NODEID_NONE,
                                 nullptr, 0, msg_buf, sizeof(msg_buf));
  REQUIRE(msglen > 0);
  uint8_t buf[256 + 12];
  int len =
      swim_pack_authed("leave_unknown", msg_buf, msglen, buf, sizeof(buf));
  REQUIRE(swim_udp_send(z, "127.0.0.1", 20781, buf, len) == 0);
  usleep(300000);

  swim_member_t members[4];
  CHECK(swim_members(inst, members, 4, true) == 0);
  poll_feed_events(feed);
  CHECK(!obs_contains("node up 127.0.0.1:20782/z"));

  swim_udp_destroy(z);
  swim_leave(inst);
  swim_feed_destroy(feed);
  swim_set_error(SWIM_OK, nullptr);
}

// Feed node events report status transitions; an incarnation change that
// keeps the status (alive -> alive, suspect -> suspect) is not one.
TEST_CASE("protocol: incarnation-only changes emit no node events") {
  clear_obs();
  swim_nodeid_idx_t y_id = swim_nodeid_find("127.0.0.1:20792/y");
  swim_nodeid_idx_t x_id = swim_nodeid_find("127.0.0.1:20793/x");

  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);
  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.name = "dup_events";
  opts.protocol_period_ms = 10000;
  opts.feed = feed;
  swim_t *inst = swim_start("127.0.0.1:20791/c1", &opts);
  REQUIRE(inst != nullptr);
  swim_udp_t *y = swim_udp_create("127.0.0.1", 20792);
  REQUIRE(y != nullptr);

  auto gossip = [&](swim_status_t status, uint64_t inc, uint32_t seq) {
    swim_gossip_queue_t *q = swim_gossip_queue_create();
    REQUIRE(q != nullptr);
    swim_gossip_queue_enqueue(q, status, x_id, inc, 1);
    uint8_t msg_buf[512];
    int msglen = swim_pack_message(SWIM_MSG_PING, y_id, seq, SWIM_NODEID_NONE,
                                   q, 1, msg_buf, sizeof(msg_buf));
    swim_gossip_queue_destroy(q);
    REQUIRE(msglen > 0);
    uint8_t buf[512 + 12];
    int len = swim_pack_authed("dup_events", msg_buf, msglen, buf, sizeof(buf));
    REQUIRE(swim_udp_send(y, "127.0.0.1", 20791, buf, len) == 0);
    usleep(100000);
  };

  gossip(SWIM_STATUS_ALIVE, 5, 1);
  gossip(SWIM_STATUS_ALIVE, 6, 2);
  gossip(SWIM_STATUS_SUSPECT, 6, 3);
  gossip(SWIM_STATUS_SUSPECT, 7, 4);
  poll_feed_events(feed);
  CHECK(obs_count("node up 127.0.0.1:20793/x") == 1);
  CHECK(obs_count("node suspect 127.0.0.1:20793/x") == 1);

  swim_udp_destroy(y);
  swim_leave(inst);
  swim_feed_destroy(feed);
  swim_set_error(SWIM_OK, nullptr);
}

// swim_pack_authed must refuse a message that cannot fit a packet, rather than
// overrun its internal hash buffer.
TEST_CASE("protocol: swim_pack_authed rejects oversized messages") {
  static uint8_t msg[2000];
  static uint8_t out[2100];
  const int max_msg = SWIM_MAX_PACKET_SIZE - 12;
  CHECK(swim_pack_authed("g", msg, max_msg, out, sizeof(out)) == max_msg + 12);
  CHECK(swim_pack_authed("g", msg, max_msg + 1, out, sizeof(out)) == -1);
  CHECK(swim_pack_authed("g", msg, 2000, out, sizeof(out)) == -1);
}

// A LEAVE must carry the leaver's real incarnation as dead(self, inc) gossip.
// Receivers used to guess "their view + 1"; one that knew the leaver only at
// incarnation 0 recorded dead@1, which peers' alive@real-inc then overrode.
TEST_CASE("protocol: leave carries dead(self) at the real incarnation") {
  const char *x_str = "127.0.0.1:20811/x";
  swim_nodeid_idx_t x_id = swim_nodeid_find(x_str);
  swim_nodeid_idx_t m_id = swim_nodeid_find("127.0.0.1:20812/m");

  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.name = "leave_inc";
  opts.protocol_period_ms = 10000;
  swim_t *inst = swim_start(x_str, &opts);
  REQUIRE(inst != nullptr);
  swim_udp_t *m = swim_udp_create("127.0.0.1", 20812);
  REQUIRE(m != nullptr);

  // M joins; X's ack carries its startup alive(self, inc).
  uint8_t msg_buf[256];
  int msglen = swim_pack_message(SWIM_MSG_PING, m_id, 1, SWIM_NODEID_NONE,
                                 nullptr, 0, msg_buf, sizeof(msg_buf));
  REQUIRE(msglen > 0);
  uint8_t buf[256 + 12];
  int len = swim_pack_authed("leave_inc", msg_buf, msglen, buf, sizeof(buf));
  REQUIRE(swim_udp_send(m, "127.0.0.1", 20811, buf, len) == 0);
  uint64_t alive_inc = recv_alive_inc(m, x_str);
  REQUIRE(alive_inc > 0);

  swim_leave(inst);

  uint64_t dead_inc = 0;
  for (int attempt = 0; attempt < 100 && !dead_inc; attempt++) {
    uint8_t rbuf[SWIM_MAX_PACKET_SIZE];
    int n = swim_udp_recv(m, rbuf, sizeof(rbuf));
    if (n <= 12) {
      usleep(10000);
      continue;
    }
    swim_message_t in;
    if (swim_unpack_message(rbuf + 12, n - 12, &in) != 0 ||
        in.type != SWIM_MSG_LEAVE)
      continue;
    for (int i = 0; i < in.gossip_count; i++)
      if (nodeid_eq(in.gossip[i].id, x_id) &&
          in.gossip[i].status == SWIM_STATUS_DEAD)
        dead_inc = in.gossip[i].incarnation;
    break;
  }
  CHECK(dead_inc > alive_inc);

  swim_udp_destroy(m);
  swim_set_error(SWIM_OK, nullptr);
}

// Join sync. A peer whose "known" cluster size is below ours gets a SYNC: a
// random sample of k = clamp(ceil(20% of the gap), 16, 32) of our members
// (plus ourselves, never the peer itself), at most once per protocol period.
namespace {
struct SyncHarness {
  const char *name;
  uint16_t x_port, m_port;
  swim_t *x = nullptr;
  swim_udp_t *m = nullptr;
  swim_nodeid_idx_t x_id, m_id;

  SyncHarness(const char *name_, uint16_t xp, uint16_t mp, uint64_t period_ms)
      : name(name_), x_port(xp), m_port(mp) {
    char s[64];
    snprintf(s, sizeof(s), "127.0.0.1:%u/x", xp);
    x_id = swim_nodeid_find(s);
    swim_start_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.name = name;
    opts.protocol_period_ms = period_ms;
    x = swim_start(s, &opts);
    snprintf(s, sizeof(s), "127.0.0.1:%u/m", mp);
    m_id = swim_nodeid_find(s);
    m = swim_udp_create("127.0.0.1", mp);
  }
  ~SyncHarness() {
    swim_udp_destroy(m);
    swim_leave(x);
    swim_set_error(SWIM_OK, nullptr);
  }
  // Send from M with the given "known" (active_members = known - 1).
  void send(uint8_t type, uint32_t seq, uint32_t known,
            swim_gossip_queue_t *q = nullptr) {
    uint8_t msg_buf[SWIM_MAX_PACKET_SIZE];
    int msglen = swim_pack_message(type, m_id, seq, SWIM_NODEID_NONE, q,
                                   known - 1, msg_buf, sizeof(msg_buf) - 12);
    REQUIRE(msglen > 0);
    uint8_t buf[SWIM_MAX_PACKET_SIZE];
    int len = swim_pack_authed(name, msg_buf, msglen, buf, sizeof(buf));
    REQUIRE(swim_udp_send(m, "127.0.0.1", x_port, buf, len) == 0);
  }
  // Wait up to timeout_ms for a message of `type` on M's socket.
  bool recv(uint8_t type, int timeout_ms, swim_message_t *in) {
    for (int waited = 0; waited < timeout_ms; waited += 10) {
      uint8_t buf[SWIM_MAX_PACKET_SIZE];
      int n;
      while ((n = swim_udp_recv(m, buf, sizeof(buf))) > 12) {
        if (swim_unpack_message(buf + 12, n - 12, in) == 0 && in->type == type)
          return true;
      }
      usleep(10000);
    }
    return false;
  }
  // Teach X about `count` extra nodes via alive gossip from M.
  void add_members(int count, uint16_t base_port) {
    swim_gossip_queue_t *q = swim_gossip_queue_create();
    for (int i = 0; i < count; i++) {
      char s[64];
      snprintf(s, sizeof(s), "127.0.0.1:%u/f", base_port + i);
      swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, swim_nodeid_find(s), 1,
                                1);
    }
    // M claims a full view, so this ping triggers no sync.
    send(SWIM_MSG_PING, 1, (uint32_t)count + 2, q);
    swim_gossip_queue_destroy(q);
    swim_message_t in;
    REQUIRE(recv(SWIM_MSG_ACK, 1000, &in));
  }
};
} // namespace

TEST_CASE("protocol: a peer that is behind receives a sync sample") {
  SyncHarness h("sync_sample", 20821, 20822, 2000);
  REQUIRE(h.x != nullptr);
  REQUIRE(h.m != nullptr);
  h.add_members(20, 20830); // X now sees 20 fakes + M: known = 22

  swim_message_t in;
  // Not behind: no sync.
  h.send(SWIM_MSG_PING, 2, 22);
  REQUIRE(h.recv(SWIM_MSG_ACK, 1000, &in));
  CHECK(!h.recv(SWIM_MSG_SYNC, 300, &in));

  // Behind by 21: k = clamp(ceil(4.2), 16, 32) = 16, drawn from 20 fakes + X.
  h.send(SWIM_MSG_PING, 3, 1);
  REQUIRE(h.recv(SWIM_MSG_SYNC, 1000, &in));
  CHECK(in.gossip_count == 16);
  CHECK(nodeid_eq(in.sender, h.x_id));
  for (int i = 0; i < in.gossip_count; i++) {
    CHECK(!nodeid_eq(in.gossip[i].id, h.m_id));
    CHECK(in.gossip[i].status == SWIM_STATUS_ALIVE);
  }

  // Rate limit: at most one sync per peer per protocol period.
  h.send(SWIM_MSG_PING, 4, 1);
  CHECK(!h.recv(SWIM_MSG_SYNC, 300, &in));
  usleep(2000000);
  h.send(SWIM_MSG_PING, 5, 1);
  CHECK(h.recv(SWIM_MSG_SYNC, 1000, &in));
}

TEST_CASE("protocol: sync records are applied but not re-gossiped") {
  SyncHarness h("sync_apply", 20841, 20842, 10000);
  REQUIRE(h.x != nullptr);
  REQUIRE(h.m != nullptr);

  // Drain X's startup self-announcement so later acks carry only new gossip.
  swim_message_t in;
  for (uint32_t seq = 1; seq <= 4; seq++) {
    h.send(SWIM_MSG_PING, seq, 2);
    REQUIRE(h.recv(SWIM_MSG_ACK, 1000, &in));
  }

  const char *z_str = "127.0.0.1:20849/z";
  swim_nodeid_idx_t z_id = swim_nodeid_find(z_str);
  swim_gossip_queue_t *q = swim_gossip_queue_create();
  swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, z_id, 7, 1);
  h.send(SWIM_MSG_SYNC, 5, 3, q);
  swim_gossip_queue_destroy(q);
  usleep(200000);

  swim_member_t members[8];
  int count = swim_members(h.x, members, 8, false);
  bool has_z = false;
  for (int i = 0; i < count; i++)
    has_z |= nodeid_eq(members[i].id, z_id);
  CHECK(has_z);

  h.send(SWIM_MSG_PING, 6, 3);
  REQUIRE(h.recv(SWIM_MSG_ACK, 1000, &in));
  for (int i = 0; i < in.gossip_count; i++)
    CHECK(!nodeid_eq(in.gossip[i].id, z_id));
}

// A sample too large for one packet must still be random. Packing through the
// gossip queue sorts records by node ID, so the overflow was always the
// alphabetically last members, and the last one never reached a joiner.
TEST_CASE("protocol: a sync sample that overflows a packet stays random") {
  SyncHarness h("sync_overflow", 20881, 20882, 200);
  REQUIRE(h.x != nullptr);
  REQUIRE(h.m != nullptr);

  // 16 fakes with 122-byte records (long IPv6 address, 63-byte cookie): only
  // about 11 fit in a packet, but the sample is k = 16 of 17 (16 fakes + X).
  // The last address group orders the IDs; the highest sorts last.
  std::string cookie(63, 'c');
  swim_nodeid_idx_t fakes[16];
  for (int i = 0; i < 16; i++) {
    char s[160];
    snprintf(s, sizeof(s), "[fd00:1111:2222:3333:4444:5555:6666:%x]:7000/%s",
             0xa000 + i, cookie.c_str());
    fakes[i] = swim_nodeid_find(s);
    REQUIRE(nodeid_valid(fakes[i]));
  }
  // Announce the fakes alive at a fresh incarnation, so X's failed probes of
  // them never get as far as declaring them dead.
  uint32_t seq = 1;
  auto announce = [&](uint64_t inc) {
    swim_message_t in;
    for (int batch = 0; batch < 2; batch++) {
      swim_gossip_queue_t *q = swim_gossip_queue_create();
      for (int i = 0; i < 8; i++)
        swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, fakes[batch * 8 + i],
                                  inc, 1);
      h.send(SWIM_MSG_PING, seq++, 18, q); // full view: no sync
      swim_gossip_queue_destroy(q);
      REQUIRE(h.recv(SWIM_MSG_ACK, 1000, &in));
    }
  };

  // Each sync carries the last fake with probability about 11/17.
  bool seen_last = false;
  for (uint64_t inc = 1; inc <= 10 && !seen_last; inc++) {
    usleep(220000); // past the once-per-period rate limit
    announce(inc);
    h.send(SWIM_MSG_PING, seq++, 1);
    swim_message_t in;
    REQUIRE(h.recv(SWIM_MSG_SYNC, 1000, &in));
    CHECK(in.gossip_count >= 10);
    for (int i = 0; i < in.gossip_count; i++)
      seen_last |= nodeid_eq(in.gossip[i].id, fakes[15]);
  }
  CHECK(seen_last);
}

// A probe still resolving when the period ends used to defer the next probe
// by a whole period. It should start as soon as the pending probe resolves.
TEST_CASE("protocol: a pending probe delays the next by a tick, not a period") {
  swim_nodeid_idx_t m_id = swim_nodeid_find("127.0.0.1:20862/m");
  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.name = "probe_retry";
  opts.protocol_period_ms = 1000;
  opts.ping_timeout_ms = 600; // direct + indirect = 1.2 s > one period
  swim_t *inst = swim_start("127.0.0.1:20861/x", &opts);
  REQUIRE(inst != nullptr);
  swim_udp_t *m = swim_udp_create("127.0.0.1", 20862);
  REQUIRE(m != nullptr);

  // M joins, then never answers: each probe of M runs its full 1.2 s.
  uint8_t msg_buf[256];
  int msglen = swim_pack_message(SWIM_MSG_PING, m_id, 1, SWIM_NODEID_NONE,
                                 nullptr, 0, msg_buf, sizeof(msg_buf));
  uint8_t buf[256 + 12];
  int len = swim_pack_authed("probe_retry", msg_buf, msglen, buf, sizeof(buf));
  REQUIRE(swim_udp_send(m, "127.0.0.1", 20861, buf, len) == 0);

  // Record when the first two probes arrive.
  double t[2];
  int seen = 0;
  auto t0 = std::chrono::steady_clock::now();
  while (seen < 2 &&
         std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
    uint8_t rbuf[SWIM_MAX_PACKET_SIZE];
    int n = swim_udp_recv(m, rbuf, sizeof(rbuf));
    swim_message_t in;
    if (n > 12 && swim_unpack_message(rbuf + 12, n - 12, &in) == 0 &&
        in.type == SWIM_MSG_PING)
      t[seen++] =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
              .count();
    else
      usleep(5000);
  }
  REQUIRE(seen == 2);
  INFO("gap " << t[1] - t[0] << " s");
  CHECK(t[1] - t[0] < 1.6); // ~1.2 s; a skipped period made it ~2.0 s

  swim_udp_destroy(m);
  swim_leave(inst);
  swim_set_error(SWIM_OK, nullptr);
}

// A timeout must never be shorter than configured. 149 ms used to round to
// one 100 ms tick, so a peer answering in 130 ms missed the direct window
// (no rtt reported, needless indirect probing).
TEST_CASE("protocol: ping timeout is never shorter than configured") {
  clear_obs();
  swim_nodeid_idx_t m_id = swim_nodeid_find("127.0.0.1:20872/m");
  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);
  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.name = "timeout_ceil";
  opts.protocol_period_ms = 1000;
  opts.ping_timeout_ms = 149;
  opts.feed = feed;
  swim_t *inst = swim_start("127.0.0.1:20871/x", &opts);
  REQUIRE(inst != nullptr);
  swim_udp_t *m = swim_udp_create("127.0.0.1", 20872);
  REQUIRE(m != nullptr);

  auto send = [&](uint8_t type, uint32_t seq) {
    uint8_t msg_buf[256];
    int msglen = swim_pack_message(type, m_id, seq, SWIM_NODEID_NONE, nullptr,
                                   0, msg_buf, sizeof(msg_buf));
    uint8_t buf[256 + 12];
    int len =
        swim_pack_authed("timeout_ceil", msg_buf, msglen, buf, sizeof(buf));
    REQUIRE(swim_udp_send(m, "127.0.0.1", 20871, buf, len) == 0);
  };
  send(SWIM_MSG_PING, 1); // join

  // Answer X's first probe after 130 ms.
  bool answered = false;
  for (int i = 0; i < 300 && !answered; i++) {
    uint8_t rbuf[SWIM_MAX_PACKET_SIZE];
    int n = swim_udp_recv(m, rbuf, sizeof(rbuf));
    swim_message_t in;
    if (n > 12 && swim_unpack_message(rbuf + 12, n - 12, &in) == 0 &&
        in.type == SWIM_MSG_PING) {
      usleep(130000);
      send(SWIM_MSG_ACK, in.seq);
      answered = true;
    } else {
      usleep(10000);
    }
  }
  REQUIRE(answered);
  usleep(200000);
  poll_feed_events(feed);
  CHECK(obs_contains("ping rtt 127.0.0.1:20872/m"));

  swim_udp_destroy(m);
  swim_leave(inst);
  swim_feed_destroy(feed);
  swim_set_error(SWIM_OK, nullptr);
}
