#ifndef SWIM_ERRNO_H
#define SWIM_ERRNO_H

#ifdef __cplusplus
#define _Thread_local thread_local
extern "C" {
#endif

// Error codes
#define SWIM_OK 0
#define SWIM_ERR_NOMEM 1
#define SWIM_ERR_INVALID 2
#define SWIM_ERR_FULL 3
#define SWIM_ERR_TIMEOUT 4
#define SWIM_ERR_BAD_STATE 5

// Per-thread error state variables
extern _Thread_local int swim_errno;

/**
 * Return the latest thread-local error message.
 *
 * @return the error message string.
 */
const char *swim_errmsg(void);

/**
 * Return a read-only string description for a given swim_errno code.
 *
 * @param err the error code.
 * @return string description of the error.
 */
const char *swim_strerror(int err);

/**
 * Set the thread-local error state.
 *
 * @param e   the error code to set.
 * @param fmt printf-like format string for the error message detail.
 */
void swim_set_error(int e, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* SWIM_ERRNO_H */
