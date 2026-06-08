#ifndef SWIM_FEED_H
#define SWIM_FEED_H

#include "swim_errno.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct swim_feed swim_feed_t;

/**
 * Callback function signature for reading a record.
 *
 * @param ctx     User context.
 * @param n       Number of strings in the record.
 * @param strs    Array of pointers to the NUL-terminated strings.
 */
typedef void (*swim_feed_cb)(void *ctx, int n, const char **strs);

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
 * @param n    Number of strings to insert.
 * @param ...  n NUL-terminated strings (const char *).
 * @return 0 on success, or -1 on error (sets swim_errno).
 */
int swim_feed_put(swim_feed_t *feed, int n, ...);

/**
 * Read the next record from the feed and invoke the callback.
 *
 * @param feed The feed instance.
 * @param ctx  User context passed to the callback.
 * @param cb   Callback function to execute on success.
 * @return 1 if a record was successfully read, 0 if the feed is empty,
 *         or -1 on error (sets swim_errno).
 */
int swim_feed_get(swim_feed_t *feed, void *ctx, swim_feed_cb cb);

#ifdef __cplusplus
}
#endif

#endif /* SWIM_FEED_H */
