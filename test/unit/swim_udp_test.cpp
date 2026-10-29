#include "doctest.h"

extern "C" {
#include "swim_errno.h"
#include "swim_udp.h"
}

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <string>

// Wait up to 1 s for a packet on u, then receive it. macOS delivers
// loopback packets asynchronously, so a sent packet is not readable at once.
static int recv_wait(swim_udp_t *u, uint8_t *buf, size_t size) {
  struct pollfd pfd = {swim_udp_fd(u), POLLIN, 0};
  poll(&pfd, 1, 1000);
  return swim_udp_recv(u, buf, size);
}

TEST_CASE("udp: non-blocking receive on empty socket returns 0") {
  swim_udp_t *u = swim_udp_create("127.0.0.1", 18001);
  REQUIRE(u != nullptr);

  uint8_t buf[256];
  int rc = swim_udp_recv(u, buf, sizeof(buf));
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

  uint8_t recv_buf[256];
  int n = recv_wait(u2, recv_buf, sizeof(recv_buf));

  REQUIRE(n == (int)strlen(msg));
  recv_buf[n] = '\0';
  CHECK(strcmp((const char *)recv_buf, msg) == 0);

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

  uint8_t recv_buf[256];
  int n = recv_wait(u2, recv_buf, sizeof(recv_buf));

  REQUIRE(n == (int)strlen(msg));
  recv_buf[n] = '\0';
  CHECK(strcmp((const char *)recv_buf, msg) == 0);

  swim_udp_destroy(u1);
  swim_udp_destroy(u2);
}

// A zoned IPv6 destination ("fe80::1%eth0") is not an inet_pton literal; it
// must still resolve, through getaddrinfo, to a reachable address.
TEST_CASE("udp: link-local ipv6 destination with a zone") {
  // Find a link-local address; skip if this machine has none.
  char host[INET6_ADDRSTRLEN + IF_NAMESIZE + 1] = "";
  struct ifaddrs *ifs = nullptr;
  if (getifaddrs(&ifs) != 0)
    return;
  for (struct ifaddrs *i = ifs; i && !host[0]; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET6)
      continue;
    struct in6_addr a = ((const struct sockaddr_in6 *)i->ifa_addr)->sin6_addr;
    if (!IN6_IS_ADDR_LINKLOCAL(&a))
      continue;
    // BSD/macOS embed the interface index in bytes 2-3; the zone carries it.
    a.s6_addr[2] = a.s6_addr[3] = 0;
    char addr[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &a, addr, sizeof(addr));
    snprintf(host, sizeof(host), "%s%%%s", addr, i->ifa_name);
  }
  freeifaddrs(ifs);
  if (!host[0])
    return;
  INFO(std::string(host));

  swim_udp_t *u1 = swim_udp_create(host, 18006);
  REQUIRE(u1 != nullptr);
  swim_udp_t *u2 = swim_udp_create(host, 18007);
  REQUIRE(u2 != nullptr);

  const char *msg = "hello zone";
  CHECK(swim_udp_send(u1, host, 18007, (const uint8_t *)msg, strlen(msg)) == 0);
  uint8_t recv_buf[256];
  int n = recv_wait(u2, recv_buf, sizeof(recv_buf));
  CHECK(n == (int)strlen(msg));

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

  uint8_t buf[64];
  int n = swim_udp_recv(recvr, buf, sizeof(buf));
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

  uint8_t buf[64];
  int n = recv_wait(recvr, buf, sizeof(buf));

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

  uint8_t buf[64];
  int n = swim_udp_recv(recvr, buf, sizeof(buf));
  CHECK(n == 0);

  // Remove the filter and verify delivery works again
  swim_udp_set_drop_filter(nullptr);

  REQUIRE(swim_udp_send(sender, "127.0.0.1", 18013, (const uint8_t *)msg,
                        strlen(msg)) == 0);
  n = recv_wait(recvr, buf, sizeof(buf));
  CHECK(n == (int)strlen(msg));

  swim_udp_destroy(sender);
  swim_udp_destroy(recvr);
}

// Send `n` packets from `from` to host:port and return how many were sent.
static int send_n(swim_udp_t *from, const char *host, uint16_t port, int n) {
  int ok = 0;
  for (int i = 0; i < n; i++)
    ok += swim_udp_send(from, host, port, (const uint8_t *)"x", 1) == 0;
  return ok;
}

TEST_CASE("udp: hostname destinations are resolved once and cached") {
  swim_udp_t *u1 = swim_udp_create("127.0.0.1", 18011);
  swim_udp_t *u2 = swim_udp_create("127.0.0.1", 18012);
  REQUIRE(u1 != nullptr);
  REQUIRE(u2 != nullptr);

  int before = swim_udp_resolve_count(u1);
  CHECK(send_n(u1, "localhost", 18012, 5) == 5);
  CHECK(swim_udp_resolve_count(u1) == before + 1);

  // The cached address reaches the IPv4 socket.
  uint8_t buf[16];
  int got = 0;
  for (int i = 0; i < 5; i++)
    got += recv_wait(u2, buf, sizeof(buf)) > 0;
  CHECK(got == 5);

  // Past the 60 s lifetime, the next send re-resolves once.
  swim_udp_advance_clock(61000);
  CHECK(send_n(u1, "localhost", 18012, 3) == 3);
  CHECK(swim_udp_resolve_count(u1) == before + 2);

  swim_udp_destroy(u1);
  swim_udp_destroy(u2);
}

TEST_CASE("udp: failed lookups are retried after 5 s, not on every send") {
  swim_udp_t *u = swim_udp_create("127.0.0.1", 18013);
  REQUIRE(u != nullptr);

  int before = swim_udp_resolve_count(u);
  CHECK(send_n(u, "nonexistent.invalid", 9, 3) == 0);
  CHECK(swim_udp_resolve_count(u) == before + 1);

  swim_udp_advance_clock(5100);
  CHECK(send_n(u, "nonexistent.invalid", 9, 3) == 0);
  CHECK(swim_udp_resolve_count(u) == before + 2);

  swim_udp_destroy(u);
  swim_set_error(SWIM_OK, nullptr);
}

// During a DNS outage a known host keeps its old address, and is retried
// at the normal lifetime: each failing lookup can block the protocol loop
// for the resolver's whole timeout.
TEST_CASE("udp: a failed refresh keeps the address and waits 60 s") {
  swim_udp_t *u = swim_udp_create("127.0.0.1", 18014);
  REQUIRE(u != nullptr);

  int before = swim_udp_resolve_count(u);
  CHECK(send_n(u, "localhost", 18015, 1) == 1);
  CHECK(swim_udp_resolve_count(u) == before + 1);

  swim_udp_set_resolve_failure(1);
  swim_udp_advance_clock(61000);
  CHECK(send_n(u, "localhost", 18015, 1) == 1); // old address still used
  CHECK(swim_udp_resolve_count(u) == before + 2);

  swim_udp_advance_clock(5100);
  CHECK(send_n(u, "localhost", 18015, 1) == 1);
  CHECK(swim_udp_resolve_count(u) == before + 2); // not yet retried

  swim_udp_advance_clock(55000);
  CHECK(send_n(u, "localhost", 18015, 1) == 1);
  CHECK(swim_udp_resolve_count(u) == before + 3);

  swim_udp_set_resolve_failure(0);
  swim_udp_destroy(u);
}
