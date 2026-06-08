#include "doctest.h"

extern "C" {
#include "swim_codec.h"
#include "swim_gossip_queue.h"
#include "swim.h"
}

#include <cstring>
#include <string>

TEST_CASE("codec: ping/ack roundtrip without events") {
  swim_node_id_t sender;
  REQUIRE(swim_node_id_parse(&sender, "127.0.0.1:8001/my_cookie") == 0);

  uint8_t buf[SWIM_MAX_PACKET_SIZE];
  int enc_len = swim_encode_message(SWIM_MSG_PING, &sender, 420, nullptr, nullptr, 0, buf, sizeof(buf));
  REQUIRE(enc_len > 0);

  swim_message_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int rc = swim_decode_message(buf, enc_len, &decoded);
  REQUIRE(rc == 0);

  CHECK(decoded.type == SWIM_MSG_PING);
  CHECK(decoded.seq == 420);
  CHECK(decoded.event_count == 0);
  CHECK(swim_node_id_compare(&decoded.sender, &sender) == 0);
}

TEST_CASE("codec: ping_req/fwd_ack roundtrip without events") {
  swim_node_id_t sender, peer;
  REQUIRE(swim_node_id_parse(&sender, "[::1]:9001/sender_cookie") == 0);
  REQUIRE(swim_node_id_parse(&peer, "[::1]:9002/target_cookie") == 0);

  uint8_t buf[SWIM_MAX_PACKET_SIZE];
  int enc_len = swim_encode_message(SWIM_MSG_PING_REQ, &sender, 1001, &peer, nullptr, 0, buf, sizeof(buf));
  REQUIRE(enc_len > 0);

  swim_message_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int rc = swim_decode_message(buf, enc_len, &decoded);
  REQUIRE(rc == 0);

  CHECK(decoded.type == SWIM_MSG_PING_REQ);
  CHECK(decoded.seq == 1001);
  CHECK(decoded.event_count == 0);
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

  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_ALIVE, &id1, 123456789ULL, 1) == 0);
  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_SUSPECT, &id2, 55ULL, 1) == 0);
  REQUIRE(swim_gossip_queue_enqueue(q, SWIM_STATUS_DEAD, &id3, 999ULL, 1) == 0);

  uint8_t buf[SWIM_MAX_PACKET_SIZE];
  int enc_len = swim_encode_message(SWIM_MSG_ACK, &sender, 99999, nullptr, q, 3, buf, sizeof(buf));
  REQUIRE(enc_len > 0);

  swim_message_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int rc = swim_decode_message(buf, enc_len, &decoded);
  REQUIRE(rc == 0);

  CHECK(decoded.type == SWIM_MSG_ACK);
  CHECK(decoded.seq == 99999);
  CHECK(decoded.event_count == 3);
  CHECK(swim_node_id_compare(&decoded.sender, &sender) == 0);

  // Since qsort sorts them by priority (DEAD (0) > SUSPECT (1) > ALIVE (2)), the events order will be:
  // 1. DEAD (id3)
  // 2. SUSPECT (id2)
  // 3. ALIVE (id1)
  CHECK(decoded.events[0].status == SWIM_STATUS_DEAD);
  CHECK(decoded.events[0].incarnation == 999ULL);
  CHECK(swim_node_id_compare(&decoded.events[0].id, &id3) == 0);

  CHECK(decoded.events[1].status == SWIM_STATUS_SUSPECT);
  CHECK(decoded.events[1].incarnation == 55ULL);
  CHECK(swim_node_id_compare(&decoded.events[1].id, &id2) == 0);

  CHECK(decoded.events[2].status == SWIM_STATUS_ALIVE);
  CHECK(decoded.events[2].incarnation == 123456789ULL);
  CHECK(swim_node_id_compare(&decoded.events[2].id, &id1) == 0);

  swim_gossip_queue_destroy(q);
}

TEST_CASE("codec: error validation and bounds checking") {
  swim_node_id_t sender;
  REQUIRE(swim_node_id_parse(&sender, "127.0.0.1:8001") == 0);

  uint8_t buf[256];
  int enc_len = swim_encode_message(SWIM_MSG_PING, &sender, 500, nullptr, nullptr, 0, buf, sizeof(buf));
  REQUIRE(enc_len > 0);

  // 1. Buffer too small for decode
  swim_message_t decoded;
  int rc = swim_decode_message(buf, 5, &decoded);
  CHECK(rc == -1);

  // 2. Corrupt message type value
  buf[0] = 99; // invalid type
  rc = swim_decode_message(buf, enc_len, &decoded);
  CHECK(rc == -1);

  // Reset type back to normal
  buf[0] = SWIM_MSG_PING;

  // 3. Corrupt node ID length leading to out-of-bounds
  // Offset of sender host_len is 5
  buf[5] = 250; // host_len 250 (which overflows bounds check)
  rc = swim_decode_message(buf, enc_len, &decoded);
  CHECK(rc == -1);

  // Restore clean errno state so other test cases are not affected
  swim_set_error(SWIM_OK, nullptr);
}

TEST_CASE("codec: integer encoding helpers") {
  uint8_t buf[16];

  // 1. Test swim_encode_int8
  int rc = swim_encode_int8(0x7F, buf, buf + sizeof(buf));
  CHECK(rc == 1);
  CHECK(buf[0] == 0x7F);

  // Overflow check
  rc = swim_encode_int8(0x7F, buf, buf);
  CHECK(rc == -1);

  // 2. Test swim_encode_int16
  rc = swim_encode_int16(0x1234, buf, buf + sizeof(buf));
  CHECK(rc == 2);
  CHECK(buf[0] == 0x12);
  CHECK(buf[1] == 0x34);

  // Overflow check
  rc = swim_encode_int16(0x1234, buf, buf + 1);
  CHECK(rc == -1);

  // Test swim_encode_int32
  rc = swim_encode_int32(0x12345678, buf, buf + sizeof(buf));
  CHECK(rc == 4);
  CHECK(buf[0] == 0x12);
  CHECK(buf[1] == 0x34);
  CHECK(buf[2] == 0x56);
  CHECK(buf[3] == 0x78);

  // Overflow check
  rc = swim_encode_int32(0x12345678, buf, buf + 3);
  CHECK(rc == -1);

  // 3. Test swim_encode_int64
  rc = swim_encode_int64(0x1122334455667788ULL, buf, buf + sizeof(buf));
  CHECK(rc == 8);
  CHECK(buf[0] == 0x11);
  CHECK(buf[1] == 0x22);
  CHECK(buf[2] == 0x33);
  CHECK(buf[3] == 0x44);
  CHECK(buf[4] == 0x55);
  CHECK(buf[5] == 0x66);
  CHECK(buf[6] == 0x77);
  CHECK(buf[7] == 0x88);

  // Overflow check
  rc = swim_encode_int64(0x1122334455667788ULL, buf, buf + 7);
  CHECK(rc == -1);

  // 4. Test swim_encode_string
  rc = swim_encode_string("hello", buf, buf + sizeof(buf));
  CHECK(rc == 7);
  CHECK(buf[0] == 0x00);
  CHECK(buf[1] == 0x05);
  CHECK(std::string((char *)buf + 2, 5) == "hello");

  // Overflow check
  rc = swim_encode_string("hello", buf, buf + 6);
  CHECK(rc == -1);
}

TEST_CASE("codec: swim_encode_membership") {
  swim_member_t member;
  REQUIRE(swim_node_id_parse(&member.id, "127.0.0.1:8001/my_cookie") == 0);
  member.status = SWIM_STATUS_ALIVE;
  member.incarnation = 42;
  member.dead_at = 12345; // Should be ignored

  uint8_t buf[256];
  int rc = swim_encode_membership(&member, buf, buf + sizeof(buf));
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
  rc = swim_encode_membership(&member, buf, buf + 33);
  CHECK(rc == 33);

  // Overflow check
  rc = swim_encode_membership(&member, buf, buf + 32);
  CHECK(rc == -1);
}

TEST_CASE("codec: decode helpers") {
  uint8_t buf[256] = {0};
  
  // 1. Test swim_decode_int8
  buf[0] = 0x55;
  uint8_t val8 = 0;
  int rc = swim_decode_int8(&val8, buf, buf + 1);
  CHECK(rc == 1);
  CHECK(val8 == 0x55);
  
  rc = swim_decode_int8(&val8, buf, buf);
  CHECK(rc == -1);

  // 2. Test swim_decode_int16
  buf[0] = 0x12;
  buf[1] = 0x34;
  uint16_t val16 = 0;
  rc = swim_decode_int16(&val16, buf, buf + 2);
  CHECK(rc == 2);
  CHECK(val16 == 0x1234);

  rc = swim_decode_int16(&val16, buf, buf + 1);
  CHECK(rc == -1);

  // 3. Test swim_decode_int32
  buf[0] = 0x12;
  buf[1] = 0x34;
  buf[2] = 0x56;
  buf[3] = 0x78;
  uint32_t val32 = 0;
  rc = swim_decode_int32(&val32, buf, buf + 4);
  CHECK(rc == 4);
  CHECK(val32 == 0x12345678);

  rc = swim_decode_int32(&val32, buf, buf + 3);
  CHECK(rc == -1);

  // 4. Test swim_decode_int64
  buf[0] = 0x11;
  buf[1] = 0x22;
  buf[2] = 0x33;
  buf[3] = 0x44;
  buf[4] = 0x55;
  buf[5] = 0x66;
  buf[6] = 0x77;
  buf[7] = 0x88;
  uint64_t val64 = 0;
  rc = swim_decode_int64(&val64, buf, buf + 8);
  CHECK(rc == 8);
  CHECK(val64 == 0x1122334455667788ULL);

  rc = swim_decode_int64(&val64, buf, buf + 7);
  CHECK(rc == -1);

  // 5. Test swim_decode_string
  buf[0] = 0x00;
  buf[1] = 0x05;
  memcpy(buf + 2, "hello", 5);
  char str[16];
  rc = swim_decode_string(str, sizeof(str), buf, buf + 7);
  CHECK(rc == 7);
  CHECK(std::string(str) == "hello");

  rc = swim_decode_string(str, sizeof(str), buf, buf + 6);
  CHECK(rc == -1);

  rc = swim_decode_string(str, 5, buf, buf + 7); // Dest too small for null term
  CHECK(rc == -1);

  // 6. Test swim_decode_node_id
  swim_node_id_t node;
  node.port = 8000;
  strcpy(node.host, "127.0.0.1");
  strcpy(node.cookie, "my_cookie");
  uint8_t enc_buf[256];
  int enc_len = swim_encode_node_id(&node, enc_buf, enc_buf + sizeof(enc_buf));
  REQUIRE(enc_len > 0);

  swim_node_id_t decoded_node;
  rc = swim_decode_node_id(&decoded_node, enc_buf, enc_buf + enc_len);
  REQUIRE(rc == enc_len);
  CHECK(swim_node_id_compare(&node, &decoded_node) == 0);

  rc = swim_decode_node_id(&decoded_node, enc_buf, enc_buf + enc_len - 1);
  CHECK(rc == -1);

  // 7. Test swim_decode_membership
  swim_member_t member;
  REQUIRE(swim_node_id_parse(&member.id, "127.0.0.1:8001/my_cookie") == 0);
  member.status = SWIM_STATUS_ALIVE;
  member.incarnation = 42;
  member.dead_at = 0;
  
  enc_len = swim_encode_membership(&member, enc_buf, enc_buf + sizeof(enc_buf));
  REQUIRE(enc_len > 0);

  swim_member_t decoded_member;
  rc = swim_decode_membership(&decoded_member, enc_buf, enc_buf + enc_len);
  REQUIRE(rc == enc_len);
  CHECK(swim_node_id_compare(&member.id, &decoded_member.id) == 0);
  CHECK(decoded_member.status == SWIM_STATUS_ALIVE);
  CHECK(decoded_member.incarnation == 42);
  CHECK(decoded_member.dead_at == 0);

  rc = swim_decode_membership(&decoded_member, enc_buf, enc_buf + enc_len - 1);
  CHECK(rc == -1);
}
