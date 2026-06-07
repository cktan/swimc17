#include "doctest.h"

extern "C" {
#include "swim_protocol.h"
#include "swim_errno.h"
#include "swim_node_id.h"
#include "swim_codec.h"
#include "swim_udp.h"
}

#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <vector>
#include <pthread.h>
#include <atomic>

namespace {

struct EventLog {
  swim_event_t event;
  swim_node_id_t node;
};

std::vector<EventLog> g_events;
pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

void test_callback(void *ctx, swim_event_t event, const swim_node_id_t *node) {
  (void)ctx;
  pthread_mutex_lock(&g_log_mutex);
  g_events.push_back({event, *node});
  pthread_mutex_unlock(&g_log_mutex);
}

void clear_log() {
  pthread_mutex_lock(&g_log_mutex);
  g_events.clear();
  pthread_mutex_unlock(&g_log_mutex);
}

size_t get_log_size() {
  pthread_mutex_lock(&g_log_mutex);
  size_t sz = g_events.size();
  pthread_mutex_unlock(&g_log_mutex);
  return sz;
}

EventLog get_log_event(size_t idx) {
  pthread_mutex_lock(&g_log_mutex);
  EventLog ev = g_events[idx];
  pthread_mutex_unlock(&g_log_mutex);
  return ev;
}

} // namespace

TEST_CASE("protocol: single node startup and leave") {
  clear_log();
  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.host = "127.0.0.1";
  opts.port = 20001;
  opts.name = "single_node";

  int rc = swim_start(&opts);
  REQUIRE(rc == 0);

  // Subscribe
  rc = swim_subscribe("single_node", test_callback, nullptr);
  CHECK(rc == 0);

  // Query members (should be empty because self is not in the list)
  swim_member_t members[5];
  int count = swim_members("single_node", members, 5, true);
  CHECK(count == 0);

  // Unsubscribe
  rc = swim_unsubscribe("single_node", test_callback, nullptr);
  CHECK(rc == 0);

  rc = swim_leave("single_node");
  CHECK(rc == 0);
  swim_set_error(SWIM_OK, nullptr);
}

TEST_CASE("protocol: multi-node auto-discovery") {
  clear_log();

  // Define 3 nodes
  swim_node_id_t id1, id2, id3;
  REQUIRE(swim_node_id_parse(&id1, "127.0.0.1:20101:c1") == 0);
  REQUIRE(swim_node_id_parse(&id2, "127.0.0.1:20102:c2") == 0);
  REQUIRE(swim_node_id_parse(&id3, "127.0.0.1:20103:c3") == 0);

  // Node 1 setup (seed: Node 2)
  swim_start_opts_t opts1;
  memset(&opts1, 0, sizeof(opts1));
  opts1.host = "127.0.0.1";
  opts1.port = 20101;
  opts1.cookie = "c1";
  opts1.name = "n1";
  opts1.seed_list = &id2;
  opts1.seed_count = 1;
  opts1.protocol_period_ms = 400;
  opts1.ping_timeout_ms = 100;
  opts1.seed_retry_interval_ms = 400;

  // Node 2 setup (seed: Node 3)
  swim_start_opts_t opts2;
  memset(&opts2, 0, sizeof(opts2));
  opts2.host = "127.0.0.1";
  opts2.port = 20102;
  opts2.cookie = "c2";
  opts2.name = "n2";
  opts2.seed_list = &id3;
  opts2.seed_count = 1;
  opts2.protocol_period_ms = 400;
  opts2.ping_timeout_ms = 100;
  opts2.seed_retry_interval_ms = 400;

  // Node 3 setup (seed: Node 1)
  swim_start_opts_t opts3;
  memset(&opts3, 0, sizeof(opts3));
  opts3.host = "127.0.0.1";
  opts3.port = 20103;
  opts3.cookie = "c3";
  opts3.name = "n3";
  opts3.seed_list = &id1;
  opts3.seed_count = 1;
  opts3.protocol_period_ms = 400;
  opts3.ping_timeout_ms = 100;
  opts3.seed_retry_interval_ms = 400;

  REQUIRE(swim_start(&opts1) == 0);
  REQUIRE(swim_start(&opts2) == 0);
  REQUIRE(swim_start(&opts3) == 0);

  // Wait for periodic probes and seed discovery to execute (approx 1.5 seconds)
  usleep(1500000);

  // Check Node 1 membership
  swim_member_t members1[5];
  int count1 = swim_members("n1", members1, 5, false);
  CHECK(count1 >= 2); // should have discovered Node 2 and Node 3

  // Check Node 2 membership
  swim_member_t members2[5];
  int count2 = swim_members("n2", members2, 5, false);
  CHECK(count2 >= 2);

  // Check Node 3 membership
  swim_member_t members3[5];
  int count3 = swim_members("n3", members3, 5, false);
  CHECK(count3 >= 2);

  // Tear down all
  CHECK(swim_leave("n1") == 0);
  CHECK(swim_leave("n2") == 0);
  CHECK(swim_leave("n3") == 0);
  swim_set_error(SWIM_OK, nullptr);
}

TEST_CASE("protocol: failure detection and liveness hint") {
  clear_log();

  swim_node_id_t self_id;
  REQUIRE(swim_node_id_parse(&self_id, "127.0.0.1:20201:c1") == 0);

  swim_node_id_t mock_id;
  REQUIRE(swim_node_id_parse(&mock_id, "127.0.0.1:20202:mock") == 0);

  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.host = "127.0.0.1";
  opts.port = 20201;
  opts.cookie = "c1";
  opts.name = "failure_node";
  opts.protocol_period_ms = 400;
  opts.ping_timeout_ms = 100;
  opts.suspicion_timeout_ms = 600;

  REQUIRE(swim_start(&opts) == 0);
  REQUIRE(swim_subscribe("failure_node", test_callback, nullptr) == 0);

  // Simulate a join from a mock node by sending a raw PING packet to opts.port
  swim_udp_t *mock_udp = swim_udp_init("127.0.0.1", 20202);
  REQUIRE(mock_udp != nullptr);

  // Construct PING packet from mock
  swim_message_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.type = SWIM_MSG_PING;
  msg.sender = mock_id;
  msg.seq = 1;
  msg.event_count = 0;

  uint8_t buf[256];
  int len = swim_codec_encode(&msg, buf, sizeof(buf));
  REQUIRE(len > 0);

  // Send packet to node
  int rc = swim_udp_send(mock_udp, &self_id, buf, len);
  REQUIRE(rc == 0);

  // Wait a moment for packet processing
  usleep(150000);

  // Verify Node 1 discovered mock node
  swim_member_t members[5];
  int count = swim_members("failure_node", members, 5, false);
  REQUIRE(count == 1);
  CHECK(swim_node_id_compare(&members[0].id, &mock_id) == 0);
  CHECK(members[0].status == SWIM_STATUS_ALIVE);

  // Verify subscription event
  REQUIRE(get_log_size() >= 1);
  {
    EventLog ev = get_log_event(0);
    CHECK(ev.event == SWIM_NODE_UP);
    CHECK(swim_node_id_compare(&ev.node, &mock_id) == 0);
  }

  // Now silently crash the mock node by closing its UDP socket
  swim_udp_final(mock_udp);

  // Wait for failure detector to execute:
  // - Direct probe will fail (ping_timeout = 100ms)
  // - Indirect probe will fail (ping_timeout = 100ms)
  // - Node will declare SUSPECT and log event (400ms protocol period + timeouts ~ 600ms)
  usleep(700000);

  // Verify node is SUSPECT
  count = swim_members("failure_node", members, 5, false);
  REQUIRE(count == 1);
  CHECK(members[0].status == SWIM_STATUS_SUSPECT);

  // Verify SUSPECT subscription event
  bool suspect_found = false;
  size_t log_sz = get_log_size();
  for (size_t i = 0; i < log_sz; i++) {
    EventLog ev = get_log_event(i);
    if (ev.event == SWIM_NODE_SUSPECT) {
      suspect_found = true;
      CHECK(swim_node_id_compare(&ev.node, &mock_id) == 0);
    }
  }
  CHECK(suspect_found);

  // Provide a liveness hint to revive the suspected node
  rc = swim_hint_alive("failure_node", &mock_id);
  CHECK(rc == 0);

  // Verify it is ALIVE again
  count = swim_members("failure_node", members, 5, false);
  REQUIRE(count == 1);
  CHECK(members[0].status == SWIM_STATUS_ALIVE);

  // Now wait long enough without hints to let it time out and die
  // - Direct/indirect timeouts will trigger again.
  // - SUSPECT transition will happen.
  // - After suspicion_timeout (600ms) -> DEAD transition.
  usleep(1500000);

  // Verify it is DEAD (not in active list since include_dead = false)
  count = swim_members("failure_node", members, 5, false);
  CHECK(count == 0);

  // Check with include_dead = true
  count = swim_members("failure_node", members, 5, true);
  REQUIRE(count == 1);
  CHECK(members[0].status == SWIM_STATUS_DEAD);

  // Verify DOWN subscription event
  bool down_found = false;
  log_sz = get_log_size();
  for (size_t i = 0; i < log_sz; i++) {
    EventLog ev = get_log_event(i);
    if (ev.event == SWIM_NODE_DOWN) {
      down_found = true;
      CHECK(swim_node_id_compare(&ev.node, &mock_id) == 0);
    }
  }
  CHECK(down_found);

  swim_leave("failure_node");
  swim_set_error(SWIM_OK, nullptr);
}

// Regression for H1: relay-table entries for ping_req targets that never ack
// must be reclaimed, otherwise the table fills to 32 and the node permanently
// stops relaying. We flood the node with 32 ping_reqs for dead targets, let
// them expire, then verify a fresh ping_req still produces a relay ping.
TEST_CASE("protocol: relay table does not permanently fill") {
  clear_log();

  swim_node_id_t node_id, requester_id, target_id;
  REQUIRE(swim_node_id_parse(&node_id, "127.0.0.1:20301:c1") == 0);
  REQUIRE(swim_node_id_parse(&requester_id, "127.0.0.1:20302:r") == 0);
  REQUIRE(swim_node_id_parse(&target_id, "127.0.0.1:20303:t") == 0);

  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.host = "127.0.0.1";
  opts.port = 20301;
  opts.cookie = "c1";
  opts.name = "relay_node";
  opts.protocol_period_ms = 1000; // keep the node's own probing out of the way
  opts.ping_timeout_ms = 100;     // relay entries expire after ~1 tick

  REQUIRE(swim_start(&opts) == 0);

  swim_udp_t *requester = swim_udp_init("127.0.0.1", 20302);
  REQUIRE(requester != nullptr);
  swim_udp_t *target = swim_udp_init("127.0.0.1", 20303);
  REQUIRE(target != nullptr);

  auto send_ping_req = [&](const swim_node_id_t &tgt, uint32_t seq) {
    swim_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = SWIM_MSG_PING_REQ;
    msg.sender = requester_id;
    msg.seq = seq;
    msg.peer = tgt; // target of the indirect probe
    uint8_t buf[256];
    int len = swim_codec_encode(&msg, buf, sizeof(buf));
    REQUIRE(len > 0);
    REQUIRE(swim_udp_send(requester, &node_id, buf, len) == 0);
  };

  // Flood the relay table (capacity 32) with ping_reqs for dead targets that
  // will never ack.
  for (uint32_t i = 0; i < 32; i++) {
    swim_node_id_t dead;
    char s[64];
    snprintf(s, sizeof(s), "127.0.0.1:%u:d", 21000 + i);
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
    uint8_t buf[256];
    int n = swim_udp_recv(target, &src, buf, sizeof(buf));
    if (n > 0) {
      swim_message_t in;
      if (swim_codec_decode(buf, n, &in) == 0 &&
          in.type == SWIM_MSG_PING && in.seq == 9999 &&
          swim_node_id_compare(&in.sender, &node_id) == 0) {
        relay_ping_seen = true;
      }
    } else {
      usleep(10000);
    }
  }
  CHECK(relay_ping_seen);

  swim_udp_final(requester);
  swim_udp_final(target);
  swim_leave("relay_node");
  swim_set_error(SWIM_OK, nullptr);
}

// Regression for H3: a subscriber callback must be able to re-enter the public
// API. Callbacks fire after the instance lock is released, so a callback that
// calls swim_members() succeeds; before the fix this self-deadlocked the worker
// thread (and would hang the whole suite here).
static std::atomic<bool> g_reentrant_ok{false};
static void reentrant_members_cb(void *ctx, swim_event_t event, const swim_node_id_t *node) {
  (void)ctx; (void)event; (void)node;
  swim_member_t m[8];
  int n = swim_members("h3_reentrant", m, 8, true);
  (void)n;
  g_reentrant_ok = true;
}

TEST_CASE("protocol: subscriber callback may re-enter the API (H3 deadlock)") {
  swim_node_id_t self_id, mock_id;
  REQUIRE(swim_node_id_parse(&self_id, "127.0.0.1:20401:c1") == 0);
  REQUIRE(swim_node_id_parse(&mock_id, "127.0.0.1:20402:mock") == 0);

  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.host = "127.0.0.1";
  opts.port = 20401;
  opts.cookie = "c1";
  opts.name = "h3_reentrant";
  opts.protocol_period_ms = 400;
  opts.ping_timeout_ms = 100;

  g_reentrant_ok = false;
  REQUIRE(swim_start(&opts) == 0);
  REQUIRE(swim_subscribe("h3_reentrant", reentrant_members_cb, nullptr) == 0);

  // A raw PING from a mock makes the node discover it -> NODE_UP -> callback.
  swim_udp_t *mock_udp = swim_udp_init("127.0.0.1", 20402);
  REQUIRE(mock_udp != nullptr);
  swim_message_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.type = SWIM_MSG_PING;
  msg.sender = mock_id;
  msg.seq = 1;
  uint8_t buf[256];
  int len = swim_codec_encode(&msg, buf, sizeof(buf));
  REQUIRE(len > 0);
  REQUIRE(swim_udp_send(mock_udp, &self_id, buf, len) == 0);

  usleep(200000); // if the callback deadlocked, the worker would be stuck here

  CHECK(g_reentrant_ok == true);

  swim_udp_final(mock_udp);
  swim_leave("h3_reentrant");
  swim_set_error(SWIM_OK, nullptr);
}

// Regression for H3 lifetime: swim_hint_alive must not touch the instance after
// releasing its locks, so it can race swim_leave (which frees the instance)
// without a use-after-free. Hammer hint_alive from several threads while the
// main thread leaves. Meaningful under ASan/TSan.
static std::atomic<bool> g_race_stop{false};
struct RaceArg { const char *name; swim_node_id_t peer; };
static void *hint_spammer(void *a) {
  RaceArg *r = (RaceArg *)a;
  while (!g_race_stop.load()) {
    swim_hint_alive(r->name, &r->peer);
  }
  return nullptr;
}
static void noop_cb(void *ctx, swim_event_t e, const swim_node_id_t *n) {
  (void)ctx; (void)e; (void)n;
}

TEST_CASE("protocol: concurrent swim_hint_alive and swim_leave are memory-safe (H3)") {
  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.host = "127.0.0.1";
  opts.port = 20403;
  opts.cookie = "c1";
  opts.name = "h3_race";
  opts.protocol_period_ms = 100;
  opts.ping_timeout_ms = 50;

  REQUIRE(swim_start(&opts) == 0);
  REQUIRE(swim_subscribe("h3_race", noop_cb, nullptr) == 0);

  RaceArg arg;
  arg.name = "h3_race";
  REQUIRE(swim_node_id_parse(&arg.peer, "127.0.0.1:20404:p") == 0);

  g_race_stop = false;
  pthread_t th[4];
  for (int i = 0; i < 4; i++) {
    REQUIRE(pthread_create(&th[i], nullptr, hint_spammer, &arg) == 0);
  }

  usleep(100000); // let the spammers spin against the live instance
  CHECK(swim_leave("h3_race") == 0);
  g_race_stop = true;
  for (int i = 0; i < 4; i++) {
    pthread_join(th[i], nullptr);
  }
  swim_set_error(SWIM_OK, nullptr);
}
