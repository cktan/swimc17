#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

extern "C" {
#include "swim_timer.h"
}

#include <cstdint>
#include <string>
#include <vector>

namespace {

// One recorded callback invocation.
struct Rec {
  swim_timer_event_t ev;
  int id; // taken from param
};

std::vector<Rec> g_log;

int id_of(void *param) { return (int)(intptr_t)param; }
void *as_param(int id) { return (void *)(intptr_t)id; }

// Plain recorder: logs (event, id).
void rec_cb(void *ctx, swim_timer_event_t ev, void *param) {
  (void)ctx;
  g_log.push_back({ev, id_of(param)});
}

// Periodic recorder: re-arms itself on ALARM using ctx as the timer.
void periodic_cb(void *ctx, swim_timer_event_t ev, void *param) {
  g_log.push_back({ev, id_of(param)});
  if (ev == SWIM_TIMER_ALARM) {
    swim_timer_t *t = (swim_timer_t *)ctx;
    int rc = swim_timer_add(t, 2, "p", periodic_cb, ctx, param);
    REQUIRE(rc == 0);
  }
}

// Helpers over g_log.
size_t count_ev(swim_timer_event_t ev) {
  size_t n = 0;
  for (auto &r : g_log) {
    if (r.ev == ev)
      n++;
  }
  return n;
}

std::vector<int> alarm_ids() {
  std::vector<int> v;
  for (auto &r : g_log) {
    if (r.ev == SWIM_TIMER_ALARM)
      v.push_back(r.id);
  }
  return v;
}

void add(swim_timer_t *t, int ticks, const char *name, int id) {
  int rc = swim_timer_add(t, ticks, name, rec_cb, nullptr, as_param(id));
  REQUIRE(rc == 0);
}

} // namespace

TEST_CASE("empty timer: tick and final are no-ops") {
  g_log.clear();
  swim_timer_t *t = swim_timer_create();
  swim_timer_tick(t);
  swim_timer_tick(t);
  CHECK(g_log.empty());
  swim_timer_destroy(t);
  CHECK(g_log.empty());
}

TEST_CASE("single alarm fires on the exact tick") {
  g_log.clear();
  swim_timer_t *t = swim_timer_create();
  add(t, 3, "a", 1);

  swim_timer_tick(t); // 1
  CHECK(g_log.empty());
  swim_timer_tick(t); // 2
  CHECK(g_log.empty());
  swim_timer_tick(t); // 3 -> fire
  REQUIRE(g_log.size() == 1);
  CHECK(g_log[0].ev == SWIM_TIMER_ALARM);
  CHECK(g_log[0].id == 1);

  swim_timer_tick(t); // nothing left
  CHECK(g_log.size() == 1);
  swim_timer_destroy(t);
}

TEST_CASE("alarms due on the same tick fire in insertion order") {
  g_log.clear();
  swim_timer_t *t = swim_timer_create();
  add(t, 2, "a", 10);
  add(t, 2, "b", 20);
  add(t, 2, "c", 30);

  swim_timer_tick(t);
  CHECK(g_log.empty());
  swim_timer_tick(t); // all three due now

  CHECK(alarm_ids() == std::vector<int>({10, 20, 30}));
  swim_timer_destroy(t);
}

TEST_CASE("out-of-order inserts fire in time order") {
  g_log.clear();
  swim_timer_t *t = swim_timer_create();
  add(t, 3, "b", 2);
  add(t, 1, "a", 1);
  add(t, 5, "c", 3);

  for (int i = 0; i < 5; i++)
    swim_timer_tick(t);

  CHECK(alarm_ids() == std::vector<int>({1, 2, 3}));
  swim_timer_destroy(t);
}

TEST_CASE("cancel fires CANCEL, not ALARM, and stops the alarm") {
  g_log.clear();
  swim_timer_t *t = swim_timer_create();
  add(t, 3, "a", 7);

  swim_timer_cancel(t, "a");
  REQUIRE(g_log.size() == 1);
  CHECK(g_log[0].ev == SWIM_TIMER_CANCEL);
  CHECK(g_log[0].id == 7);

  for (int i = 0; i < 5; i++)
    swim_timer_tick(t);
  CHECK(count_ev(SWIM_TIMER_ALARM) == 0);
  swim_timer_destroy(t);
}

TEST_CASE("cancel preserves the successor's fire time") {
  g_log.clear();
  swim_timer_t *t = swim_timer_create();
  add(t, 2, "a", 1); // stored delta 2
  add(t, 5, "b", 2); // stored delta 3 (relative to a)

  swim_timer_cancel(t, "a"); // b must still fire at absolute 5

  for (int i = 0; i < 4; i++)
    swim_timer_tick(t); // ticks 1..4
  CHECK(count_ev(SWIM_TIMER_ALARM) == 0);
  swim_timer_tick(t); // tick 5 -> b
  REQUIRE(alarm_ids() == std::vector<int>({2}));
  swim_timer_destroy(t);
}

TEST_CASE("cancel removes only the first match of a duplicate name") {
  g_log.clear();
  swim_timer_t *t = swim_timer_create();
  add(t, 2, "dup", 1);
  add(t, 4, "dup", 2);

  swim_timer_cancel(t, "dup"); // removes id 1 (first/earliest)
  REQUIRE(g_log.size() == 1);
  CHECK(g_log[0].ev == SWIM_TIMER_CANCEL);
  CHECK(g_log[0].id == 1);

  for (int i = 0; i < 4; i++)
    swim_timer_tick(t);
  CHECK(alarm_ids() == std::vector<int>({2}));
  swim_timer_destroy(t);
}

TEST_CASE("cancel of an unknown name is a no-op") {
  g_log.clear();
  swim_timer_t *t = swim_timer_create();
  add(t, 2, "a", 1);

  swim_timer_cancel(t, "nope");
  CHECK(g_log.empty());

  swim_timer_tick(t);
  swim_timer_tick(t);
  CHECK(alarm_ids() == std::vector<int>({1}));
  swim_timer_destroy(t);
}

TEST_CASE("cancel_all empties the timer but leaves it reusable") {
  g_log.clear();
  swim_timer_t *t = swim_timer_create();
  add(t, 2, "a", 1);
  add(t, 4, "b", 2);

  swim_timer_cancel_all(t);
  CHECK(count_ev(SWIM_TIMER_CANCEL) == 2);
  CHECK(count_ev(SWIM_TIMER_ALARM) == 0);

  // Reuse after cancel_all.
  g_log.clear();
  add(t, 1, "c", 3);
  swim_timer_tick(t);
  CHECK(alarm_ids() == std::vector<int>({3}));
  swim_timer_destroy(t);
}

TEST_CASE("final fires CANCEL on every survivor") {
  g_log.clear();
  swim_timer_t *t = swim_timer_create();
  add(t, 2, "a", 1);
  add(t, 3, "b", 2);
  add(t, 4, "c", 3);

  swim_timer_destroy(t);
  CHECK(count_ev(SWIM_TIMER_CANCEL) == 3);
  CHECK(count_ev(SWIM_TIMER_ALARM) == 0);
}

TEST_CASE("re-arm: cancel then add with the same name") {
  g_log.clear();
  swim_timer_t *t = swim_timer_create();
  add(t, 5, "x", 1);
  swim_timer_cancel(t, "x");
  add(t, 2, "x", 2);

  swim_timer_tick(t);
  CHECK(count_ev(SWIM_TIMER_ALARM) == 0);
  swim_timer_tick(t); // fires the re-armed alarm at 2
  CHECK(alarm_ids() == std::vector<int>({2}));
  swim_timer_destroy(t);
}

TEST_CASE("periodic: a self-rearming callback fires every period") {
  g_log.clear();
  swim_timer_t *t = swim_timer_create();
  int rc = swim_timer_add(t, 2, "p", periodic_cb, t, as_param(9));
  REQUIRE(rc == 0);

  for (int i = 0; i < 6; i++)
    swim_timer_tick(t); // expect at 2,4,6
  CHECK(alarm_ids() == std::vector<int>({9, 9, 9}));

  // The last re-armed alarm is still pending; final cancels it.
  g_log.clear();
  swim_timer_destroy(t);
  CHECK(count_ev(SWIM_TIMER_CANCEL) == 1);
}

TEST_CASE("ctx and param are passed through unchanged") {
  g_log.clear();
  int marker = 0;
  struct Captured {
    void *ctx;
    void *param;
  } cap{nullptr, nullptr};

  auto cb = [](void *ctx, swim_timer_event_t ev, void *param) {
    (void)ev;
    auto *c = (Captured *)ctx;
    c->ctx = ctx;
    c->param = param;
  };

  swim_timer_t *t = swim_timer_create();
  int rc = swim_timer_add(t, 1, "k", cb, &cap, &marker);
  REQUIRE(rc == 0);
  swim_timer_tick(t);

  CHECK(cap.ctx == &cap);
  CHECK(cap.param == &marker);
  swim_timer_destroy(t);
}

TEST_CASE("swim_errno interface works as expected") {
  // Verify default state
  CHECK(swim_errno() == SWIM_OK);
  CHECK(strcmp(swim_errmsg(), "") == 0);

  // Set thread-local error state directly
  swim_set_error(SWIM_ERR_NOMEM, NULL);
  CHECK(swim_errno() == SWIM_ERR_NOMEM);

  // Set thread-local error state using swim_set_error
  swim_set_error(SWIM_ERR_TIMEOUT, "Request timed out after %d ms", 5000);
  CHECK(swim_errno() == SWIM_ERR_TIMEOUT);
  CHECK(strcmp(swim_errmsg(), "Request timed out after 5000 ms") == 0);

  // Set with NULL format
  swim_set_error(SWIM_ERR_INVALID, nullptr);
  CHECK(swim_errno() == SWIM_ERR_INVALID);
  CHECK(strcmp(swim_errmsg(), "") == 0);

  // Test swim_strerror mapping
  CHECK(strcmp(swim_strerror(SWIM_OK), "Success") == 0);
  CHECK(strcmp(swim_strerror(SWIM_ERR_NOMEM), "Out of memory") == 0);
  CHECK(strcmp(swim_strerror(SWIM_ERR_INVALID), "Invalid argument") == 0);
  CHECK(strcmp(swim_strerror(SWIM_ERR_FULL), "Container is full") == 0);
  CHECK(strcmp(swim_strerror(SWIM_ERR_TIMEOUT), "Operation timed out") == 0);
  CHECK(strcmp(swim_strerror(SWIM_ERR_BAD_STATE), "Object in bad state") == 0);
  CHECK(strcmp(swim_strerror(999), "Unknown error") == 0);
}

TEST_CASE("swim_node_id manual initialization, formatting, and copying") {
  swim_node_id_t id1 = {};
  strcpy(id1.host, "127.0.0.1");
  id1.port = 8080;
  strcpy(id1.cookie, "cookie1");

  CHECK(strcmp(id1.host, "127.0.0.1") == 0);
  CHECK(id1.port == 8080);
  CHECK(strcmp(id1.cookie, "cookie1") == 0);

  char buf[256];
  // Format check
  REQUIRE(swim_node_id_format(&id1, buf, sizeof(buf)) == 0);
  CHECK(strcmp(buf, "127.0.0.1:8080/cookie1") == 0);

  swim_node_id_t id2 = {};
  strcpy(id2.host, "example.com");
  id2.port = 80;
  id2.cookie[0] = '\0';

  CHECK(strcmp(id2.host, "example.com") == 0);
  CHECK(id2.port == 80);
  CHECK(strcmp(id2.cookie, "") == 0);

  // Format check without cookie
  REQUIRE(swim_node_id_format(&id2, buf, sizeof(buf)) == 0);
  CHECK(strcmp(buf, "example.com:80") == 0);

  // Copy check using structure assignment
  swim_node_id_t copy_id = id1;
  CHECK(strcmp(copy_id.host, "127.0.0.1") == 0);
  CHECK(copy_id.port == 8080);
  CHECK(strcmp(copy_id.cookie, "cookie1") == 0);
}

TEST_CASE("swim_node_id comparison and sorting") {
  swim_node_id_t a = {};
  strcpy(a.host, "host1");
  a.port = 100;
  strcpy(a.cookie, "cook");

  swim_node_id_t b = {};
  strcpy(b.host, "host1");
  b.port = 100;
  strcpy(b.cookie, "cook");

  CHECK(swim_node_id_compare(&a, &b) == 0);

  // Host difference
  strcpy(b.host, "host2");
  CHECK(swim_node_id_compare(&a, &b) < 0);
  CHECK(swim_node_id_compare(&b, &a) > 0);

  // Port difference
  strcpy(b.host, "host1");
  b.port = 101;
  CHECK(swim_node_id_compare(&a, &b) < 0);

  // Cookie difference
  b.port = 100;
  strcpy(b.cookie, "cook2");
  CHECK(swim_node_id_compare(&a, &b) < 0);
}

TEST_CASE("swim_node_id parsing functionality") {
  swim_node_id_t id;
  char buf[256];

  // Parse IPv4 with cookie
  REQUIRE(swim_node_id_parse(&id, "192.168.1.1:8888/mycookie") == 0);
  CHECK(strcmp(id.host, "192.168.1.1") == 0);
  CHECK(id.port == 8888);
  CHECK(strcmp(id.cookie, "mycookie") == 0);

  // Parse IPv4 without cookie
  REQUIRE(swim_node_id_parse(&id, "127.0.0.1:443") == 0);
  CHECK(strcmp(id.host, "127.0.0.1") == 0);
  CHECK(id.port == 443);
  CHECK(strcmp(id.cookie, "") == 0);

  // Parse bracketed IPv6 with cookie
  REQUIRE(swim_node_id_parse(&id, "[2001:db8::1]:8080/ipv6cook") == 0);
  CHECK(strcmp(id.host, "2001:db8::1") == 0);
  CHECK(id.port == 8080);
  CHECK(strcmp(id.cookie, "ipv6cook") == 0);
  REQUIRE(swim_node_id_format(&id, buf, sizeof(buf)) == 0);
  CHECK(strcmp(buf, "[2001:db8::1]:8080/ipv6cook") == 0);

  // Parse bracketed IPv6 without cookie
  REQUIRE(swim_node_id_parse(&id, "[::1]:22") == 0);
  CHECK(strcmp(id.host, "::1") == 0);
  CHECK(id.port == 22);
  CHECK(strcmp(id.cookie, "") == 0);

  // Failures
  CHECK(swim_node_id_parse(&id, "127.0.0.1") == -1);       // No port
  CHECK(swim_node_id_parse(&id, "127.0.0.1:abc") == -1);   // Invalid port
  CHECK(swim_node_id_parse(&id, "127.0.0.1:65536") == -1); // Port out of range
  CHECK(swim_node_id_parse(&id, "[::1]") == -1); // Missing port after brackets
  CHECK(swim_node_id_parse(&id, "[::1:80") == -1); // Unclosed bracket
}
