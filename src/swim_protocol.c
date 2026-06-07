#define _GNU_SOURCE
#include "swim_protocol.h"
#include "swim_errno.h"
#include "swim_timer.h"
#include "swim_gossip_queue.h"
#include "swim_codec.h"
#include "swim_udp.h"

#include <pthread.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <math.h>

// Helper to get monotonic time in milliseconds
static uint64_t get_monotonic_time_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// Helper to get wall clock time in milliseconds
static uint64_t get_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// Structures for internal protocol use
typedef struct {
  swim_callback_t cb;
  void *ctx;
} swim_sub_t;

typedef enum {
  PROBE_NONE = 0,
  PROBE_DIRECT,
  PROBE_INDIRECT
} probe_state_t;

typedef struct {
  probe_state_t state;
  swim_node_id_t target;
  uint32_t seq;
} pending_probe_t;

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

  pthread_t thread;
  pthread_mutex_t mutex;
  bool running;

  uint64_t protocol_period_ms;
  uint64_t ping_timeout_ms;
  uint32_t ping_req_fanout;
  uint64_t suspicion_timeout_ms;
  uint64_t seed_retry_interval_ms;
  uint64_t dead_node_expiry_ms;

  swim_node_id_t *seeds;
  int seed_count;

  // Round-robin shuffle list
  swim_member_t *shuffle_list;
  int shuffle_count;
  int shuffle_idx;

  pending_probe_t pending_probe;

  relay_probe_t relays[32];
  int relay_count;

  swim_sub_t subscribers[16];
  int subscriber_count;
  
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

// Forward declarations
static void *swim_protocol_thread_entry(void *arg);
static void *swim_protocol_loop(swim_instance_t *instance);
static void notify_subscribers(swim_instance_t *inst, swim_event_t event, const swim_node_id_t *node);
static void send_ping(swim_instance_t *inst, const swim_node_id_t *dest, uint32_t seq);
static void send_ack(swim_instance_t *inst, const swim_node_id_t *dest, uint32_t seq);
static void send_ping_req(swim_instance_t *inst, const swim_node_id_t *helper, const swim_node_id_t *target, uint32_t seq);
static void send_fwd_ack(swim_instance_t *inst, const swim_node_id_t *dest, const swim_node_id_t *source, uint32_t seq);
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
static inline int arm_probe_timeout(swim_instance_t *inst, uint32_t ticks, uint32_t seq) {
  return swim_timer_add(inst->timer, ticks, "probe_timeout", global_alarm_cb, inst, (void *)(intptr_t)seq);
}

// Timer Callbacks
static void probe_timer_cb(void *ctx, swim_timer_event_t ev, void *param) {
  (void)param;
  swim_instance_t *inst = (swim_instance_t *)ctx;
  if (ev == SWIM_TIMER_CANCEL) return;

  // 1. Garbage collect expired dead members before running a probe cycle
  swim_membership_gc(inst->membership, inst->dead_node_expiry_ms, get_monotonic_time_ms());

  // 2. Select target
  // Retrieve all active members (excluding self)
  int active_count = swim_membership_count(inst->membership);
  if (active_count == 0) {
    // Re-arm probe timer
    swim_timer_add(inst->timer, inst->protocol_period_ms / 100, "probe", probe_timer_cb, inst, NULL);
    return;
  }

  // Shuffle target selection
  if (inst->shuffle_idx >= inst->shuffle_count) {
    free(inst->shuffle_list);
    inst->shuffle_list = malloc(active_count * sizeof(swim_member_t));
    if (inst->shuffle_list) {
      inst->shuffle_count = swim_membership_list(inst->membership, inst->shuffle_list, active_count, false);
      // Fisher-Yates Shuffle
      for (int i = inst->shuffle_count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
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

    send_ping(inst, &target, seq);

    // Arm timeout alarm
    arm_probe_timeout(inst, inst->ping_timeout_ms / 100, seq);
  }

  // Re-arm probe timer
  swim_timer_add(inst->timer, inst->protocol_period_ms / 100, "probe", probe_timer_cb, inst, NULL);
}

static void suspicion_timer_cb(void *ctx, swim_timer_event_t ev, void *param) {
  swim_instance_t *inst = (swim_instance_t *)ctx;
  swim_node_id_t *target = (swim_node_id_t *)param;

  if (ev == SWIM_TIMER_ALARM) {
    const swim_member_t *m = swim_membership_get(inst->membership, target);
    if (m && m->status == SWIM_STATUS_SUSPECT) {
      // Declare dead
      swim_membership_apply_event(inst->membership, SWIM_STATUS_DEAD, target, m->incarnation, get_monotonic_time_ms());
      swim_gossip_queue_enqueue(inst->gossip_queue, SWIM_STATUS_DEAD, target, m->incarnation, 1);
      notify_subscribers(inst, SWIM_NODE_DOWN, target);
    }
  }
  free(target);
}

static void probe_timeout_cb(swim_instance_t *inst, uint32_t seq) {
  if (inst->pending_probe.state == PROBE_DIRECT && inst->pending_probe.seq == seq) {
    // Direct ping timed out, transition to indirect pinging
    inst->pending_probe.state = PROBE_INDIRECT;

    // Retrieve active members to select helpers
    int active_count = swim_membership_count(inst->membership);
    if (active_count > 0) {
      swim_member_t *helpers = malloc(active_count * sizeof(swim_member_t));
      if (helpers) {
        int count = swim_membership_list(inst->membership, helpers, active_count, false);
        // Exclude target and self from helpers list
        int write_idx = 0;
        for (int i = 0; i < count; i++) {
          if (swim_node_id_compare(&helpers[i].id, &inst->pending_probe.target) != 0) {
            helpers[write_idx++] = helpers[i];
          }
        }
        count = write_idx;

        // Pick up to k random helpers
        uint32_t k = inst->ping_req_fanout;
        for (uint32_t i = 0; i < k && count > 0; i++) {
          int idx = rand() % count;
          send_ping_req(inst, &helpers[idx].id, &inst->pending_probe.target, seq);
          
          // Remove selected helper from list
          helpers[idx] = helpers[count - 1];
          count--;
        }
        free(helpers);
      }
    }

    // Arm second timeout alarm for indirect probe
    arm_probe_timeout(inst, inst->ping_timeout_ms / 100, seq);

  } else if (inst->pending_probe.state == PROBE_INDIRECT && inst->pending_probe.seq == seq) {
    // Indirect ping also timed out -> Declare suspect
    swim_node_id_t target = inst->pending_probe.target;
    inst->pending_probe.state = PROBE_NONE;

    const swim_member_t *m = swim_membership_get(inst->membership, &target);
    if (m && m->status == SWIM_STATUS_ALIVE) {
      swim_membership_apply_event(inst->membership, SWIM_STATUS_SUSPECT, &target, m->incarnation, get_monotonic_time_ms());
      swim_gossip_queue_enqueue(inst->gossip_queue, SWIM_STATUS_SUSPECT, &target, m->incarnation, 1);
      notify_subscribers(inst, SWIM_NODE_SUSPECT, &target);

      // Start suspicion timer
      swim_node_id_t *suspect_param = malloc(sizeof(swim_node_id_t));
      if (suspect_param) {
        *suspect_param = target;
        char alarm_name[384];
        snprintf(alarm_name, sizeof(alarm_name), "suspect:%s:%u", target.host, target.port);
        swim_timer_add(inst->timer, inst->suspicion_timeout_ms / 100, alarm_name, suspicion_timer_cb, inst, suspect_param);
      }
    }
  }
}

static void seed_retry_timer_cb(void *ctx, swim_timer_event_t ev, void *param) {
  (void)param;
  swim_instance_t *inst = (swim_instance_t *)ctx;
  if (ev == SWIM_TIMER_CANCEL) return;

  if (inst->seed_count > 0) {
    int active_count = swim_membership_count(inst->membership);
    if (active_count == 0) {
      // Alone: Ping all seeds to discover cluster
      for (int i = 0; i < inst->seed_count; i++) {
        send_ping(inst, &inst->seeds[i], inst->seq++);
      }
    } else {
      // In a cluster: Ping one random seed to heal partitions
      int idx = rand() % inst->seed_count;
      send_ping(inst, &inst->seeds[idx], inst->seq++);
    }
  }

  // Re-arm seed retry timer
  swim_timer_add(inst->timer, inst->seed_retry_interval_ms / 100, "seed_retry", seed_retry_timer_cb, inst, NULL);
}

// Helper functions moved above

// Helper to send messages
static void send_message(swim_instance_t *inst, const swim_node_id_t *dest, swim_message_t *msg) {
  uint8_t buf[SWIM_MAX_PACKET_SIZE];
  
  // Pack gossip events (limit budget to 1000 bytes for security and headers)
  int active_members = swim_membership_count(inst->membership);
  msg->event_count = swim_gossip_queue_pack(inst->gossip_queue, active_members, 1000, msg->events, SWIM_MAX_EVENTS);

  int len = swim_codec_encode(msg, buf, sizeof(buf));
  if (len > 0) {
    swim_udp_send(inst->udp, dest, buf, len);
  }
}

static void send_ping(swim_instance_t *inst, const swim_node_id_t *dest, uint32_t seq) {
  swim_message_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.type = SWIM_MSG_PING;
  msg.sender = inst->self_id;
  msg.seq = seq;
  send_message(inst, dest, &msg);
}

static void send_ack(swim_instance_t *inst, const swim_node_id_t *dest, uint32_t seq) {
  swim_message_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.type = SWIM_MSG_ACK;
  msg.sender = inst->self_id;
  msg.seq = seq;
  send_message(inst, dest, &msg);
}

static void send_ping_req(swim_instance_t *inst, const swim_node_id_t *helper, const swim_node_id_t *target, uint32_t seq) {
  swim_message_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.type = SWIM_MSG_PING_REQ;
  msg.sender = inst->self_id;
  msg.seq = seq;
  msg.peer = *target;
  send_message(inst, helper, &msg);
}

static void send_fwd_ack(swim_instance_t *inst, const swim_node_id_t *dest, const swim_node_id_t *source, uint32_t seq) {
  swim_message_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.type = SWIM_MSG_FWD_ACK;
  msg.sender = inst->self_id;
  msg.seq = seq;
  msg.peer = *source;
  send_message(inst, dest, &msg);
}

static void notify_subscribers(swim_instance_t *inst, swim_event_t event, const swim_node_id_t *node) {
  for (int i = 0; i < inst->subscriber_count; i++) {
    inst->subscribers[i].cb(inst->subscribers[i].ctx, event, node);
  }
}

static void update_node_alive(swim_instance_t *inst, const swim_node_id_t *node) {
  if (swim_node_id_compare(node, &inst->self_id) == 0) {
    return;
  }

  const swim_member_t *m = swim_membership_get(inst->membership, node);
  if (!m) {
    int rc = swim_membership_apply_event(inst->membership, SWIM_STATUS_ALIVE, node, 0, get_monotonic_time_ms());
    if (rc == 0) {
      swim_gossip_queue_enqueue(inst->gossip_queue, SWIM_STATUS_ALIVE, node, 0, 1);
      notify_subscribers(inst, SWIM_NODE_UP, node);
    }
  } else if (m->status == SWIM_STATUS_SUSPECT) {
    swim_membership_set_alive(inst->membership, node, m->incarnation);
    
    char alarm_name[384];
    snprintf(alarm_name, sizeof(alarm_name), "suspect:%s:%u", node->host, node->port);
    swim_timer_cancel(inst->timer, alarm_name);
    
    swim_gossip_queue_enqueue(inst->gossip_queue, SWIM_STATUS_ALIVE, node, m->incarnation, 1);
    notify_subscribers(inst, SWIM_NODE_UP, node);
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

// Packet receiver and protocol handler
static void swim_protocol_handle_incoming(swim_instance_t *inst) {
  swim_node_id_t src;
  uint8_t buf[SWIM_MAX_PACKET_SIZE];
  int rc;
  
  int len = swim_udp_recv(inst->udp, &src, buf, sizeof(buf));
  if (len <= 0) return;

  swim_message_t msg;
  if (swim_codec_decode(buf, len, &msg) != 0) return;

  // Since UDP recv doesn't contain the cookie, populate it from the decoded message sender ID
  strcpy(src.cookie, msg.sender.cookie);

  // Apply gossip rules for incoming sender status first
  update_node_alive(inst, &msg.sender);

  // 1. Process Piggybacked Gossip Events
  for (int i = 0; i < msg.event_count; i++) {
    const swim_member_t *ev = &msg.events[i];

    // Exclude self-events from direct updates (dealt with via refutation below)
    if (swim_node_id_compare(&ev->id, &inst->self_id) != 0) {
      rc = swim_membership_apply_event(inst->membership, ev->status, &ev->id, ev->incarnation, get_monotonic_time_ms());
      if (rc == 0) {
        swim_gossip_queue_enqueue(inst->gossip_queue, ev->status, &ev->id, ev->incarnation, 1);
        
        if (ev->status == SWIM_STATUS_DEAD) {
          notify_subscribers(inst, SWIM_NODE_DOWN, &ev->id);
          // Cancel suspicion timer if any
          char alarm_name[384];
          snprintf(alarm_name, sizeof(alarm_name), "suspect:%s:%u", ev->id.host, ev->id.port);
          swim_timer_cancel(inst->timer, alarm_name);
        } else if (ev->status == SWIM_STATUS_SUSPECT) {
          notify_subscribers(inst, SWIM_NODE_SUSPECT, &ev->id);
          
          // Start suspicion timer
          swim_node_id_t *suspect_param = malloc(sizeof(swim_node_id_t));
          if (suspect_param) {
            *suspect_param = ev->id;
            char alarm_name[384];
            snprintf(alarm_name, sizeof(alarm_name), "suspect:%s:%u", ev->id.host, ev->id.port);
            swim_timer_add(inst->timer, inst->suspicion_timeout_ms / 100, alarm_name, suspicion_timer_cb, inst, suspect_param);
          }
        } else if (ev->status == SWIM_STATUS_ALIVE) {
          notify_subscribers(inst, SWIM_NODE_UP, &ev->id);
          // Cancel suspicion timer
          char alarm_name[384];
          snprintf(alarm_name, sizeof(alarm_name), "suspect:%s:%u", ev->id.host, ev->id.port);
          swim_timer_cancel(inst->timer, alarm_name);
        }
      }
    } else {
      // Rumor about ourselves! Self-refutation logic
      if (ev->status == SWIM_STATUS_SUSPECT || ev->status == SWIM_STATUS_DEAD) {
        if (ev->incarnation >= inst->incarnation) {
          inst->incarnation = ev->incarnation + 1;
          swim_gossip_queue_enqueue(inst->gossip_queue, SWIM_STATUS_ALIVE, &inst->self_id, inst->incarnation, 2);
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
  }
}

// Background event loop thread
static void *swim_protocol_thread_entry(void *arg) {
  return swim_protocol_loop((swim_instance_t *)arg);
}

static void *swim_protocol_loop(swim_instance_t *instance) {
  uint64_t now = get_monotonic_time_ms();
  uint64_t exp = now + 100;

  while (instance->running) {
    now = get_monotonic_time_ms();

    // 1. Tick logical timer
    while (now > exp) {
      pthread_mutex_lock(&instance->mutex);
      instance->current_tick++;
      swim_timer_tick(instance->timer);
      pthread_mutex_unlock(&instance->mutex);
      exp += 100;
    }

    // 2. Compute timeout for poll()
    int timeout_ms = (int)(exp - now);
    if (timeout_ms < 0) timeout_ms = 0;

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

int swim_start(const swim_start_opts_t *opts) {
  if (!opts || !opts->host || opts->port == 0 || !opts->name || opts->name[0] == '\0') {
    return swim_set_error(SWIM_ERR_INVALID, "Invalid start options: host, port, and name are mandatory");
  }

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
  inst->protocol_period_ms = opts->protocol_period_ms ? opts->protocol_period_ms : 1000;
  inst->ping_timeout_ms = opts->ping_timeout_ms ? opts->ping_timeout_ms : 200;
  inst->ping_req_fanout = opts->ping_req_fanout ? opts->ping_req_fanout : 3;
  inst->suspicion_timeout_ms = opts->suspicion_timeout_ms ? opts->suspicion_timeout_ms : 3000;
  inst->seed_retry_interval_ms = opts->seed_retry_interval_ms ? opts->seed_retry_interval_ms : 5000;
  inst->dead_node_expiry_ms = opts->dead_node_expiry_ms ? opts->dead_node_expiry_ms : 6000;

  // Initialize helper sub-modules
  inst->udp = swim_udp_init(opts->host, opts->port);
  if (!inst->udp) goto error_cleanup;

  inst->timer = swim_timer_init();
  if (!inst->timer) goto error_cleanup;

  inst->membership = swim_membership_init();
  if (!inst->membership) goto error_cleanup;

  inst->gossip_queue = swim_gossip_queue_init();
  if (!inst->gossip_queue) goto error_cleanup;

  // Initialize self Node ID
  strncpy(inst->self_id.host, opts->host, sizeof(inst->self_id.host) - 1);
  inst->self_id.port = opts->port;
  strncpy(inst->self_id.cookie, opts->cookie ? opts->cookie : "", sizeof(inst->self_id.cookie) - 1);

  // Seed incarnation with current time (wall-clock time in milliseconds)
  inst->incarnation = get_now_ms();
  inst->seq = 1;

  // Copy seeds list
  if (opts->seed_count > 0 && opts->seed_list) {
    inst->seeds = malloc(opts->seed_count * sizeof(swim_node_id_t));
    if (!inst->seeds) goto error_cleanup;
    memcpy(inst->seeds, opts->seed_list, opts->seed_count * sizeof(swim_node_id_t));
    inst->seed_count = opts->seed_count;
  }

  // Initialize thread synchronization
  pthread_mutex_init(&inst->mutex, NULL);
  inst->running = true;

  // Seed rand
  srand((unsigned int)time(NULL) ^ (unsigned int)pthread_self());

  // Enqueue self-announcement
  swim_gossip_queue_enqueue(inst->gossip_queue, SWIM_STATUS_ALIVE, &inst->self_id, inst->incarnation, 1);

  // Set up logical alarms
  // 1. Probe cycle timer (self-rearming)
  swim_timer_add(inst->timer, inst->protocol_period_ms / 100, "probe", probe_timer_cb, inst, NULL);
  // 2. Seed retry timer (self-rearming) - Schedule the first retry timer to run in 1 tick (100ms) to bootstrap immediately
  swim_timer_add(inst->timer, 1, "seed_retry", seed_retry_timer_cb, inst, NULL);

  // Start background event loop thread
  if (pthread_create(&inst->thread, NULL, swim_protocol_thread_entry, inst) != 0) {
    inst->running = false;
    pthread_mutex_destroy(&inst->mutex);
    goto error_cleanup;
  }

  g_instances[slot] = inst;

  pthread_mutex_unlock(&g_instances_mutex);
  return 0;

error_cleanup:
  if (inst->udp) swim_udp_final(inst->udp);
  if (inst->timer) swim_timer_final(inst->timer);
  if (inst->membership) swim_membership_final(inst->membership);
  if (inst->gossip_queue) swim_gossip_queue_final(inst->gossip_queue);
  free(inst->seeds);
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

  // Stop the protocol task
  pthread_mutex_lock(&inst->mutex);
  inst->running = false;
  pthread_mutex_unlock(&inst->mutex);

  pthread_mutex_unlock(&g_instances_mutex);

  // Join background thread
  pthread_join(inst->thread, NULL);

  pthread_mutex_lock(&g_instances_mutex);
  // Perform cleanup
  int slot = -1;
  for (int i = 0; i < 16; i++) {
    if (g_instances[i] == inst) {
      slot = i;
      break;
    }
  }
  if (slot != -1) {
    g_instances[slot] = NULL;
  }

  // Graceful broadcast of self death directly to random peers (not via gossip queue)
  pthread_mutex_lock(&inst->mutex);
  int active = swim_membership_count(inst->membership);
  if (active > 0) {
    swim_member_t *list = malloc(active * sizeof(swim_member_t));
    if (list) {
      int count = swim_membership_list(inst->membership, list, active, false);
      uint32_t fanout = (uint32_t)ceil(count * 0.25);
      if (fanout < 8) fanout = 8;
      
      // Bump incarnation first
      inst->incarnation = get_now_ms();
      
      // Send directly to up to fanout peers
      swim_message_t leave_msg;
      memset(&leave_msg, 0, sizeof(leave_msg));
      leave_msg.type = SWIM_MSG_ACK; // Graceful stop packet format
      leave_msg.sender = inst->self_id;
      leave_msg.seq = inst->seq++;
      leave_msg.event_count = 1;
      leave_msg.events[0].id = inst->self_id;
      leave_msg.events[0].status = SWIM_STATUS_DEAD;
      leave_msg.events[0].incarnation = inst->incarnation;
      
      uint8_t buf[SWIM_MAX_PACKET_SIZE];
      int len = swim_codec_encode(&leave_msg, buf, sizeof(buf));
      if (len > 0) {
        for (uint32_t i = 0; i < fanout && count > 0; i++) {
          int idx = rand() % count;
          swim_udp_send(inst->udp, &list[idx].id, buf, len);
          list[idx] = list[count - 1];
          count--;
        }
      }
      free(list);
    }
  }
  pthread_mutex_unlock(&inst->mutex);

  // Destroy structures
  swim_udp_final(inst->udp);
  swim_timer_final(inst->timer);
  swim_membership_final(inst->membership);
  swim_gossip_queue_final(inst->gossip_queue);
  free(inst->seeds);
  free(inst->shuffle_list);
  pthread_mutex_destroy(&inst->mutex);
  free(inst);

  pthread_mutex_unlock(&g_instances_mutex);
  return 0;
}

int swim_members(const char *name, swim_member_t *out_list, int max_len, bool include_dead) {
  if (!name || name[0] == '\0') {
    return swim_set_error(SWIM_ERR_INVALID, "Instance name is mandatory");
  }
  pthread_mutex_lock(&g_instances_mutex);
  swim_instance_t *inst = find_instance(name);
  if (!inst) {
    pthread_mutex_unlock(&g_instances_mutex);
    return swim_set_error(SWIM_ERR_BAD_STATE, "Instance not found");
  }

  pthread_mutex_lock(&inst->mutex);
  int ret = swim_membership_list(inst->membership, out_list, max_len, include_dead);
  pthread_mutex_unlock(&inst->mutex);

  pthread_mutex_unlock(&g_instances_mutex);
  return ret;
}

int swim_subscribe(const char *name, swim_callback_t callback, void *ctx) {
  if (!name || name[0] == '\0') {
    return swim_set_error(SWIM_ERR_INVALID, "Instance name is mandatory");
  }
  if (!callback) {
    return swim_set_error(SWIM_ERR_INVALID, "Invalid NULL callback in swim_subscribe");
  }

  pthread_mutex_lock(&g_instances_mutex);
  swim_instance_t *inst = find_instance(name);
  if (!inst) {
    pthread_mutex_unlock(&g_instances_mutex);
    return swim_set_error(SWIM_ERR_BAD_STATE, "Instance not found");
  }

  pthread_mutex_lock(&inst->mutex);
  if (inst->subscriber_count >= 16) {
    pthread_mutex_unlock(&inst->mutex);
    pthread_mutex_unlock(&g_instances_mutex);
    return swim_set_error(SWIM_ERR_FULL, "Maximum subscriber limit reached");
  }

  inst->subscribers[inst->subscriber_count].cb = callback;
  inst->subscribers[inst->subscriber_count].ctx = ctx;
  inst->subscriber_count++;

  pthread_mutex_unlock(&inst->mutex);
  pthread_mutex_unlock(&g_instances_mutex);
  return 0;
}

int swim_unsubscribe(const char *name, swim_callback_t callback, void *ctx) {
  if (!name || name[0] == '\0') {
    return swim_set_error(SWIM_ERR_INVALID, "Instance name is mandatory");
  }
  pthread_mutex_lock(&g_instances_mutex);
  swim_instance_t *inst = find_instance(name);
  if (!inst) {
    pthread_mutex_unlock(&g_instances_mutex);
    return swim_set_error(SWIM_ERR_BAD_STATE, "Instance not found");
  }

  pthread_mutex_lock(&inst->mutex);
  int idx = -1;
  for (int i = 0; i < inst->subscriber_count; i++) {
    if (inst->subscribers[i].cb == callback && inst->subscribers[i].ctx == ctx) {
      idx = i;
      break;
    }
  }

  if (idx != -1) {
    inst->subscribers[idx] = inst->subscribers[inst->subscriber_count - 1];
    inst->subscriber_count--;
  }

  pthread_mutex_unlock(&inst->mutex);
  pthread_mutex_unlock(&g_instances_mutex);
  return 0;
}

int swim_hint_alive(const char *name, const swim_node_id_t *peer) {
  if (!name || name[0] == '\0') {
    return swim_set_error(SWIM_ERR_INVALID, "Instance name is mandatory");
  }
  if (!peer) {
    return swim_set_error(SWIM_ERR_INVALID, "Invalid NULL peer in swim_hint_alive");
  }

  pthread_mutex_lock(&g_instances_mutex);
  swim_instance_t *inst = find_instance(name);
  if (!inst) {
    pthread_mutex_unlock(&g_instances_mutex);
    return swim_set_error(SWIM_ERR_BAD_STATE, "Instance not found");
  }

  pthread_mutex_lock(&inst->mutex);
  const swim_member_t *m = swim_membership_get(inst->membership, peer);
  if (m) {
    if (m->status == SWIM_STATUS_SUSPECT) {
      // Cancel suspicion timer
      char alarm_name[384];
      snprintf(alarm_name, sizeof(alarm_name), "suspect:%s:%u", peer->host, peer->port);
      swim_timer_cancel(inst->timer, alarm_name);

      // Re-announce as alive
      swim_membership_set_alive(inst->membership, peer, m->incarnation);
      swim_gossip_queue_enqueue(inst->gossip_queue, SWIM_STATUS_ALIVE, peer, m->incarnation, 1);
      notify_subscribers(inst, SWIM_NODE_UP, peer);
    }

    // Cancel outstanding probes to this target
    if (inst->pending_probe.state != PROBE_NONE &&
        swim_node_id_compare(&inst->pending_probe.target, peer) == 0) {
      inst->pending_probe.state = PROBE_NONE;
      swim_timer_cancel(inst->timer, "probe_timeout");
    }
  }
  pthread_mutex_unlock(&inst->mutex);
  pthread_mutex_unlock(&g_instances_mutex);
  return 0;
}
