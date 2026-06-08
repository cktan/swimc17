/*
 * swim_main.c — SWIM gossip protocol engine.
 *
 * Implements the full SWIM+Suspicion protocol: probe
 * selection, direct ping, indirect ping-req via helpers,
 * suspicion timers, dead-node GC, and membership event
 * dissemination via the gossip queue.
 *
 * Up to 16 named instances are registered in a global
 * array (g_instances) protected by g_instances_mutex.
 * Each instance runs a background thread that calls
 * swim_timer_tick() every protocol_period_ms and processes
 * incoming UDP packets.
 *
 * Locking: g_instances_mutex is used only to look up (or
 * register/remove) an instance. find_and_lock_instance()
 * acquires inst->mutex while holding g_instances_mutex —
 * preventing a concurrent swim_stop() from removing the
 * instance between lookup and lock — then immediately
 * releases g_instances_mutex so the global lock is not
 * held during the actual work.
 *
 * Callbacks are extracted into a notify_batch_t under
 * inst->mutex, then dispatched after the lock is released.
 * This allows subscriber callbacks to re-enter the API
 * without deadlocking.
 */
#define _GNU_SOURCE
#include "swim.h"
#include "swim_errno.h"
#include "swim_codec.h"
#include "swim_feed.h"
#include "swim_gossip_queue.h"
#include "swim_protocol_internal.h"
#include "swim_timer.h"
#include "swim_udp.h"

#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Monotonic time for all local duration/elapsed-time tracking: RTT
// measurement, dead_at timestamps, GC expiry, and the protocol tick loop.
// Must not use wall time here — clock slews would corrupt elapsed arithmetic.
static uint64_t get_monotonic_time_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// Wall time used only for incarnation numbers. A restarting node must have
// a higher incarnation than any stale gossip still circulating, so the value
// needs to be globally ordered across machines, not just locally monotonic.
static uint64_t get_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// Round n up to the next multiple of 8.
static inline size_t align8(size_t n) { return (n + 7) & ~(size_t)7; }

// Build the suspicion-timer key for a node. The cookie is part of node
// identity (DESIGN S3), so it must be in the key: otherwise two members that
// share a host:port collide on one key and an event for one cancels the
// other's suspicion timer. Centralized so the arm/cancel keys cannot drift.
static void suspect_key(char *buf, size_t n, const swim_node_id_t *id) {
  snprintf(buf, n, "suspect:%s:%u:%s", id->host, id->port, id->cookie);
}

// Structures for internal protocol use
typedef struct {
  swim_callback_t cb;
  void *ctx;
} swim_sub_t;

// A membership change discovered inside a critical section. Callbacks are
// never run at the point of discovery (that would invoke user code while
// holding inst->mutex, deadlocking if the callback re-enters the API).
// Instead they are queued here and dispatched after the locks are released.
typedef struct {
  swim_event_t event;
  swim_node_id_t node;
} pending_notify_t;

// A self-contained batch of work taken out of an instance under its lock and
// then dispatched after unlocking. It holds copies only (no pointers into the
// instance), so dispatch touches nothing owned by the instance and is immune
// to the instance being freed concurrently (e.g. by swim_leave).
typedef struct {
  pending_notify_t *items;
  int count;
  swim_sub_t subs[16];
  int sub_count;
} notify_batch_t;

typedef enum { PROBE_NONE = 0, PROBE_DIRECT, PROBE_INDIRECT } probe_state_t;

// State of the single outstanding liveness probe for the current protocol
// round. Starts DIRECT (we ping the target); escalates to INDIRECT (we send
// PING-REQs to helpers) on timeout. Reset to PROBE_NONE on ACK or final
// timeout.
typedef struct {
  probe_state_t state;
  swim_node_id_t target;
  uint32_t seq;
  uint64_t sent_ms; // monotonic send time of the direct ping, for RTT
} pending_probe_t;

// An in-flight indirect probe we are relaying on behalf of another node's
// failure detector. We forward the PING-REQ to the target and forward any
// ACK back to the requester. Expires after expiry_tick.
typedef struct {
  swim_node_id_t requester;
  swim_node_id_t target;
  uint32_t seq;
  uint64_t expiry_tick;
} relay_probe_t;

typedef struct swim_instance_t swim_instance_t;

struct swim_instance_t {
  char name[64];
  swim_udp_t *udp;
  swim_timer_t *timer;
  swim_membership_t *membership;
  swim_gossip_queue_t *gossip_queue;

  swim_node_id_t self_id;
  uint64_t incarnation;
  uint32_t seq;
  unsigned int rand_state; // per-instance PRNG state for rand_r()

  pthread_t thread;
  pthread_mutex_t mutex;
  atomic_bool running;

  uint64_t protocol_period_ms;
  uint64_t ping_timeout_ms;
  uint32_t ping_req_fanout;
  uint64_t suspicion_timeout_ms;
  uint64_t seed_retry_interval_ms;
  uint64_t dead_node_expiry_ms;

  // Bootstrap addresses from opts->seeds; pinged periodically until
  // at least one responds and the node joins the cluster.
  swim_node_id_t *seeds;
  int seed_count;

  // Round-robin probe list: a shuffled snapshot of alive members rebuilt
  // each time shuffle_idx reaches shuffle_count.
  swim_member_t *shuffle_list;
  int shuffle_count;
  int shuffle_idx;
  int shuffle_cap; // allocated capacity in elements (a multiple of 8)

  // The single outstanding liveness probe (direct or indirect) per SWIM round.
  pending_probe_t pending_probe;

  // In-flight PING-REQ relays: indirect probes sent to helpers when a direct
  // probe times out. Capped at 32; entries are removed on ACK or timeout.
  relay_probe_t relays[32];
  int relay_count;

  // Event subscribers. swim_subscribe returns SWIM_ERR_FULL when all 16 slots
  // are occupied.
  swim_sub_t subscribers[16];
  int subscriber_count;

  // Notifications discovered under the lock, dispatched after unlocking.
  pending_notify_t *pending;
  int pending_count;
  int pending_capacity;

  // Telemetry feed.
  swim_feed_t *feed;
  uint64_t last_cluster_size; // last emitted cluster size, for on-change gating

  uint64_t current_tick;
};

// Global named instance registry
static swim_instance_t *g_instances[16] = {0};
static pthread_mutex_t g_instances_mutex = PTHREAD_MUTEX_INITIALIZER;

static swim_instance_t *find_instance(const char *name) {
  if (!name || name[0] == '\0') {
    return NULL;
  }
  for (int i = 0; i < 16; i++) {
    if (g_instances[i] && strcmp(g_instances[i]->name, name) == 0) {
      return g_instances[i];
    }
  }
  return NULL;
}

/* Lock g_instances_mutex, find the instance, lock inst->mutex, release
 * g_instances_mutex, and return inst.  Returns NULL (nothing held) when
 * the instance does not exist. */
static swim_instance_t *find_and_lock_instance(const char *name) {
  pthread_mutex_lock(&g_instances_mutex);
  swim_instance_t *inst = find_instance(name);
  if (inst) {
    pthread_mutex_lock(&inst->mutex);
  }
  pthread_mutex_unlock(&g_instances_mutex);
  return inst;
}

// Forward declarations
static void *swim_protocol_thread_entry(void *arg);
static void *swim_protocol_loop(swim_instance_t *instance);
static void queue_notification(swim_instance_t *inst, swim_event_t event,
                               const swim_node_id_t *node);
static void send_ping(swim_instance_t *inst, const swim_node_id_t *dest,
                      uint32_t seq);
static void send_ack(swim_instance_t *inst, const swim_node_id_t *dest,
                     uint32_t seq);
static void send_ping_req(swim_instance_t *inst, const swim_node_id_t *helper,
                          const swim_node_id_t *target, uint32_t seq);
static void send_fwd_ack(swim_instance_t *inst, const swim_node_id_t *dest,
                         const swim_node_id_t *source, uint32_t seq);
static void probe_timeout_cb(swim_instance_t *inst, uint32_t seq);

// Global alarm callback router to intercept timeout triggers
static void global_alarm_cb(void *ctx, swim_timer_event_t ev, void *param) {
  swim_instance_t *inst = (swim_instance_t *)ctx;
  uint32_t seq = (uint32_t)(intptr_t)param;
  if (ev == SWIM_TIMER_ALARM) {
    probe_timeout_cb(inst, seq);
  }
}

// Override swim_timer_add when adding probe_timeout to use our interceptor
static inline int arm_probe_timeout(swim_instance_t *inst, uint32_t ticks,
                                    uint32_t seq) {
  return swim_timer_add(inst->timer, ticks, "probe_timeout", global_alarm_cb,
                        inst, (void *)(intptr_t)seq);
}

// Timer Callbacks
static void probe_timer_cb(void *ctx, swim_timer_event_t ev, void *param) {
  (void)param;
  swim_instance_t *inst = (swim_instance_t *)ctx;
  if (ev == SWIM_TIMER_CANCEL)
    return;

  // 1. Garbage collect expired dead members before running a probe cycle
  swim_membership_gc(inst->membership, inst->dead_node_expiry_ms,
                     get_monotonic_time_ms());

  // 2. Select target
  // Retrieve all active members (excluding self)
  int active_count = swim_membership_count(inst->membership);
  if (active_count == 0) {
    // Re-arm probe timer
    swim_timer_add(inst->timer, inst->protocol_period_ms / 100, "probe",
                   probe_timer_cb, inst, NULL);
    return;
  }

  // Shuffle target selection
  if (inst->shuffle_idx >= inst->shuffle_count) {
    // Grow the shuffle list in 8-element buckets so a real reallocation
    // only happens when the cluster crosses an 8-member boundary.
    int need = (int)align8((size_t)active_count);
    if (need > inst->shuffle_cap) {
      swim_member_t *grown =
          realloc(inst->shuffle_list, (size_t)need * sizeof(swim_member_t));
      if (grown) {
        inst->shuffle_list = grown;
        inst->shuffle_cap = need;
      }
    }
    // shuffle_cap < active_count only if the realloc above failed (it leaves
    // the old buffer intact, so nothing leaks). In that case skip probing this
    // period; the timer re-arms and the next period retries the grow. A missed
    // probe is harmless for a failure detector, and there is no logging channel
    // to report it through (see Observability, DESIGN.md S12).
    if (inst->shuffle_cap >= active_count) {
      inst->shuffle_count = swim_membership_list(
          inst->membership, inst->shuffle_list, active_count, false);
      // Fisher-Yates Shuffle
      for (int i = inst->shuffle_count - 1; i > 0; i--) {
        int j = rand_r(&inst->rand_state) % (i + 1);
        swim_member_t tmp = inst->shuffle_list[i];
        inst->shuffle_list[i] = inst->shuffle_list[j];
        inst->shuffle_list[j] = tmp;
      }
    } else {
      inst->shuffle_count = 0;
    }
    inst->shuffle_idx = 0;
  }

  if (inst->shuffle_count > 0) {
    swim_node_id_t target = inst->shuffle_list[inst->shuffle_idx].id;
    inst->shuffle_idx++;

    // Probe selected target
    uint32_t seq = inst->seq++;
    inst->pending_probe.state = PROBE_DIRECT;
    inst->pending_probe.target = target;
    inst->pending_probe.seq = seq;
    inst->pending_probe.sent_ms = get_monotonic_time_ms();

    send_ping(inst, &target, seq);

    // Arm timeout alarm
    arm_probe_timeout(inst, inst->ping_timeout_ms / 100, seq);
  }

  // Re-arm probe timer
  swim_timer_add(inst->timer, inst->protocol_period_ms / 100, "probe",
                 probe_timer_cb, inst, NULL);
}


// Fires when a suspected node has not refuted suspicion within
// suspicion_timeout_ms. Declares it dead and enqueues a gossip event.
static void suspicion_timer_cb(void *ctx, swim_timer_event_t ev, void *param) {
  swim_instance_t *inst = (swim_instance_t *)ctx;
  swim_node_id_t *target = (swim_node_id_t *)param;

  if (ev == SWIM_TIMER_ALARM) {
    const swim_member_t *m = swim_membership_get(inst->membership, target);
    if (m && m->status == SWIM_STATUS_SUSPECT) {
      // Declare dead
      swim_membership_apply_event(inst->membership, SWIM_STATUS_DEAD, target,
                                  m->incarnation, get_monotonic_time_ms());
      swim_gossip_queue_enqueue(inst->gossip_queue, SWIM_STATUS_DEAD, target,
                                m->incarnation, 1);
      queue_notification(inst, SWIM_NODE_DOWN, target);
    }
  }
  free(target);
}

// Fires when a direct ping times out. Escalates to indirect probing by sending
// PING-REQs to a random subset of helpers. If no helpers are available, or a
// second timeout fires with state still INDIRECT, the target is declared dead.
static void probe_timeout_cb(swim_instance_t *inst, uint32_t seq) {
  if (inst->pending_probe.state == PROBE_DIRECT &&
      inst->pending_probe.seq == seq) {
    // Direct ping timed out, transition to indirect pinging
    inst->pending_probe.state = PROBE_INDIRECT;

    // Retrieve active members to select helpers
    int active_count = swim_membership_count(inst->membership);
    if (active_count > 0) {
      swim_member_t *helpers = malloc(active_count * sizeof(swim_member_t));
      if (helpers) {
        int count = swim_membership_list(inst->membership, helpers,
                                         active_count, false);
        // Exclude target and self from helpers list
        int write_idx = 0;
        for (int i = 0; i < count; i++) {
          if (swim_node_id_compare(&helpers[i].id,
                                   &inst->pending_probe.target) != 0) {
            helpers[write_idx++] = helpers[i];
          }
        }
        count = write_idx;

        // Pick up to k random helpers
        uint32_t k = inst->ping_req_fanout;
        for (uint32_t i = 0; i < k && count > 0; i++) {
          int idx = rand_r(&inst->rand_state) % count;
          send_ping_req(inst, &helpers[idx].id, &inst->pending_probe.target,
                        seq);

          // Remove selected helper from list
          helpers[idx] = helpers[count - 1];
          count--;
        }
        free(helpers);
      }
    }

    // Arm second timeout alarm for indirect probe
    arm_probe_timeout(inst, inst->ping_timeout_ms / 100, seq);

  } else if (inst->pending_probe.state == PROBE_INDIRECT &&
             inst->pending_probe.seq == seq) {
    // Indirect ping also timed out -> Declare suspect
    swim_node_id_t target = inst->pending_probe.target;
    inst->pending_probe.state = PROBE_NONE;

    const swim_member_t *m = swim_membership_get(inst->membership, &target);
    if (m && m->status == SWIM_STATUS_ALIVE) {
      swim_membership_apply_event(inst->membership, SWIM_STATUS_SUSPECT,
                                  &target, m->incarnation,
                                  get_monotonic_time_ms());
      swim_gossip_queue_enqueue(inst->gossip_queue, SWIM_STATUS_SUSPECT,
                                &target, m->incarnation, 1);
      queue_notification(inst, SWIM_NODE_SUSPECT, &target);

      // Start suspicion timer
      swim_node_id_t *suspect_param = malloc(sizeof(swim_node_id_t));
      if (suspect_param) {
        *suspect_param = target;
        char alarm_name[384];
        suspect_key(alarm_name, sizeof(alarm_name), &target);
        swim_timer_add(inst->timer, inst->suspicion_timeout_ms / 100,
                       alarm_name, suspicion_timer_cb, inst, suspect_param);
      }
    }
  }
}

// Periodic timer that pings seed nodes until the cluster is joined. While
// alone, pings all seeds; once peers are known, pings a random seed to keep
// the path open. Re-arms itself unconditionally.
static void seed_retry_timer_cb(void *ctx, swim_timer_event_t ev, void *param) {
  (void)param;
  swim_instance_t *inst = (swim_instance_t *)ctx;
  if (ev == SWIM_TIMER_CANCEL)
    return;

  if (inst->seed_count > 0) {
    int active_count = swim_membership_count(inst->membership);
    if (active_count == 0) {
      // Alone: Ping all seeds to discover cluster
      for (int i = 0; i < inst->seed_count; i++) {
        send_ping(inst, &inst->seeds[i], inst->seq++);
      }
    } else {
      // In a cluster: Ping one random seed to heal partitions
      int idx = rand_r(&inst->rand_state) % inst->seed_count;
      send_ping(inst, &inst->seeds[idx], inst->seq++);
    }
  }

  // Re-arm seed retry timer
  swim_timer_add(inst->timer, inst->seed_retry_interval_ms / 100, "seed_retry",
                 seed_retry_timer_cb, inst, NULL);
}

// Helper functions moved above

// Helper to send messages
static void send_message(swim_instance_t *inst, const swim_node_id_t *dest,
                         uint8_t type, const swim_node_id_t *sender,
                         uint32_t seq, const swim_node_id_t *peer) {
  uint8_t buf[SWIM_MAX_PACKET_SIZE];
  swim_gossip_queue_t *q_to_pass =
      (type == SWIM_MSG_LEAVE) ? NULL : inst->gossip_queue;
  int len = swim_encode_message(type, sender, seq, peer, q_to_pass,
                                swim_membership_count(inst->membership), buf,
                                sizeof(buf));
  if (len > 0) {
    swim_udp_send(inst->udp, dest, buf, len);
  } else {
    char node_str[350];
    if (swim_node_id_format(dest, node_str, sizeof(node_str)) == 0) {
      char warn_msg[512];
      snprintf(warn_msg, sizeof(warn_msg), "message dropped to %s", node_str);
      swim_feed_put(inst->feed, 2, "warning", warn_msg);
    }
  }
}

static void send_ping(swim_instance_t *inst, const swim_node_id_t *dest,
                      uint32_t seq) {
  send_message(inst, dest, SWIM_MSG_PING, &inst->self_id, seq, NULL);
}

static void send_ack(swim_instance_t *inst, const swim_node_id_t *dest,
                     uint32_t seq) {
  send_message(inst, dest, SWIM_MSG_ACK, &inst->self_id, seq, NULL);
}

static void send_ping_req(swim_instance_t *inst, const swim_node_id_t *helper,
                          const swim_node_id_t *target, uint32_t seq) {
  send_message(inst, helper, SWIM_MSG_PING_REQ, &inst->self_id, seq, target);
}

static void send_fwd_ack(swim_instance_t *inst, const swim_node_id_t *dest,
                         const swim_node_id_t *source, uint32_t seq) {
  send_message(inst, dest, SWIM_MSG_FWD_ACK, &inst->self_id, seq, source);
}

// Queue a membership change for later delivery. Caller must hold inst->mutex.
// Does not invoke any callback. On allocation failure the notification is
// dropped (membership state is still correct; only the observer signal is
// lost), which is preferable to running callbacks under the lock.
static void queue_notification(swim_instance_t *inst, swim_event_t event,
                               const swim_node_id_t *node) {
  if (inst->pending_count == inst->pending_capacity) {
    int new_cap = inst->pending_capacity == 0 ? 16 : inst->pending_capacity * 2;
    pending_notify_t *p =
        realloc(inst->pending, new_cap * sizeof(pending_notify_t));
    if (!p)
      return;
    inst->pending = p;
    inst->pending_capacity = new_cap;
  }
  inst->pending[inst->pending_count].event = event;
  inst->pending[inst->pending_count].node = *node;
  inst->pending_count++;

  const char *verb = event == SWIM_NODE_UP        ? "up"
                     : event == SWIM_NODE_SUSPECT ? "suspect"
                                                  : "down";
  char node_str[350];
  if (swim_node_id_format(node, node_str, sizeof(node_str)) == 0) {
    swim_feed_put(inst->feed, 3, "node", verb, node_str);
  }
}

// Move the queued notifications and a snapshot of the subscriber list out of
// the instance into a self-contained batch. Caller must hold inst->mutex.
// After this returns the instance owns no pending work, so the batch can be
// dispatched after every lock is dropped.
static void take_notifications(swim_instance_t *inst, notify_batch_t *batch) {
  batch->items = inst->pending;
  batch->count = inst->pending_count;
  inst->pending = NULL;
  inst->pending_count = 0;
  inst->pending_capacity = 0;

  batch->sub_count = inst->subscriber_count;
  if (batch->sub_count > 0) {
    memcpy(batch->subs, inst->subscribers,
           batch->sub_count * sizeof(swim_sub_t));
  }
}

// Deliver a batch to its subscribers. MUST be called with no locks held: the
// callbacks may re-enter the public API. Touches only the batch (stack/heap
// copies), never the originating instance, so it is safe even if that instance
// is being torn down concurrently. Consumes (frees) the batch.
static void dispatch_notifications(notify_batch_t *batch) {
  for (int i = 0; i < batch->count; i++) {
    char node_str[350];
    swim_node_id_format(&batch->items[i].node, node_str, sizeof(node_str));
    for (int s = 0; s < batch->sub_count; s++) {
      batch->subs[s].cb(batch->subs[s].ctx, batch->items[i].event, node_str);
    }
  }
  free(batch->items);
  batch->items = NULL;
  batch->count = 0;
}

// Mark a node as alive on receipt of any message from it. Adds it if unknown,
// clears SUSPECT status if it was suspected. Self-messages are ignored.
static void update_node_alive(swim_instance_t *inst,
                              const swim_node_id_t *node) {
  if (swim_node_id_compare(node, &inst->self_id) == 0) {
    return;
  }

  const swim_member_t *m = swim_membership_get(inst->membership, node);
  if (!m) {
    int rc = swim_membership_apply_event(inst->membership, SWIM_STATUS_ALIVE,
                                         node, 0, get_monotonic_time_ms());
    if (rc == 0) {
      swim_gossip_queue_enqueue(inst->gossip_queue, SWIM_STATUS_ALIVE, node, 0,
                                1);
      queue_notification(inst, SWIM_NODE_UP, node);
    }
  } else if (m->status == SWIM_STATUS_SUSPECT) {
    swim_membership_set_alive(inst->membership, node, m->incarnation);

    char alarm_name[384];
    suspect_key(alarm_name, sizeof(alarm_name), node);
    swim_timer_cancel(inst->timer, alarm_name);

    swim_gossip_queue_enqueue(inst->gossip_queue, SWIM_STATUS_ALIVE, node,
                              m->incarnation, 1);
    queue_notification(inst, SWIM_NODE_UP, node);
  }
}

// Drop relay entries whose indirect-probe window has elapsed. Without this,
// entries for targets that never ack are never reclaimed and the table fills
// up, permanently blocking new ping_req relay work.
static void relay_gc(swim_instance_t *inst) {
  for (int i = 0; i < inst->relay_count; i++) {
    if (inst->current_tick >= inst->relays[i].expiry_tick) {
      inst->relays[i] = inst->relays[inst->relay_count - 1];
      inst->relay_count--;
      i--;
    }
  }
}

static int recv_message(swim_instance_t *inst, swim_node_id_t *src,
                        swim_message_t *msg) {
  uint8_t buf[SWIM_MAX_PACKET_SIZE];
  int len = swim_udp_recv(inst->udp, src, buf, sizeof(buf));
  if (len <= 0) {
    return -1;
  }
  return swim_decode_message(buf, len, msg);
}

// Packet receiver and protocol handler
static void swim_protocol_handle_incoming(swim_instance_t *inst) {
  swim_node_id_t src;
  swim_message_t msg;
  int rc;

  if (recv_message(inst, &src, &msg) != 0)
    return;

  // Since UDP recv doesn't contain the cookie, populate it from the decoded
  // message sender ID
  strcpy(src.cookie, msg.sender.cookie);

  // Apply gossip rules for incoming sender status first
  update_node_alive(inst, &msg.sender);

  // 1. Process Piggybacked Gossip Events
  for (int i = 0; i < msg.event_count; i++) {
    const swim_member_t *ev = &msg.events[i];

    // Exclude self-events from direct updates (dealt with via refutation below)
    if (swim_node_id_compare(&ev->id, &inst->self_id) != 0) {
      rc =
          swim_membership_apply_event(inst->membership, ev->status, &ev->id,
                                      ev->incarnation, get_monotonic_time_ms());
      if (rc == 0) {
        swim_gossip_queue_enqueue(inst->gossip_queue, ev->status, &ev->id,
                                  ev->incarnation, 1);

        if (ev->status == SWIM_STATUS_DEAD) {
          queue_notification(inst, SWIM_NODE_DOWN, &ev->id);
          // Cancel suspicion timer if any
          char alarm_name[384];
          suspect_key(alarm_name, sizeof(alarm_name), &ev->id);
          swim_timer_cancel(inst->timer, alarm_name);
        } else if (ev->status == SWIM_STATUS_SUSPECT) {
          queue_notification(inst, SWIM_NODE_SUSPECT, &ev->id);

          // Start suspicion timer
          swim_node_id_t *suspect_param = malloc(sizeof(swim_node_id_t));
          if (suspect_param) {
            *suspect_param = ev->id;
            char alarm_name[384];
            suspect_key(alarm_name, sizeof(alarm_name), &ev->id);
            swim_timer_add(inst->timer, inst->suspicion_timeout_ms / 100,
                           alarm_name, suspicion_timer_cb, inst, suspect_param);
          }
        } else if (ev->status == SWIM_STATUS_ALIVE) {
          queue_notification(inst, SWIM_NODE_UP, &ev->id);
          // Cancel suspicion timer
          char alarm_name[384];
          suspect_key(alarm_name, sizeof(alarm_name), &ev->id);
          swim_timer_cancel(inst->timer, alarm_name);
        }
      }
    } else {
      // Rumor about ourselves! Self-refutation logic
      if (ev->status == SWIM_STATUS_SUSPECT || ev->status == SWIM_STATUS_DEAD) {
        if (ev->incarnation >= inst->incarnation) {
          inst->incarnation = ev->incarnation + 1;
          swim_gossip_queue_enqueue(inst->gossip_queue, SWIM_STATUS_ALIVE,
                                    &inst->self_id, inst->incarnation, 2);
        }
      }
    }
  }

  // 2. Dispatch message by type
  switch (msg.type) {
  case SWIM_MSG_PING:
    send_ack(inst, &msg.sender, msg.seq);
    break;

  case SWIM_MSG_ACK:
    if (inst->pending_probe.state != PROBE_NONE &&
        swim_node_id_compare(&msg.sender, &inst->pending_probe.target) == 0 &&
        msg.seq == inst->pending_probe.seq) {
      // Report RTT only for a clean direct round-trip.
      if (inst->pending_probe.state == PROBE_DIRECT) {
        uint64_t rtt = get_monotonic_time_ms() - inst->pending_probe.sent_ms;
        char node_str[350];
        if (swim_node_id_format(&msg.sender, node_str, sizeof(node_str)) == 0) {
          char rtt_str[32];
          snprintf(rtt_str, sizeof(rtt_str), "%llu", (unsigned long long)rtt);
          swim_feed_put(inst->feed, 4, "ping", "rtt", node_str, rtt_str);
        }
      }
      inst->pending_probe.state = PROBE_NONE;
      swim_timer_cancel(inst->timer, "probe_timeout");
    }
    // Process potential helper relays
    for (int i = 0; i < inst->relay_count; i++) {
      if (swim_node_id_compare(&msg.sender, &inst->relays[i].target) == 0 &&
          msg.seq == inst->relays[i].seq &&
          inst->current_tick < inst->relays[i].expiry_tick) {
        send_fwd_ack(inst, &inst->relays[i].requester, &msg.sender, msg.seq);

        // Clear entry
        inst->relays[i] = inst->relays[inst->relay_count - 1];
        inst->relay_count--;
        i--;
      }
    }
    break;

  case SWIM_MSG_PING_REQ:
    // We act as a helper relay: ping msg.peer on behalf of msg.sender
    relay_gc(inst);
    /* 32-slot cap. Practical risk is low: with ping_req_fanout=3 and
     * short-lived entries (removed on ack or timeout), hitting 32
     * concurrent relays requires tens of simultaneous failed probes. */
    if (inst->relay_count < 32) {
      relay_probe_t *r = &inst->relays[inst->relay_count++];
      r->requester = msg.sender;
      r->target = msg.peer;
      r->seq = msg.seq;
      r->expiry_tick = inst->current_tick + (inst->ping_timeout_ms / 100);

      send_ping(inst, &msg.peer, msg.seq);
    }
    break;

  case SWIM_MSG_FWD_ACK:
    // Relayed ack received via helper
    if (inst->pending_probe.state != PROBE_NONE &&
        swim_node_id_compare(&msg.peer, &inst->pending_probe.target) == 0 &&
        msg.seq == inst->pending_probe.seq) {
      inst->pending_probe.state = PROBE_NONE;
      swim_timer_cancel(inst->timer, "probe_timeout");
    }
    break;

  case SWIM_MSG_LEAVE: {
    // Graceful leave notification: transition the sender node to DEAD.
    uint64_t inc = 0;
    const swim_member_t *m_info =
        swim_membership_get(inst->membership, &msg.sender);
    if (m_info) {
      inc = m_info->incarnation + 1;
    } else {
      inc = get_now_ms();
    }
    rc = swim_membership_apply_event(inst->membership, SWIM_STATUS_DEAD,
                                     &msg.sender, inc, get_monotonic_time_ms());
    if (rc == 0) {
      swim_gossip_queue_enqueue(inst->gossip_queue, SWIM_STATUS_DEAD,
                                &msg.sender, inc, 1);
      queue_notification(inst, SWIM_NODE_DOWN, &msg.sender);
      char alarm_name[384];
      suspect_key(alarm_name, sizeof(alarm_name), &msg.sender);
      swim_timer_cancel(inst->timer, alarm_name);
    }
    break;
  }
  }
}

// Background event loop thread
static void *swim_protocol_thread_entry(void *arg) {
  return swim_protocol_loop((swim_instance_t *)arg);
}

static void *swim_protocol_loop(swim_instance_t *instance) {
  uint64_t now = get_monotonic_time_ms();
  uint64_t exp = now + 100;

  while (atomic_load_explicit(&instance->running, memory_order_relaxed)) {
    now = get_monotonic_time_ms();

    // 1. Tick logical timer
    while (now > exp) {
      notify_batch_t batch;
      pthread_mutex_lock(&instance->mutex);
      instance->current_tick++;
      swim_timer_tick(instance->timer);
      // Emit cluster size once per tick, only when it has changed.
      uint64_t sz = (uint64_t)swim_membership_count(instance->membership);
      if (sz != instance->last_cluster_size) {
        instance->last_cluster_size = sz;
        char size_str[32];
        snprintf(size_str, sizeof(size_str), "%llu", (unsigned long long)sz);
        swim_feed_put(instance->feed, 3, "cluster", "size", size_str);
      }
      take_notifications(instance, &batch);
      pthread_mutex_unlock(&instance->mutex);
      dispatch_notifications(&batch);
      exp += 100;
    }

    // 2. Compute timeout for poll()
    int timeout_ms = (int)(exp - now);
    if (timeout_ms < 0)
      timeout_ms = 0;

    // 3. Poll socket descriptor
    struct pollfd pfd;
    pfd.fd = swim_udp_fd(instance->udp);
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret > 0 && (pfd.revents & POLLIN)) {
      notify_batch_t batch;
      pthread_mutex_lock(&instance->mutex);
      swim_protocol_handle_incoming(instance);
      take_notifications(instance, &batch);
      pthread_mutex_unlock(&instance->mutex);
      dispatch_notifications(&batch);
    }
  }
  return NULL;
}

// --- Public API Implementations ---

int swim_start(const swim_start_opts_t *opts) {
  if (!opts || !opts->self || !opts->name || opts->name[0] == '\0') {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Invalid start options: self and name are mandatory");
  }

  swim_node_id_t self_id;
  if (swim_node_id_parse(&self_id, opts->self) != 0)
    return -1;

  pthread_mutex_lock(&g_instances_mutex);

  // Look for existing named instance
  if (find_instance(opts->name) != NULL) {
    pthread_mutex_unlock(&g_instances_mutex);
    return swim_set_error(SWIM_ERR_BAD_STATE, "Instance already exists");
  }

  // Find slot in g_instances
  int slot = -1;
  for (int i = 0; i < 16; i++) {
    if (g_instances[i] == NULL) {
      slot = i;
      break;
    }
  }
  if (slot == -1) {
    pthread_mutex_unlock(&g_instances_mutex);
    return swim_set_error(SWIM_ERR_FULL, "Maximum active instances exceeded");
  }

  swim_instance_t *inst = calloc(1, sizeof(*inst));
  if (!inst) {
    pthread_mutex_unlock(&g_instances_mutex);
    return swim_set_error(SWIM_ERR_NOMEM, "Failed to allocate swim_instance_t");
  }

  // Populate configuration
  strncpy(inst->name, opts->name, sizeof(inst->name) - 1);
  inst->protocol_period_ms =
      opts->protocol_period_ms ? opts->protocol_period_ms : 1000;
  inst->ping_timeout_ms = opts->ping_timeout_ms ? opts->ping_timeout_ms : 200;
  inst->ping_req_fanout = opts->ping_req_fanout ? opts->ping_req_fanout : 3;
  inst->suspicion_timeout_ms =
      opts->suspicion_timeout_ms ? opts->suspicion_timeout_ms : 3000;
  inst->seed_retry_interval_ms =
      opts->seed_retry_interval_ms ? opts->seed_retry_interval_ms : 5000;
  inst->dead_node_expiry_ms =
      opts->dead_node_expiry_ms ? opts->dead_node_expiry_ms : 6000;

  // Initialize helper sub-modules
  inst->udp = swim_udp_create(self_id.host, self_id.port);
  if (!inst->udp)
    goto error_cleanup;

  inst->timer = swim_timer_create();
  if (!inst->timer)
    goto error_cleanup;

  inst->membership = swim_membership_create();
  if (!inst->membership)
    goto error_cleanup;

  inst->gossip_queue = swim_gossip_queue_create();
  if (!inst->gossip_queue)
    goto error_cleanup;

  inst->feed = swim_feed_create();
  if (!inst->feed)
    goto error_cleanup;

  // Initialize self Node ID
  inst->self_id = self_id;

  // Seed incarnation with current time (wall-clock time in milliseconds)
  inst->incarnation = get_now_ms();
  inst->seq = 1;

  // Parse seeds list
  if (opts->seeds) {
    int n = 0;
    while (opts->seeds[n]) n++;
    if (n > 0) {
      inst->seeds = malloc(n * sizeof(swim_node_id_t));
      if (!inst->seeds)
        goto error_cleanup;
      for (int i = 0; i < n; i++) {
        if (swim_node_id_parse(&inst->seeds[i], opts->seeds[i]) != 0) {
          free(inst->seeds);
          inst->seeds = NULL;
          goto error_cleanup;
        }
      }
      inst->seed_count = n;
    }
  }

  // Initialize thread synchronization
  pthread_mutex_init(&inst->mutex, NULL);
  atomic_store_explicit(&inst->running, true, memory_order_relaxed);

  // Seed this instance's private PRNG. Mix in the instance pointer so
  // instances started in the same second diverge.
  inst->rand_state = (unsigned int)time(NULL) ^ (unsigned int)pthread_self() ^
                     (unsigned int)(uintptr_t)inst;

  // Enqueue self-announcement
  swim_gossip_queue_enqueue(inst->gossip_queue, SWIM_STATUS_ALIVE,
                            &inst->self_id, inst->incarnation, 1);

  // Set up logical alarms
  // 1. Probe cycle timer (self-rearming)
  swim_timer_add(inst->timer, inst->protocol_period_ms / 100, "probe",
                 probe_timer_cb, inst, NULL);
  // 2. Seed retry timer (self-rearming) - Schedule the first retry timer to run
  // in 1 tick (100ms) to bootstrap immediately
  swim_timer_add(inst->timer, 1, "seed_retry", seed_retry_timer_cb, inst, NULL);

  // Start background event loop thread
  if (pthread_create(&inst->thread, NULL, swim_protocol_thread_entry, inst) !=
      0) {
    atomic_store_explicit(&inst->running, false, memory_order_relaxed);
    pthread_mutex_destroy(&inst->mutex);
    goto error_cleanup;
  }

  g_instances[slot] = inst;

  pthread_mutex_unlock(&g_instances_mutex);
  return 0;

error_cleanup:
  if (inst->feed)
    swim_feed_destroy(inst->feed);
  if (inst->udp)
    swim_udp_destroy(inst->udp);
  if (inst->timer)
    swim_timer_destroy(inst->timer);
  if (inst->membership)
    swim_membership_destroy(inst->membership);
  if (inst->gossip_queue)
    swim_gossip_queue_destroy(inst->gossip_queue);
  free(inst->seeds);
  free(inst->pending);
  free(inst);
  pthread_mutex_unlock(&g_instances_mutex);
  return -1;
}

int swim_leave(const char *name) {
  if (!name || name[0] == '\0') {
    return swim_set_error(SWIM_ERR_INVALID, "Instance name is mandatory");
  }
  pthread_mutex_lock(&g_instances_mutex);
  swim_instance_t *inst = find_instance(name);
  if (!inst) {
    pthread_mutex_unlock(&g_instances_mutex);
    return swim_set_error(SWIM_ERR_BAD_STATE, "Instance not found");
  }

  // Remove from the registry before releasing the lock so a racing
  // swim_leave/lookup can no longer find this instance (it gets BAD_STATE).
  // After this point inst is private to this call and is torn down without
  // holding the global lock.
  for (int i = 0; i < 16; i++) {
    if (g_instances[i] == inst) {
      g_instances[i] = NULL;
      break;
    }
  }

  // Stop the protocol task. running is atomic, so the worker's unlocked
  // read in swim_protocol_loop sees this store without a data race.
  atomic_store_explicit(&inst->running, false, memory_order_relaxed);

  pthread_mutex_unlock(&g_instances_mutex);

  // Join background thread
  pthread_join(inst->thread, NULL);

  // Graceful broadcast of self death directly to random peers (not via gossip
  // queue)
  pthread_mutex_lock(&inst->mutex);
  int active = swim_membership_count(inst->membership);
  if (active > 0) {
    swim_member_t *list = malloc(active * sizeof(swim_member_t));
    if (list) {
      int count = swim_membership_list(inst->membership, list, active, false);
      uint32_t fanout = (uint32_t)ceil(count * 0.25);
      if (fanout < 8)
        fanout = 8;

      // Bump incarnation first
      inst->incarnation = get_now_ms();

      for (uint32_t i = 0; i < fanout && count > 0; i++) {
        int idx = rand_r(&inst->rand_state) % count;
        send_message(inst, &list[idx].id, SWIM_MSG_LEAVE, &inst->self_id,
                     inst->seq++, NULL);
        list[idx] = list[count - 1];
        count--;
      }
      free(list);
    }
  }
  pthread_mutex_unlock(&inst->mutex);

  // Destroy structures
  swim_udp_destroy(inst->udp);
  swim_timer_destroy(inst->timer);
  swim_membership_destroy(inst->membership);
  swim_gossip_queue_destroy(inst->gossip_queue);
  swim_feed_destroy(inst->feed);
  free(inst->seeds);
  free(inst->shuffle_list);
  free(inst->pending);
  pthread_mutex_destroy(&inst->mutex);
  free(inst);

  return 0;
}

int swim_members(const char *name, swim_member_t *out_list, int max_len,
                 bool include_dead) {
  if (!name || name[0] == '\0') {
    return swim_set_error(SWIM_ERR_INVALID, "Instance name is mandatory");
  }
  swim_instance_t *inst = find_and_lock_instance(name);
  if (!inst) {
    return swim_set_error(SWIM_ERR_BAD_STATE, "Instance not found");
  }

  int ret =
      swim_membership_list(inst->membership, out_list, max_len, include_dead);
  pthread_mutex_unlock(&inst->mutex);
  return ret;
}

char *swim_peers(const char *name, bool include_dead, int *count) {
  if (!name || name[0] == '\0' || !count) {
    swim_set_error(SWIM_ERR_INVALID, "Invalid arguments to swim_peers");
    return NULL;
  }
  swim_instance_t *inst = find_and_lock_instance(name);
  if (!inst) {
    swim_set_error(SWIM_ERR_BAD_STATE, "Instance not found");
    return NULL;
  }
  char *buf = swim_membership_peers(inst->membership, include_dead, count);
  pthread_mutex_unlock(&inst->mutex);
  return buf;
}

int swim_subscribe(const char *name, swim_callback_t callback, void *ctx) {
  if (!name || name[0] == '\0') {
    return swim_set_error(SWIM_ERR_INVALID, "Instance name is mandatory");
  }
  if (!callback) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Invalid NULL callback in swim_subscribe");
  }

  swim_instance_t *inst = find_and_lock_instance(name);
  if (!inst) {
    return swim_set_error(SWIM_ERR_BAD_STATE, "Instance not found");
  }

  if (inst->subscriber_count >= 16) {
    pthread_mutex_unlock(&inst->mutex);
    return swim_set_error(SWIM_ERR_FULL, "Maximum subscriber limit reached");
  }

  inst->subscribers[inst->subscriber_count].cb = callback;
  inst->subscribers[inst->subscriber_count].ctx = ctx;
  inst->subscriber_count++;

  pthread_mutex_unlock(&inst->mutex);
  return 0;
}

int swim_unsubscribe(const char *name, swim_callback_t callback, void *ctx) {
  if (!name || name[0] == '\0') {
    return swim_set_error(SWIM_ERR_INVALID, "Instance name is mandatory");
  }
  swim_instance_t *inst = find_and_lock_instance(name);
  if (!inst) {
    return swim_set_error(SWIM_ERR_BAD_STATE, "Instance not found");
  }

  int idx = -1;
  for (int i = 0; i < inst->subscriber_count; i++) {
    if (inst->subscribers[i].cb == callback &&
        inst->subscribers[i].ctx == ctx) {
      idx = i;
      break;
    }
  }

  if (idx != -1) {
    inst->subscribers[idx] = inst->subscribers[inst->subscriber_count - 1];
    inst->subscriber_count--;
  }

  pthread_mutex_unlock(&inst->mutex);
  return 0;
}

int swim_get_event(const char *name, int bufsz, char *buf, int nptr,
                   char **ptr) {
  if (!name || name[0] == '\0') {
    return swim_set_error(SWIM_ERR_INVALID, "Instance name is mandatory");
  }
  pthread_mutex_lock(&g_instances_mutex);
  swim_instance_t *inst = find_instance(name);
  if (!inst) {
    pthread_mutex_unlock(&g_instances_mutex);
    return swim_set_error(SWIM_ERR_BAD_STATE, "Instance not found");
  }

  int rc = swim_feed_get(inst->feed, bufsz, buf, nptr, ptr);
  pthread_mutex_unlock(&g_instances_mutex);
  return rc;
}

int swim_hint_alive(const char *name, const char *peer) {
  if (!name || name[0] == '\0') {
    return swim_set_error(SWIM_ERR_INVALID, "Instance name is mandatory");
  }
  if (!peer) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Invalid NULL peer in swim_hint_alive");
  }
  swim_node_id_t peer_id;
  if (swim_node_id_parse(&peer_id, peer) != 0)
    return -1;

  swim_instance_t *inst = find_and_lock_instance(name);
  if (!inst) {
    return swim_set_error(SWIM_ERR_BAD_STATE, "Instance not found");
  }

  const swim_member_t *m = swim_membership_get(inst->membership, &peer_id);
  if (m) {
    if (m->status == SWIM_STATUS_SUSPECT) {
      // Cancel suspicion timer
      char alarm_name[384];
      suspect_key(alarm_name, sizeof(alarm_name), &peer_id);
      swim_timer_cancel(inst->timer, alarm_name);

      // Re-announce as alive
      swim_membership_set_alive(inst->membership, &peer_id, m->incarnation);
      swim_gossip_queue_enqueue(inst->gossip_queue, SWIM_STATUS_ALIVE, &peer_id,
                                m->incarnation, 1);
      queue_notification(inst, SWIM_NODE_UP, &peer_id);
    }

    // Cancel outstanding probes to this target
    if (inst->pending_probe.state != PROBE_NONE &&
        swim_node_id_compare(&inst->pending_probe.target, &peer_id) == 0) {
      inst->pending_probe.state = PROBE_NONE;
      swim_timer_cancel(inst->timer, "probe_timeout");
    }
  }
  notify_batch_t batch;
  take_notifications(inst, &batch);
  pthread_mutex_unlock(&inst->mutex);

  // Callbacks run with no locks held and touch only the batch, so they may
  // re-enter the API and are unaffected if the instance is torn down meanwhile.
  dispatch_notifications(&batch);
  return 0;
}
