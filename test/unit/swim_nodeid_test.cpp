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
    INFO(std::string(s));
    CHECK(!nodeid_valid(swim_nodeid_find(s)));
  }

  std::string max_cookie = "127.0.0.1:7000/" + std::string(63, 'c');
  const char *good[] = {"127.0.0.1:7000", "127.0.0.1:7000/",
                        "host.example:65535/a b!", "[::1]:7000/c",
                        max_cookie.c_str()};
  for (const char *s : good) {
    INFO(std::string(s));
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

// Every spelling of the same endpoint must resolve to one identity, stored
// in a single canonical form.
TEST_CASE("nodeid: equivalent spellings normalize to one identity") {
  struct {
    const char *canonical;
    const char *aliases[4];
  } cases[] = {
      {"10.0.0.1:7000",
       {"10.0.0.1:07000", "[10.0.0.1]:7000", "10.0.0.1:7000/"}},
      {"10.0.0.1:7000/c", {"10.0.0.1:007000/c", "[10.0.0.1]:7000/c"}},
      {"[::1]:7000", {"[0:0:0:0:0:0:0:1]:7000", "[0::1]:7000/", "[::1]:07000"}},
      {"[2001:db8::a]:80/x", {"[2001:DB8:0:0:0:0:0:A]:80/x"}},
      {"host.example:80/c", {"Host.Example:80/c", "HOST.EXAMPLE:0080/c"}},
  };
  for (const auto &c : cases) {
    INFO(std::string(c.canonical));
    swim_nodeid_idx_t id = swim_nodeid_find(c.canonical);
    REQUIRE(nodeid_valid(id));
    CHECK(std::string(swim_nodeid_lookup(id)) == c.canonical);
    for (const char *alias : c.aliases) {
      if (!alias)
        break;
      INFO(std::string(alias));
      CHECK(nodeid_eq(swim_nodeid_find(alias), id));
      CHECK(nodeid_eq(swim_nodeid_get(alias), id));
    }
  }

  // An alias registered first is stored in canonical form.
  swim_nodeid_idx_t id = swim_nodeid_find("[0:0::1]:7100/");
  REQUIRE(nodeid_valid(id));
  CHECK(std::string(swim_nodeid_lookup(id)) == "[::1]:7100");

  // Cookies are opaque and stay case-sensitive.
  CHECK(!nodeid_eq(swim_nodeid_find("h.example:80/C"),
                   swim_nodeid_find("h.example:80/c")));
}

TEST_CASE("nodeid: ambiguous hosts are rejected") {
  const char *bad[] = {"010.0.0.1:7000", // octal-looking, not IPv4
                       "10.0.0:7000",    // numeric but not dotted-quad
                       "[myhost]:7000",  // brackets only around IPs
                       "[fe80::1%eth0]:7000", "a/b:7000"};
  for (const char *s : bad) {
    INFO(std::string(s));
    CHECK(!nodeid_valid(swim_nodeid_find(s)));
  }
}
