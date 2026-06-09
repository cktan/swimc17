#include "doctest.h"

extern "C" {
#include "swim.h"
#include "swim_codec.h"
#include "swim_errno.h"
#include "swim_gossip_queue.h"
}

#include <cstring>
#include <string>

TEST_CASE("codec: ping/ack roundtrip without events") {
  swim_node_id_t sender;
  REQUIRE(swim_node_id_parse(&sender, "127.0.0.1:8001/my_cookie") == 0);

  uint8_t buf[SWIM_MAX_PACKET_SIZE];
  int enc_len = swim_pack_message(SWIM_MSG_PING, &sender, 420, nullptr, nullptr,
                                  0, buf, sizeof(buf));
  REQUIRE(enc_len > 0);

  swim_message_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int rc = swim_unpack_message(buf, enc_len, &decoded);
  REQUIRE(rc == 0);

  CHECK(decoded.type == SWIM_MSG_PING);
  CHECK(decoded.seq == 420);
  CHECK(decoded.gossip_count == 0);
  CHECK(swim_node_id_compare(&decoded.sender, &sender) == 0);
}

TEST_CASE("codec: ping_req/fwd_ack roundtrip without events") {
  swim_node_id_t sender, peer;
  REQUIRE(swim_node_id_parse(&sender, "[::1]:9001/sender_cookie") == 0);
  REQUIRE(swim_node_id_parse(&peer, "[::1]:9002/target_cookie") == 0);

  uint8_t buf[SWIM_MAX_PACKET_SIZE];
  int enc_len = swim_pack_message(SWIM_MSG_PING_REQ, &sender, 1001, &peer,
                                  nullptr, 0, buf, sizeof(buf));
  REQUIRE(enc_len > 0);

  swim_message_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int rc = swim_unpack_message(buf, enc_len, &decoded);
  REQUIRE(rc == 0);

  CHECK(decoded.type == SWIM_MSG_PING_REQ);
  CHECK(decoded.seq == 1001);
  CHECK(decoded.gossip_count == 0);
  CHECK(swim_node_id_compare(&decoded.sender, &sender) == 0);
  CHECK(swim_node_id_compare(&decoded.peer, &peer) == 0);
}

TEST_CASE("codec: full message roundtrip with events") {
  swim_node_id_t sender;
  REQUIRE(swim_node_id_parse(&sender, "192.168.0.10:80") == 0);

  swim_gossip_queue_t *q = swim_gossip_queue_create();
  REQUIRE(q != nullptr);

  swim_node_id_t id1, id2, id3;
  REQUIRE(swim_node_id_parse(&id1, "192.168.0.11:81/c1") == 0);
  REQUIRE(swim_node_id_parse(&id2, "192.168.0.12:82/c2") == 0);
  REQUIRE(swim_node_id_parse(&id3, "192.168.0.13:83/c3") == 0);

  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, &id1, 123456789ULL,
                                    1) == 0);
  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_SUSPECT, &id2, 55ULL, 1) ==
          0);
  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_DEAD, &id3, 999ULL, 1) == 0);

  uint8_t buf[SWIM_MAX_PACKET_SIZE];
  int enc_len = swim_pack_message(SWIM_MSG_ACK, &sender, 99999, nullptr, q, 3,
                                  buf, sizeof(buf));
  REQUIRE(enc_len > 0);

  swim_message_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int rc = swim_unpack_message(buf, enc_len, &decoded);
  REQUIRE(rc == 0);

  CHECK(decoded.type == SWIM_MSG_ACK);
  CHECK(decoded.seq == 99999);
  CHECK(decoded.gossip_count == 3);
  CHECK(swim_node_id_compare(&decoded.sender, &sender) == 0);

  // Since qsort sorts them by priority (DEAD (0) > SUSPECT (1) > ALIVE (2)),
  // the events order will be:
  // 1. DEAD (id3)
  // 2. SUSPECT (id2)
  // 3. ALIVE (id1)
  CHECK(decoded.gossip[0].status == SWIM_STATUS_DEAD);
  CHECK(decoded.gossip[0].incarnation == 999ULL);
  CHECK(swim_node_id_compare(&decoded.gossip[0].id, &id3) == 0);

  CHECK(decoded.gossip[1].status == SWIM_STATUS_SUSPECT);
  CHECK(decoded.gossip[1].incarnation == 55ULL);
  CHECK(swim_node_id_compare(&decoded.gossip[1].id, &id2) == 0);

  CHECK(decoded.gossip[2].status == SWIM_STATUS_ALIVE);
  CHECK(decoded.gossip[2].incarnation == 123456789ULL);
  CHECK(swim_node_id_compare(&decoded.gossip[2].id, &id1) == 0);

  swim_gossip_queue_destroy(q);
}

TEST_CASE("codec: error validation and bounds checking") {
  swim_node_id_t sender;
  REQUIRE(swim_node_id_parse(&sender, "127.0.0.1:8001") == 0);

  uint8_t buf[256];
  int enc_len = swim_pack_message(SWIM_MSG_PING, &sender, 500, nullptr, nullptr,
                                  0, buf, sizeof(buf));
  REQUIRE(enc_len > 0);

  // 1. Buffer too small for decode
  swim_message_t decoded;
  int rc = swim_unpack_message(buf, 5, &decoded);
  CHECK(rc == -1);

  // 2. Corrupt message type value
  buf[0] = 99; // invalid type
  rc = swim_unpack_message(buf, enc_len, &decoded);
  CHECK(rc == -1);

  // Reset type back to normal
  buf[0] = SWIM_MSG_PING;

  // 3. Corrupt node ID length leading to out-of-bounds
  // Offset of sender host_len is 5
  buf[5] = 250; // host_len 250 (which overflows bounds check)
  rc = swim_unpack_message(buf, enc_len, &decoded);
  CHECK(rc == -1);

  // Restore clean errno state so other test cases are not affected
  swim_set_error(SWIM_OK, nullptr);
}

TEST_CASE("codec: swim_pack_membership") {
  swim_member_t member;
  REQUIRE(swim_node_id_parse(&member.id, "127.0.0.1:8001/my_cookie") == 0);
  member.status = SWIM_STATUS_ALIVE;
  member.incarnation = 42;
  member.dead_at = 12345; // Should be ignored

  uint8_t buf[256];
  int rc = swim_pack_membership(&member, buf, buf + sizeof(buf));
  REQUIRE(rc == 33);

  // Decode/check the fields manually
  CHECK(buf[0] == 0x00);
  CHECK(buf[1] == 0x09);
  CHECK(std::string((char *)buf + 2, 9) == "127.0.0.1");
  CHECK(buf[11] == 0x1F);
  CHECK(buf[12] == 0x41);
  CHECK(buf[13] == 0x00);
  CHECK(buf[14] == 0x09);
  CHECK(std::string((char *)buf + 15, 9) == "my_cookie");
  CHECK(buf[24] == SWIM_STATUS_ALIVE);
  CHECK(buf[25] == 0);
  CHECK(buf[32] == 42);

  // Exact-fit encode must succeed (q is one past the end)
  rc = swim_pack_membership(&member, buf, buf + 33);
  CHECK(rc == 33);

  // Overflow check
  rc = swim_pack_membership(&member, buf, buf + 32);
  CHECK(rc == -1);
}
