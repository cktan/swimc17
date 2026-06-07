#include "doctest.h"

extern "C" {
#include "swim_errno.h"
#include "swim_node_id.h"
#include "swim_udp.h"
}

#include <cstring>

TEST_CASE("udp: non-blocking receive on empty socket returns 0") {
  swim_udp_t *u = swim_udp_init("127.0.0.1", 18001);
  REQUIRE(u != nullptr);

  swim_node_id_t src;
  uint8_t buf[256];
  int rc = swim_udp_recv(u, &src, buf, sizeof(buf));
  CHECK(rc == 0); // EAGAIN / EWOULDBLOCK

  swim_udp_final(u);
}

TEST_CASE("udp: ipv4 loopback packet exchange") {
  swim_udp_t *u1 = swim_udp_init("127.0.0.1", 18002);
  swim_udp_t *u2 = swim_udp_init("127.0.0.1", 18003);
  REQUIRE(u1 != nullptr);
  REQUIRE(u2 != nullptr);

  swim_node_id_t dest2;
  REQUIRE(swim_node_id_parse(&dest2, "127.0.0.1:18003") == 0);

  const char *msg = "hello IPv4";
  int rc = swim_udp_send(u1, &dest2, (const uint8_t *)msg, strlen(msg));
  REQUIRE(rc == 0);

  // Allow a tiny window or simply read since it's loopback and synchronous in
  // OS
  swim_node_id_t src;
  uint8_t recv_buf[256];
  int n = 0;
  for (int i = 0; i < 100; i++) {
    n = swim_udp_recv(u2, &src, recv_buf, sizeof(recv_buf));
    if (n > 0)
      break;
  }

  REQUIRE(n == (int)strlen(msg));
  recv_buf[n] = '\0';
  CHECK(strcmp((const char *)recv_buf, msg) == 0);
  CHECK(strcmp(src.host, "127.0.0.1") == 0);
  CHECK(src.port == 18002);

  swim_udp_final(u1);
  swim_udp_final(u2);
}

TEST_CASE("udp: ipv6 loopback packet exchange") {
  // Try to bind IPv6, skip test if IPv6 loopback is not supported in this
  // environment
  swim_udp_t *u1 = swim_udp_init("::1", 18004);
  if (!u1) {
    // IPv6 might not be supported/enabled on this machine's loopback interface
    return;
  }
  swim_udp_t *u2 = swim_udp_init("::1", 18005);
  REQUIRE(u2 != nullptr);

  swim_node_id_t dest2;
  REQUIRE(swim_node_id_parse(&dest2, "[::1]:18005") == 0);

  const char *msg = "hello IPv6";
  int rc = swim_udp_send(u1, &dest2, (const uint8_t *)msg, strlen(msg));
  REQUIRE(rc == 0);

  swim_node_id_t src;
  uint8_t recv_buf[256];
  int n = 0;
  for (int i = 0; i < 100; i++) {
    n = swim_udp_recv(u2, &src, recv_buf, sizeof(recv_buf));
    if (n > 0)
      break;
  }

  REQUIRE(n == (int)strlen(msg));
  recv_buf[n] = '\0';
  CHECK(strcmp((const char *)recv_buf, msg) == 0);
  CHECK(strcmp(src.host, "::1") == 0);
  CHECK(src.port == 18004);

  swim_udp_final(u1);
  swim_udp_final(u2);
}

TEST_CASE("udp: invalid binding address fails cleanly") {
  // Bind to an invalid IP
  swim_udp_t *u = swim_udp_init("999.999.999.999", 18006);
  CHECK(u == nullptr);
  CHECK(swim_errno != SWIM_OK);

  // Restore clean state for other tests
  swim_set_error(SWIM_OK, nullptr);
}
