#include "doctest.h"

extern "C" {
#include "swim_feed.h"
}

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// Read one record via the copy-out interface and check it matches `expected`.
// Returns the swim_feed_get return value (string count, 0 if empty, -1 error).
static int read_and_check(swim_feed_t *feed,
                          std::vector<std::string> expected) {
  char buf[4096];
  char *ptr[10];
  int n = swim_feed_get(feed, sizeof(buf), buf, 10, ptr);
  if (n > 0) {
    CHECK(n == (int)expected.size());
    for (int i = 0; i < n && i < (int)expected.size(); i++) {
      CHECK(std::string(ptr[i]) == expected[i]);
    }
  }
  return n;
}

TEST_CASE("swim_feed: basic put and get") {
  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);

  // Initial state: empty read
  CHECK(read_and_check(feed, {}) == 0);

  // Insert a single record
  CHECK(swim_feed_put(feed, 3, "hello", "world", "!") == 0);

  // Read the record
  CHECK(read_and_check(feed, {"hello", "world", "!"}) == 3);

  // Read again: should be empty
  CHECK(read_and_check(feed, {}) == 0);

  swim_feed_destroy(feed);
}

TEST_CASE("swim_feed: multiple put and get (FIFO)") {
  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);

  // Put three records
  CHECK(swim_feed_put(feed, 1, "first") == 0);
  CHECK(swim_feed_put(feed, 2, "second", "part") == 0);
  CHECK(swim_feed_put(feed, 1, "third") == 0);

  // Read in FIFO order
  CHECK(read_and_check(feed, {"first"}) == 1);
  CHECK(read_and_check(feed, {"second", "part"}) == 2);
  CHECK(read_and_check(feed, {"third"}) == 1);

  // Read fourth (empty)
  CHECK(read_and_check(feed, {}) == 0);

  swim_feed_destroy(feed);
}

TEST_CASE("swim_feed: error handling and input validation") {
  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);

  char buf[4096];
  char *ptr[10];

  // Invalid parameters to swim_feed_put
  swim_set_error(SWIM_OK, NULL);
  CHECK(swim_feed_put(nullptr, 1, "test") == -1);
  CHECK(swim_errno() == SWIM_ERR_INVALID);

  swim_set_error(SWIM_OK, NULL);
  CHECK(swim_feed_put(feed, 0) == -1);
  CHECK(swim_errno() == SWIM_ERR_INVALID);

  swim_set_error(SWIM_OK, NULL);
  CHECK(swim_feed_put(feed, -1) == -1);
  CHECK(swim_errno() == SWIM_ERR_INVALID);

  swim_set_error(SWIM_OK, NULL);
  CHECK(swim_feed_put(feed, 2, "one", nullptr) == -1);
  CHECK(swim_errno() == SWIM_ERR_INVALID);

  // Invalid parameters to swim_feed_get
  swim_set_error(SWIM_OK, NULL);
  CHECK(swim_feed_get(nullptr, sizeof(buf), buf, 10, ptr) == -1);
  CHECK(swim_errno() == SWIM_ERR_INVALID);

  swim_set_error(SWIM_OK, NULL);
  CHECK(swim_feed_get(feed, sizeof(buf), nullptr, 10, ptr) == -1);
  CHECK(swim_errno() == SWIM_ERR_INVALID);

  swim_set_error(SWIM_OK, NULL);
  CHECK(swim_feed_get(feed, 0, buf, 10, ptr) == -1);
  CHECK(swim_errno() == SWIM_ERR_INVALID);

  swim_set_error(SWIM_OK, NULL);
  CHECK(swim_feed_get(feed, sizeof(buf), buf, 10, nullptr) == -1);
  CHECK(swim_errno() == SWIM_ERR_INVALID);

  swim_set_error(SWIM_OK, NULL);
  CHECK(swim_feed_get(feed, sizeof(buf), buf, 0, ptr) == -1);
  CHECK(swim_errno() == SWIM_ERR_INVALID);

  swim_feed_destroy(feed);
}

TEST_CASE("swim_feed: record that does not fit is left in the feed") {
  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);

  CHECK(swim_feed_put(feed, 3, "a", "b", "c") == 0);

  char buf[4096];
  char *ptr[10];

  // Too few pointers: 3 strings cannot fit in nptr=2. Record is not consumed.
  swim_set_error(SWIM_OK, NULL);
  CHECK(swim_feed_get(feed, sizeof(buf), buf, 2, ptr) == -1);
  CHECK(swim_errno() == SWIM_ERR_INVALID);

  // Too small a buffer: "a\0b\0c\0" is 6 bytes, does not fit in bufsz=3.
  swim_set_error(SWIM_OK, NULL);
  CHECK(swim_feed_get(feed, 3, buf, 10, ptr) == -1);
  CHECK(swim_errno() == SWIM_ERR_INVALID);

  // The record survived both failures and can still be read with room.
  CHECK(read_and_check(feed, {"a", "b", "c"}) == 3);
  CHECK(read_and_check(feed, {}) == 0);

  swim_feed_destroy(feed);
}

TEST_CASE("swim_feed: buffer compaction and auto-draining") {
  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);

  // Construct a long string (approx 1200 bytes)
  std::string long_str(1200, 'A');

  // Insert 3 records of long strings (each takes 1200 + sizeof(int) + 1 = 1205
  // bytes) 3 records will fit: 3 * 1205 = 3615 bytes, which fits in 4096.
  CHECK(swim_feed_put(feed, 1, long_str.c_str()) == 0);
  CHECK(swim_feed_put(feed, 1, long_str.c_str()) == 0);
  CHECK(swim_feed_put(feed, 1, long_str.c_str()) == 0);

  // Putting a 4th record of 1205 bytes will exceed 4096 (total would be 4820).
  // This must trigger auto-draining of the oldest records.
  // Discarding 1st record: remaining is 2 * 1205 = 2410 bytes, + 1205 = 3615
  // (fits). So the 1st record is discarded automatically.
  CHECK(swim_feed_put(feed, 1, long_str.c_str()) == 0);

  // Let's verify that the 1st record is gone, and the next readable is the 2nd
  // record.
  CHECK(read_and_check(feed, {long_str}) == 1);
  CHECK(read_and_check(feed, {long_str}) == 1);
  CHECK(read_and_check(feed, {long_str}) == 1);

  swim_feed_destroy(feed);
}

TEST_CASE("swim_feed: oversized record failure") {
  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);

  // Construct a string larger than 4KB buffer capacity
  std::string massive_str(4100, 'B');

  // Insert record with massive string - should fail with SWIM_ERR_INVALID
  swim_set_error(SWIM_OK, NULL);
  CHECK(swim_feed_put(feed, 1, massive_str.c_str()) == -1);
  CHECK(swim_errno() == SWIM_ERR_INVALID);

  swim_feed_destroy(feed);
}

// Thread safety test structures
struct ThreadArg {
  swim_feed_t *feed;
  std::atomic<int> *total_written;
  std::atomic<int> *total_read;
  int limit;
};

static void *writer_thread(void *arg) {
  ThreadArg *ta = (ThreadArg *)arg;
  for (int i = 0; i < ta->limit; i++) {
    while (swim_feed_put(ta->feed, 1, "payload") != 0) {
      std::this_thread::yield();
    }
    ta->total_written->fetch_add(1);
  }
  return nullptr;
}

static void *reader_thread(void *arg) {
  ThreadArg *ta = (ThreadArg *)arg;
  char buf[4096];
  char *ptr[10];
  while (true) {
    int current_read = ta->total_read->load();
    if (current_read >= ta->limit * 4) {
      break;
    }

    int rc = swim_feed_get(ta->feed, sizeof(buf), buf, 10, ptr);
    if (rc == 0) {
      std::this_thread::yield();
    } else if (rc < 0) {
      break;
    } else if (strcmp(ptr[0], "payload") == 0) {
      ta->total_read->fetch_add(1);
    }
  }
  return nullptr;
}

TEST_CASE("swim_feed: thread safety concurrent put and get") {
  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);

  std::atomic<int> total_written(0);
  std::atomic<int> total_read(0);
  int limit_per_thread = 50;

  ThreadArg arg = {feed, &total_written, &total_read, limit_per_thread};

  pthread_t writers[4];
  pthread_t readers[4];

  for (int i = 0; i < 4; i++) {
    REQUIRE(pthread_create(&writers[i], nullptr, writer_thread, &arg) == 0);
    REQUIRE(pthread_create(&readers[i], nullptr, reader_thread, &arg) == 0);
  }

  for (int i = 0; i < 4; i++) {
    pthread_join(writers[i], nullptr);
  }
  for (int i = 0; i < 4; i++) {
    pthread_join(readers[i], nullptr);
  }

  CHECK(total_written.load() == limit_per_thread * 4);
  CHECK(total_read.load() == limit_per_thread * 4);

  // Empty feed check
  CHECK(read_and_check(feed, {}) == 0);

  swim_feed_destroy(feed);
}
