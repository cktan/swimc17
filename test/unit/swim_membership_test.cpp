#include "doctest.h"

extern "C" {
#include "swim_membership.h"
}

#include <cstring>
#include <vector>

TEST_CASE("membership: initialization and finalization") {
  swim_membership_t *m = swim_membership_init();
  REQUIRE(m != nullptr);
  CHECK(swim_membership_count(m) == 0);

  swim_membership_final(m);
}

TEST_CASE("membership: add and get node") {
  swim_membership_t *m = swim_membership_init();
  REQUIRE(m != nullptr);

  swim_node_id_t id1;
  REQUIRE(swim_node_id_parse(&id1, "127.0.0.1:8001:cookie1") == 0);

  // Get unknown node
  const swim_member_t *res = swim_membership_get(m, &id1);
  CHECK(res == nullptr);

  // Add node
  int rc = swim_membership_add(m, &id1, 10);
  CHECK(rc == 0);
  CHECK(swim_membership_count(m) == 1);

  // Get known node
  res = swim_membership_get(m, &id1);
  REQUIRE(res != nullptr);
  CHECK(swim_node_id_compare(&res->id, &id1) == 0);
  CHECK(res->status == SWIM_STATUS_ALIVE);
  CHECK(res->incarnation == 10);
  CHECK(res->dead_at == 0);

  swim_membership_final(m);
}

TEST_CASE("membership: set_alive forces ALIVE state") {
  swim_membership_t *m = swim_membership_init();
  REQUIRE(m != nullptr);

  swim_node_id_t id1;
  REQUIRE(swim_node_id_parse(&id1, "[::1]:8001:cookie1") == 0);

  // Force set on empty membership
  int rc = swim_membership_set_alive(m, &id1, 5);
  CHECK(rc == 0);
  CHECK(swim_membership_count(m) == 1);

  const swim_member_t *res = swim_membership_get(m, &id1);
  REQUIRE(res != nullptr);
  CHECK(res->status == SWIM_STATUS_ALIVE);
  CHECK(res->incarnation == 5);

  // Transition to suspect via event
  rc = swim_membership_apply_event(m, SWIM_STATUS_SUSPECT, &id1, 5, 100);
  CHECK(rc == 0);
  res = swim_membership_get(m, &id1);
  REQUIRE(res != nullptr);
  CHECK(res->status == SWIM_STATUS_SUSPECT);

  // Force set back to ALIVE with same incarnation
  rc = swim_membership_set_alive(m, &id1, 5);
  CHECK(rc == 0);
  res = swim_membership_get(m, &id1);
  REQUIRE(res != nullptr);
  CHECK(res->status == SWIM_STATUS_ALIVE);
  CHECK(res->incarnation == 5);

  // Transition to DEAD via event
  rc = swim_membership_apply_event(m, SWIM_STATUS_DEAD, &id1, 5, 200);
  CHECK(rc == 0);
  res = swim_membership_get(m, &id1);
  REQUIRE(res != nullptr);
  CHECK(res->status == SWIM_STATUS_DEAD);

  // Force set back to ALIVE on DEAD node
  rc = swim_membership_set_alive(m, &id1, 6);
  CHECK(rc == 0);
  res = swim_membership_get(m, &id1);
  REQUIRE(res != nullptr);
  CHECK(res->status == SWIM_STATUS_ALIVE);
  CHECK(res->incarnation == 6);
  CHECK(res->dead_at == 0);

  swim_membership_final(m);
}

TEST_CASE("membership: event precedence rules for unknown nodes") {
  swim_membership_t *m = swim_membership_init();
  REQUIRE(m != nullptr);

  swim_node_id_t id1;
  REQUIRE(swim_node_id_parse(&id1, "127.0.0.1:8001") == 0);

  // Unknown nodes only accept ALIVE events
  int rc = swim_membership_apply_event(m, SWIM_STATUS_SUSPECT, &id1, 1, 100);
  CHECK(rc == 1); // Ignored
  CHECK(swim_membership_count(m) == 0);

  rc = swim_membership_apply_event(m, SWIM_STATUS_DEAD, &id1, 1, 100);
  CHECK(rc == 1); // Ignored
  CHECK(swim_membership_count(m) == 0);

  rc = swim_membership_apply_event(m, SWIM_STATUS_ALIVE, &id1, 1, 100);
  CHECK(rc == 0); // Added
  CHECK(swim_membership_count(m) == 1);

  swim_membership_final(m);
}

TEST_CASE("membership: event precedence rules for ALIVE nodes") {
  swim_membership_t *m = swim_membership_init();
  REQUIRE(m != nullptr);

  swim_node_id_t id1;
  REQUIRE(swim_node_id_parse(&id1, "127.0.0.1:8001") == 0);

  // Add as ALIVE, incarnation 10
  REQUIRE(swim_membership_add(m, &id1, 10) == 0);

  // 1. Stale incarnation event (9)
  int rc = swim_membership_apply_event(m, SWIM_STATUS_SUSPECT, &id1, 9, 100);
  CHECK(rc == 1); // Ignored
  CHECK(swim_membership_get(m, &id1)->status == SWIM_STATUS_ALIVE);

  rc = swim_membership_apply_event(m, SWIM_STATUS_DEAD, &id1, 9, 100);
  CHECK(rc == 1); // Ignored
  CHECK(swim_membership_get(m, &id1)->status == SWIM_STATUS_ALIVE);

  // 2. Same incarnation event (10)
  rc = swim_membership_apply_event(m, SWIM_STATUS_ALIVE, &id1, 10, 100);
  CHECK(rc == 1); // Ignored (no-op/stale)

  rc = swim_membership_apply_event(m, SWIM_STATUS_SUSPECT, &id1, 10, 100);
  CHECK(rc == 0); // Updated to SUSPECT
  CHECK(swim_membership_get(m, &id1)->status == SWIM_STATUS_SUSPECT);

  // Reset back to ALIVE (using set_alive)
  REQUIRE(swim_membership_set_alive(m, &id1, 10) == 0);

  rc = swim_membership_apply_event(m, SWIM_STATUS_DEAD, &id1, 10, 150);
  CHECK(rc == 0); // Updated to DEAD
  const swim_member_t *res = swim_membership_get(m, &id1);
  REQUIRE(res != nullptr);
  CHECK(res->status == SWIM_STATUS_DEAD);
  CHECK(res->dead_at == 150);

  // 3. Higher incarnation event (11) on active node
  // First make it ALIVE at 10 again
  REQUIRE(swim_membership_set_alive(m, &id1, 10) == 0);

  // Higher incarnation ALIVE
  rc = swim_membership_apply_event(m, SWIM_STATUS_ALIVE, &id1, 11, 200);
  CHECK(rc == 0);
  CHECK(swim_membership_get(m, &id1)->incarnation == 11);
  CHECK(swim_membership_get(m, &id1)->status == SWIM_STATUS_ALIVE);

  // Higher incarnation SUSPECT
  rc = swim_membership_apply_event(m, SWIM_STATUS_SUSPECT, &id1, 12, 200);
  CHECK(rc == 0);
  CHECK(swim_membership_get(m, &id1)->incarnation == 12);
  CHECK(swim_membership_get(m, &id1)->status == SWIM_STATUS_SUSPECT);

  // Higher incarnation DEAD
  rc = swim_membership_apply_event(m, SWIM_STATUS_DEAD, &id1, 13, 250);
  CHECK(rc == 0);
  CHECK(swim_membership_get(m, &id1)->incarnation == 13);
  CHECK(swim_membership_get(m, &id1)->status == SWIM_STATUS_DEAD);
  CHECK(swim_membership_get(m, &id1)->dead_at == 250);

  swim_membership_final(m);
}

TEST_CASE("membership: event precedence rules for SUSPECT nodes") {
  swim_membership_t *m = swim_membership_init();
  REQUIRE(m != nullptr);

  swim_node_id_t id1;
  REQUIRE(swim_node_id_parse(&id1, "127.0.0.1:8001") == 0);

  // Add and make SUSPECT at incarnation 10
  REQUIRE(swim_membership_add(m, &id1, 10) == 0);
  REQUIRE(swim_membership_apply_event(m, SWIM_STATUS_SUSPECT, &id1, 10, 100) ==
          0);

  // Same incarnation ALIVE should be ignored (DEAD > SUSPECT > ALIVE)
  int rc = swim_membership_apply_event(m, SWIM_STATUS_ALIVE, &id1, 10, 150);
  CHECK(rc == 1);
  CHECK(swim_membership_get(m, &id1)->status == SWIM_STATUS_SUSPECT);

  // Same incarnation SUSPECT should be ignored (no-op)
  rc = swim_membership_apply_event(m, SWIM_STATUS_SUSPECT, &id1, 10, 150);
  CHECK(rc == 1);

  // Same incarnation DEAD should update
  rc = swim_membership_apply_event(m, SWIM_STATUS_DEAD, &id1, 10, 200);
  CHECK(rc == 0);
  CHECK(swim_membership_get(m, &id1)->status == SWIM_STATUS_DEAD);

  swim_membership_final(m);
}

TEST_CASE("membership: event precedence rules for DEAD nodes") {
  swim_membership_t *m = swim_membership_init();
  REQUIRE(m != nullptr);

  swim_node_id_t id1;
  REQUIRE(swim_node_id_parse(&id1, "127.0.0.1:8001") == 0);

  // Add and make DEAD at incarnation 10
  REQUIRE(swim_membership_add(m, &id1, 10) == 0);
  REQUIRE(swim_membership_apply_event(m, SWIM_STATUS_DEAD, &id1, 10, 100) == 0);

  // 1. Same or lower incarnation events are completely ignored
  int rc = swim_membership_apply_event(m, SWIM_STATUS_ALIVE, &id1, 10, 150);
  CHECK(rc == 1);
  rc = swim_membership_apply_event(m, SWIM_STATUS_SUSPECT, &id1, 10, 150);
  CHECK(rc == 1);
  rc = swim_membership_apply_event(m, SWIM_STATUS_DEAD, &id1, 10, 150);
  CHECK(rc == 1);

  rc = swim_membership_apply_event(m, SWIM_STATUS_ALIVE, &id1, 9, 150);
  CHECK(rc == 1);

  // 2. Higher incarnation SUSPECT or DEAD are ignored (cannot revive except via
  // ALIVE)
  rc = swim_membership_apply_event(m, SWIM_STATUS_SUSPECT, &id1, 11, 150);
  CHECK(rc == 1);
  CHECK(swim_membership_get(m, &id1)->status == SWIM_STATUS_DEAD);

  rc = swim_membership_apply_event(m, SWIM_STATUS_DEAD, &id1, 11, 150);
  CHECK(rc == 1);

  // 3. Higher incarnation ALIVE revives the node
  rc = swim_membership_apply_event(m, SWIM_STATUS_ALIVE, &id1, 11, 200);
  CHECK(rc == 0);
  const swim_member_t *res = swim_membership_get(m, &id1);
  REQUIRE(res != nullptr);
  CHECK(res->status == SWIM_STATUS_ALIVE);
  CHECK(res->incarnation == 11);
  CHECK(res->dead_at == 0);

  swim_membership_final(m);
}

TEST_CASE("membership: garbage collection") {
  swim_membership_t *m = swim_membership_init();
  REQUIRE(m != nullptr);

  swim_node_id_t id1, id2, id3;
  REQUIRE(swim_node_id_parse(&id1, "127.0.0.1:8001") == 0);
  REQUIRE(swim_node_id_parse(&id2, "127.0.0.1:8002") == 0);
  REQUIRE(swim_node_id_parse(&id3, "127.0.0.1:8003") == 0);

  // Add 3 nodes
  REQUIRE(swim_membership_add(m, &id1, 1) == 0);
  REQUIRE(swim_membership_add(m, &id2, 1) == 0);
  REQUIRE(swim_membership_add(m, &id3, 1) == 0);

  // Make id1 dead at now_ms = 1000
  REQUIRE(swim_membership_apply_event(m, SWIM_STATUS_DEAD, &id1, 1, 1000) == 0);
  // Make id2 dead at now_ms = 1200
  REQUIRE(swim_membership_apply_event(m, SWIM_STATUS_DEAD, &id2, 1, 1200) == 0);
  // id3 remains ALIVE

  // Run GC at now_ms = 1500 with expiry = 500.
  // id1: 1500 - 1000 = 500 >= 500 (pruned)
  // id2: 1500 - 1200 = 300 < 500 (kept)
  // id3: ALIVE (kept)
  swim_membership_gc(m, 500, 1500);

  CHECK(swim_membership_get(m, &id1) == nullptr);
  CHECK(swim_membership_get(m, &id2) != nullptr);
  CHECK(swim_membership_get(m, &id3) != nullptr);

  // Run GC at now_ms = 1700 with expiry = 500.
  // id2: 1700 - 1200 = 500 >= 500 (pruned)
  swim_membership_gc(m, 500, 1700);

  CHECK(swim_membership_get(m, &id2) == nullptr);
  CHECK(swim_membership_get(m, &id3) != nullptr);

  swim_membership_final(m);
}

TEST_CASE("membership: list and count members") {
  swim_membership_t *m = swim_membership_init();
  REQUIRE(m != nullptr);

  // We want to make sure nodes are kept sorted by swim_node_id_compare
  swim_node_id_t id1, id2, id3;
  REQUIRE(swim_node_id_parse(&id1, "127.0.0.1:8003") == 0); // Port 8003
  REQUIRE(swim_node_id_parse(&id2, "127.0.0.1:8001") == 0); // Port 8001
  REQUIRE(swim_node_id_parse(&id3, "127.0.0.1:8002") == 0); // Port 8002

  REQUIRE(swim_membership_add(m, &id1, 1) == 0);
  REQUIRE(swim_membership_add(m, &id2, 1) == 0);
  REQUIRE(swim_membership_add(m, &id3, 1) == 0);

  // Since it's sorted, the order in list should be: id2 (8001), id3 (8002), id1
  // (8003) Let's check active count
  CHECK(swim_membership_count(m) == 3);

  swim_member_t list[5];
  int count = swim_membership_list(m, list, 5, true);
  REQUIRE(count == 3);
  CHECK(swim_node_id_compare(&list[0].id, &id2) == 0);
  CHECK(swim_node_id_compare(&list[1].id, &id3) == 0);
  CHECK(swim_node_id_compare(&list[2].id, &id1) == 0);

  // Make id3 (8002) DEAD
  REQUIRE(swim_membership_apply_event(m, SWIM_STATUS_DEAD, &id3, 1, 1000) == 0);

  // Active count should now be 2
  CHECK(swim_membership_count(m) == 2);

  // List without dead
  count = swim_membership_list(m, list, 5, false);
  REQUIRE(count == 2);
  CHECK(swim_node_id_compare(&list[0].id, &id2) == 0);
  CHECK(swim_node_id_compare(&list[1].id, &id1) == 0);

  // List with dead but limited size
  count = swim_membership_list(m, list, 2, true);
  REQUIRE(count == 2);
  CHECK(swim_node_id_compare(&list[0].id, &id2) == 0);
  CHECK(swim_node_id_compare(&list[1].id, &id3) ==
        0); // id3 is included here because it's second in sorted order

  swim_membership_final(m);
}
