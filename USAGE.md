# libswimc17 Usage Guide

libswimc17 implements the SWIM+INF+Susp cluster
membership protocol. Nodes discover each other, detect
failures, and propagate membership changes via gossip
over UDP sockets.

## Contents

1. [Installation](#installation)
2. [Quick Start](#quick-start)
3. [Node Identity](#node-identity)
4. [API Reference](#api-reference)
5. [Configuration](#configuration)
6. [Multiple Instances](#multiple-instances)
7. [Testing](#testing)

---

## Installation

Include the main header file:

```c
#include "swim_protocol.h"
```

Link the compiled library with your application along with
pthread and math libraries:

```bash
cc my_app.c -lswimc17 -lpthread -lm
```

If pkg-config is configured on your system:

```bash
cc my_app.c $(pkg-config --cflags --libs libswimc17)
```

---

## Quick Start

Initialize your configuration options and start the cluster
membership protocol thread. The first node starts alone, and
subsequent nodes join by pointing to one or more known
seeds.

```c
#include "swim_protocol.h"
#include <stdio.h>

int main() {
    swim_node_id_t seed;
    if (swim_node_id_parse(&seed, "10.0.0.1:7771:c1") != 0) {
        fprintf(stderr, "Failed to parse seed node ID\n");
        return 1;
    }

    swim_start_opts_t opts = {0};
    opts.host = "10.0.0.7";
    opts.port = 7771;
    opts.cookie = "c1";
    opts.name = "my_cluster";
    opts.seed_list = &seed;
    opts.seed_count = 1;

    if (swim_start(&opts) != 0) {
        fprintf(stderr, "Failed to start SWIM: %s\n", swim_errmsg());
        return 1;
    }

    // Node is running and discovering peers in the background...

    // Perform graceful leave upon termination
    swim_leave("my_cluster");
    return 0;
}
```

Join is asynchronous. Membership converges in the
background within a few protocol periods. Once running,
you can query membership or subscribe to change events.

---

## Node Identity

Each node is uniquely identified by the `swim_node_id_t`
struct:

```c
typedef struct {
  char host[256];
  uint16_t port;
  char cookie[64];
} swim_node_id_t;
```

`host` and `port` are required. `cookie` is a user-defined
string (default `""`) that can distinguish multiple logically
separate clusters sharing the same network. A different
cookie on the same address represents a distinct node.

Helper functions are provided to format and parse node IDs:

```c
swim_node_id_t id;
swim_node_id_parse(&id, "10.0.0.7:7771:c1");

char buf[384];
swim_node_id_format(&id, buf, sizeof(buf));
printf("Node: %s\n", buf);
```

---

## API Reference

All public functions accept a `name` argument to specify the
named instance. If `NULL` is passed, the name defaults to
`"swim"`.

---

### `swim_start`

```c
int swim_start(const swim_start_opts_t *opts);
```

Starts the background protocol worker thread and registers
a new named cluster membership instance.

The `opts` argument is a pointer to a `swim_start_opts_t`
structure. It must be non-NULL, and `opts->host` and
`opts->port` must be configured.

Returns `0` on success. On failure, returns `-1` and sets
the thread-local `swim_errno` state to:
- `SWIM_ERR_INVALID`: `opts` is NULL, or `opts->host` /
  `opts->port` are empty or invalid.
- `SWIM_ERR_BAD_STATE`: An instance with the same name
  is already running.
- `SWIM_ERR_FULL`: Maximum active instances (16) exceeded.
- `SWIM_ERR_NOMEM`: Memory allocation failed.

Acquires the global instance registry mutex, registers the
name, initializes internal sub-modules (gossip queue, UDP
sockets, membership list, and delta-list timers), and Spawns
the worker loop thread using `pthread_create`. Thread-safe.

---

### `swim_leave`

```c
int swim_leave(const char *name);
```

Performs a graceful leave sequence, stops the background
thread, and deallocates all associated resources.

The `name` argument specifies the instance. Defaults to `"swim"`
if `NULL`.

Returns `0` on success. On failure, returns `-1` and sets:
- `SWIM_ERR_BAD_STATE`: No instance found matching `name`.

Acquires the global registry lock, extracts the instance,
stops the background loop, increments the incarnation count,
directly broadcasts a `DEAD` gossip event to up to
`max(⌈N×0.25⌉, 8)` random peers, closes the UDP socket,
terminates active timers, joins the worker thread, and
releases memory. Thread-safe.

---

### `swim_members`

```c
int swim_members(const char *name, swim_member_t *out_list, int max_len, bool include_dead);
```

Retrieves a snapshot of the current membership registry.

The `name` argument specifies the instance. Defaults to `"swim"`
if `NULL`. `out_list` is a pre-allocated buffer of
`swim_member_t` elements, and `max_len` is its capacity.
Set `include_dead` to `true` to return dead/quarantined
entries, or `false` for active nodes only.

Returns the number of elements written to `out_list` on
success (can be `0` or greater). Returns `-1` on error:
- `SWIM_ERR_BAD_STATE`: No instance found matching `name`.
- `SWIM_ERR_INVALID`: `out_list` is NULL or `max_len <= 0`.

Acquires the instance mutex, copies matching elements
directly to the provided array, and releases the lock.
Thread-safe.

---

### `swim_subscribe`

```c
int swim_subscribe(const char *name, swim_callback_t callback, void *ctx);
```

Registers a subscriber callback function to receive
membership events (joins, suspicions, and node failures).

The `name` argument specifies the instance. Defaults to `"swim"`
if `NULL`. `callback` is a function pointer of type
`swim_callback_t`, and `ctx` is an opaque context pointer
passed back to the callback.

The callback prototype is defined as:
```c
typedef void (*swim_callback_t)(void *ctx, swim_event_t event, const swim_node_id_t *node);
```
Where `event` is:
- `SWIM_NODE_UP`: A new node has joined or suspect node revived.
- `SWIM_NODE_SUSPECT`: A node missed a ping.
- `SWIM_NODE_DOWN`: A node was declared dead or left.

Returns `0` on success. On failure, returns `-1` and sets:
- `SWIM_ERR_BAD_STATE`: No instance found matching `name`.
- `SWIM_ERR_INVALID`: `callback` is NULL.
- `SWIM_ERR_FULL`: Maximum subscriber limit (16) reached.

Acquires the instance mutex to register the callback.
**Caution**: Callback functions are executed in the context
of the background worker thread. Callbacks must be quick,
non-blocking, and thread-safe.

---

### `swim_unsubscribe`

```c
int swim_unsubscribe(const char *name, swim_callback_t callback, void *ctx);
```

Deregisters a previously registered subscriber callback.

The `name` argument specifies the instance. Defaults to `"swim"`
if `NULL`. `callback` is the callback function pointer to
deregister, and `ctx` is the context pointer matching the
subscription.

Returns `0` on success. On failure, returns `-1` and sets:
- `SWIM_ERR_BAD_STATE`: No instance found matching `name`.

Acquires the instance lock, searches for a matching
callback and context, swaps it with the last registered
subscriber, and updates the count. Thread-safe.

---

### `swim_hint_alive`

```c
int swim_hint_alive(const char *name, const swim_node_id_t *peer);
```

Feeds an out-of-band reachability signal into the failure
detector to cancel suspicion and revive a node.

The `name` argument specifies the instance. Defaults to `"swim"`
if `NULL`. `peer` is a pointer to the `swim_node_id_t`
identifying the target peer node.

Returns `0` on success. On failure, returns `-1` and sets:
- `SWIM_ERR_BAD_STATE`: No instance found matching `name`.
- `SWIM_ERR_INVALID`: `peer` is NULL.

Acquires the instance lock. If the peer is currently in
`SUSPECT` status, it revives the node to `ALIVE` locally,
cancels its suspicion timer, enqueues an `alive` gossip
event, and triggers a `SWIM_NODE_UP` notification. Also
cancels in-flight probe timeouts to avoid immediate
re-suspicion. Releases the lock. Thread-safe.

---

### Node ID Helpers

```c
// Parse string format "host:port" or "host:port:cookie"
int swim_node_id_parse(swim_node_id_t *id, const char *str);

// Format node ID back to string
int swim_node_id_format(const swim_node_id_t *id, char *buf, size_t size);

// Compare two node IDs (returns negative, zero, or positive)
int swim_node_id_compare(const swim_node_id_t *a, const swim_node_id_t *b);
```

---

### Error Handling

Thread-local error states can be inspected using the following
utility functions:

```c
// Retrieve thread-local error details string
const char *swim_errmsg(void);

// Retrieve error code description
const char *swim_strerror(int err);
```

---

## Configuration

Options are configured in the `swim_start_opts_t` struct:

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `host` | `const char*` | **required** | Hostname or IP to bind |
| `port` | `uint16_t` | **required** | UDP port to bind |
| `cookie` | `const char*` | `""` | User-defined node cookie |
| `name` | `const char*` | `"swim"` | Instance name (for multi-cluster) |
| `seed_list` | `const swim_node_id_t*` | `NULL` | Seed nodes for join |
| `seed_count` | `int` | `0` | Count of seed nodes |
| `protocol_period_ms` | `uint64_t` | `1000` | How often to probe one peer |
| `ping_timeout_ms` | `uint64_t` | `200` | Direct ACK wait time |
| `ping_req_fanout` | `uint32_t` | `3` | Indirect ping relay count |
| `suspicion_timeout_ms` | `uint64_t` | `3000` | Suspect → dead delay |
| `seed_retry_interval_ms` | `uint64_t` | `5000` | Retry interval if no peers |
| `dead_node_expiry_ms` | `uint64_t` | `6000` | How long to keep dead entries |

### Tuning for cluster size

The defaults target an 8-node cluster. For larger clusters,
scale the timeouts with the `log2` of the cluster size `N`:

```c
#include <math.h>

int n = 50;
swim_start_opts_t opts = {0};
opts.host = "10.0.0.1";
opts.port = 7771;
opts.protocol_period_ms = 1000;
opts.suspicion_timeout_ms = (uint64_t)ceil(log2(n + 1)) * 1000;
opts.dead_node_expiry_ms = (uint64_t)ceil(log2(n + 1)) * 2000;
```

---

## Multiple Instances

Run multiple independent SWIM clusters in the same process
by giving each a unique name in the startup options:

```c
swim_start_opts_t opts_a = { .host = "10.0.0.1", .port = 7771, .name = "cluster_a" };
swim_start_opts_t opts_b = { .host = "10.0.0.1", .port = 7772, .name = "cluster_b" };

swim_start(&opts_a);
swim_start(&opts_b);

swim_member_t members[32];
swim_members("cluster_a", members, 32, false);
```

---

## Testing

For unit and integration test implementation details,
see the test suite files in `test/unit/`.
