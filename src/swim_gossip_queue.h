#ifndef SWIM_GOSSIP_QUEUE_H
#define SWIM_GOSSIP_QUEUE_H

#include "swim_membership.h"
#include "swim_protocol.h"

typedef struct swim_gossip_queue_t swim_gossip_queue_t;

/**
 * Initialize a new, empty gossip queue.
 *
 * @return a new gossip queue; free it with swim_gossip_queue_destroy().
 */
SWIM_EXTERN swim_gossip_queue_t *swim_gossip_queue_create(void);

/**
 * Destroy the gossip queue and free all associated memory.
 *
 * @param q The gossip queue instance.
 */
SWIM_EXTERN void swim_gossip_queue_destroy(swim_gossip_queue_t *q);

/**
 * Enqueue a gossip event for dissemination.
 * Follows supersession rules: higher incarnation wins.
 * If incarnations are equal, higher priority (DEAD > SUSPECT > ALIVE) wins.
 *
 * @param q           The gossip queue instance.
 * @param status      Gossip event status (SWIM_STATUS_ALIVE/SUSPECT/DEAD).
 * @param id          The unique ID of the node.
 * @param incarnation Incarnation number from the gossip event.
 * @param multiplier  Transmission multiplier (extra transmit slots). Must be
 * >= 1.
 * @return 0 on success, -1 on error.
 */
SWIM_EXTERN int swim_gossip_queue_enqueue(swim_gossip_queue_t *q,
                                          swim_status_t status,
                                          const swim_node_id_t *id,
                                          uint64_t incarnation,
                                          uint32_t multiplier);

/**
 * Pack events into an outgoing message buffer using the gossip wire format.
 * Writes as many packed members as possible into the range [p, q).
 *
 * @param queue        The gossip queue instance.
 * @param cluster_size Number of alive + suspect nodes in the cluster.
 * @param p            Pointer to start of the first free byte.
 * @param q            Pointer to the end of the buffer (one past the end).
 * @return The number of bytes written on success, or -1 on error.
 */
SWIM_EXTERN int swim_gossip_queue_pack_ex(swim_gossip_queue_t *queue,
                                          uint32_t cluster_size, uint8_t *p,
                                          uint8_t *q);

/**
 * Return the number of events currently in the gossip queue.
 *
 * @param q The gossip queue instance.
 * @return The count of queued events.
 */
SWIM_EXTERN int swim_gossip_queue_size(const swim_gossip_queue_t *q);

/**
 * For testing and debugging: retrieve the list of queued events in priority
 * order.
 *
 * @param q            The gossip queue instance.
 * @param out_events   Output array to copy members.
 * @param max_len      Size of the out_events array.
 * @return The number of events copied, or -1 on error.
 */
SWIM_EXTERN int swim_gossip_queue_peek(const swim_gossip_queue_t *q,
                                       swim_member_t *out_events, int max_len);

#endif // SWIM_GOSSIP_QUEUE_H
