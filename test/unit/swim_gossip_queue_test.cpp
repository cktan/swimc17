#include "doctest.h"

extern "C" {
#include "swim_errno.h"
#include "swim_gossip_queue.h"
#include "swim_node_id.h"
}

#include <cstring>
#include <vector>

TEST_CASE("gossip_queue: init and final") {
  swim_gossip_queue_t *q = swim_gossip_queue_init();
  REQUIRE(q != nullptr);
  CHECK(swim_gossip_queue_size(q) == 0);
  swim_gossip_queue_final(q);
}

TEST_CASE("gossip_queue: basic enqueue and supersession") {
  swim_gossip_queue_t *q = swim_gossip_queue_init();
  REQUIRE(q != nullptr);

  swim_node_id_t id1;
  REQUIRE(swim_node_id_parse(&id1, "127.0.0.1:8001:cookie1") == 0);

  // 1. Enqueue brand new event (ALIVE, inc 10)
  int rc = swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, &id1, 10, 1);
  CHECK(rc == 0);
  CHECK(swim_gossip_queue_size(q) == 1);

  swim_member_t peek[5];
  int count = swim_gossip_queue_peek(q, peek, 5);
  REQUIRE(count == 1);
  CHECK(peek[0].status == SWIM_STATUS_ALIVE);
  CHECK(peek[0].incarnation == 10);

  // 2. Same incarnation, same priority -> ignore (no-op)
  rc = swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, &id1, 10, 1);
  CHECK(rc == 0);
  CHECK(swim_gossip_queue_size(q) == 1);

  // 3. Same incarnation, higher priority (SUSPECT > ALIVE) -> replace
  rc = swim_gossip_queue_enqueue(q, SWIM_STATUS_SUSPECT, &id1, 10, 1);
  CHECK(rc == 0);
  count = swim_gossip_queue_peek(q, peek, 5);
  REQUIRE(count == 1);
  CHECK(peek[0].status == SWIM_STATUS_SUSPECT);

  // 4. Same incarnation, lower priority (ALIVE < SUSPECT) -> ignore
  rc = swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, &id1, 10, 1);
  CHECK(rc == 0);
  count = swim_gossip_queue_peek(q, peek, 5);
  REQUIRE(count == 1);
  CHECK(peek[0].status == SWIM_STATUS_SUSPECT);

  // 5. Higher incarnation, lower priority (ALIVE at 11 > SUSPECT at 10) ->
  // replace
  rc = swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, &id1, 11, 1);
  CHECK(rc == 0);
  count = swim_gossip_queue_peek(q, peek, 5);
  REQUIRE(count == 1);
  CHECK(peek[0].status == SWIM_STATUS_ALIVE);
  CHECK(peek[0].incarnation == 11);

  // 6. Stale incarnation event (SUSPECT at 10 < ALIVE at 11) -> ignore
  rc = swim_gossip_queue_enqueue(q, SWIM_STATUS_SUSPECT, &id1, 10, 1);
  CHECK(rc == 0);
  count = swim_gossip_queue_peek(q, peek, 5);
  REQUIRE(count == 1);
  CHECK(peek[0].status == SWIM_STATUS_ALIVE);
  CHECK(peek[0].incarnation == 11);

  swim_gossip_queue_final(q);
}

TEST_CASE("gossip_queue: priority sorting order") {
  swim_gossip_queue_t *q = swim_gossip_queue_init();
  REQUIRE(q != nullptr);

  swim_node_id_t idA, idB, idC;
  REQUIRE(swim_node_id_parse(&idA, "127.0.0.1:8001") == 0); // ALIVE
  REQUIRE(swim_node_id_parse(&idB, "127.0.0.1:8002") == 0); // SUSPECT
  REQUIRE(swim_node_id_parse(&idC, "127.0.0.1:8003") == 0); // DEAD

  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, &idA, 1, 1) == 0);
  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_SUSPECT, &idB, 1, 1) == 0);
  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_DEAD, &idC, 1, 1) == 0);

  swim_member_t peek[5];
  int count = swim_gossip_queue_peek(q, peek, 5);
  REQUIRE(count == 3);

  // Expect priority: DEAD (0) > SUSPECT (1) > ALIVE (2)
  CHECK(swim_node_id_compare(&peek[0].id, &idC) == 0); // DEAD
  CHECK(peek[0].status == SWIM_STATUS_DEAD);
  CHECK(swim_node_id_compare(&peek[1].id, &idB) == 0); // SUSPECT
  CHECK(peek[1].status == SWIM_STATUS_SUSPECT);
  CHECK(swim_node_id_compare(&peek[2].id, &idA) == 0); // ALIVE
  CHECK(peek[2].status == SWIM_STATUS_ALIVE);

  swim_gossip_queue_final(q);
}

TEST_CASE("gossip_queue: transmit count priority tie-breaker") {
  swim_gossip_queue_t *q = swim_gossip_queue_init();
  REQUIRE(q != nullptr);

  swim_node_id_t idA, idB;
  REQUIRE(swim_node_id_parse(&idA, "127.0.0.1:8001") == 0);
  REQUIRE(swim_node_id_parse(&idB, "127.0.0.1:8002") == 0);

  // Both same priority
  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, &idA, 1, 1) == 0);
  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, &idB, 1, 1) == 0);

  // Pack with a budget that only fits one event.
  // Wire sizes: type (1) + inc (8) + host_len (1) + host (9) + port (2) +
  // cookie_len (1) + cookie (0) = 22 bytes
  swim_member_t out[1];
  int packed = swim_gossip_queue_pack(q, 5, 25, out, 1);
  REQUIRE(packed == 1);
  CHECK(swim_node_id_compare(&out[0].id, &idA) ==
        0); // idA packed first (sorted by node ID tie-breaker)

  // Now idA has transmit_count = 1, idB has transmit_count = 0.
  // Next pack should prioritize idB because its transmit count is lower (0 <
  // 1).
  packed = swim_gossip_queue_pack(q, 5, 25, out, 1);
  REQUIRE(packed == 1);
  CHECK(swim_node_id_compare(&out[0].id, &idB) == 0);

  swim_gossip_queue_final(q);
}

TEST_CASE("gossip_queue: event size limit packing budget") {
  swim_gossip_queue_t *q = swim_gossip_queue_init();
  REQUIRE(q != nullptr);

  swim_node_id_t idA, idB, idC;
  REQUIRE(swim_node_id_parse(&idA, "127.0.0.1:8001") == 0); // 22 bytes
  REQUIRE(swim_node_id_parse(&idB, "127.0.0.1:8002") == 0); // 22 bytes
  REQUIRE(swim_node_id_parse(&idC, "127.0.0.1:8003") == 0); // 22 bytes

  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, &idA, 1, 1) == 0);
  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, &idB, 1, 1) == 0);
  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, &idC, 1, 1) == 0);

  swim_member_t out[5];
  // Budget is 50 bytes. 22 * 2 = 44 bytes (fits), 22 * 3 = 66 bytes (exceeds).
  // So it should pack exactly 2 events.
  int packed = swim_gossip_queue_pack(q, 5, 50, out, 5);
  CHECK(packed == 2);
  CHECK(swim_gossip_queue_size(q) ==
        3); // Since transmit count hasn't reached limit, all 3 are still in
            // queue (2 have tc=1, 1 has tc=0)

  swim_gossip_queue_final(q);
}

TEST_CASE("gossip_queue: transmit limit pruning") {
  swim_gossip_queue_t *q = swim_gossip_queue_init();
  REQUIRE(q != nullptr);

  swim_node_id_t idA;
  REQUIRE(swim_node_id_parse(&idA, "127.0.0.1:8001") == 0);

  // Cluster size N = 3.
  // limit = ceil(log2(N + 1)) * 3 = ceil(log2(4)) * 3 = 2 * 3 = 6.
  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, &idA, 1, 1) == 0);

  swim_member_t out[1];
  for (int i = 0; i < 5; i++) {
    int packed = swim_gossip_queue_pack(q, 3, 100, out, 1);
    REQUIRE(packed == 1);
    CHECK(swim_gossip_queue_size(q) == 1); // Not yet pruned
  }

  // 6th pack reaches the transmit limit of 6
  int packed = swim_gossip_queue_pack(q, 3, 100, out, 1);
  REQUIRE(packed == 1);
  CHECK(swim_gossip_queue_size(q) == 0); // Pruned from the queue!

  swim_gossip_queue_final(q);
}

TEST_CASE("gossip_queue: transmit limit pruning with multiplier") {
  swim_gossip_queue_t *q = swim_gossip_queue_init();
  REQUIRE(q != nullptr);

  swim_node_id_t idA;
  REQUIRE(swim_node_id_parse(&idA, "127.0.0.1:8001") == 0);

  // Cluster size N = 3 -> limit = 6.
  // Multiplier = 2 -> total limit = 12.
  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, &idA, 1, 2) == 0);

  swim_member_t out[1];
  for (int i = 0; i < 11; i++) {
    int packed = swim_gossip_queue_pack(q, 3, 100, out, 1);
    REQUIRE(packed == 1);
    CHECK(swim_gossip_queue_size(q) == 1); // Not yet pruned
  }

  // 12th pack reaches the total limit of 12
  int packed = swim_gossip_queue_pack(q, 3, 100, out, 1);
  REQUIRE(packed == 1);
  CHECK(swim_gossip_queue_size(q) == 0); // Pruned!

  swim_gossip_queue_final(q);
}
