/*
 * swim_feed.c — Paged telemetry queue.
 *
 * Stores multi-string records (e.g. "node up", "127.0.0.1:8000")
 * across a chain of 4 KB pages for consumption by swim_feed_get().
 * Records are encoded as [int n][str0\0][str1\0]...[strN-1\0].
 *
 * Records never span pages. A record that does not fit at the end of
 * the current tail page goes onto a freshly allocated page, wasting
 * the tail of the old page. Max record size is ~1028 bytes
 * (SWIM_FEED_MAX_RECORD_SIZE + sizeof(int)), well under 4 KB.
 *
 * Growth: when the tail page has no room, a new page is appended.
 * Under OOM, oldest pages are freed until malloc succeeds. If only
 * the tail page remains and malloc still fails, the write returns
 * SWIM_ERR_NOMEM.
 *
 * Shrink: after a get, if the head page is fully consumed (bot==top)
 * and is not the tail (next!=NULL), it is freed immediately. The
 * tail page is reset to bot=top=0 instead of freed.
 *
 * On get, the record is not consumed if it exceeds the caller's
 * bufsz or nptr; the caller must retry with a larger buffer.
 */
#include "swim.h"
#include "swim_errno.h"
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SWIM_FEED_PAGE_SIZE 4096

typedef struct feed_page {
  struct feed_page *next;
  size_t bot; // next byte to read
  size_t top; // next byte to write
  char data[SWIM_FEED_PAGE_SIZE];
} feed_page_t;

struct swim_feed {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  feed_page_t *head; // oldest page (read from here)
  feed_page_t *tail; // newest page (write to here)
};

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
  if (pthread_cond_init(&feed->cond, NULL) != 0) {
    pthread_mutex_destroy(&feed->mutex);
    free(feed);
    swim_set_error(SWIM_ERR_NOMEM, "Failed to initialize condition variable");
    return NULL;
  }
  feed->head = NULL;
  feed->tail = NULL;
  return feed;
}

void swim_feed_destroy(swim_feed_t *feed) {
  if (!feed)
    return;
  pthread_cond_destroy(&feed->cond);
  pthread_mutex_destroy(&feed->mutex);
  feed_page_t *page = feed->head;
  while (page) {
    feed_page_t *next = page->next;
    free(page);
    page = next;
  }
  free(feed);
}

int swim_feed_put(swim_feed_t *feed, int n, ...) {
  if (!feed)
    return swim_set_error(SWIM_ERR_INVALID, "Feed cannot be NULL");
  if (n < 1 || n > SWIM_FEED_MAX_ELEMENTS)
    return swim_set_error(SWIM_ERR_INVALID, "String count n must be 1..%d",
                          SWIM_FEED_MAX_ELEMENTS);

  pthread_mutex_lock(&feed->mutex);

  // First pass: validate strings and measure bytes needed.
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

  if (needed - sizeof(int) > SWIM_FEED_MAX_RECORD_SIZE) {
    pthread_mutex_unlock(&feed->mutex);
    return swim_set_error(SWIM_ERR_INVALID, "Record size %zu exceeds limit %d",
                          needed - sizeof(int), SWIM_FEED_MAX_RECORD_SIZE);
  }

  // Ensure the tail page has room. A fresh page always has room because
  // needed <= SWIM_FEED_MAX_RECORD_SIZE + sizeof(int) < SWIM_FEED_PAGE_SIZE.
  if (feed->tail == NULL || feed->tail->top + needed > SWIM_FEED_PAGE_SIZE) {
    feed_page_t *page;
    while ((page = malloc(sizeof(feed_page_t))) == NULL) {
      if (feed->head == feed->tail) {
        // No pages left to sacrifice (empty or only tail remains).
        pthread_mutex_unlock(&feed->mutex);
        return swim_set_error(SWIM_ERR_NOMEM, "Out of memory allocating feed page");
      }
      feed_page_t *old_head = feed->head;
      feed->head = old_head->next;
      free(old_head);
    }
    page->next = NULL;
    page->bot = page->top = 0;
    if (feed->tail == NULL) {
      feed->tail = feed->head = page;
    } else {
      feed->tail = feed->tail->next = page;
    }
  }

  // Write the record into the tail page.
  memcpy(feed->tail->data + feed->tail->top, &n, sizeof(int));
  feed->tail->top += sizeof(int);

  va_start(args, n);
  for (int i = 0; i < n; i++) {
    const char *str = va_arg(args, const char *);
    size_t len = strlen(str) + 1;
    memcpy(feed->tail->data + feed->tail->top, str, len);
    feed->tail->top += len;
  }
  va_end(args);

  pthread_cond_signal(&feed->cond);
  pthread_mutex_unlock(&feed->mutex);
  return 0;
}

int swim_feed_wait(swim_feed_t *feed, uint64_t timeout_ms) {
  if (!feed)
    return swim_set_error(SWIM_ERR_INVALID, "Feed cannot be NULL");

  pthread_mutex_lock(&feed->mutex);

  bool has_data = (feed->head != NULL && feed->head->bot != feed->head->top);
  if (has_data) {
    pthread_mutex_unlock(&feed->mutex);
    return 0;
  }

  int rc;
  if (timeout_ms == SWIM_WAIT_FOREVER) {
    rc = pthread_cond_wait(&feed->cond, &feed->mutex);
  } else {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += (time_t)(timeout_ms / 1000);
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
      ts.tv_sec++;
      ts.tv_nsec -= 1000000000L;
    }
    rc = pthread_cond_timedwait(&feed->cond, &feed->mutex, &ts);
  }
  pthread_mutex_unlock(&feed->mutex);

  if (rc == ETIMEDOUT)
    return 1;
  if (rc != 0)
    return swim_set_error(SWIM_ERR_INVALID, "swim_feed_wait: cond wait failed");
  return 0;
}

int swim_feed_get(swim_feed_t *feed, int bufsz, char *buf, int nptr,
                  char **ptr) {
  if (!feed)
    return swim_set_error(SWIM_ERR_INVALID, "Feed cannot be NULL");
  if (!buf || bufsz <= 0)
    return swim_set_error(SWIM_ERR_INVALID, "Output buffer is invalid");
  if (!ptr || nptr <= 0)
    return swim_set_error(SWIM_ERR_INVALID, "Pointer array is invalid");

  pthread_mutex_lock(&feed->mutex);

  if (feed->head == NULL || feed->head->bot == feed->head->top) {
    pthread_mutex_unlock(&feed->mutex);
    return 0;
  }

  if (feed->head->bot + sizeof(int) > feed->head->top) {
    pthread_mutex_unlock(&feed->mutex);
    return swim_set_error(SWIM_ERR_INVALID,
                          "Corrupt buffer: not enough bytes for record header");
  }

  int n;
  memcpy(&n, feed->head->data + feed->head->bot, sizeof(int));
  if (n < 1) {
    pthread_mutex_unlock(&feed->mutex);
    return swim_set_error(SWIM_ERR_INVALID,
                          "Corrupt buffer: invalid string count %d", n);
  }

  // Scan string extents within the head page.
  size_t payload_start = feed->head->bot + sizeof(int);
  size_t scan_off = payload_start;
  int valid = 1;
  for (int i = 0; i < n; i++) {
    if (scan_off >= feed->head->top) {
      valid = 0;
      break;
    }
    char *nul_pos = memchr(feed->head->data + scan_off, '\0',
                           feed->head->top - scan_off);
    if (!nul_pos) {
      valid = 0;
      break;
    }
    scan_off = (nul_pos - feed->head->data) + 1;
  }
  if (!valid) {
    pthread_mutex_unlock(&feed->mutex);
    return swim_set_error(SWIM_ERR_INVALID,
                          "Corrupt buffer: incomplete strings for record");
  }

  size_t payload_len = scan_off - payload_start;

  // Leave the record in place if it doesn't fit the caller's buffers.
  if (n > nptr) {
    pthread_mutex_unlock(&feed->mutex);
    return swim_set_error(SWIM_ERR_INVALID,
                          "Record has %d strings, exceeds nptr %d", n, nptr);
  }
  if (payload_len > (size_t)bufsz) {
    pthread_mutex_unlock(&feed->mutex);
    return swim_set_error(SWIM_ERR_INVALID, "Record size %zu exceeds bufsz %d",
                          payload_len, bufsz);
  }

  memcpy(buf, feed->head->data + payload_start, payload_len);
  feed->head->bot = scan_off;

  if (feed->head->bot == feed->head->top) {
    if (feed->head->next != NULL) {
      feed_page_t *old_head = feed->head;
      feed->head = old_head->next;
      free(old_head);
    } else {
      // Tail page: reset rather than free.
      feed->head->bot = 0;
      feed->head->top = 0;
    }
  }

  pthread_mutex_unlock(&feed->mutex);

  size_t off = 0;
  for (int i = 0; i < n; i++) {
    ptr[i] = buf + off;
    off += strlen(buf + off) + 1;
  }

  return n;
}

void swim_feed_wakeall(swim_feed_t *feed) {
  if (!feed)
    return;
  pthread_mutex_lock(&feed->mutex);
  pthread_cond_broadcast(&feed->cond);
  pthread_mutex_unlock(&feed->mutex);
}

bool swim_feed_empty(swim_feed_t *feed) {
  if (!feed)
    return true;
  pthread_mutex_lock(&feed->mutex);
  bool empty = (feed->head == NULL || feed->head->bot == feed->head->top);
  pthread_mutex_unlock(&feed->mutex);
  return empty;
}
