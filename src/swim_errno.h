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
extern _Thread_local char swim_errmsg[200];

/**
 * Return a read-only string description for a given swim_errno code.
 *
 * @param err the error code.
 * @return string description of the error.
 */
const char *swim_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* SWIM_ERRNO_H */
