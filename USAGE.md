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
#include "swim.h"
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
membership protocol thread. The `self` and `name` options
are mandatory. The first node starts alone,
and subsequent nodes join by pointing to one or more known
seeds.

```c
#include "swim.h"
#include <stdio.h>

int main() {
    swim_start_opts_t opts = {0};
    opts.self = "10.0.0.7:7771/c1";
    opts.name = "my_cluster";
    const char *seeds[] = { "10.0.0.1:7771/c1", NULL };
    opts.seeds = seeds;

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
string (default `""`) that distinguishes different instances,
sessions, or restarts of a node running on the same host and
port. A different cookie on the same address represents a
distinct node. (Separating logically distinct clusters on the
same network is the `name` argument's job, not the cookie's.)

Helper functions are provided to format and parse node IDs:

```c
swim_node_id_t id;
swim_node_id_parse(&id, "10.0.0.7:7771/c1");

char buf[384];
swim_node_id_format(&id, buf, sizeof(buf));
printf("Node: %s\n", buf);
```

---

## API Reference

All public functions accept a `name` argument to specify the
named instance. The `name` argument is strictly mandatory and
cannot be `NULL` or empty. Passing a `NULL` or empty name will
trigger a `SWIM_ERR_INVALID` error.

---

### `swim_opts_for`

```c
swim_start_opts_t swim_opts_for(int n, uint64_t detect_ms);
```

Computes a `swim_start_opts_t` with all timing fields
derived from cluster size `n` and a desired worst-case
failure-detection latency `detect_ms` (milliseconds).

The SWIM paper's fundamental knob is the protocol period
`T`. This function inverts the detection-latency formula
so you can configure the library in terms you already
know rather than in raw protocol timings:

```
T = detect_ms / (1.4 + ceil(log2(n)))
```

All timing fields are computed from `T`:

| Field | Value |
|-------|-------|
| `protocol_period_ms` | `T` |
| `ping_timeout_ms` | `T / 5` |
| `ping_req_fanout` | `3` (SWIM paper default) |
| `suspicion_timeout_ms` | `ceil(log2(n)) × T` |
| `dead_node_expiry_ms` | `2 × suspicion_timeout_ms` |
| `seed_retry_interval_ms` | `5 × T` |

The pointer fields (`self`, `name`, `seeds`) are set to
`NULL`; the caller must fill them before passing the
struct to `swim_start()`.

If `n <= 1`, `detect_ms == 0`, or the derived `T` rounds
to zero, the function returns the built-in defaults
(equivalent to an 8-node cluster with T = 1000 ms).
No error is reported.

```c
swim_start_opts_t opts = swim_opts_for(50, 10000);
opts.self  = "10.0.0.1:7771";
opts.name  = "my_cluster";
swim_start(&opts);
```

---

### `swim_start`

```c
int swim_start(const swim_start_opts_t *opts);
```

Starts the background protocol worker thread and registers
a new named cluster membership instance.

The `opts` argument is a pointer to a `swim_start_opts_t`
structure. It must be non-NULL, and `opts->self` and
`opts->name` must be configured.

Returns `0` on success. On failure, returns `-1` and sets
the thread-local `swim_errno` state to:
- `SWIM_ERR_INVALID`: `opts` is NULL, or `opts->self` /
  `opts->name` are empty or invalid.
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

The `name` argument specifies the instance. It must be
non-NULL and non-empty.

Returns `0` on success. On failure, returns `-1` and sets:
- `SWIM_ERR_INVALID`: `name` is NULL or empty.
- `SWIM_ERR_BAD_STATE`: No instance found matching `name`.

Acquires the global registry lock, extracts the instance,
stops the background loop, increments the incarnation count,
directly broadcasts a `DEAD` gossip update to up to
`max(⌈N×0.25⌉, 8)` random peers, closes the UDP socket,
terminates active timers, joins the worker thread, and
releases memory. Thread-safe.

---

### `swim_peers`

```c
char *swim_peers(const char *name, bool include_dead, int *count);
```

Returns a snapshot of current peers as a packed string
buffer. Each peer is formatted as `"host:port"` or
`"host:port/cookie"`; the `*count` strings are packed
consecutively, each NUL-terminated. The caller must
`free()` the returned pointer.

Set `include_dead` to `true` to include dead/quarantined
entries, or `false` for active nodes only.

Returns a valid pointer (with `*count` set) on success,
or `NULL` on error:
- `SWIM_ERR_INVALID`: `name` or `count` is NULL, or
  `name` is empty.
- `SWIM_ERR_BAD_STATE`: No instance found matching `name`.
- `SWIM_ERR_NOMEM`: Memory allocation failed.

Thread-safe.

---

### `swim_subscribe`

```c
int swim_subscribe(const char *name, swim_callback_t callback, void *ctx);
```

Registers a subscriber callback function to receive
membership events (joins, suspicions, and node failures).

The `name` argument specifies the instance. It must be
non-NULL and non-empty. `callback` is a function pointer of
type `swim_callback_t`, and `ctx` is an opaque context
pointer passed back to the callback.

The callback prototype is defined as:
```c
typedef void (*swim_callback_t)(void *ctx, swim_event_t event, const char *node);
```
Where `event` is:
- `SWIM_NODE_UP`: A new node has joined or suspect node revived.
- `SWIM_NODE_SUSPECT`: A node missed a ping.
- `SWIM_NODE_DOWN`: A node was declared dead or left.

Returns `0` on success. On failure, returns `-1` and sets:
- `SWIM_ERR_INVALID`: `name` is NULL or empty, or `callback`
  is NULL.
- `SWIM_ERR_BAD_STATE`: No instance found matching `name`.
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

The `name` argument specifies the instance. It must be
non-NULL and non-empty. `callback` is the callback function
pointer to deregister, and `ctx` is the context pointer
matching the subscription.

Returns `0` on success. On failure, returns `-1` and sets:
- `SWIM_ERR_INVALID`: `name` is NULL or empty.
- `SWIM_ERR_BAD_STATE`: No instance found matching `name`.

Acquires the instance lock, searches for a matching
callback and context, swaps it with the last registered
subscriber, and updates the count. Thread-safe.

---

### `swim_hint_alive`

```c
int swim_hint_alive(const char *name, const char *peer);
```

Feeds an out-of-band reachability signal into the failure
detector to cancel suspicion and revive a node.

The `name` argument specifies the instance. It must be
non-NULL and non-empty. `peer` is a string identifying the
target peer node.

Returns `0` on success. On failure, returns `-1` and sets:
- `SWIM_ERR_INVALID`: `name` is NULL or empty, or `peer` is
  NULL.
- `SWIM_ERR_BAD_STATE`: No instance found matching `name`.

Acquires the instance lock. If the peer is currently in
`SUSPECT` status, it revives the node to `ALIVE` locally,
cancels its suspicion timer, enqueues an `alive` gossip
update, and triggers a `SWIM_NODE_UP` notification. Also
cancels in-flight probe timeouts to avoid immediate
re-suspicion. Releases the lock. Thread-safe.

---

### `swim_read_feed`

```c
int swim_read_feed(const char *name, int bufsz, char *buf, int nptr, char **ptr);
```

Drains and retrieves the next event from the instance's
telemetry feed.

The `name` argument specifies the instance (must be
non-NULL and non-empty). `bufsz` specifies the size of
`buf` (recommended 4096). `nptr` specifies the maximum
number of string pointers `ptr` can hold (recommended 10).

On success, it copies the NUL-terminated event strings
contiguously into `buf` and populates `ptr[0..n-1]` with
pointers to each string inside `buf`.

Returns the number of strings copied (>= 1) on success,
`0` when the feed is empty, or `-1` on error:
- `SWIM_ERR_INVALID`: `name` is NULL or empty.
- `SWIM_ERR_BAD_STATE`: No instance found matching `name`.

Records have the following formats:

| `ptr[0]`  | `ptr[1]`  | `ptr[2]`         | `ptr[3]` |
|-----------|-----------|------------------|----------|
| `"node"`  | `"up"`    | `"10.0.0.2:7772/abc"` | —   |
| `"node"`  | `"suspect"` | `"10.0.0.3:7773"` | —     |
| `"node"`  | `"down"`  | `"10.0.0.4:7774"` | —       |
| `"ping"`  | `"rtt"`   | `"10.0.0.2:7772"` | `"14"` (ms) |
| `"cluster"` | `"size"` | `"5"`           | —        |
| `"warning"` | `"<message>"` | —          | —        |

Example:
```c
char buf[4096];
char *ptr[10];
int n;
while ((n = swim_read_feed("my_cluster", sizeof(buf), buf, 10, ptr)) > 0) {
    if (strcmp(ptr[0], "node") == 0) {
        printf("node %s: %s\n", ptr[2], ptr[1]);
    } else if (strcmp(ptr[0], "ping") == 0) {
        printf("rtt to %s: %s ms\n", ptr[2], ptr[3]);
    } else if (strcmp(ptr[0], "cluster") == 0) {
        printf("cluster size: %s\n", ptr[2]);
    } else if (strcmp(ptr[0], "warning") == 0) {
        fprintf(stderr, "swim warning: %s\n", ptr[1]);
    }
}
```

---

### Node ID Helpers

```c
// Parse string format "host:port" or "host:port/cookie"
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
// Retrieve thread-local error code
int swim_errno(void);

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
| `self` | `const char*` | **required** | `"host:port"` or `"host:port/cookie"` |
| `name` | `const char*` | **required** | Unique instance name |
| `seeds` | `const char**` | `NULL` | NULL-terminated seed list (`"host:port/cookie"`) |
| `protocol_period_ms` | `uint64_t` | `1000` | How often to probe one peer |
| `ping_timeout_ms` | `uint64_t` | `200` | Direct ACK wait time |
| `ping_req_fanout` | `uint32_t` | `3` | Indirect ping relay count |
| `suspicion_timeout_ms` | `uint64_t` | `3000` | Suspect → dead delay |
| `seed_retry_interval_ms` | `uint64_t` | `5000` | Retry interval if no peers |
| `dead_node_expiry_ms` | `uint64_t` | `6000` | How long to keep dead entries |

### Tuning for cluster size

Use `swim_opts_for(n, detect_ms)` to compute all timing
parameters from two inputs you already know: the expected
cluster size and the worst-case failure-detection latency
you want. Then set the identity fields and start:

```c
swim_start_opts_t opts = swim_opts_for(50, 10000);
opts.self  = "10.0.0.1:7771";
opts.name  = "my_cluster";
swim_start(&opts);
```

The defaults (zero-initialized fields) target an 8-node
cluster with ~5-second worst-case detection latency.

---

## Multiple Instances

Run multiple independent SWIM clusters in the same process
by giving each a unique name in the startup options:

```c
swim_start_opts_t opts_a = { .self = "10.0.0.1:7771", .name = "cluster_a" };
swim_start_opts_t opts_b = { .self = "10.0.0.1:7772", .name = "cluster_b" };

swim_start(&opts_a);
swim_start(&opts_b);

int count;
char *p = swim_peers("cluster_a", false, &count);
// iterate or use p...
free(p);
```

---

## Testing

For unit and integration test implementation details,
see the test suite files in `test/unit/`.
