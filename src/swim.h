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

// Feed record limits (enforced at write time)
#define SWIM_FEED_MAX_RECORD_SIZE 1024 // max string-payload bytes per record
#define SWIM_FEED_MAX_ELEMENTS 10      // max strings per record

// Pass as timeout_ms to swim_feed_wait to block without a deadline.
#define SWIM_WAIT_FOREVER UINT64_MAX

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

// --- Telemetry feed ---

typedef struct swim_feed swim_feed_t;

/**
 * Create a new swim_feed_t instance. The caller owns the feed and
 * must pass it to swim_feed_destroy() when done.
 *
 * @return A pointer to the newly allocated swim_feed_t, or NULL on error.
 */
SWIM_EXTERN swim_feed_t *swim_feed_create(void);

/**
 * Destroy and free a swim_feed_t instance.
 *
 * Caller must ensure no threads are blocked in
 * swim_feed_wait before calling this function. The
 * typical shutdown sequence is: set a flag, call
 * swim_feed_wakeall, join reader threads, then call
 * swim_feed_destroy.
 *
 * @param feed The feed to destroy.
 */
SWIM_EXTERN void swim_feed_destroy(swim_feed_t *feed);

/**
 * Wake all threads currently blocked in swim_feed_wait.
 * Does not modify the feed contents. Intended for orderly
 * shutdown: set a flag, call this, then join reader threads
 * before calling swim_feed_destroy.
 *
 * @param feed The feed instance.
 */
SWIM_EXTERN void swim_feed_wakeall(swim_feed_t *feed);

/**
 * Insert a record of n NUL-terminated strings into the feed. Fails
 * if n is outside 1..SWIM_FEED_MAX_ELEMENTS or the total string
 * payload exceeds SWIM_FEED_MAX_RECORD_SIZE bytes.
 *
 * @param feed The feed instance.
 * @param n    Number of strings (1..SWIM_FEED_MAX_ELEMENTS).
 * @param ...  n NUL-terminated strings (const char *).
 * @return 0 on success, or -1 on error (sets swim_errno).
 */
SWIM_EXTERN int swim_feed_put(swim_feed_t *feed, int n, ...);

/**
 * Read the next record from the feed, copying its strings out to the
 * caller. On success the NUL-terminated strings are copied
 * contiguously into `buf` and `ptr[0..n-1]` point at each one.
 *
 * Use bufsz >= SWIM_FEED_MAX_RECORD_SIZE and
 * nptr >= SWIM_FEED_MAX_ELEMENTS to accept any record the feed can
 * hold. If the record exceeds bufsz or nptr, returns -1 and leaves
 * the record in the feed.
 *
 * @param feed  The feed instance.
 * @param bufsz Capacity of `buf` in bytes.
 * @param buf   Destination buffer for the string bytes.
 * @param nptr  Capacity of `ptr` in entries.
 * @param ptr   Array populated with pointers into `buf`.
 * @return number of strings (>= 1) on success, 0 if the feed is
 *         empty, or -1 on error (sets swim_errno).
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

/**
 * Block until the feed is signalled or timeout_ms milliseconds
 * elapse. Returns 0 immediately if the feed already contains
 * unread data. A return value of 0 means data may be present;
 * it does NOT guarantee a record is available (spurious wakeups
 * or concurrent readers may drain). Always follow with
 * swim_feed_get.
 *
 * Pass SWIM_WAIT_FOREVER to block without a deadline.
 *
 * @param feed       The feed instance.
 * @param timeout_ms Maximum wait in ms, or SWIM_WAIT_FOREVER.
 * @return 0 if signalled or data present, 1 on timeout, -1 on error.
 */
SWIM_EXTERN int swim_feed_wait(swim_feed_t *feed, uint64_t timeout_ms);

// --- Instance lifecycle ---

typedef struct swim_t swim_t;

typedef struct {
  const char *self; // "host:port" or "host:port/cookie" (mandatory)
  const char *name; // Unique instance name (mandatory)
  const char *
      *seeds; // NULL-terminated list of seed strings ("host:port/cookie")

  uint64_t protocol_period_ms;     // default 1000
  uint64_t ping_timeout_ms;        // default 200
  uint32_t ping_req_fanout;        // default 3
  uint64_t suspicion_timeout_ms;   // default 3000
  uint64_t seed_retry_interval_ms; // default 5000
  uint64_t dead_node_expiry_ms;    // default 6000

  swim_feed_t *feed; // optional; caller owns; NULL disables telemetry
} swim_start_opts_t;

/**
 * Compute a swim_start_opts_t with all timing fields derived from
 * two operator-friendly inputs: expected cluster size and
 * worst-case failure-detection latency.
 *
 * Background — why not expose T directly:
 *   The SWIM paper's fundamental knob is the protocol period T
 *   (probe interval). T is not intuitive; operators know how
 *   many machines they have provisioned, not what value of T
 *   to pick. swim_opts_for() inverts the detection-latency
 *   formula so you can reason in terms you already know.
 *
 * Worst-case detection latency model:
 *   A node can fail at any point in a probe cycle. In the
 *   worst case it fails immediately after being probed, so the
 *   detector waits up to one full period T before the next
 *   probe fires. That probe times out twice — once for the
 *   direct ping (T/5) and once for the indirect ping-req
 *   (T/5) — and the resulting suspicion timer runs for
 *   ceil(log2(N)) x T before the node is declared dead:
 *
 *     detect_ms = T + 2x(T/5) + ceil(log2(N))xT
 *               = T x (1.4 + ceil(log2(N)))
 *
 *   Solving for T:
 *
 *     T = detect_ms / (1.4 + ceil(log2(N)))
 *
 * Derived fields:
 *   protocol_period_ms     = T
 *   ping_timeout_ms        = T / 5
 *   ping_req_fanout        = 3   (SWIM paper default)
 *   suspicion_timeout_ms   = ceil(log2(N)) x T
 *   dead_node_expiry_ms    = 2 x suspicion_timeout_ms
 *   seed_retry_interval_ms = 5 x T  (5 probe periods)
 *
 * The pointer fields (self, name, seeds) are set to NULL;
 * the caller must fill them before passing to swim_start().
 *
 * Fallback to defaults:
 *   If n <= 1, detect_ms == 0, or the derived T rounds to 0,
 *   the function returns the same defaults swim_start() applies
 *   for zero-initialized fields (T = 1000 ms, tuned for an
 *   8-node cluster). No error is reported.
 *
 * Example — 50-node cluster, detect failures within 15 s:
 *
 *   swim_start_opts_t opts = swim_opts_for(50, 15000);
 *   opts.self = "10.0.0.1:7771";
 *   opts.name = "my_cluster";
 *   swim_start(&opts);
 *
 * @param n          Expected cluster size (number of nodes).
 * @param detect_ms  Worst-case failure-detection latency (ms).
 * @return           swim_start_opts_t with timing fields set.
 */
SWIM_EXTERN swim_start_opts_t swim_opts_for(int n, uint64_t detect_ms);

/**
 * Initialize and start a named SWIM protocol instance. Spawns a
 * background thread that monitors the UDP port and runs the SWIM
 * loop; the thread exits when swim_leave() is called. Returns once
 * the thread is running.
 *
 * @param opts Startup options (opts->self and opts->name are mandatory).
 * @return Opaque instance handle on success, NULL on failure.
 */
SWIM_EXTERN swim_t *swim_start(const swim_start_opts_t *opts);

/**
 * Stop an instance, perform a graceful leave (notify peers),
 * and free resources. The handle is invalid after this call.
 *
 * @param inst The instance handle returned by swim_start.
 * @return 0 on success, -1 on failure.
 */
SWIM_EXTERN int swim_leave(swim_t *inst);

/**
 * Return the current peer list as a packed string buffer.
 * Each peer is formatted as "host:port" or "host:port/cookie";
 * the *count strings are packed consecutively, each NUL-terminated.
 * Caller must free() the returned pointer.
 *
 * @param inst         The instance handle returned by swim_start.
 * @param include_dead Whether to include dead/quarantined peers.
 * @param count        Set to the number of peers on success.
 * @return Packed string buffer, or NULL on error.
 */
SWIM_EXTERN char *swim_peers(swim_t *inst, bool include_dead, int *count);

/**
 * Feed out-of-band reachability signal to cancel suspicion and
 * revive a node.
 *
 * @param inst The instance handle returned by swim_start.
 * @param peer The node ID of the peer.
 * @return 0 on success, -1 on failure.
 */
SWIM_EXTERN int swim_hint_alive(swim_t *inst, const char *peer);

#endif // SWIM_H
