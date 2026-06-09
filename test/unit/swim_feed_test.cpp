#include "doctest.h"

extern "C" {
#include "swim.h"
#include "swim_errno.h"
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

  swim_set_error(SWIM_OK, NULL);
  CHECK(swim_feed_put(feed, SWIM_FEED_MAX_ELEMENTS + 1) == -1);
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

TEST_CASE("swim_feed: multi-page lifecycle") {
  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);

  // Record layout: sizeof(int)=4 + 1001-byte payload = 1005 bytes.
  // floor(4096 / 1005) = 4 records per page (4*1005=4020; 5th won't fit).
  const int rpp = 4;
  std::string s(1000, 'X');

  auto write_n = [&](int n) {
    for (int i = 0; i < n; i++)
      CHECK(swim_feed_put(feed, 1, s.c_str()) == 0);
  };
  auto read_n = [&](int n) {
    for (int i = 0; i < n; i++)
      CHECK(read_and_check(feed, {s}) == 1);
  };

  // Grow to 5 pages (20 records: 4 per page).
  write_n(5 * rpp);

  // Drain to 2 pages: read 3 pages worth, leaving pages 4 and 5.
  read_n(3 * rpp);

  // Grow back to 5 pages: write 3 more pages worth (pages 6, 7, 8).
  write_n(3 * rpp);

  // Drain all the way: 2 surviving + 3 new pages = 5 pages = 20 records.
  read_n(5 * rpp);
  CHECK(read_and_check(feed, {}) == 0);

  // Grow to 2 pages: 4 records fill the recycled tail page, 1 starts a new page.
  write_n(rpp + 1);

  // Drain all the way again.
  read_n(rpp + 1);
  CHECK(read_and_check(feed, {}) == 0);

  swim_feed_destroy(feed);
}

TEST_CASE("swim_feed: oversized record failure") {
  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);

  // String one byte over SWIM_FEED_MAX_RECORD_SIZE — must be rejected.
  std::string oversized_str(SWIM_FEED_MAX_RECORD_SIZE + 1, 'B');

  swim_set_error(SWIM_OK, NULL);
  CHECK(swim_feed_put(feed, 1, oversized_str.c_str()) == -1);
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
