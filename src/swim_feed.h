#ifndef SWIM_FEED_H
#define SWIM_FEED_H

#include "swim_errno.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct swim_feed swim_feed_t;

/**
 * Create a new swim_feed_t instance.
 *
 * @return A pointer to the newly allocated swim_feed_t, or NULL on error.
 */
swim_feed_t *swim_feed_create(void);

/**
 * Destroy and free a swim_feed_t instance.
 *
 * @param feed The feed to destroy.
 */
void swim_feed_destroy(swim_feed_t *feed);

/**
 * Insert a record of n NUL-terminated strings into the feed.
 *
 * @param feed The feed instance.
 * @param n    Number of strings to insert (must be >= 1).
 * @param ...  n NUL-terminated strings (const char *).
 * @return 0 on success, or -1 on error (sets swim_errno).
 */
int swim_feed_put(swim_feed_t *feed, int n, ...);

/**
 * Read the next record from the feed, copying its strings out to the caller.
 *
 * On success the record's NUL-terminated strings are copied contiguously into
 * `buf`, and `ptr[0..count-1]` are set to point at each string within `buf`.
 *
 * `bufsz` should be 4096 (SWIM_FEED_BUFFER_SIZE) and `nptr` should be 10, which
 * are large enough to hold any record the feed can store.
 *
 * @param feed  The feed instance.
 * @param bufsz Size of `buf` in bytes (should be 4096).
 * @param buf   Destination buffer for the record's string bytes.
 * @param nptr  Number of entries in `ptr` (should be 10).
 * @param ptr   Destination array of string pointers into `buf`.
 * @return the number of strings copied (>= 1) on success, 0 if the feed is
 *         empty, or -1 on error (sets swim_errno). If the record does not fit
 *         within `bufsz` or has more strings than `nptr`, returns -1 and the
 *         record is left in the feed.
 */
int swim_feed_get(swim_feed_t *feed, int bufsz, char *buf, int nptr,
                  char **ptr);

#ifdef __cplusplus
}
#endif

#endif /* SWIM_FEED_H */
