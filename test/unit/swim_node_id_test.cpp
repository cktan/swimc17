#include "doctest.h"

extern "C" {
#include "swim_node_id.h"
}

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
