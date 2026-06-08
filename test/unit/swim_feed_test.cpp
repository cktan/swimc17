#include "doctest.h"

extern "C" {
#include "swim_errno.h"
#include "swim_feed.h"
}

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

struct TestCtx {
  int expected_n;
  std::vector<std::string> expected_strs;
  int call_count;
};

static void test_cb(void *ctx, int n, const char **strs) {
  TestCtx *c = (TestCtx *)ctx;
  c->call_count++;
  CHECK(n == c->expected_n);
  for (int i = 0; i < n; i++) {
    CHECK(std::string(strs[i]) == c->expected_strs[i]);
  }
}

TEST_CASE("swim_feed: basic put and get") {
  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);

  // Initial state: empty read
  TestCtx ctx = {0, {}, 0};
  int rc = swim_feed_get(feed, &ctx, test_cb);
  CHECK(rc == 0);
  CHECK(ctx.call_count == 0);

  // Insert a single record
  rc = swim_feed_put(feed, 3, "hello", "world", "!");
  CHECK(rc == 0);

  // Read the record
  ctx = {3, {"hello", "world", "!"}, 0};
  rc = swim_feed_get(feed, &ctx, test_cb);
  CHECK(rc == 1);
  CHECK(ctx.call_count == 1);

  // Read again: should be empty
  rc = swim_feed_get(feed, &ctx, test_cb);
  CHECK(rc == 0);

  swim_feed_destroy(feed);
}

TEST_CASE("swim_feed: multiple put and get (FIFO)") {
  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);

  // Put three records
  CHECK(swim_feed_put(feed, 1, "first") == 0);
  CHECK(swim_feed_put(feed, 2, "second", "part") == 0);
  CHECK(swim_feed_put(feed, 1, "third") == 0);

  // Read first
  TestCtx ctx1 = {1, {"first"}, 0};
  CHECK(swim_feed_get(feed, &ctx1, test_cb) == 1);
  CHECK(ctx1.call_count == 1);

  // Read second
  TestCtx ctx2 = {2, {"second", "part"}, 0};
  CHECK(swim_feed_get(feed, &ctx2, test_cb) == 1);
  CHECK(ctx2.call_count == 1);

  // Read third
  TestCtx ctx3 = {1, {"third"}, 0};
  CHECK(swim_feed_get(feed, &ctx3, test_cb) == 1);
  CHECK(ctx3.call_count == 1);

  // Read fourth (empty)
  TestCtx ctx4 = {0, {}, 0};
  CHECK(swim_feed_get(feed, &ctx4, test_cb) == 0);
  CHECK(ctx4.call_count == 0);

  swim_feed_destroy(feed);
}

TEST_CASE("swim_feed: error handling and input validation") {
  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);

  // Invalid parameters to swim_feed_put
  swim_errno = SWIM_OK;
  CHECK(swim_feed_put(nullptr, 1, "test") == -1);
  CHECK(swim_errno == SWIM_ERR_INVALID);

  swim_errno = SWIM_OK;
  CHECK(swim_feed_put(feed, -1) == -1);
  CHECK(swim_errno == SWIM_ERR_INVALID);

  swim_errno = SWIM_OK;
  CHECK(swim_feed_put(feed, 2, "one", nullptr) == -1);
  CHECK(swim_errno == SWIM_ERR_INVALID);

  // Invalid parameters to swim_feed_get
  TestCtx ctx = {0, {}, 0};
  swim_errno = SWIM_OK;
  CHECK(swim_feed_get(nullptr, &ctx, test_cb) == -1);
  CHECK(swim_errno == SWIM_ERR_INVALID);

  swim_errno = SWIM_OK;
  CHECK(swim_feed_get(feed, &ctx, nullptr) == -1);
  CHECK(swim_errno == SWIM_ERR_INVALID);

  swim_feed_destroy(feed);
}

TEST_CASE("swim_feed: buffer compaction and GC") {
  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);

  // Construct a long string to occupy buffer space
  std::string long_str(300, 'A');

  // Put a record that takes over half of default buffer (512 bytes)
  CHECK(swim_feed_put(feed, 1, long_str.c_str()) == 0);

  // Read and consume it (read_off becomes > 300)
  TestCtx ctx1 = {1, {long_str}, 0};
  CHECK(swim_feed_get(feed, &ctx1, test_cb) == 1);

  // The next put does not fit in remaining ~200 bytes, but fits after
  // compaction. Compaction should be triggered automatically to avoid realloc.
  CHECK(swim_feed_put(feed, 1, long_str.c_str()) == 0);

  TestCtx ctx2 = {1, {long_str}, 0};
  CHECK(swim_feed_get(feed, &ctx2, test_cb) == 1);

  swim_feed_destroy(feed);
}

TEST_CASE("swim_feed: buffer growth") {
  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);

  // Construct a string larger than initial buffer capacity
  std::string massive_str(1000, 'B');

  // Insert record with massive string - triggers resize
  CHECK(swim_feed_put(feed, 1, massive_str.c_str()) == 0);

  TestCtx ctx = {1, {massive_str}, 0};
  CHECK(swim_feed_get(feed, &ctx, test_cb) == 1);

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
      // Retry on failure (e.g. if memory allocation fails, though unlikely)
      std::this_thread::yield();
    }
    ta->total_written->fetch_add(1);
  }
  return nullptr;
}

static void concurrent_cb(void *ctx, int n, const char **strs) {
  std::atomic<int> *count = (std::atomic<int> *)ctx;
  (void)n;
  if (strcmp(strs[0], "payload") == 0) {
    count->fetch_add(1);
  }
}

static void *reader_thread(void *arg) {
  ThreadArg *ta = (ThreadArg *)arg;
  while (true) {
    int current_read = ta->total_read->load();
    if (current_read >= ta->limit * 4) {
      break;
    }

    int rc = swim_feed_get(ta->feed, ta->total_read, concurrent_cb);
    if (rc == 0) {
      std::this_thread::yield();
    } else if (rc < 0) {
      // Error occurred
      break;
    }
  }
  return nullptr;
}

TEST_CASE("swim_feed: thread safety concurrent put and get") {
  swim_feed_t *feed = swim_feed_create();
  REQUIRE(feed != nullptr);

  std::atomic<int> total_written(0);
  std::atomic<int> total_read(0);
  int limit_per_thread = 500;

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
  TestCtx ctx = {0, {}, 0};
  CHECK(swim_feed_get(feed, &ctx, test_cb) == 0);

  swim_feed_destroy(feed);
}
