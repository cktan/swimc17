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
  int id;  // taken from param
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
    swim_timer_add(t, 2, "p", periodic_cb, ctx, param);
  }
}

// Helpers over g_log.
size_t count_ev(swim_timer_event_t ev) {
  size_t n = 0;
  for (auto &r : g_log) {
    if (r.ev == ev) n++;
  }
  return n;
}

std::vector<int> alarm_ids() {
  std::vector<int> v;
  for (auto &r : g_log) {
    if (r.ev == SWIM_TIMER_ALARM) v.push_back(r.id);
  }
  return v;
}

void add(swim_timer_t *t, int ticks, const char *name, int id) {
  swim_timer_add(t, ticks, name, rec_cb, nullptr, as_param(id));
}

}  // namespace

TEST_CASE("empty timer: tick and final are no-ops") {
  g_log.clear();
  swim_timer_t *t = swim_timer_init();
  swim_timer_tick(t);
  swim_timer_tick(t);
  CHECK(g_log.empty());
  swim_timer_final(t);
  CHECK(g_log.empty());
}

TEST_CASE("single alarm fires on the exact tick") {
  g_log.clear();
  swim_timer_t *t = swim_timer_init();
  add(t, 3, "a", 1);

  swim_timer_tick(t);  // 1
  CHECK(g_log.empty());
  swim_timer_tick(t);  // 2
  CHECK(g_log.empty());
  swim_timer_tick(t);  // 3 -> fire
  REQUIRE(g_log.size() == 1);
  CHECK(g_log[0].ev == SWIM_TIMER_ALARM);
  CHECK(g_log[0].id == 1);

  swim_timer_tick(t);  // nothing left
  CHECK(g_log.size() == 1);
  swim_timer_final(t);
}

TEST_CASE("alarms due on the same tick fire in insertion order") {
  g_log.clear();
  swim_timer_t *t = swim_timer_init();
  add(t, 2, "a", 10);
  add(t, 2, "b", 20);
  add(t, 2, "c", 30);

  swim_timer_tick(t);
  CHECK(g_log.empty());
  swim_timer_tick(t);  // all three due now

  CHECK(alarm_ids() == std::vector<int>({10, 20, 30}));
  swim_timer_final(t);
}

TEST_CASE("out-of-order inserts fire in time order") {
  g_log.clear();
  swim_timer_t *t = swim_timer_init();
  add(t, 3, "b", 2);
  add(t, 1, "a", 1);
  add(t, 5, "c", 3);

  for (int i = 0; i < 5; i++) swim_timer_tick(t);

  CHECK(alarm_ids() == std::vector<int>({1, 2, 3}));
  swim_timer_final(t);
}

TEST_CASE("cancel fires CANCEL, not ALARM, and stops the alarm") {
  g_log.clear();
  swim_timer_t *t = swim_timer_init();
  add(t, 3, "a", 7);

  swim_timer_cancel(t, "a");
  REQUIRE(g_log.size() == 1);
  CHECK(g_log[0].ev == SWIM_TIMER_CANCEL);
  CHECK(g_log[0].id == 7);

  for (int i = 0; i < 5; i++) swim_timer_tick(t);
  CHECK(count_ev(SWIM_TIMER_ALARM) == 0);
  swim_timer_final(t);
}

TEST_CASE("cancel preserves the successor's fire time") {
  g_log.clear();
  swim_timer_t *t = swim_timer_init();
  add(t, 2, "a", 1);  // stored delta 2
  add(t, 5, "b", 2);  // stored delta 3 (relative to a)

  swim_timer_cancel(t, "a");  // b must still fire at absolute 5

  for (int i = 0; i < 4; i++) swim_timer_tick(t);  // ticks 1..4
  CHECK(count_ev(SWIM_TIMER_ALARM) == 0);
  swim_timer_tick(t);  // tick 5 -> b
  REQUIRE(alarm_ids() == std::vector<int>({2}));
  swim_timer_final(t);
}

TEST_CASE("cancel removes only the first match of a duplicate name") {
  g_log.clear();
  swim_timer_t *t = swim_timer_init();
  add(t, 2, "dup", 1);
  add(t, 4, "dup", 2);

  swim_timer_cancel(t, "dup");  // removes id 1 (first/earliest)
  REQUIRE(g_log.size() == 1);
  CHECK(g_log[0].ev == SWIM_TIMER_CANCEL);
  CHECK(g_log[0].id == 1);

  for (int i = 0; i < 4; i++) swim_timer_tick(t);
  CHECK(alarm_ids() == std::vector<int>({2}));
  swim_timer_final(t);
}

TEST_CASE("cancel of an unknown name is a no-op") {
  g_log.clear();
  swim_timer_t *t = swim_timer_init();
  add(t, 2, "a", 1);

  swim_timer_cancel(t, "nope");
  CHECK(g_log.empty());

  swim_timer_tick(t);
  swim_timer_tick(t);
  CHECK(alarm_ids() == std::vector<int>({1}));
  swim_timer_final(t);
}

TEST_CASE("cancel_all empties the timer but leaves it reusable") {
  g_log.clear();
  swim_timer_t *t = swim_timer_init();
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
  swim_timer_final(t);
}

TEST_CASE("final fires CANCEL on every survivor") {
  g_log.clear();
  swim_timer_t *t = swim_timer_init();
  add(t, 2, "a", 1);
  add(t, 3, "b", 2);
  add(t, 4, "c", 3);

  swim_timer_final(t);
  CHECK(count_ev(SWIM_TIMER_CANCEL) == 3);
  CHECK(count_ev(SWIM_TIMER_ALARM) == 0);
}

TEST_CASE("re-arm: cancel then add with the same name") {
  g_log.clear();
  swim_timer_t *t = swim_timer_init();
  add(t, 5, "x", 1);
  swim_timer_cancel(t, "x");
  add(t, 2, "x", 2);

  swim_timer_tick(t);
  CHECK(count_ev(SWIM_TIMER_ALARM) == 0);
  swim_timer_tick(t);  // fires the re-armed alarm at 2
  CHECK(alarm_ids() == std::vector<int>({2}));
  swim_timer_final(t);
}

TEST_CASE("periodic: a self-rearming callback fires every period") {
  g_log.clear();
  swim_timer_t *t = swim_timer_init();
  swim_timer_add(t, 2, "p", periodic_cb, t, as_param(9));

  for (int i = 0; i < 6; i++) swim_timer_tick(t);  // expect at 2,4,6
  CHECK(alarm_ids() == std::vector<int>({9, 9, 9}));

  // The last re-armed alarm is still pending; final cancels it.
  g_log.clear();
  swim_timer_final(t);
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

  swim_timer_t *t = swim_timer_init();
  swim_timer_add(t, 1, "k", cb, &cap, &marker);
  swim_timer_tick(t);

  CHECK(cap.ctx == &cap);
  CHECK(cap.param == &marker);
  swim_timer_final(t);
}
