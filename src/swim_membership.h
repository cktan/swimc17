#ifndef SWIM_MEMBERSHIP_H
#define SWIM_MEMBERSHIP_H

#include "swim.h"
#include "swim_node_id.h"

typedef char swim_status_t;
#define SWIM_STATUS_ALIVE 'A'
#define SWIM_STATUS_SUSPECT 'S'
#define SWIM_STATUS_DEAD 'D'

typedef struct {
  swim_node_id_t id;
  swim_status_t status;
  uint64_t incarnation;
  uint64_t dead_at; // Monotonic time in milliseconds when marked dead
} swim_member_t;

typedef struct swim_membership_t swim_membership_t;

/**
 * Initialize an empty membership list.
 *
 * @return a new membership list; free it with swim_membership_destroy().
 */
SWIM_EXTERN swim_membership_t *swim_membership_create(void);

/**
 * Destroy the membership list and free all associated memory.
 *
 * @param m The membership list instance.
 */
SWIM_EXTERN void swim_membership_destroy(swim_membership_t *m);

/**
 * Add a new node to the membership list as ALIVE.
 *
 * @param m           The membership list instance.
 * @param id          The unique ID of the node.
 * @param incarnation Incarnation number from the join event.
 * @return 0 on success, -1 on error.
 */
SWIM_EXTERN int swim_membership_add(swim_membership_t *m,
                                    const swim_node_id_t *id,
                                    uint64_t incarnation);

/**
 * Force a node to be alive in the membership list.
 *
 * @param m           The membership list instance.
 * @param id          The unique ID of the node.
 * @param incarnation Incarnation number.
 * @return 0 on success, -1 on error.
 */
SWIM_EXTERN int swim_membership_set_alive(swim_membership_t *m,
                                          const swim_node_id_t *id,
                                          uint64_t incarnation);

/**
 * Look up a node's membership details by its node ID.
 *
 * @param m           The membership list instance.
 * @param id          The unique ID of the node.
 * @return Pointer to the member entry if found, or NULL if not found.
 */
SWIM_EXTERN const swim_member_t *swim_membership_get(const swim_membership_t *m,
                                                     const swim_node_id_t *id);

/**
 * Apply a gossip event (ALIVE, SUSPECT, or DEAD) to the membership list.
 * Follows the SWIM+Suspension precedence and incarnation number rules.
 *
 * @param m           The membership list instance.
 * @param status      Gossip event status (ALIVE/SUSPECT/DEAD).
 * @param id          The unique ID of the node.
 * @param incarnation Incarnation number from the gossip event.
 * @param now_ms      Current monotonic time in milliseconds.
 * @return 0 if the state was updated, 1 if the event was ignored (stale), -1 on
 * error.
 */
SWIM_EXTERN int swim_membership_apply_event(swim_membership_t *m,
                                            swim_status_t status,
                                            const swim_node_id_t *id,
                                            uint64_t incarnation,
                                            uint64_t now_ms);

/**
 * Garbage collect nodes that have been dead for longer than expiry_ms.
 *
 * @param m         The membership list instance.
 * @param expiry_ms How long a dead node should be kept around (quarantined) in
 * milliseconds.
 * @param now_ms    Current monotonic time in milliseconds.
 */
SWIM_EXTERN void swim_membership_gc(swim_membership_t *m, uint64_t expiry_ms,
                                    uint64_t now_ms);

/**
 * Return the count of active members (non-dead: ALIVE or SUSPECT).
 *
 * @param m The membership list instance.
 * @return The count of active members.
 */
SWIM_EXTERN int swim_membership_count(const swim_membership_t *m);

/**
 * List members in the list.
 *
 * @param m            The membership list instance.
 * @param out_list     Output buffer for members.
 * @param max_len      Size of the out_list buffer.
 * @param include_dead Whether to include dead nodes in the list.
 * @return The number of members copied, or -1 on error.
 */
SWIM_EXTERN int swim_membership_list(const swim_membership_t *m,
                                     swim_member_t *out_list, int max_len,
                                     bool include_dead);

#endif // SWIM_MEMBERSHIP_H
