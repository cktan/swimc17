#include "doctest.h"

extern "C" {
#include "swim.h"
#include "swim_errno.h"
#include "swim_nodeid.h"
}

#include <cstring>
#include <string>

TEST_CASE("nodeid: malformed ids are rejected") {
  std::string long_cookie = "127.0.0.1:7000/" + std::string(64, 'c');
  const char *bad[] = {"nohost",    ":7000",    "127.0.0.1:",       "h:0",
                       "h:70000",   "h:12x",    "[::1]7000",        "[]:7000",
                       "[::1:7000", "::1:7000", long_cookie.c_str()};
  for (const char *s : bad) {
    INFO(s);
    CHECK(!nodeid_valid(swim_nodeid_find(s)));
  }

  std::string max_cookie = "127.0.0.1:7000/" + std::string(63, 'c');
  const char *good[] = {"127.0.0.1:7000", "127.0.0.1:7000/",
                        "host.example:65535/a b!", "[::1]:7000/c",
                        max_cookie.c_str()};
  for (const char *s : good) {
    INFO(s);
    CHECK(nodeid_valid(swim_nodeid_find(s)));
  }
}

TEST_CASE("nodeid: split strips IPv6 brackets") {
  swim_nodeid_idx_t id = swim_nodeid_find("[::1]:8001/cookie1");
  REQUIRE(nodeid_valid(id));
  char host[254], cookie[64];
  int port = 0;
  REQUIRE(swim_nodeid_split(id, host, &port, cookie) == 0);
  CHECK(std::string(host) == "::1");
  CHECK(port == 8001);
  CHECK(std::string(cookie) == "cookie1");
}

TEST_CASE("nodeid: get does not register") {
  const char *s = "127.0.0.1:7001/get-only";
  CHECK(!nodeid_valid(swim_nodeid_get(s)));
  swim_nodeid_idx_t id = swim_nodeid_find(s);
  REQUIRE(nodeid_valid(id));
  CHECK(nodeid_eq(swim_nodeid_get(s), id));
}

TEST_CASE("nodeid: swim_hint_alive does not register unknown peers") {
  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.name = "hint_noreg";
  swim_t *inst = swim_start("127.0.0.1:20751/c1", &opts);
  REQUIRE(inst != nullptr);

  const char *peer = "127.0.0.1:20752/never-seen";
  CHECK(swim_hint_alive(inst, peer) == 0);
  CHECK(!nodeid_valid(swim_nodeid_get(peer)));

  swim_leave(inst);
  swim_set_error(SWIM_OK, nullptr);
}

TEST_CASE("nodeid: an IPv6 node id can start an instance") {
  swim_start_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.name = "ipv6_start";
  swim_t *inst = swim_start("[::1]:20761/v6", &opts);
  CHECK(inst != nullptr);
  if (inst)
    swim_leave(inst);
  swim_set_error(SWIM_OK, nullptr);
}
