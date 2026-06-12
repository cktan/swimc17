/*
 * swim_main.c — SWIM gossip protocol engine.
 *
 * Implements the full SWIM+Suspicion protocol: probe
 * selection, direct ping, indirect ping-req via helpers,
 * suspicion timers, dead-node GC, and membership event
 * dissemination via the gossip queue.
 *
 * swim_start() allocates a swim_t and returns an opaque
 * handle. Each instance owns its own timer and runs a
 * background protocol thread. swim_leave() tears down
 * the instance and frees the handle.
 *
 * Locking: inst->mutex guards all mutable instance state.
 * swim_leave() uses an atomic_bool (leaving) to ensure
 * only one concurrent caller performs teardown.
 *
 * Telemetry is written directly to the caller-supplied
 * swim_feed_t (optional; NULL disables telemetry).
 */
#define _GNU_SOURCE
#include "swim.h"
#include "swim_codec.h"
#include "swim_errno.h"
#include "swim_gossip_queue.h"
#include "swim_internal.h"
#include "swim_timer.h"
#include "swim_udp.h"

#include <arpa/inet.h>
#include <endian.h>
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

// SipHash-2-4: keyed 64-bit hash (2 compression rounds, 4 finalization rounds).
// Reference: https://131002.net/siphash/siphash.pdf
#define SWIM_ROTL64(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define SWIM_SIPROUND(v0, v1, v2, v3)                                          \
  do {                                                                         \
    (v0) += (v1);                                                              \
    (v1) = SWIM_ROTL64((v1), 13);                                              \
    (v1) ^= (v0);                                                              \
    (v0) = SWIM_ROTL64((v0), 32);                                              \
    (v2) += (v3);                                                              \
    (v3) = SWIM_ROTL64((v3), 16);                                              \
    (v3) ^= (v2);                                                              \
    (v0) += (v3);                                                              \
    (v3) = SWIM_ROTL64((v3), 21);                                              \
    (v3) ^= (v0);                                                              \
    (v2) += (v1);                                                              \
    (v1) = SWIM_ROTL64((v1), 17);                                              \
    (v1) ^= (v2);                                                              \
    (v2) = SWIM_ROTL64((v2), 32);                                              \
  } while (0)

static uint64_t siphash24(const uint8_t key[16], const uint8_t *msg,
                          size_t len) {
  uint64_t k0, k1;
  memcpy(&k0, key, 8);
  memcpy(&k1, key + 8, 8);
  k0 = le64toh(k0);
  k1 = le64toh(k1);

  uint64_t v0 = k0 ^ UINT64_C(0x736f6d6570736575);
  uint64_t v1 = k1 ^ UINT64_C(0x646f72616e646f6d);
  uint64_t v2 = k0 ^ UINT64_C(0x6c7967656e657261);
  uint64_t v3 = k1 ^ UINT64_C(0x7465646279746573);

  const uint8_t *end = msg + (len & ~(size_t)7);
  for (const uint8_t *p = msg; p < end; p += 8) {
    uint64_t m;
    memcpy(&m, p, 8);
    m = le64toh(m);
    v3 ^= m;
    SWIM_SIPROUND(v0, v1, v2, v3);
    SWIM_SIPROUND(v0, v1, v2, v3);
    v0 ^= m;
  }

  uint64_t last = (uint64_t)(len & 0xff) << 56;
  switch (len & 7) {
  case 7:
    last |= (uint64_t)end[6] << 48; /* fallthrough */
  case 6:
    last |= (uint64_t)end[5] << 40; /* fallthrough */
  case 5:
    last |= (uint64_t)end[4] << 32; /* fallthrough */
  case 4:
    last |= (uint64_t)end[3] << 24; /* fallthrough */
  case 3:
    last |= (uint64_t)end[2] << 16; /* fallthrough */
  case 2:
    last |= (uint64_t)end[1] << 8; /* fallthrough */
  case 1:
    last |= (uint64_t)end[0]; /* fallthrough */
  case 0:;
  }

  v3 ^= last;
  SWIM_SIPROUND(v0, v1, v2, v3);
  SWIM_SIPROUND(v0, v1, v2, v3);
  v0 ^= last;

  v2 ^= UINT64_C(0xff);
  SWIM_SIPROUND(v0, v1, v2, v3);
  SWIM_SIPROUND(v0, v1, v2, v3);
  SWIM_SIPROUND(v0, v1, v2, v3);
  SWIM_SIPROUND(v0, v1, v2, v3);

  return v0 ^ v1 ^ v2 ^ v3;
}

// Build the suspicion-timer key for a node. The cookie is part of node
// identity (DESIGN S3), so it must be in the key: otherwise two members that
// share a host:port collide on one key and an event for one cancels the
// other's suspicion timer. Centralized so the arm/cancel keys cannot drift.
static void suspect_key(char *buf, size_t n, const swim_node_id_t *id) {
  snprintf(buf, n, "suspect:%s:%u:%s", id->host, id->port, id->cookie);
}

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

struct swim_t {
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
  atomic_bool leaving;

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

  // In-flight PING-REQ relays: probes we perform as a helper on behalf of
  // another node's failure detector. Capped at 32; entries are removed on
  // ACK or timeout.
  relay_probe_t relays[32];
  int relay_count;

  // Telemetry feed (caller-owned; NULL if not supplied).
  swim_feed_t *feed;
  uint64_t last_cluster_size; // last emitted cluster size, for on-change gating

  uint64_t current_tick;
};

typedef swim_t swim_instance_t;

// Forward declarations
static void *swim_protocol_thread_entry(void *arg);
static void *swim_protocol_loop(swim_instance_t *instance);
static void queue_notification(swim_instance_t *inst, const char *verb,
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
    if (swim_timer_add(inst->timer, inst->protocol_period_ms / 100, "probe",
                       probe_timer_cb, inst, NULL) != 0) {
      if (inst->feed)
        swim_feed_put(inst->feed, 2, "warning",
                      "probe timer failed to re-arm: probing stopped");
    }
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
  if (swim_timer_add(inst->timer, inst->protocol_period_ms / 100, "probe",
                     probe_timer_cb, inst, NULL) != 0) {
    if (inst->feed)
      swim_feed_put(inst->feed, 2, "warning",
                    "probe timer failed to re-arm: probing stopped");
  }
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
      queue_notification(inst, "down", target);
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
      if (!helpers) {
        if (inst->feed)
          swim_feed_put(inst->feed, 2, "warning",
                        "indirect probe skipped: out of memory");
      } else {
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
      queue_notification(inst, "suspect", &target);

      // Start suspicion timer
      swim_node_id_t *suspect_param = malloc(sizeof(swim_node_id_t));
      if (!suspect_param) {
        if (inst->feed)
          swim_feed_put(inst->feed, 2, "warning",
                        "suspicion timer not armed: out of memory");
      } else {
        *suspect_param = target;
        char alarm_name[384];
        suspect_key(alarm_name, sizeof(alarm_name), &target);
        if (swim_timer_add(inst->timer, inst->suspicion_timeout_ms / 100,
                           alarm_name, suspicion_timer_cb, inst,
                           suspect_param) != 0) {
          free(suspect_param);
          if (inst->feed)
            swim_feed_put(inst->feed, 2, "warning",
                          "suspicion timer not armed: timer add failed");
        }
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
  if (swim_timer_add(inst->timer, inst->seed_retry_interval_ms / 100,
                     "seed_retry", seed_retry_timer_cb, inst, NULL) != 0) {
    if (inst->feed)
      swim_feed_put(
          inst->feed, 2, "warning",
          "seed retry timer failed to re-arm: seed discovery stopped");
  }
}

// Helper to send messages
static void send_message(swim_instance_t *inst, const swim_node_id_t *dest,
                         uint8_t type, const swim_node_id_t *sender,
                         uint32_t seq, const swim_node_id_t *peer) {
  uint8_t buf[SWIM_MAX_PACKET_SIZE];
  swim_gossip_queue_t *q_to_pass =
      (type == SWIM_MSG_LEAVE) ? NULL : inst->gossip_queue;
  // Pack message into buf+12, reserving the first 12 bytes for the auth header.
  int len = swim_pack_message(type, sender, seq, peer, q_to_pass,
                              swim_membership_count(inst->membership), buf + 12,
                              (int)sizeof(buf) - 12);
  if (len > 0) {
    // Auth header: [tval 4B BE][hval 8B]
    uint32_t tval = (uint32_t)(get_now_ms() / 1000);
    uint32_t tval_be = htonl(tval);
    memcpy(buf, &tval_be, 4);

    uint8_t hash_in[SWIM_MAX_PACKET_SIZE];
    memcpy(hash_in, &tval_be, 4);
    memcpy(hash_in + 4, buf + 12, (size_t)len);
    uint8_t key[16] = {0};
    size_t namelen = strlen(inst->name);
    memcpy(key, inst->name, namelen < 16 ? namelen : 16);
    uint64_t hval_be = htobe64(siphash24(key, hash_in, 4 + (size_t)len));
    memcpy(buf + 4, &hval_be, 8);

    swim_udp_send(inst->udp, dest, buf, (size_t)(12 + len));
  } else {
    char node_str[350];
    if (inst->feed &&
        swim_node_id_format(dest, node_str, sizeof(node_str)) == 0) {
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

// Write a membership-change record to the feed. Caller must hold inst->mutex.
static void queue_notification(swim_instance_t *inst, const char *verb,
                               const swim_node_id_t *node) {
  if (!inst->feed)
    return;
  char node_str[350];
  if (swim_node_id_format(node, node_str, sizeof(node_str)) == 0)
    swim_feed_put(inst->feed, 3, "node", verb, node_str);
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
      queue_notification(inst, "up", node);
    }
  } else if (m->status == SWIM_STATUS_SUSPECT) {
    swim_membership_set_alive(inst->membership, node, m->incarnation);

    char alarm_name[384];
    suspect_key(alarm_name, sizeof(alarm_name), node);
    swim_timer_cancel(inst->timer, alarm_name);

    swim_gossip_queue_enqueue(inst->gossip_queue, SWIM_STATUS_ALIVE, node,
                              m->incarnation, 1);
    queue_notification(inst, "up", node);
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
  if (len <= 0)
    return -1;
  if (len < 12)
    return -1;

  // Validate tval (±10s of receiver wall clock)
  uint32_t tval_be;
  memcpy(&tval_be, buf, 4);
  uint32_t tval = ntohl(tval_be);
  uint32_t now = (uint32_t)(get_now_ms() / 1000);
  uint32_t diff = tval > now ? tval - now : now - tval;
  if (diff > 10)
    return -1;

  // Validate hval: SipHash-2-4(tval_be4 || message_bytes, key=name_padded_16)
  int msglen = len - 12;
  uint8_t hash_in[SWIM_MAX_PACKET_SIZE];
  memcpy(hash_in, &tval_be, 4);
  memcpy(hash_in + 4, buf + 12, (size_t)msglen);
  uint8_t key[16] = {0};
  size_t namelen = strlen(inst->name);
  memcpy(key, inst->name, namelen < 16 ? namelen : 16);
  uint64_t expected_be = htobe64(siphash24(key, hash_in, 4 + (size_t)msglen));
  uint64_t received_be;
  memcpy(&received_be, buf + 4, 8);
  if (expected_be != received_be)
    return -1;

  return swim_unpack_message(buf + 12, (size_t)msglen, msg);
}

// Packet receiver and protocol handler — drains all pending packets.
static void swim_protocol_handle_incoming(swim_instance_t *inst) {
  swim_node_id_t src;
  swim_message_t msg;
  int rc;

  while (recv_message(inst, &src, &msg) == 0) {

    // Since UDP recv doesn't contain the cookie, populate it from the decoded
    // message sender ID
    strcpy(src.cookie, msg.sender.cookie);

    // Apply gossip rules for incoming sender status first
    update_node_alive(inst, &msg.sender);

    // 1. Process Piggybacked Gossip Events
    for (int i = 0; i < msg.gossip_count; i++) {
      const swim_member_t *ev = &msg.gossip[i];

      // Exclude self-events from direct updates (dealt with via refutation
      // below)
      if (swim_node_id_compare(&ev->id, &inst->self_id) != 0) {
        // Apply the event to local membership state
        rc = swim_membership_apply_event(inst->membership, ev->status, &ev->id,
                                         ev->incarnation,
                                         get_monotonic_time_ms());
        if (rc == 0) {
          // state changed -> re-gossip the event
          swim_gossip_queue_enqueue(inst->gossip_queue, ev->status, &ev->id,
                                    ev->incarnation, 1);

          if (ev->status == SWIM_STATUS_DEAD) {
            queue_notification(inst, "down", &ev->id);
            // Cancel suspicion timer if any
            char alarm_name[384];
            suspect_key(alarm_name, sizeof(alarm_name), &ev->id);
            swim_timer_cancel(inst->timer, alarm_name);
          } else if (ev->status == SWIM_STATUS_SUSPECT) {
            queue_notification(inst, "suspect", &ev->id);

            // Heap-allocate the ID; the suspicion callback takes ownership and
            // frees it. Start suspicion timer
            swim_node_id_t *suspect_param = malloc(sizeof(swim_node_id_t));
            if (!suspect_param) {
              if (inst->feed)
                swim_feed_put(inst->feed, 2, "warning",
                              "suspicion timer not armed: out of memory");
            } else {
              *suspect_param = ev->id;
              char alarm_name[384];
              suspect_key(alarm_name, sizeof(alarm_name), &ev->id);
              if (swim_timer_add(inst->timer, inst->suspicion_timeout_ms / 100,
                                 alarm_name, suspicion_timer_cb, inst,
                                 suspect_param) != 0) {
                free(suspect_param);
                if (inst->feed)
                  swim_feed_put(inst->feed, 2, "warning",
                                "suspicion timer not armed: timer add failed");
              }
            }
          } else if (ev->status == SWIM_STATUS_ALIVE) {
            queue_notification(inst, "up", &ev->id);
            // Cancel suspicion timer
            char alarm_name[384];
            suspect_key(alarm_name, sizeof(alarm_name), &ev->id);
            swim_timer_cancel(inst->timer, alarm_name);
          }
        }
      } else {
        // Rumor about ourselves! Self-refutation logic
        if (ev->status == SWIM_STATUS_SUSPECT ||
            ev->status == SWIM_STATUS_DEAD) {
          if (ev->incarnation >= inst->incarnation) {
            inst->incarnation = ev->incarnation + 1;
            swim_gossip_queue_enqueue(inst->gossip_queue, SWIM_STATUS_ALIVE,
                                      &inst->self_id, inst->incarnation,
                                      REFUTATION_MULTIPLIER);
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
        if (inst->pending_probe.state == PROBE_DIRECT && inst->feed) {
          uint64_t rtt = get_monotonic_time_ms() - inst->pending_probe.sent_ms;
          char node_str[350];
          if (swim_node_id_format(&msg.sender, node_str, sizeof(node_str)) ==
              0) {
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
                                       &msg.sender, inc,
                                       get_monotonic_time_ms());
      if (rc == 0) {
        swim_gossip_queue_enqueue(inst->gossip_queue, SWIM_STATUS_DEAD,
                                  &msg.sender, inc, 1);
        queue_notification(inst, "down", &msg.sender);
        char alarm_name[384];
        suspect_key(alarm_name, sizeof(alarm_name), &msg.sender);
        swim_timer_cancel(inst->timer, alarm_name);
      }
      break;
    }
    } // end switch
  } // end while
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
      pthread_mutex_lock(&instance->mutex);
      instance->current_tick++;
      swim_timer_tick(instance->timer);
      // Emit cluster size once per tick, only when it has changed.
      if (instance->feed) {
        uint64_t sz = (uint64_t)swim_membership_count(instance->membership);
        if (sz != instance->last_cluster_size) {
          instance->last_cluster_size = sz;
          char size_str[32];
          snprintf(size_str, sizeof(size_str), "%llu", (unsigned long long)sz);
          swim_feed_put(instance->feed, 3, "cluster", "size", size_str);
        }
      }
      pthread_mutex_unlock(&instance->mutex);
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
      pthread_mutex_lock(&instance->mutex);
      swim_protocol_handle_incoming(instance);
      pthread_mutex_unlock(&instance->mutex);
    }
  }
  return NULL;
}

// --- Public API Implementations ---

swim_start_opts_t swim_opts_for(int n, uint64_t detect_ms) {
  swim_start_opts_t opts = {0};

  if (n > 1 && detect_ms > 0) {
    // ceil(log2(n)) is the suspicion multiplier from the SWIM+Susp paper.
    // It represents the number of protocol periods a suspect node is given
    // to refute the accusation before being declared dead. Scaling by log2(N)
    // ensures the suspicion has propagated to every node with high probability
    // before the declaration is made.
    double logn = ceil(log2((double)n));

    // Invert the worst-case detection latency formula to solve for T:
    //   detect_ms = T x (1.4 + logn)
    //   T = detect_ms / (1.4 + logn)
    // The 1.4 factor accounts for one full probe period (1.0) plus two
    // ping timeouts at T/5 each (0.4), covering both the direct and
    // indirect probe phases before the suspicion timer starts.
    uint64_t T = (uint64_t)((double)detect_ms / (1.4 + logn));

    if (T > 0) {
      opts.protocol_period_ms = T;
      opts.ping_timeout_ms = T / 5;
      opts.ping_req_fanout = 3; // SWIM paper default; independent of N and T
      opts.suspicion_timeout_ms = (uint64_t)(logn * (double)T);
      opts.dead_node_expiry_ms = 2 * opts.suspicion_timeout_ms;
      opts.seed_retry_interval_ms =
          5 * T; // retry unreachable seeds every 5 periods
      return opts;
    }
  }

  // Inputs are degenerate (n <= 1, detect_ms == 0, or T rounded to 0).
  // Return the same defaults swim_start() applies for zero-initialized fields.
  opts.protocol_period_ms = 1000;
  opts.ping_timeout_ms = 200;
  opts.ping_req_fanout = 3;
  opts.suspicion_timeout_ms = 3000;
  opts.seed_retry_interval_ms = 5000;
  opts.dead_node_expiry_ms = 6000;
  return opts;
}

swim_t *swim_start(const swim_start_opts_t *opts) {
  if (!opts || !opts->self || !opts->name || opts->name[0] == '\0') {
    swim_set_error(SWIM_ERR_INVALID,
                   "Invalid start options: self and name are mandatory");
    return NULL;
  }

  // Extract self_id
  swim_node_id_t self_id;
  if (swim_node_id_parse(&self_id, opts->self)) {
    return NULL;
  }

  // Allocate an instance
  swim_instance_t *inst = calloc(1, sizeof(*inst));
  if (!inst) {
    swim_set_error(SWIM_ERR_NOMEM, "Failed to allocate swim_instance_t");
    return NULL;
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

  inst->feed = opts->feed;

  // Initialize self Node ID
  inst->self_id = self_id;

  // Seed incarnation with current time (wall-clock time in milliseconds)
  inst->incarnation = get_now_ms();
  inst->seq = 1;

  // Parse seeds list
  if (opts->seeds) {
    // Count how many seeds
    int n = 0;
    while (opts->seeds[n])
      n++;
    if (n > 0) {
      // Alloc array and fill
      inst->seeds = malloc(n * sizeof(swim_node_id_t));
      if (!inst->seeds) {
        swim_set_error(SWIM_ERR_NOMEM, "Failed to allocate seeds array");
        goto error_cleanup;
      }
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
  atomic_store_explicit(&inst->leaving, false, memory_order_relaxed);

  // Seed this instance's private PRNG. Mix in the instance pointer so
  // instances started in the same second diverge.
  inst->rand_state = (unsigned int)time(NULL) ^ (unsigned int)pthread_self() ^
                     (unsigned int)(uintptr_t)inst;

  // Enqueue self-announcement
  swim_gossip_queue_enqueue(inst->gossip_queue, SWIM_STATUS_ALIVE,
                            &inst->self_id, inst->incarnation, 1);

  // Set up logical alarms
  // 1. Probe cycle timer (self-rearming)
  if (swim_timer_add(inst->timer, inst->protocol_period_ms / 100, "probe",
                     probe_timer_cb, inst, NULL) != 0) {
    swim_set_error(SWIM_ERR_NOMEM, "Failed to arm probe timer");
    goto error_cleanup;
  }
  // 2. Seed retry timer (self-rearming) — fire after 1 tick (100ms) to
  // bootstrap seed discovery immediately rather than waiting a full period.
  if (swim_timer_add(inst->timer, 1, "seed_retry", seed_retry_timer_cb, inst,
                     NULL) != 0) {
    swim_set_error(SWIM_ERR_NOMEM, "Failed to arm seed retry timer");
    goto error_cleanup;
  }

  // Start background event loop thread
  if (pthread_create(&inst->thread, NULL, swim_protocol_thread_entry, inst) !=
      0) {
    swim_set_error(SWIM_ERR_BAD_STATE, "Failed to create protocol thread");
    atomic_store_explicit(&inst->running, false, memory_order_relaxed);
    pthread_mutex_destroy(&inst->mutex);
    goto error_cleanup;
  }

  return inst;

error_cleanup:
  if (inst->udp)
    swim_udp_destroy(inst->udp);
  if (inst->timer)
    swim_timer_destroy(inst->timer);
  if (inst->membership)
    swim_membership_destroy(inst->membership);
  if (inst->gossip_queue)
    swim_gossip_queue_destroy(inst->gossip_queue);
  free(inst->seeds);
  free(inst);
  return NULL;
}

int swim_leave(swim_t *inst) {
  if (!inst) {
    return swim_set_error(SWIM_ERR_INVALID, "NULL instance");
  }

  // Claim exclusive ownership of teardown. Only one concurrent caller proceeds;
  // the rest get BAD_STATE.
  bool expected = false;
  if (!atomic_compare_exchange_strong_explicit(&inst->leaving, &expected, true,
                                               memory_order_acq_rel,
                                               memory_order_acquire)) {
    return swim_set_error(SWIM_ERR_BAD_STATE, "Instance already leaving");
  }

  // Stop the protocol thread. running is atomic so the worker's unlocked read
  // in swim_protocol_loop sees this store without a data race.
  atomic_store_explicit(&inst->running, false, memory_order_relaxed);

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
  free(inst->seeds);
  free(inst->shuffle_list);
  pthread_mutex_destroy(&inst->mutex);
  free(inst);

  return 0;
}

int swim_members(swim_t *inst, swim_member_t *out_list, int max_len,
                 bool include_dead) {
  if (!inst) {
    return swim_set_error(SWIM_ERR_INVALID, "NULL instance");
  }
  pthread_mutex_lock(&inst->mutex);
  int ret =
      swim_membership_list(inst->membership, out_list, max_len, include_dead);
  pthread_mutex_unlock(&inst->mutex);
  return ret;
}

int swim_pack_authed(const char *name, const uint8_t *msg, int msglen,
                     uint8_t *out, int outsz) {
  if (!name || !msg || msglen < 0 || outsz < 12 + msglen)
    return -1;
  uint32_t tval = (uint32_t)(get_now_ms() / 1000);
  uint32_t tval_be = htonl(tval);
  memcpy(out, &tval_be, 4);
  uint8_t hash_in[4 + SWIM_MAX_PACKET_SIZE];
  memcpy(hash_in, &tval_be, 4);
  memcpy(hash_in + 4, msg, (size_t)msglen);
  uint8_t key[16] = {0};
  size_t namelen = strlen(name);
  memcpy(key, name, namelen < 16 ? namelen : 16);
  uint64_t hval_be = htobe64(siphash24(key, hash_in, 4 + (size_t)msglen));
  memcpy(out + 4, &hval_be, 8);
  memcpy(out + 12, msg, (size_t)msglen);
  return 12 + msglen;
}

char *swim_peers(swim_t *inst, bool include_dead, int *count) {
  if (!inst || !count) {
    swim_set_error(SWIM_ERR_INVALID, "Invalid arguments to swim_peers");
    return NULL;
  }
  pthread_mutex_lock(&inst->mutex);
  char *buf = swim_membership_peers(inst->membership, include_dead, count);
  pthread_mutex_unlock(&inst->mutex);
  return buf;
}

int swim_hint_alive(swim_t *inst, const char *peer) {
  if (!inst) {
    return swim_set_error(SWIM_ERR_INVALID, "NULL instance");
  }
  if (!peer) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Invalid NULL peer in swim_hint_alive");
  }
  swim_node_id_t peer_id;
  if (swim_node_id_parse(&peer_id, peer) != 0)
    return -1;

  pthread_mutex_lock(&inst->mutex);

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
      queue_notification(inst, "up", &peer_id);
    }

    // Cancel outstanding probes to this target
    if (inst->pending_probe.state != PROBE_NONE &&
        swim_node_id_compare(&inst->pending_probe.target, &peer_id) == 0) {
      inst->pending_probe.state = PROBE_NONE;
      swim_timer_cancel(inst->timer, "probe_timeout");
    }
  }
  pthread_mutex_unlock(&inst->mutex);
  return 0;
}
