#include "swim_feed.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

struct swim_feed {
  pthread_mutex_t mutex;
  char *buf;
  size_t capacity;
  size_t read_off;  // Start of the first unread record
  size_t write_off; // End of the last written record (first free byte)
};

#define SWIM_FEED_BUFFER_SIZE 4096

swim_feed_t *swim_feed_create(void) {
  swim_feed_t *feed = malloc(sizeof(*feed));
  if (!feed) {
    swim_set_error(SWIM_ERR_NOMEM, "Failed to allocate swim_feed_t");
    return NULL;
  }

  if (pthread_mutex_init(&feed->mutex, NULL) != 0) {
    free(feed);
    swim_set_error(SWIM_ERR_NOMEM, "Failed to initialize mutex");
    return NULL;
  }

  feed->buf = malloc(SWIM_FEED_BUFFER_SIZE);
  if (!feed->buf) {
    pthread_mutex_destroy(&feed->mutex);
    free(feed);
    swim_set_error(SWIM_ERR_NOMEM, "Failed to allocate internal buffer");
    return NULL;
  }

  feed->capacity = SWIM_FEED_BUFFER_SIZE;
  feed->read_off = 0;
  feed->write_off = 0;

  return feed;
}

void swim_feed_destroy(swim_feed_t *feed) {
  if (!feed) {
    return;
  }
  pthread_mutex_destroy(&feed->mutex);
  free(feed->buf);
  free(feed);
}

static void swim_feed_compact_locked(swim_feed_t *feed) {
  if (feed->read_off == 0) {
    return;
  }
  size_t active_len = feed->write_off - feed->read_off;
  if (active_len > 0) {
    memmove(feed->buf, feed->buf + feed->read_off, active_len);
  }
  feed->write_off = active_len;
  feed->read_off = 0;
}

int swim_feed_put(swim_feed_t *feed, int n, ...) {
  if (!feed) {
    return swim_set_error(SWIM_ERR_INVALID, "Feed cannot be NULL");
  }
  if (n < 0) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "String count n must be non-negative");
  }

  pthread_mutex_lock(&feed->mutex);

  // First pass: validate strings and calculate size needed
  size_t needed = sizeof(int);
  va_list args;
  va_start(args, n);
  for (int i = 0; i < n; i++) {
    const char *str = va_arg(args, const char *);
    if (!str) {
      va_end(args);
      pthread_mutex_unlock(&feed->mutex);
      return swim_set_error(SWIM_ERR_INVALID, "String at index %d is NULL", i);
    }
    needed += strlen(str) + 1;
  }
  va_end(args);

  // If the record exceeds the maximum fixed buffer size, it can never fit.
  if (needed > SWIM_FEED_BUFFER_SIZE) {
    pthread_mutex_unlock(&feed->mutex);
    return swim_set_error(SWIM_ERR_INVALID,
                          "Record size %zu exceeds buffer capacity %d", needed,
                          SWIM_FEED_BUFFER_SIZE);
  }

  // Auto-draining: discard oldest records if space is insufficient
  while (SWIM_FEED_BUFFER_SIZE - (feed->write_off - feed->read_off) < needed) {
    // We have active records in the buffer. Let's discard the oldest one at
    // read_off.
    if (feed->read_off + sizeof(int) > feed->write_off) {
      // Corrupt buffer state (should not happen normally) - reset offsets
      feed->read_off = 0;
      feed->write_off = 0;
      break;
    }

    int old_n;
    memcpy(&old_n, feed->buf + feed->read_off, sizeof(int));
    if (old_n < 0) {
      // Corrupt record count - reset offsets
      feed->read_off = 0;
      feed->write_off = 0;
      break;
    }

    size_t scan_off = feed->read_off + sizeof(int);
    int valid = 1;
    for (int i = 0; i < old_n; i++) {
      if (scan_off >= feed->write_off) {
        valid = 0;
        break;
      }
      char *nul_pos =
          memchr(feed->buf + scan_off, '\0', feed->write_off - scan_off);
      if (!nul_pos) {
        valid = 0;
        break;
      }
      scan_off = (nul_pos - feed->buf) + 1;
    }

    if (!valid) {
      // Corrupt record layout - reset offsets
      feed->read_off = 0;
      feed->write_off = 0;
      break;
    }

    // Advance read_off to discard this record
    feed->read_off = scan_off;

    if (feed->read_off == feed->write_off) {
      feed->read_off = 0;
      feed->write_off = 0;
    }
  }

  // Now compact the buffer to make space contiguous at the beginning
  swim_feed_compact_locked(feed);

  // Write the record
  memcpy(feed->buf + feed->write_off, &n, sizeof(int));
  feed->write_off += sizeof(int);

  va_start(args, n);
  for (int i = 0; i < n; i++) {
    const char *str = va_arg(args, const char *);
    size_t len = strlen(str) + 1;
    memcpy(feed->buf + feed->write_off, str, len);
    feed->write_off += len;
  }
  va_end(args);

  pthread_mutex_unlock(&feed->mutex);
  return 0;
}

int swim_feed_get(swim_feed_t *feed, void *ctx, swim_feed_cb cb) {
  if (!feed) {
    return swim_set_error(SWIM_ERR_INVALID, "Feed cannot be NULL");
  }
  if (!cb) {
    return swim_set_error(SWIM_ERR_INVALID, "Callback cannot be NULL");
  }

  pthread_mutex_lock(&feed->mutex);

  if (feed->read_off == feed->write_off) {
    pthread_mutex_unlock(&feed->mutex);
    return 0; // End of stream / empty
  }

  // Ensure we can read the integer `n`
  if (feed->read_off + sizeof(int) > feed->write_off) {
    pthread_mutex_unlock(&feed->mutex);
    return swim_set_error(SWIM_ERR_INVALID,
                          "Corrupt buffer: not enough bytes for record header");
  }

  int n;
  memcpy(&n, feed->buf + feed->read_off, sizeof(int));
  if (n < 0) {
    pthread_mutex_unlock(&feed->mutex);
    return swim_set_error(SWIM_ERR_INVALID,
                          "Corrupt buffer: negative string count %d", n);
  }

  // Allocate array of pointers. Use stack for small n, malloc for larger n.
  const char *small_ptrs[16];
  const char **ptrs = small_ptrs;
  if (n > 16) {
    ptrs = malloc(n * sizeof(char *));
    if (!ptrs) {
      pthread_mutex_unlock(&feed->mutex);
      return swim_set_error(SWIM_ERR_NOMEM, "Failed to allocate pointer array");
    }
  }

  size_t scan_off = feed->read_off + sizeof(int);
  int valid = 1;
  for (int i = 0; i < n; i++) {
    if (scan_off >= feed->write_off) {
      valid = 0;
      break;
    }
    ptrs[i] = feed->buf + scan_off;
    // Scan for NUL terminator within buffer bounds
    char *nul_pos =
        memchr(feed->buf + scan_off, '\0', feed->write_off - scan_off);
    if (!nul_pos) {
      valid = 0;
      break;
    }
    scan_off = (nul_pos - feed->buf) + 1;
  }

  if (!valid) {
    if (n > 16) {
      free(ptrs);
    }
    pthread_mutex_unlock(&feed->mutex);
    return swim_set_error(SWIM_ERR_INVALID,
                          "Corrupt buffer: incomplete strings for record");
  }

  // Call the callback while holding the lock
  cb(ctx, n, ptrs);

  if (n > 16) {
    free(ptrs);
  }

  // Consume the record
  feed->read_off = scan_off;

  // Automatic GC / compaction
  if (feed->read_off == feed->write_off) {
    feed->read_off = 0;
    feed->write_off = 0;
  } else if (feed->read_off >= feed->capacity / 2) {
    swim_feed_compact_locked(feed);
  }

  pthread_mutex_unlock(&feed->mutex);
  return 1;
}
