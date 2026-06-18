#include "doctest.h"

extern "C" {
#include "swim_errno.h"
#include "swim_node_id.h"
}

#include <cstring>
#include <string>

TEST_CASE("node_id: valid inputs parse successfully") {
  swim_node_id_t id;
  CHECK(swim_node_id_parse(&id, "example.com:8080/my_cookie") == 0);
  CHECK(swim_node_id_parse(&id, "127.0.0.1:9000/abc-123") == 0);
  CHECK(swim_node_id_parse(&id, "127.0.0.1:9000") == 0);
  CHECK(swim_node_id_parse(&id, "[::1]:9000/tok") == 0);
  CHECK(swim_node_id_parse(&id, "[2001:db8::1]:443/x") == 0);
}

TEST_CASE("node_id: invalid host characters rejected") {
  swim_node_id_t id;
  CHECK(swim_node_id_parse(&id, "bad host:80") != 0);
  CHECK(swim_node_id_parse(&id, "bad@host:80") != 0);
  CHECK(swim_node_id_parse(&id, "host!:80") != 0);
}

TEST_CASE("node_id: invalid cookie characters rejected") {
  swim_node_id_t id;
  CHECK(swim_node_id_parse(&id, "127.0.0.1:80/bad cookie") != 0);
  CHECK(swim_node_id_parse(&id, "127.0.0.1:80/bad\tcookie") != 0);
  CHECK(swim_node_id_parse(&id, "127.0.0.1:80/bad\x01cookie") != 0);
}

TEST_CASE("node_id: format NULL args return error") {
  swim_node_id_t id = {};
  strcpy(id.host, "127.0.0.1");
  id.port = 8080;

  char buf[64];
  CHECK(swim_node_id_format(nullptr, buf, sizeof(buf)) != 0);
  CHECK(swim_node_id_format(&id, nullptr, sizeof(buf)) != 0);
  CHECK(swim_node_id_format(&id, buf, 0) != 0);
}

TEST_CASE("node_id: format buffer too small returns error") {
  swim_node_id_t id = {};
  strcpy(id.host, "127.0.0.1");
  id.port = 8080;
  strcpy(id.cookie, "ck");

  char buf[5]; // "127.0.0.1:8080/ck" is 18 chars — won't fit
  CHECK(swim_node_id_format(&id, buf, sizeof(buf)) != 0);
}

TEST_CASE("node_id: format IPv6 with and without cookie") {
  swim_node_id_t id = {};
  strcpy(id.host, "::1");
  id.port = 9000;

  char buf[64];
  REQUIRE(swim_node_id_format(&id, buf, sizeof(buf)) == 0);
  CHECK(strcmp(buf, "[::1]:9000") == 0);

  strcpy(id.cookie, "tok");
  REQUIRE(swim_node_id_format(&id, buf, sizeof(buf)) == 0);
  CHECK(strcmp(buf, "[::1]:9000/tok") == 0);
}

TEST_CASE("node_id: parse NULL args return error") {
  swim_node_id_t id;
  CHECK(swim_node_id_parse(nullptr, "127.0.0.1:80") != 0);
  CHECK(swim_node_id_parse(&id, nullptr) != 0);
}

TEST_CASE("node_id: parse rejects port 0 and oversized fields") {
  swim_node_id_t id;

  // Port 0 is invalid
  CHECK(swim_node_id_parse(&id, "127.0.0.1:0") != 0);

  // Cookie too long (>= 64 chars)
  std::string long_cookie(64, 'x');
  std::string input = "127.0.0.1:80/" + long_cookie;
  CHECK(swim_node_id_parse(&id, input.c_str()) != 0);

  // Host too long (>= 256 chars)
  std::string long_host(256, 'a');
  input = long_host + ":80";
  CHECK(swim_node_id_parse(&id, input.c_str()) != 0);
}
