#include "doctest.h"

extern "C" {
#include "swim.h"
#include "swim_codec.h"
#include "swim_errno.h"
#include "swim_gossip_queue.h"
#include "swim_internal.h"
#include "swim_udp.h"
}

#include <atomic>
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
  opts.self = "127.0.0.1:20001";
  opts.name = "single_node";

  swim_t *inst = swim_start(&opts);
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
  opts1.self = "127.0.0.1:20101/c1";
  opts1.name = "multi_test";
  opts1.seeds = seeds1;
  opts1.protocol_period_ms = 400;
  opts1.ping_timeout_ms = 100;
  opts1.seed_retry_interval_ms = 400;

  // Node 2 setup (seed: Node 3)
  const char *seeds2[] = {"127.0.0.1:20103/c3", nullptr};
  swim_start_opts_t opts2;
  memset(&opts2, 0, sizeof(opts2));
  opts2.self = "127.0.0.1:20102/c2";
  opts2.name = "multi_test";
  opts2.seeds = seeds2;
  opts2.protocol_period_ms = 400;
  opts2.ping_timeout_ms = 100;
  opts2.seed_retry_interval_ms = 400;

  // Node 3 setup (seed: Node 1)
  const char *seeds3[] = {"127.0.0.1:20101/c1", nullptr};
  swim_start_opts_t opts3;
  memset(&opts3, 0, sizeof(opts3));
  opts3.self = "127.0.0.1:20103/c3";
  opts3.name = "multi_test";
  opts3.seeds = seeds3;
  opts3.protocol_period_ms = 400;
  opts3.ping_timeout_ms = 100;
  opts3.seed_retry_interval_ms = 400;

  swim_t *n1 = swim_start(&opts1);
  swim_t *n2 = swim_start(&opts2);
  swim_t *n3 = swim_start(&opts3);
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

  swim_node_id_t self_id;
  REQUIRE(swim_node_id_parse(&self_id, "127.0.0.1:20201/c1") == 0);

  swim_node_id_t mock_id;
  REQUIRE(swim_node_id_parse(&mock_id, "127.0.0.1:20202/mock") == 0);

  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);

  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.self = "127.0.0.1:20201/c1";
  opts.name = "failure_node";
  opts.protocol_period_ms = 400;
  opts.ping_timeout_ms = 100;
  opts.suspicion_timeout_ms = 600;
  opts.feed = feed;

  swim_t *inst = swim_start(&opts);
  REQUIRE(inst != nullptr);

  // Simulate a join from a mock node by sending a raw PING packet to opts.port
  swim_udp_t *mock_udp = swim_udp_create("127.0.0.1", 20202);
  REQUIRE(mock_udp != nullptr);

  // Construct authenticated PING packet from mock
  uint8_t msg_buf[256];
  int msglen = swim_pack_message(SWIM_MSG_PING, &mock_id, 1, nullptr, nullptr,
                                 0, msg_buf, sizeof(msg_buf));
  REQUIRE(msglen > 0);
  uint8_t buf[256 + 12];
  int len = swim_pack_authed("failure_node", msg_buf, msglen, buf, sizeof(buf));
  REQUIRE(len == msglen + 12);

  // Send packet to node
  int rc = swim_udp_send(mock_udp, &self_id, buf, len);
  REQUIRE(rc == 0);

  // Wait a moment for packet processing
  usleep(150000);

  // Verify Node 1 discovered mock node
  swim_member_t members[5];
  int count = swim_members(inst, members, 5, false);
  REQUIRE(count == 1);
  CHECK(swim_node_id_compare(&members[0].id, &mock_id) == 0);
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
  swim_node_id_t node_id, requester_id, target_id;
  REQUIRE(swim_node_id_parse(&node_id, "127.0.0.1:20301/c1") == 0);
  REQUIRE(swim_node_id_parse(&requester_id, "127.0.0.1:20302/r") == 0);
  REQUIRE(swim_node_id_parse(&target_id, "127.0.0.1:20303/t") == 0);

  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.self = "127.0.0.1:20301/c1";
  opts.name = "relay_node";
  opts.protocol_period_ms = 1000; // keep the node's own probing out of the way
  opts.ping_timeout_ms = 100;     // relay entries expire after ~1 tick

  swim_t *inst = swim_start(&opts);
  REQUIRE(inst != nullptr);

  swim_udp_t *requester = swim_udp_create("127.0.0.1", 20302);
  REQUIRE(requester != nullptr);
  swim_udp_t *target = swim_udp_create("127.0.0.1", 20303);
  REQUIRE(target != nullptr);

  auto send_ping_req = [&](const swim_node_id_t &tgt, uint32_t seq) {
    uint8_t msg_buf[256];
    int msglen = swim_pack_message(SWIM_MSG_PING_REQ, &requester_id, seq, &tgt,
                                   nullptr, 0, msg_buf, sizeof(msg_buf));
    REQUIRE(msglen > 0);
    uint8_t buf[256 + 12];
    int len = swim_pack_authed("relay_node", msg_buf, msglen, buf, sizeof(buf));
    REQUIRE(len == msglen + 12);
    REQUIRE(swim_udp_send(requester, &node_id, buf, len) == 0);
  };

  // Flood the relay table (capacity 32) with ping_reqs for dead targets that
  // will never ack.
  for (uint32_t i = 0; i < 32; i++) {
    swim_node_id_t dead;
    char s[64];
    snprintf(s, sizeof(s), "127.0.0.1:%u/d", 21000 + i);
    REQUIRE(swim_node_id_parse(&dead, s) == 0);
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
    swim_node_id_t src;
    uint8_t buf[256 + 12];
    int n = swim_udp_recv(target, &src, buf, sizeof(buf));
    if (n > 12) {
      swim_message_t in;
      if (swim_unpack_message(buf + 12, n - 12, &in) == 0 &&
          in.type == SWIM_MSG_PING && in.seq == 9999 &&
          swim_node_id_compare(&in.sender, &node_id) == 0) {
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
  opts.self = "127.0.0.1:20403/c1";
  opts.name = "h3_race";
  opts.protocol_period_ms = 100;
  opts.ping_timeout_ms = 50;

  swim_t *inst = swim_start(&opts);
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

// Regression for M2: concurrent swim_leave on the same instance must be
// memory-safe. An atomic CAS on inst->leaving ensures exactly one caller
// tears the instance down (returns 0) and the rest get BAD_STATE without
// double-joining or double-freeing. Meaningful under ASan/TSan.
struct LeaveArg {
  swim_t *inst;
  std::atomic<int> *successes;
};
static void *leave_racer(void *a) {
  LeaveArg *r = (LeaveArg *)a;
  if (swim_leave(r->inst) == 0) {
    r->successes->fetch_add(1);
  }
  return nullptr;
}

TEST_CASE("protocol: concurrent swim_leave is memory-safe (M2)") {
  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.self = "127.0.0.1:20402/c1";
  opts.name = "m2_race";

  swim_t *inst = swim_start(&opts);
  REQUIRE(inst != nullptr);

  std::atomic<int> successes{0};
  LeaveArg arg;
  arg.inst = inst;
  arg.successes = &successes;

  pthread_t th[4];
  for (int i = 0; i < 4; i++) {
    REQUIRE(pthread_create(&th[i], nullptr, leave_racer, &arg) == 0);
  }
  for (int i = 0; i < 4; i++) {
    pthread_join(th[i], nullptr);
  }

  // Exactly one thread tore the instance down; the others saw BAD_STATE.
  CHECK(successes.load() == 1);
  swim_set_error(SWIM_OK, nullptr);
}

TEST_CASE("protocol: gossip byte budget does not exceed MTU (M1)") {
  swim_node_id_t self_id, mock_id;
  REQUIRE(swim_node_id_parse(&self_id, "127.0.0.1:20501/c1") == 0);
  REQUIRE(swim_node_id_parse(&mock_id, "127.0.0.1:20502/mock") == 0);

  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.self = "127.0.0.1:20501/c1";
  opts.name = "m1_budget";
  opts.protocol_period_ms = 10000; // slow
  opts.ping_timeout_ms = 1000;

  swim_t *inst = swim_start(&opts);
  REQUIRE(inst != nullptr);

  swim_udp_t *mock_udp = swim_udp_create("127.0.0.1", 20502);
  REQUIRE(mock_udp != nullptr);

  // Send 5 PINGs, each containing 6 large gossip events, to populate
  // the node's gossip queue with 30 large events.
  for (uint32_t p = 0; p < 5; p++) {
    swim_gossip_queue_t *test_q = swim_gossip_queue_create();
    for (int i = 0; i < 6; i++) {
      int idx = p * 6 + i;
      swim_node_id_t ev_id;
      char host[256];
      sprintf(host,
              "node-with-a-very-long-name-to-make-it-large-and-fill-up-the-"
              "packet-budget-limit-domain-%d.com",
              idx);
      strcpy(ev_id.host, host);
      ev_id.port = 30000 + idx;
      sprintf(ev_id.cookie, "cookie-cookie-cookie-cookie-%d", idx);
      swim_gossip_queue_enqueue(test_q, SWIM_STATUS_ALIVE, &ev_id, 100 + idx,
                                1);
    }

    uint8_t msg_buf[1024];
    int msglen = swim_pack_message(SWIM_MSG_PING, &mock_id, 100 + p, nullptr,
                                   test_q, 1, msg_buf, sizeof(msg_buf));
    swim_gossip_queue_destroy(test_q);
    REQUIRE(msglen > 0);
    uint8_t send_buf[1024 + 12];
    int len = swim_pack_authed("m1_budget", msg_buf, msglen, send_buf,
                               sizeof(send_buf));
    REQUIRE(len == msglen + 12);
    REQUIRE(swim_udp_send(mock_udp, &self_id, send_buf, len) == 0);

    // Wait and drain the reply ACK for this PING so they don't pile up.
    bool ack_seen = false;
    uint8_t recv_buf[2048];
    for (int attempt = 0; attempt < 50 && !ack_seen; attempt++) {
      swim_node_id_t src;
      int n = swim_udp_recv(mock_udp, &src, recv_buf, sizeof(recv_buf));
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

  swim_node_id_t sender;
  REQUIRE(swim_node_id_parse(&sender, "127.0.0.1:8001/sender_cookie") == 0);
  swim_node_id_t peer;
  REQUIRE(swim_node_id_parse(&peer, "127.0.0.1:8002/peer_cookie") == 0);

  // Enqueue a gossip event
  swim_node_id_t gossip_node;
  REQUIRE(swim_node_id_parse(&gossip_node, "127.0.0.1:9001/gossip_cookie") ==
          0);
  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, &gossip_node, 100,
                                    1) == 0);

  uint8_t buf[2048];
  int bytes = swim_pack_message(SWIM_MSG_PING_REQ, &sender, 12345, &peer, q,
                                swim_membership_count(m), buf, sizeof(buf));
  REQUIRE(bytes > 0);

  swim_message_t msg;
  memset(&msg, 0, sizeof(msg));
  int rc = swim_unpack_message(buf, bytes, &msg);
  REQUIRE(rc == 0);

  CHECK(msg.type == SWIM_MSG_PING_REQ);
  CHECK(msg.seq == 12345);
  CHECK(swim_node_id_compare(&msg.sender, &sender) == 0);
  CHECK(swim_node_id_compare(&msg.peer, &peer) == 0);
  CHECK(msg.gossip_count == 1);
  CHECK(msg.gossip[0].status == SWIM_STATUS_ALIVE);
  CHECK(msg.gossip[0].incarnation == 100);
  CHECK(swim_node_id_compare(&msg.gossip[0].id, &gossip_node) == 0);

  // Check error handling with buffer too small
  int err_bytes = swim_pack_message(SWIM_MSG_PING_REQ, &sender, 12345, &peer, q,
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

  swim_node_id_t sender;
  REQUIRE(swim_node_id_parse(&sender, "127.0.0.1:8001/sender_cookie") == 0);

  // Enqueue a gossip event (should be ignored for LEAVE)
  swim_node_id_t gossip_node;
  REQUIRE(swim_node_id_parse(&gossip_node, "127.0.0.1:9001/gossip_cookie") ==
          0);
  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, &gossip_node, 100,
                                    1) == 0);

  uint8_t buf[2048];
  // Pass NULL gossip queue
  int bytes = swim_pack_message(SWIM_MSG_LEAVE, &sender, 12345, nullptr,
                                nullptr, 0, buf, sizeof(buf));
  REQUIRE(bytes > 0);

  swim_message_t msg;
  memset(&msg, 0, sizeof(msg));
  int rc = swim_unpack_message(buf, bytes, &msg);
  REQUIRE(rc == 0);

  CHECK(msg.type == SWIM_MSG_LEAVE);
  CHECK(msg.seq == 12345);
  CHECK(swim_node_id_compare(&msg.sender, &sender) == 0);
  CHECK(msg.gossip_count == 0);

  swim_membership_destroy(m);
  swim_gossip_queue_destroy(q);
}

// L3: the observer receives membership transitions and the cluster-size gauge.
TEST_CASE("protocol: observer telemetry — transitions, cluster size, escaping "
          "(L3)") {
  clear_obs();

  swim_node_id_t self_id;
  REQUIRE(swim_node_id_parse(&self_id, "127.0.0.1:20501/c1") == 0);

  // Mock peer with a cookie containing non-alphanumeric bytes (space, '!').
  swim_node_id_t mock_id;
  REQUIRE(swim_node_id_parse(&mock_id, "127.0.0.1:20502") == 0);
  strcpy(mock_id.cookie, "a b!");

  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);

  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.self = "127.0.0.1:20501/c1";
  opts.name = "obs_node";
  opts.protocol_period_ms = 200;
  opts.feed = feed;

  swim_t *inst = swim_start(&opts);
  REQUIRE(inst != nullptr);

  swim_udp_t *mock_udp = swim_udp_create("127.0.0.1", 20502);
  REQUIRE(mock_udp != nullptr);

  uint8_t msg_buf[256];
  int msglen = swim_pack_message(SWIM_MSG_PING, &mock_id, 1, nullptr, nullptr,
                                 0, msg_buf, sizeof(msg_buf));
  REQUIRE(msglen > 0);
  uint8_t buf[256 + 12];
  int len = swim_pack_authed("obs_node", msg_buf, msglen, buf, sizeof(buf));
  REQUIRE(len == msglen + 12);
  REQUIRE(swim_udp_send(mock_udp, &self_id, buf, len) == 0);

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
  a.self = "127.0.0.1:20511/a1";
  a.name = "rtt_test";
  a.seeds = seedsA;
  a.protocol_period_ms = 200;
  a.ping_timeout_ms = 100;
  a.seed_retry_interval_ms = 200;
  a.feed = feed_a;

  const char *seedsB[] = {"127.0.0.1:20511/a1", nullptr};
  swim_start_opts_t b;
  memset(&b, 0, sizeof(b));
  b.self = "127.0.0.1:20512/b1";
  b.name = "rtt_test";
  b.seeds = seedsB;
  b.protocol_period_ms = 200;
  b.ping_timeout_ms = 100;
  b.seed_retry_interval_ms = 200;

  swim_t *na = swim_start(&a);
  swim_t *nb = swim_start(&b);
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

  swim_node_id_t self_id, mock_id;
  REQUIRE(swim_node_id_parse(&self_id, "127.0.0.1:20601/c1") == 0);
  REQUIRE(swim_node_id_parse(&mock_id, "127.0.0.1:20602/mock") == 0);

  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);

  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.self = "127.0.0.1:20601/c1";
  opts.name = "feed_test";
  opts.protocol_period_ms = 200;
  opts.feed = feed;

  swim_t *inst = swim_start(&opts);
  REQUIRE(inst != nullptr);

  swim_udp_t *mock_udp = swim_udp_create("127.0.0.1", 20602);
  REQUIRE(mock_udp != nullptr);

  uint8_t msg_buf[256];
  int msglen = swim_pack_message(SWIM_MSG_PING, &mock_id, 1, nullptr, nullptr,
                                 0, msg_buf, sizeof(msg_buf));
  REQUIRE(msglen > 0);
  uint8_t auth_buf[256 + 12];
  int authlen = swim_pack_authed("feed_test", msg_buf, msglen, auth_buf,
                                 sizeof(auth_buf));
  REQUIRE(authlen == msglen + 12);
  REQUIRE(swim_udp_send(mock_udp, &self_id, auth_buf, authlen) == 0);

  usleep(300000);

  poll_feed_events(feed);
  CHECK(obs_contains("node up 127.0.0.1:20602/mock"));

  swim_udp_destroy(mock_udp);
  swim_leave(inst);
  swim_feed_destroy(feed);
  swim_set_error(SWIM_OK, nullptr);
}
