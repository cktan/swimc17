#include "doctest.h"

extern "C" {
#include "swim_codec.h"
#include "swim_errno.h"
#include "swim_node_id.h"
}

#include <cstring>

TEST_CASE("codec: ping/ack roundtrip without events") {
  swim_message_t orig;
  memset(&orig, 0, sizeof(orig));
  orig.type = SWIM_MSG_PING;
  orig.seq = 420;
  orig.event_count = 0;
  REQUIRE(swim_node_id_parse(&orig.sender, "127.0.0.1:8001:my_cookie") == 0);

  uint8_t buf[SWIM_MAX_PACKET_SIZE];
  int enc_len = swim_codec_encode(&orig, buf, sizeof(buf));
  REQUIRE(enc_len > 0);

  swim_message_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int rc = swim_codec_decode(buf, enc_len, &decoded);
  REQUIRE(rc == 0);

  CHECK(decoded.type == SWIM_MSG_PING);
  CHECK(decoded.seq == 420);
  CHECK(decoded.event_count == 0);
  CHECK(swim_node_id_compare(&decoded.sender, &orig.sender) == 0);
}

TEST_CASE("codec: ping_req/fwd_ack roundtrip without events") {
  swim_message_t orig;
  memset(&orig, 0, sizeof(orig));
  orig.type = SWIM_MSG_PING_REQ;
  orig.seq = 1001;
  orig.event_count = 0;
  REQUIRE(swim_node_id_parse(&orig.sender, "[::1]:9001:sender_cookie") == 0);
  REQUIRE(swim_node_id_parse(&orig.peer, "[::1]:9002:target_cookie") == 0);

  uint8_t buf[SWIM_MAX_PACKET_SIZE];
  int enc_len = swim_codec_encode(&orig, buf, sizeof(buf));
  REQUIRE(enc_len > 0);

  swim_message_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int rc = swim_codec_decode(buf, enc_len, &decoded);
  REQUIRE(rc == 0);

  CHECK(decoded.type == SWIM_MSG_PING_REQ);
  CHECK(decoded.seq == 1001);
  CHECK(decoded.event_count == 0);
  CHECK(swim_node_id_compare(&decoded.sender, &orig.sender) == 0);
  CHECK(swim_node_id_compare(&decoded.peer, &orig.peer) == 0);
}

TEST_CASE("codec: full message roundtrip with events") {
  swim_message_t orig;
  memset(&orig, 0, sizeof(orig));
  orig.type = SWIM_MSG_ACK;
  orig.seq = 99999;
  orig.event_count = 3;
  REQUIRE(swim_node_id_parse(&orig.sender, "192.168.0.10:80") == 0);

  // Event 1: ALIVE
  orig.events[0].status = SWIM_STATUS_ALIVE;
  orig.events[0].incarnation = 123456789ULL;
  REQUIRE(swim_node_id_parse(&orig.events[0].id, "192.168.0.11:81:c1") == 0);

  // Event 2: SUSPECT
  orig.events[1].status = SWIM_STATUS_SUSPECT;
  orig.events[1].incarnation = 55ULL;
  REQUIRE(swim_node_id_parse(&orig.events[1].id, "192.168.0.12:82:c2") == 0);

  // Event 3: DEAD
  orig.events[2].status = SWIM_STATUS_DEAD;
  orig.events[2].incarnation = 999ULL;
  REQUIRE(swim_node_id_parse(&orig.events[2].id, "192.168.0.13:83:c3") == 0);

  uint8_t buf[SWIM_MAX_PACKET_SIZE];
  int enc_len = swim_codec_encode(&orig, buf, sizeof(buf));
  REQUIRE(enc_len > 0);

  swim_message_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int rc = swim_codec_decode(buf, enc_len, &decoded);
  REQUIRE(rc == 0);

  CHECK(decoded.type == SWIM_MSG_ACK);
  CHECK(decoded.seq == 99999);
  CHECK(decoded.event_count == 3);
  CHECK(swim_node_id_compare(&decoded.sender, &orig.sender) == 0);

  // Event 1 verification
  CHECK(decoded.events[0].status == SWIM_STATUS_ALIVE);
  CHECK(decoded.events[0].incarnation == 123456789ULL);
  CHECK(swim_node_id_compare(&decoded.events[0].id, &orig.events[0].id) == 0);

  // Event 2 verification
  CHECK(decoded.events[1].status == SWIM_STATUS_SUSPECT);
  CHECK(decoded.events[1].incarnation == 55ULL);
  CHECK(swim_node_id_compare(&decoded.events[1].id, &orig.events[1].id) == 0);

  // Event 3 verification
  CHECK(decoded.events[2].status == SWIM_STATUS_DEAD);
  CHECK(decoded.events[2].incarnation == 999ULL);
  CHECK(swim_node_id_compare(&decoded.events[2].id, &orig.events[2].id) == 0);
}

TEST_CASE("codec: error validation and bounds checking") {
  swim_message_t orig;
  memset(&orig, 0, sizeof(orig));
  orig.type = SWIM_MSG_PING;
  orig.seq = 500;
  orig.event_count = 0;
  REQUIRE(swim_node_id_parse(&orig.sender, "127.0.0.1:8001") == 0);

  uint8_t buf[256];

  // 1. Buffer too small for encode
  // Minimal encode size is: 1 (type) + 4 (seq) + 1 (count) + 1 (host_len) + 9 (host) + 2 (port) + 1 (cookie_len) + 0 (cookie) = 19 bytes.
  int enc_len = swim_codec_encode(&orig, buf, 10);
  CHECK(enc_len == -1);

  enc_len = swim_codec_encode(&orig, buf, sizeof(buf));
  REQUIRE(enc_len >= 19);

  // 2. Buffer too small for decode
  swim_message_t decoded;
  int rc = swim_codec_decode(buf, 5, &decoded);
  CHECK(rc == -1);

  // 3. Corrupt message type value
  buf[0] = 99; // invalid type
  rc = swim_codec_decode(buf, enc_len, &decoded);
  CHECK(rc == -1);

  // Reset type back to normal
  buf[0] = SWIM_MSG_PING;

  // 4. Corrupt event count (too high)
  buf[5] = 99; // event count 99 exceeds SWIM_MAX_EVENTS (64)
  rc = swim_codec_decode(buf, enc_len, &decoded);
  CHECK(rc == -1);

  // 5. Corrupt node ID length leading to out-of-bounds
  buf[5] = 0; // count 0
  buf[6] = 250; // host_len 250 (which overflows bounds check)
  rc = swim_codec_decode(buf, enc_len, &decoded);
  CHECK(rc == -1);

  // Restore clean errno state so other test cases are not affected
  swim_set_error(SWIM_OK, nullptr);
}
