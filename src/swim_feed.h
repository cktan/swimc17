#ifndef SWIM_FEED_H
#define SWIM_FEED_H

#include "swim.h"
#include <pthread.h>

typedef struct swim_feed swim_feed_t;

/**
 * Create a new swim_feed_t instance.
 *
 * @return A pointer to the newly allocated swim_feed_t, or NULL on error.
 */
SWIM_EXTERN swim_feed_t *swim_feed_create(void);

/**
 * Destroy and free a swim_feed_t instance.
 *
 * @param feed The feed to destroy.
 */
SWIM_EXTERN void swim_feed_destroy(swim_feed_t *feed);

/**
 * Insert a record of n NUL-terminated strings into the
 * feed. Fails if n is outside 1..SWIM_FEED_MAX_ELEMENTS
 * or the total string payload exceeds
 * SWIM_FEED_MAX_RECORD_SIZE bytes.
 *
 * @param feed The feed instance.
 * @param n    Number of strings (1..SWIM_FEED_MAX_ELEMENTS).
 * @param ...  n NUL-terminated strings (const char *).
 * @return 0 on success, or -1 on error (sets swim_errno).
 */
SWIM_EXTERN int swim_feed_put(swim_feed_t *feed, int n, ...);

/**
 * Read the next record from the feed, copying its strings
 * out to the caller. On success the NUL-terminated strings
 * are copied contiguously into `buf` and `ptr[0..n-1]`
 * point at each one.
 *
 * Use bufsz >= SWIM_FEED_MAX_RECORD_SIZE and
 * nptr >= SWIM_FEED_MAX_ELEMENTS to accept any record
 * the feed can hold.
 *
 * @param feed  The feed instance.
 * @param bufsz Capacity of `buf` in bytes.
 * @param buf   Destination buffer for the string bytes.
 * @param nptr  Capacity of `ptr` in entries.
 * @param ptr   Array populated with pointers into `buf`.
 * @return number of strings (>= 1) on success, 0 if the
 *         feed is empty, or -1 on error (sets swim_errno).
 *         If the record exceeds bufsz or nptr, returns -1
 *         and leaves the record in the feed.
 */
SWIM_EXTERN int swim_feed_get(swim_feed_t *feed, int bufsz, char *buf, int nptr,
                              char **ptr);

/**
 * Return true if the feed has no unread records.
 *
 * @param feed The feed instance.
 * @return true if empty, false if records are available.
 */
SWIM_EXTERN bool swim_feed_empty(swim_feed_t *feed);

#endif /* SWIM_FEED_H */
