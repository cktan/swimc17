#include "doctest.h"

extern "C" {
#include "swim.h"
#include "swim_codec.h"
#include "swim_errno.h"
#include "swim_gossip_queue.h"
#include "swim_nodeid.h"
}

#include <arpa/inet.h>
#include <cstring>
#include <string>

TEST_CASE("codec: ping/ack roundtrip without events") {
  swim_nodeid_idx_t sender = swim_nodeid_find("127.0.0.1:8001/my_cookie");
  REQUIRE(nodeid_valid(sender));

  uint8_t buf[SWIM_MAX_PACKET_SIZE];
  int enc_len = swim_pack_message(SWIM_MSG_PING, sender, 420, SWIM_NODEID_NONE,
                                  nullptr, 0, buf, sizeof(buf));
  REQUIRE(enc_len > 0);

  swim_message_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int rc = swim_unpack_message(buf, enc_len, &decoded);
  REQUIRE(rc == 0);

  CHECK(decoded.type == SWIM_MSG_PING);
  CHECK(decoded.seq == 420);
  CHECK(decoded.gossip_count == 0);
  CHECK(nodeid_eq(decoded.sender, sender));
}

TEST_CASE("codec: ping_req/fwd_ack roundtrip without events") {
  swim_nodeid_idx_t sender = swim_nodeid_find("[::1]:9001/sender_cookie");
  swim_nodeid_idx_t peer = swim_nodeid_find("[::1]:9002/target_cookie");
  REQUIRE(nodeid_valid(sender));
  REQUIRE(nodeid_valid(peer));

  uint8_t buf[SWIM_MAX_PACKET_SIZE];
  int enc_len = swim_pack_message(SWIM_MSG_PING_REQ, sender, 1001, peer,
                                  nullptr, 0, buf, sizeof(buf));
  REQUIRE(enc_len > 0);

  swim_message_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int rc = swim_unpack_message(buf, enc_len, &decoded);
  REQUIRE(rc == 0);

  CHECK(decoded.type == SWIM_MSG_PING_REQ);
  CHECK(decoded.seq == 1001);
  CHECK(decoded.gossip_count == 0);
  CHECK(nodeid_eq(decoded.sender, sender));
  CHECK(nodeid_eq(decoded.peer, peer));
}

TEST_CASE("codec: full message roundtrip with events") {
  swim_nodeid_idx_t sender = swim_nodeid_find("192.168.0.10:80");
  REQUIRE(nodeid_valid(sender));

  swim_gossip_queue_t *q = swim_gossip_queue_create();
  REQUIRE(q != nullptr);

  swim_nodeid_idx_t id1 = swim_nodeid_find("192.168.0.11:81/c1");
  swim_nodeid_idx_t id2 = swim_nodeid_find("192.168.0.12:82/c2");
  swim_nodeid_idx_t id3 = swim_nodeid_find("192.168.0.13:83/c3");
  REQUIRE(nodeid_valid(id1));
  REQUIRE(nodeid_valid(id2));
  REQUIRE(nodeid_valid(id3));

  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, id1, 123456789ULL,
                                    1) == 0);
  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_SUSPECT, id2, 55ULL, 1) ==
          0);
  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_DEAD, id3, 999ULL, 1) == 0);

  uint8_t buf[SWIM_MAX_PACKET_SIZE];
  int enc_len = swim_pack_message(SWIM_MSG_ACK, sender, 99999, SWIM_NODEID_NONE,
                                  q, 3, buf, sizeof(buf));
  REQUIRE(enc_len > 0);

  swim_message_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int rc = swim_unpack_message(buf, enc_len, &decoded);
  REQUIRE(rc == 0);

  CHECK(decoded.type == SWIM_MSG_ACK);
  CHECK(decoded.seq == 99999);
  CHECK(decoded.gossip_count == 3);
  CHECK(nodeid_eq(decoded.sender, sender));

  // Since qsort sorts them by priority (DEAD (0) > SUSPECT (1) > ALIVE (2)),
  // the events order will be:
  // 1. DEAD (id3)
  // 2. SUSPECT (id2)
  // 3. ALIVE (id1)
  CHECK(decoded.gossip[0].status == SWIM_STATUS_DEAD);
  CHECK(decoded.gossip[0].incarnation == 999ULL);
  CHECK(nodeid_eq(decoded.gossip[0].id, id3));

  CHECK(decoded.gossip[1].status == SWIM_STATUS_SUSPECT);
  CHECK(decoded.gossip[1].incarnation == 55ULL);
  CHECK(nodeid_eq(decoded.gossip[1].id, id2));

  CHECK(decoded.gossip[2].status == SWIM_STATUS_ALIVE);
  CHECK(decoded.gossip[2].incarnation == 123456789ULL);
  CHECK(nodeid_eq(decoded.gossip[2].id, id1));

  swim_gossip_queue_destroy(q);
}

TEST_CASE("codec: error validation and bounds checking") {
  swim_nodeid_idx_t sender = swim_nodeid_find("127.0.0.1:8001");
  REQUIRE(nodeid_valid(sender));

  uint8_t buf[256];
  int enc_len = swim_pack_message(SWIM_MSG_PING, sender, 500, SWIM_NODEID_NONE,
                                  nullptr, 0, buf, sizeof(buf));
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
  // Wire layout: type(1) + seq(4) + known(2) + nodeid_str_len(2) + ...
  // Offset 7 is the high byte of the nodeid string length.
  buf[7] = 250; // makes string_len huge, overflows bounds check
  rc = swim_unpack_message(buf, enc_len, &decoded);
  CHECK(rc == -1);

  // Restore clean errno state so other test cases are not affected
  swim_set_error(SWIM_OK, nullptr);
}

TEST_CASE("codec: swim_pack_membership") {
  swim_member_t member;
  member.id = swim_nodeid_find("127.0.0.1:8001/my_cookie");
  REQUIRE(nodeid_valid(member.id));
  member.status = SWIM_STATUS_ALIVE;
  member.incarnation = 42;
  member.dead_at = 12345; // Should be ignored

  // Wire: [str_len(2)] ["127.0.0.1:8001/my_cookie"(24)] [status(1)]
  //       [incarnation(8)] = 35 bytes
  uint8_t buf[256];
  int rc = swim_pack_membership(&member, buf, buf + sizeof(buf));
  REQUIRE(rc == 35);

  // Decode/check the fields manually
  CHECK(buf[0] == 0x00);
  CHECK(buf[1] == 0x18); // 24 = 0x18
  CHECK(std::string((char *)buf + 2, 24) == "127.0.0.1:8001/my_cookie");
  CHECK(buf[26] == SWIM_STATUS_ALIVE);
  CHECK(buf[27] == 0);
  CHECK(buf[34] == 42);

  // Exact-fit encode must succeed
  rc = swim_pack_membership(&member, buf, buf + 35);
  CHECK(rc == 35);

  // Overflow check
  rc = swim_pack_membership(&member, buf, buf + 34);
  CHECK(rc == -1);
}

TEST_CASE("codec: fwd_ack roundtrip") {
  swim_nodeid_idx_t sender = swim_nodeid_find("10.0.0.1:7001/s");
  swim_nodeid_idx_t peer = swim_nodeid_find("10.0.0.2:7002/p");
  REQUIRE(nodeid_valid(sender));
  REQUIRE(nodeid_valid(peer));

  uint8_t buf[SWIM_MAX_PACKET_SIZE];
  int enc_len = swim_pack_message(SWIM_MSG_FWD_ACK, sender, 777, peer, nullptr,
                                  0, buf, sizeof(buf));
  REQUIRE(enc_len > 0);

  swim_message_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int rc = swim_unpack_message(buf, enc_len, &decoded);
  REQUIRE(rc == 0);

  CHECK(decoded.type == SWIM_MSG_FWD_ACK);
  CHECK(decoded.seq == 777);
  CHECK(decoded.gossip_count == 0);
  CHECK(nodeid_eq(decoded.sender, sender));
  CHECK(nodeid_eq(decoded.peer, peer));
}

TEST_CASE("codec: gossip with an unknown status is rejected") {
  swim_nodeid_idx_t sender = swim_nodeid_find("10.0.0.1:7001/s");
  swim_nodeid_idx_t other = swim_nodeid_find("10.0.0.3:7003/o");
  REQUIRE(nodeid_valid(sender));
  REQUIRE(nodeid_valid(other));

  uint8_t buf[SWIM_MAX_PACKET_SIZE];
  int len = swim_pack_message(SWIM_MSG_PING, sender, 1, SWIM_NODEID_NONE,
                              nullptr, 0, buf, sizeof(buf));
  REQUIRE(len > 0);
  swim_member_t ev;
  memset(&ev, 0, sizeof(ev));
  ev.id = other;
  ev.status = 'X';
  ev.incarnation = 9;
  int n = swim_pack_membership(&ev, buf + len, buf + sizeof(buf));
  REQUIRE(n > 0);

  swim_message_t decoded;
  CHECK(swim_unpack_message(buf, len + n, &decoded) == -1);
}

// A record rejected for its status must not register its node ID: pool
// slots are limited and never freed.
TEST_CASE("codec: rejected gossip record does not register its node id") {
  swim_nodeid_idx_t sender = swim_nodeid_find("10.0.0.1:7001/s");
  REQUIRE(nodeid_valid(sender));

  uint8_t buf[SWIM_MAX_PACKET_SIZE];
  int len = swim_pack_message(SWIM_MSG_PING, sender, 1, SWIM_NODEID_NONE,
                              nullptr, 0, buf, sizeof(buf));
  REQUIRE(len > 0);

  // Hand-encode [len 2B][id][status 1B][incarnation 8B] so the id is never
  // registered by the encoder.
  const char *id = "10.9.9.9:7009/never-registered";
  uint16_t idlen = htons((uint16_t)strlen(id));
  memcpy(buf + len, &idlen, 2);
  len += 2;
  memcpy(buf + len, id, strlen(id));
  len += (int)strlen(id);
  buf[len++] = 'X';
  memset(buf + len, 0, 8);
  len += 8;

  swim_message_t decoded;
  CHECK(swim_unpack_message(buf, len, &decoded) == -1);
  CHECK(!nodeid_valid(swim_nodeid_get(id)));
}

// Every message carries the sender's view of the cluster size (members it
// sees as alive or suspect, plus itself), so a receiver can tell when the
// sender is behind.
TEST_CASE("codec: known cluster size roundtrip") {
  swim_nodeid_idx_t sender = swim_nodeid_find("10.0.0.1:7001/s");
  REQUIRE(nodeid_valid(sender));
  uint8_t buf[SWIM_MAX_PACKET_SIZE];
  swim_message_t decoded;

  int len = swim_pack_message(SWIM_MSG_ACK, sender, 1, SWIM_NODEID_NONE,
                              nullptr, 41, buf, sizeof(buf));
  REQUIRE(len > 0);
  REQUIRE(swim_unpack_message(buf, len, &decoded) == 0);
  CHECK(decoded.known == 42);

  // Saturates rather than wrapping.
  len = swim_pack_message(SWIM_MSG_ACK, sender, 1, SWIM_NODEID_NONE, nullptr,
                          100000, buf, sizeof(buf));
  REQUIRE(swim_unpack_message(buf, len, &decoded) == 0);
  CHECK(decoded.known == 65535);
}

TEST_CASE("codec: sync roundtrip carries membership records") {
  swim_nodeid_idx_t sender = swim_nodeid_find("10.0.0.1:7001/s");
  swim_nodeid_idx_t a = swim_nodeid_find("10.0.0.5:7005/a");
  swim_nodeid_idx_t b = swim_nodeid_find("10.0.0.6:7006/b");
  swim_gossip_queue_t *q = swim_gossip_queue_create();
  REQUIRE(q != nullptr);
  swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, a, 5, 1);
  swim_gossip_queue_enqueue(q, SWIM_STATUS_SUSPECT, b, 6, 1);

  uint8_t buf[SWIM_MAX_PACKET_SIZE];
  int len = swim_pack_message(SWIM_MSG_SYNC, sender, 9, SWIM_NODEID_NONE, q, 2,
                              buf, sizeof(buf));
  swim_gossip_queue_destroy(q);
  REQUIRE(len > 0);

  swim_message_t decoded;
  REQUIRE(swim_unpack_message(buf, len, &decoded) == 0);
  CHECK(decoded.type == SWIM_MSG_SYNC);
  CHECK(decoded.known == 3);
  CHECK(decoded.gossip_count == 2);
}
