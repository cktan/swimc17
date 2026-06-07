#ifndef SWIM_GOSSIP_QUEUE_H
#define SWIM_GOSSIP_QUEUE_H

#include "swim_node_id.h"
#include "swim_membership.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct swim_gossip_queue_t swim_gossip_queue_t;

/**
 * Initialize a new, empty gossip queue.
 *
 * @return a new gossip queue; free it with swim_gossip_queue_final().
 */
swim_gossip_queue_t *swim_gossip_queue_init(void);

/**
 * Destroy the gossip queue and free all associated memory.
 *
 * @param q The gossip queue instance.
 */
void swim_gossip_queue_final(swim_gossip_queue_t *q);

/**
 * Enqueue a gossip event for dissemination.
 * Follows supersession rules: higher incarnation wins.
 * If incarnations are equal, higher priority (DEAD > SUSPECT > ALIVE) wins.
 *
 * @param q           The gossip queue instance.
 * @param status      Gossip event status (SWIM_STATUS_ALIVE/SUSPECT/DEAD).
 * @param id          The unique ID of the node.
 * @param incarnation Incarnation number from the gossip event.
 * @param multiplier  Transmission multiplier (extra transmit slots). Must be >= 1.
 * @return 0 on success, -1 on error.
 */
int swim_gossip_queue_enqueue(swim_gossip_queue_t *q, swim_status_t status,
                              const swim_node_id_t *id, uint64_t incarnation,
                              uint32_t multiplier);

/**
 * Pack events into an outgoing message buffer up to max_bytes budget.
 * Automatically increments transmit counts for packed events, and prunes
 * events that exceed their transmit limit (calculated dynamically using cluster_size).
 *
 * @param q            The gossip queue instance.
 * @param cluster_size Number of alive + suspect nodes in the cluster (excluding self).
 * @param max_bytes    Maximum byte size budget on the wire for the packed events.
 * @param out_events   Output array of swim_member_t to receive packed events.
 * @param max_len      Size of the out_events array.
 * @return The number of events packed, or -1 on error.
 */
int swim_gossip_queue_pack(swim_gossip_queue_t *q, uint32_t cluster_size,
                            size_t max_bytes, swim_member_t *out_events,
                            int max_len);

/**
 * Return the number of events currently in the gossip queue.
 *
 * @param q The gossip queue instance.
 * @return The count of queued events.
 */
int swim_gossip_queue_size(const swim_gossip_queue_t *q);

/**
 * For testing and debugging: retrieve the list of queued events in priority order.
 *
 * @param q            The gossip queue instance.
 * @param out_events   Output array to copy members.
 * @param max_len      Size of the out_events array.
 * @return The number of events copied, or -1 on error.
 */
int swim_gossip_queue_peek(const swim_gossip_queue_t *q, swim_member_t *out_events,
                            int max_len);

#ifdef __cplusplus
}
#endif

#endif // SWIM_GOSSIP_QUEUE_H
