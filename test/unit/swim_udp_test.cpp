#include "doctest.h"

extern "C" {
#include "swim_errno.h"
#include "swim_udp.h"
}

#include <cstring>

TEST_CASE("udp: non-blocking receive on empty socket returns 0") {
  swim_udp_t *u = swim_udp_create("127.0.0.1", 18001);
  REQUIRE(u != nullptr);

  char src_host[256];
  uint16_t src_port;
  uint8_t buf[256];
  int rc = swim_udp_recv(u, src_host, &src_port, buf, sizeof(buf));
  CHECK(rc == 0); // EAGAIN / EWOULDBLOCK

  swim_udp_destroy(u);
}

TEST_CASE("udp: ipv4 loopback packet exchange") {
  swim_udp_t *u1 = swim_udp_create("127.0.0.1", 18002);
  swim_udp_t *u2 = swim_udp_create("127.0.0.1", 18003);
  REQUIRE(u1 != nullptr);
  REQUIRE(u2 != nullptr);

  const char *msg = "hello IPv4";
  int rc =
      swim_udp_send(u1, "127.0.0.1", 18003, (const uint8_t *)msg, strlen(msg));
  REQUIRE(rc == 0);

  char src_host[256];
  uint16_t src_port;
  uint8_t recv_buf[256];
  int n = 0;
  for (int i = 0; i < 100; i++) {
    n = swim_udp_recv(u2, src_host, &src_port, recv_buf, sizeof(recv_buf));
    if (n > 0)
      break;
  }

  REQUIRE(n == (int)strlen(msg));
  recv_buf[n] = '\0';
  CHECK(strcmp((const char *)recv_buf, msg) == 0);
  CHECK(strcmp(src_host, "127.0.0.1") == 0);
  CHECK(src_port == 18002);

  swim_udp_destroy(u1);
  swim_udp_destroy(u2);
}

TEST_CASE("udp: ipv6 loopback packet exchange") {
  // Try to bind IPv6, skip test if IPv6 loopback is not supported in this
  // environment
  swim_udp_t *u1 = swim_udp_create("::1", 18004);
  if (!u1) {
    // IPv6 might not be supported/enabled on this machine's loopback interface
    return;
  }
  swim_udp_t *u2 = swim_udp_create("::1", 18005);
  REQUIRE(u2 != nullptr);

  const char *msg = "hello IPv6";
  int rc = swim_udp_send(u1, "::1", 18005, (const uint8_t *)msg, strlen(msg));
  REQUIRE(rc == 0);

  char src_host[256];
  uint16_t src_port;
  uint8_t recv_buf[256];
  int n = 0;
  for (int i = 0; i < 100; i++) {
    n = swim_udp_recv(u2, src_host, &src_port, recv_buf, sizeof(recv_buf));
    if (n > 0)
      break;
  }

  REQUIRE(n == (int)strlen(msg));
  recv_buf[n] = '\0';
  CHECK(strcmp((const char *)recv_buf, msg) == 0);
  CHECK(strcmp(src_host, "::1") == 0);
  CHECK(src_port == 18004);

  swim_udp_destroy(u1);
  swim_udp_destroy(u2);
}

TEST_CASE("udp: invalid binding address fails cleanly") {
  // Bind to an invalid IP
  swim_udp_t *u = swim_udp_create("999.999.999.999", 18006);
  CHECK(u == nullptr);
  CHECK(swim_errno() != SWIM_OK);

  // Restore clean state for other tests
  swim_set_error(SWIM_OK, nullptr);
}

TEST_CASE("udp: swim_udp_fd returns valid descriptor") {
  swim_udp_t *u = swim_udp_create("127.0.0.1", 18007);
  REQUIRE(u != nullptr);

  CHECK(swim_udp_fd(u) >= 0);
  CHECK(swim_udp_fd(nullptr) == -1);

  swim_udp_destroy(u);
}

TEST_CASE("udp: packet loss at 100% drops all packets") {
  swim_udp_t *sender = swim_udp_create("127.0.0.1", 18008);
  swim_udp_t *recvr = swim_udp_create("127.0.0.1", 18009);
  REQUIRE(sender != nullptr);
  REQUIRE(recvr != nullptr);

  // 100% loss on sender port — every outgoing packet is dropped
  swim_udp_set_packet_loss(18008, 100);

  const char *msg = "dropped";
  REQUIRE(swim_udp_send(sender, "127.0.0.1", 18009, (const uint8_t *)msg,
                        strlen(msg)) == 0);

  char src_host[256];
  uint16_t src_port;
  uint8_t buf[64];
  int n = swim_udp_recv(recvr, src_host, &src_port, buf, sizeof(buf));
  CHECK(n == 0); // nothing received

  swim_clear_udp_loss();
  swim_udp_destroy(sender);
  swim_udp_destroy(recvr);
}

TEST_CASE("udp: clear_udp_loss restores delivery") {
  swim_udp_t *sender = swim_udp_create("127.0.0.1", 18010);
  swim_udp_t *recvr = swim_udp_create("127.0.0.1", 18011);
  REQUIRE(sender != nullptr);
  REQUIRE(recvr != nullptr);

  swim_udp_set_packet_loss(18010, 100);
  swim_clear_udp_loss();

  const char *msg = "delivered";
  REQUIRE(swim_udp_send(sender, "127.0.0.1", 18011, (const uint8_t *)msg,
                        strlen(msg)) == 0);

  char src_host[256];
  uint16_t src_port;
  uint8_t buf[64];
  int n = 0;
  for (int i = 0; i < 100 && n == 0; i++)
    n = swim_udp_recv(recvr, src_host, &src_port, buf, sizeof(buf));

  CHECK(n == (int)strlen(msg));

  swim_udp_destroy(sender);
  swim_udp_destroy(recvr);
}

static int drop_all_filter(int /*src_port*/, int /*dst_port*/) { return 1; }

TEST_CASE("udp: drop filter blocks packets") {
  swim_udp_t *sender = swim_udp_create("127.0.0.1", 18012);
  swim_udp_t *recvr = swim_udp_create("127.0.0.1", 18013);
  REQUIRE(sender != nullptr);
  REQUIRE(recvr != nullptr);

  swim_udp_set_drop_filter(drop_all_filter);

  const char *msg = "blocked";
  REQUIRE(swim_udp_send(sender, "127.0.0.1", 18013, (const uint8_t *)msg,
                        strlen(msg)) == 0);

  char src_host[256];
  uint16_t src_port;
  uint8_t buf[64];
  int n = swim_udp_recv(recvr, src_host, &src_port, buf, sizeof(buf));
  CHECK(n == 0);

  // Remove the filter and verify delivery works again
  swim_udp_set_drop_filter(nullptr);

  REQUIRE(swim_udp_send(sender, "127.0.0.1", 18013, (const uint8_t *)msg,
                        strlen(msg)) == 0);
  n = 0;
  for (int i = 0; i < 100 && n == 0; i++)
    n = swim_udp_recv(recvr, src_host, &src_port, buf, sizeof(buf));
  CHECK(n == (int)strlen(msg));

  swim_udp_destroy(sender);
  swim_udp_destroy(recvr);
}
