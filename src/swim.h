#ifndef SWIM_H
#define SWIM_H

#ifdef __cplusplus
#define SWIM_EXTERN extern "C"
#else
#define SWIM_EXTERN
#endif

#include <stdbool.h>
#include <stdint.h>

// Error codes
#define SWIM_OK 0
#define SWIM_ERR_NOMEM 1
#define SWIM_ERR_INVALID 2
#define SWIM_ERR_FULL 3
#define SWIM_ERR_TIMEOUT 4
#define SWIM_ERR_BAD_STATE 5

/**
 * Return the latest thread-local errno
 *
 * @return the error message string.
 */
SWIM_EXTERN int swim_errno(void);

/**
 * Return the latest thread-local error message.
 *
 * @return the error message string.
 */
SWIM_EXTERN const char *swim_errmsg(void);

/**
 * Return a read-only string description for a given swim_errno code.
 *
 * @param err the error code.
 * @return string description of the error.
 */
SWIM_EXTERN const char *swim_strerror(int err);

// Subscriber callback notification event
typedef enum {
  SWIM_NODE_UP,      // Node joined or became active
  SWIM_NODE_SUSPECT, // Node is suspected of failing
  SWIM_NODE_DOWN     // Node is declared dead
} swim_event_t;

typedef void (*swim_callback_t)(void *ctx, swim_event_t event,
                                const char *node);

typedef struct {
  const char *self;                // "host:port" or "host:port/cookie" (mandatory)
  const char *name;                // Unique instance name (mandatory)
  const char **seeds;              // NULL-terminated list of seed strings ("host:port/cookie")

  uint64_t protocol_period_ms;     // default 1000
  uint64_t ping_timeout_ms;        // default 200
  uint32_t ping_req_fanout;        // default 3
  uint64_t suspicion_timeout_ms;   // default 3000
  uint64_t seed_retry_interval_ms; // default 5000
  uint64_t dead_node_expiry_ms;    // default 6000
} swim_start_opts_t;

/**
 * Initialize and start a named SWIM protocol instance. Spawns a
 * background thread that monitors the UDP port and runs the SWIM
 * loop; the thread exits when swim_leave() is called. Returns once
 * the thread is running.
 *
 * @param opts Startup options (opts->name is mandatory).
 * @return 0 on success, -1 on failure.
 */
SWIM_EXTERN int swim_start(const swim_start_opts_t *opts);

/**
 * Stop a named instance, perform a graceful leave (notify peers), and free
 * resources.
 *
 * @param name The name of the instance (mandatory).
 * @return 0 on success, -1 on failure.
 */
SWIM_EXTERN int swim_leave(const char *name);

/**
 * Return the current peer list as a packed string buffer.
 * Each peer is formatted as "host:port" or "host:port/cookie";
 * the *count strings are packed consecutively, each NUL-terminated.
 * Caller must free() the returned pointer.
 *
 * @param name         The name of the instance (mandatory).
 * @param include_dead Whether to include dead/quarantined peers.
 * @param count        Set to the number of peers on success.
 * @return Packed string buffer, or NULL on error.
 */
SWIM_EXTERN char *swim_peers(const char *name, bool include_dead, int *count);

/**
 * Subscribe a callback to receive membership events from a named instance.
 *
 * @param name     The name of the instance (mandatory).
 * @param callback The subscriber callback function.
 * @param ctx      Opaque context passed back to callback.
 * @return 0 on success, -1 on failure.
 */
SWIM_EXTERN int swim_subscribe(const char *name, swim_callback_t callback,
                               void *ctx);

/**
 * Deregister a membership event subscriber.
 *
 * @param name     The name of the instance (mandatory).
 * @param callback The registered callback function.
 * @param ctx      Opaque context.
 * @return 0 on success, -1 on failure.
 */
SWIM_EXTERN int swim_unsubscribe(const char *name, swim_callback_t callback,
                                 void *ctx);

/**
 * Read the next event from the feed of the named instance, copying its strings
 * out to the caller. On success the event's NUL-terminated strings are copied
 * contiguously into `buf` and `ptr[0..count-1]` point at each string in `buf`.
 *
 * `bufsz` should be 4096 and `nptr` should be 10, which are large enough to
 * hold any event the feed can store.
 *
 * @param name  The name of the instance (mandatory).
 * @param bufsz Size of `buf` in bytes (should be 4096).
 * @param buf   Destination buffer for the event's string bytes.
 * @param nptr  Number of entries in `ptr` (should be 10).
 * @param ptr   Destination array of string pointers into `buf`.
 * @return the number of strings copied (>= 1) on success, 0 if the feed is
 *         empty, or -1 on error (sets swim_errno).
 */
SWIM_EXTERN int swim_get_event(const char *name, int bufsz, char *buf, int nptr,
                               char **ptr);

/**
 * Feed out-of-band reachability signal to cancel suspicion and revive a node.
 *
 * @param name The name of the instance (mandatory).
 * @param peer The node ID of the peer.
 * @return 0 on success, -1 on failure.
 */
SWIM_EXTERN int swim_hint_alive(const char *name, const char *peer);

#endif // SWIM_H
