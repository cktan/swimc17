#include "doctest.h"

extern "C" {
#include "swim_errno.h"
#include "swim_gossip_queue.h"
#include "swim_node_id.h"
}

#include <cstring>
#include <string>
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
  // 2 bytes header + 28 bytes event = 30 bytes.
  char buf[50];
  int packed_bytes = swim_gossip_queue_pack_ex(q, 5, buf, 50);
  REQUIRE(packed_bytes == 30);
  
  uint16_t count = ((uint8_t)buf[0] << 8) | (uint8_t)buf[1];
  CHECK(count == 1);

  // The packed event is idA because of stable tie-breaker
  swim_member_t peek[2];
  int peek_count = swim_gossip_queue_peek(q, peek, 2);
  REQUIRE(peek_count == 2);
  // idB should now be first because it has transmit_count 0, whereas idA has transmit_count 1
  CHECK(swim_node_id_compare(&peek[0].id, &idB) == 0);
  CHECK(swim_node_id_compare(&peek[1].id, &idA) == 0);

  // Pack again with a budget that only fits one event.
  packed_bytes = swim_gossip_queue_pack_ex(q, 5, buf, 50);
  REQUIRE(packed_bytes == 30);
  count = ((uint8_t)buf[0] << 8) | (uint8_t)buf[1];
  CHECK(count == 1);

  // Now both idA and idB have transmit_count 1. So tie-breaker node ID comparison puts idA first again
  peek_count = swim_gossip_queue_peek(q, peek, 2);
  REQUIRE(peek_count == 2);
  CHECK(swim_node_id_compare(&peek[0].id, &idA) == 0);
  CHECK(swim_node_id_compare(&peek[1].id, &idB) == 0);

  swim_gossip_queue_final(q);
}

TEST_CASE("gossip_queue: event size limit packing budget") {
  swim_gossip_queue_t *q = swim_gossip_queue_init();
  REQUIRE(q != nullptr);

  swim_node_id_t idA, idB, idC;
  REQUIRE(swim_node_id_parse(&idA, "127.0.0.1:8001") == 0); // 28 bytes
  REQUIRE(swim_node_id_parse(&idB, "127.0.0.1:8002") == 0); // 28 bytes
  REQUIRE(swim_node_id_parse(&idC, "127.0.0.1:8003") == 0); // 28 bytes

  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, &idA, 1, 1) == 0);
  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, &idB, 1, 1) == 0);
  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, &idC, 1, 1) == 0);

  char buf[200];
  // Budget is 60 bytes. 
  // 2 bytes header + 28 bytes = 30 bytes (fits)
  // 2 bytes header + 28 * 2 = 58 bytes (fits)
  // 2 bytes header + 28 * 3 = 86 bytes (exceeds)
  // So it should pack exactly 2 events.
  int packed_bytes = swim_gossip_queue_pack_ex(q, 5, buf, 60);
  CHECK(packed_bytes == 58);
  uint16_t count = ((uint8_t)buf[0] << 8) | (uint8_t)buf[1];
  CHECK(count == 2);
  
  CHECK(swim_gossip_queue_size(q) == 3); // Since transmit count hasn't reached limit, all 3 are still in queue

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

  char buf[100];
  for (int i = 0; i < 5; i++) {
    int packed_bytes = swim_gossip_queue_pack_ex(q, 3, buf, 100);
    REQUIRE(packed_bytes == 30);
    uint16_t count = ((uint8_t)buf[0] << 8) | (uint8_t)buf[1];
    REQUIRE(count == 1);
    CHECK(swim_gossip_queue_size(q) == 1); // Not yet pruned
  }

  // 6th pack reaches the transmit limit of 6
  int packed_bytes = swim_gossip_queue_pack_ex(q, 3, buf, 100);
  REQUIRE(packed_bytes == 30);
  uint16_t count = ((uint8_t)buf[0] << 8) | (uint8_t)buf[1];
  REQUIRE(count == 1);
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

  char buf[100];
  for (int i = 0; i < 11; i++) {
    int packed_bytes = swim_gossip_queue_pack_ex(q, 3, buf, 100);
    REQUIRE(packed_bytes == 30);
    uint16_t count = ((uint8_t)buf[0] << 8) | (uint8_t)buf[1];
    REQUIRE(count == 1);
    CHECK(swim_gossip_queue_size(q) == 1); // Not yet pruned
  }

  // 12th pack reaches the total limit of 12
  int packed_bytes = swim_gossip_queue_pack_ex(q, 3, buf, 100);
  REQUIRE(packed_bytes == 30);
  uint16_t count = ((uint8_t)buf[0] << 8) | (uint8_t)buf[1];
  REQUIRE(count == 1);
  CHECK(swim_gossip_queue_size(q) == 0); // Pruned!

  swim_gossip_queue_final(q);
}

TEST_CASE("gossip_queue: swim_gossip_queue_pack_ex basic encoding") {
  swim_gossip_queue_t *q = swim_gossip_queue_init();
  REQUIRE(q != nullptr);

  // 1. Pack empty queue
  char buf[512];
  int rc = swim_gossip_queue_pack_ex(q, 3, buf, sizeof(buf));
  CHECK(rc == 2);
  CHECK(((uint8_t)buf[0] << 8 | (uint8_t)buf[1]) == 0);

  // 2. Add a member
  swim_node_id_t id1;
  REQUIRE(swim_node_id_parse(&id1, "127.0.0.1:8001:cookie1") == 0);
  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, &id1, 42, 1) == 0);

  memset(buf, 0, sizeof(buf));
  rc = swim_gossip_queue_pack_ex(q, 3, buf, sizeof(buf));
  CHECK(rc == 37);

  // Verify the layout:
  // Offset 0: count (uint16) -> 1
  uint8_t *p = (uint8_t *)buf;
  CHECK((p[0] << 8 | p[1]) == 1);

  // Offset 2: member_body_size (uint16)
  uint16_t member_body_size = p[2] << 8 | p[3];
  p += 4; // now pointing to member BODY (node_id)

  // node_id_body_size (uint16)
  uint16_t node_id_body_size = p[0] << 8 | p[1];
  p += 2; // now pointing to node ID BODY

  // host_len (uint16)
  uint16_t host_len = p[0] << 8 | p[1];
  CHECK(host_len == 9);
  p += 2;
  CHECK(std::string((char *)p, host_len) == "127.0.0.1");
  p += host_len;

  // port (uint16)
  uint16_t port = p[0] << 8 | p[1];
  CHECK(port == 8001);
  p += 2;

  // cookie_len (uint16)
  uint16_t cookie_len = p[0] << 8 | p[1];
  CHECK(cookie_len == 7);
  p += 2;
  CHECK(std::string((char *)p, cookie_len) == "cookie1");
  p += cookie_len;

  // status (uint8)
  CHECK(p[0] == SWIM_STATUS_ALIVE);
  p += 1;

  // incarnation (uint64)
  uint64_t incarnation = ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
                         ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
                         ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
                         ((uint64_t)p[6] << 8) | (uint64_t)p[7];
  CHECK(incarnation == 42);
  p += 8;

  // Verify total sizes match offset differences
  CHECK(member_body_size == (p - ((uint8_t *)buf + 4)));
  CHECK(node_id_body_size == (p - 9 - ((uint8_t *)buf + 6)));

  // 3. Error case: buffer too small for header
  rc = swim_gossip_queue_pack_ex(q, 3, buf, 1);
  CHECK(rc == -1);

  swim_gossip_queue_final(q);
}

TEST_CASE("gossip_queue: swim_gossip_queue_pack_ex budget limits") {
  swim_gossip_queue_t *q = swim_gossip_queue_init();
  REQUIRE(q != nullptr);

  swim_node_id_t id1, id2;
  REQUIRE(swim_node_id_parse(&id1, "127.0.0.1:8001:cookie1") == 0); // length of event: 19 + 9 + 7 = 35 bytes
  REQUIRE(swim_node_id_parse(&id2, "127.0.0.1:8002:cookie2") == 0); // length of event: 35 bytes

  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, &id1, 10, 1) == 0);
  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, &id2, 10, 1) == 0);

  char buf[50]; // Fits 1 event (2 bytes header + 35 bytes event = 37 bytes) but not both (2 + 35 + 35 = 72 bytes)
  int rc = swim_gossip_queue_pack_ex(q, 3, buf, 50);
  CHECK(rc == 37); // Only one event packed, consuming 37 bytes

  // Remaining queue should have size 2 (since the one packed hasn't reached limit, and the other wasn't packed)
  CHECK(swim_gossip_queue_size(q) == 2);

  swim_gossip_queue_final(q);
}

