#ifndef SWIM_PROTOCOL_H
#define SWIM_PROTOCOL_H

#include "swim_membership.h"
#include "swim_node_id.h"
#include "swim_gossip_queue.h"
#include "swim_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

// Subscriber callback notification event
typedef enum {
  SWIM_NODE_UP,      // Node joined or became active
  SWIM_NODE_SUSPECT, // Node is suspected of failing
  SWIM_NODE_DOWN     // Node is declared dead
} swim_event_t;

typedef void (*swim_callback_t)(void *ctx, swim_event_t event,
                                const swim_node_id_t *node);

typedef struct {
  const char *host;
  uint16_t port;
  const char *name;                // Unique instance name (mandatory)
  const char *cookie;              // Optional cookie, defaults to ""
  const swim_node_id_t *seed_list; // List of seed nodes
  int seed_count;                  // Number of seeds

  uint64_t protocol_period_ms;     // default 1000
  uint64_t ping_timeout_ms;        // default 200
  uint32_t ping_req_fanout;        // default 3
  uint64_t suspicion_timeout_ms;   // default 3000
  uint64_t seed_retry_interval_ms; // default 5000
  uint64_t dead_node_expiry_ms;    // default 6000
} swim_start_opts_t;

/**
 * Initialize and start a named SWIM protocol instance in the background.
 *
 * @param opts Startup options (opts->name is mandatory).
 * @return 0 on success, -1 on failure.
 */
int swim_start(const swim_start_opts_t *opts);

/**
 * Stop a named instance, perform a graceful leave (notify peers), and free
 * resources.
 *
 * @param name The name of the instance (mandatory).
 * @return 0 on success, -1 on failure.
 */
int swim_leave(const char *name);

/**
 * Query current cluster membership list for a named instance.
 *
 * @param name         The name of the instance (mandatory).
 * @param out_list     Output buffer for members.
 * @param max_len      Size of the out_list buffer.
 * @param include_dead Whether to include dead nodes in the list.
 * @return The number of members copied, or -1 on error.
 */
int swim_members(const char *name, swim_member_t *out_list, int max_len,
                 bool include_dead);

/**
 * Subscribe a callback to receive membership events from a named instance.
 *
 * @param name     The name of the instance (mandatory).
 * @param callback The subscriber callback function.
 * @param ctx      Opaque context passed back to callback.
 * @return 0 on success, -1 on failure.
 */
int swim_subscribe(const char *name, swim_callback_t callback, void *ctx);

/**
 * Deregister a membership event subscriber.
 *
 * @param name     The name of the instance (mandatory).
 * @param callback The registered callback function.
 * @param ctx      Opaque context.
 * @return 0 on success, -1 on failure.
 */
int swim_unsubscribe(const char *name, swim_callback_t callback, void *ctx);

/**
 * Feed out-of-band reachability signal to cancel suspicion and revive a node.
 *
 * @param name The name of the instance (mandatory).
 * @param peer The node ID of the peer.
 * @return 0 on success, -1 on failure.
 */
int swim_hint_alive(const char *name, const swim_node_id_t *peer);

/**
 * Pack a message and gossip events into a buffer.
 *
 * @param q            Gossip queue.
 * @param active_members Number of active members.
 * @param buf          Output buffer.
 * @param bufsz        Buffer size.
 * @param type         Message type.
 * @param sender       Sender Node ID.
 * @param seq          Sequence number.
 * @param peer         Peer Node ID (if applicable, otherwise NULL).
 * @return Number of bytes written on success, or -1 on error.
 */
int pack_message(uint8_t type, const swim_node_id_t *sender, uint32_t seq,
                 const swim_node_id_t *peer, swim_gossip_queue_t *q,
                 uint32_t active_members, uint8_t *buf, int bufsz);

/**
 * Unpack a message buffer into a swim_message_t.
 *
 * @param buf          Input buffer.
 * @param len          Buffer length.
 * @param msg          Output message structure.
 * @return 0 on success, or -1 on error.
 */
int unpack_message(const uint8_t *buf, int len, swim_message_t *msg);

#ifdef __cplusplus
}
#endif

#endif // SWIM_PROTOCOL_H
