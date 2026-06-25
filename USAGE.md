# libswimc17 Usage Guide

libswimc17 implements the SWIM+INF+Susp cluster
membership protocol. Nodes discover each other, detect
failures, and propagate membership changes via gossip
over UDP sockets.

## Contents

1. [Concepts](#concepts)
2. [Installation](#installation)
3. [Quick Start](#quick-start)
4. [Telemetry Feed](#telemetry-feed)
5. [API Reference](#api-reference)
6. [Configuration](#configuration)
7. [Multiple Instances](#multiple-instances)
8. [Testing](#testing)

---

## Concepts

**What the library does** — libswimc17 tracks which nodes
in a cluster are alive or dead. Each node runs a background
thread that periodically probes a peer, propagates
membership changes via gossip, and delivers events to the
application through a feed queue. The library does not
replicate data, elect leaders, or provide reliable
messaging; it only tells you who is up and who is down.

**Node identity** — every node is identified by the string
`"host:port/cookie"`. The `host` and `port` are the UDP
address the node binds and advertises. The `cookie` is
arbitrary text with no protocol meaning; it exists solely
to distinguish nodes that share the same `host:port` —
for example, successive restarts of a process on the same
port, or multiple logical instances on one machine.
`host:port/AAA` and `host:port/BBB` are treated as two
completely different nodes. The cookie may be omitted,
giving the shorter form `"host:port"` (equivalent to an
empty cookie).

**Group name** — `swim_start_opts_t.name` is a group name,
not an instance name. Only nodes that share the same group
name exchange messages with each other; packets from nodes
with a different group name are silently dropped. This is
how independent clusters can coexist on the same network
without interfering. The group name also serves as the
authentication key for every packet (see DESIGN.md §6),
so all members of a group must use the same value.

**Feed** — the library reports membership events through a
caller-owned `swim_feed_t` queue. The application creates
the feed, passes it to `swim_start`, and drains it from a
separate thread at its own pace. The feed is the only
output channel; the library itself does no logging. Passing
`NULL` disables telemetry entirely.

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

Initialize your configuration options and start the cluster membership
protocol thread. `mynodeid` and `opts->name` are mandatory. Each
node starts and joins by pointing to one or more known seeds.

```c
#include "swim.h"
#include <stdio.h>

int main() {
    swim_feed_t *feed = swim_feed_create();

    swim_start_opts_t opts = {0};
    opts.name  = "my_cluster";
    opts.feed  = feed;
    const char *seeds[] = { "10.0.0.1:7771/c1", NULL };
    opts.seeds = seeds;

    swim_t *inst = swim_start("10.0.0.7:7771/c1", &opts);
    if (!inst) {
        fprintf(stderr, "Failed to start SWIM: %s\n", swim_errmsg());
        return 1;
    }

    // Drain telemetry in a loop.
    char buf[SWIM_FEED_MAX_RECORD_SIZE];
    char *ptr[SWIM_FEED_MAX_ELEMENTS];
    int n;
    while (keep_running) {
        swim_feed_wait(feed, 1000);
        while ((n = swim_feed_get(feed, sizeof(buf),
                                  buf, 10, ptr)) > 0) {
            if (strcmp(ptr[0], "node") == 0)
                printf("node %s: %s\n", ptr[2], ptr[1]);
        }
    }

    swim_leave(inst);

    swim_feed_wakeall(feed);
    // join any reader threads here
    swim_feed_destroy(feed);
    return 0;
}
```

Join is asynchronous. Membership converges in the
background within a few protocol periods.

---

## Telemetry Feed

The library reports membership events through a
caller-owned `swim_feed_t` queue. The caller creates the
feed, passes it to `swim_start`, and reads from it at
its own pace.

### Creating and attaching a feed

```c
swim_feed_t *feed = swim_feed_create();

swim_start_opts_t opts = {0};
opts.name = "my_cluster";
opts.feed = feed;   // NULL disables telemetry
swim_start("10.0.0.1:7771", &opts);
```

### Reading records

Each call to `swim_feed_get` returns one record as a set
of NUL-terminated strings copied into a caller-supplied
buffer:

```c
char buf[SWIM_FEED_MAX_RECORD_SIZE];
char *ptr[SWIM_FEED_MAX_ELEMENTS];
int n;

while ((n = swim_feed_get(feed, sizeof(buf),
                          buf, 10, ptr)) > 0) {
    if (strcmp(ptr[0], "node") == 0)
        printf("node %s: %s\n", ptr[2], ptr[1]);
    else if (strcmp(ptr[0], "ping") == 0)
        printf("rtt to %s: %s ms\n", ptr[2], ptr[3]);
    else if (strcmp(ptr[0], "cluster") == 0)
        printf("cluster size: %s\n", ptr[2]);
    else if (strcmp(ptr[0], "warning") == 0)
        fprintf(stderr, "swim warning: %s\n", ptr[1]);
}
```

Record formats:

| `ptr[0]`    | `ptr[1]`    | `ptr[2]`              | `ptr[3]`   |
|-------------|-------------|-----------------------|------------|
| `"node"`    | `"up"`      | `"10.0.0.2:7772/abc"` | —          |
| `"node"`    | `"suspect"` | `"10.0.0.3:7773"`     | —          |
| `"node"`    | `"down"`    | `"10.0.0.4:7774"`     | —          |
| `"ping"`    | `"rtt"`     | `"10.0.0.2:7772"`     | `"14"` ms  |
| `"cluster"` | `"size"`    | `"5"`                 | —          |
| `"warning"` | `"<msg>"`   | —                     | —          |

### Blocking wait

Call `swim_feed_wait` to sleep until the feed is
signalled rather than busy-polling. It returns 0
immediately if the feed already has unread data.

```c
// Wait up to 1 second, then drain.
swim_feed_wait(feed, 1000);

// Block without a deadline.
swim_feed_wait(feed, SWIM_WAIT_FOREVER);
```

`swim_feed_wait` returning 0 does not guarantee a record
is present — spurious wakeups or concurrent readers may
drain the feed first. Always drain with `swim_feed_get`
after the wait.

### Shutdown sequence

The caller must ensure no threads are blocked in
`swim_feed_wait` before calling `swim_feed_destroy`.
The recommended pattern:

```c
// 1. Signal reader threads to exit.
atomic_store(&running, false);

// 2. Unblock any thread sleeping in swim_feed_wait.
swim_feed_wakeall(feed);

// 3. Join reader threads.
pthread_join(reader_tid, NULL);

// 4. Safe to destroy.
swim_feed_destroy(feed);
```

---

## API Reference

All instance functions accept a `swim_t *inst` handle
returned by `swim_start`.

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

The pointer fields (`name`, `seeds`, `feed`) are
set to `NULL`; the caller must fill them before passing
the struct to `swim_start()`.

If `n <= 1`, `detect_ms == 0`, or the derived `T` rounds
to zero, the function returns the built-in defaults
(equivalent to an 8-node cluster with T = 1000 ms).
No error is reported.

```c
swim_start_opts_t opts = swim_opts_for(50, 10000);
opts.name  = "my_cluster";
opts.feed  = feed;
swim_start("10.0.0.1:7771", &opts);
```

---

### `swim_start`

```c
swim_t *swim_start(const char *mynodeid,
                   const swim_start_opts_t *opts);
```

Starts the background protocol worker thread and
registers a new cluster membership instance.

`mynodeid` is this node's identity: `"host:port"` or
`"host:port/cookie"`. The host and port are used to
bind and advertise; the cookie is arbitrary text that
distinguishes nodes sharing the same host:port.

`mynodeid` and `opts->name` are mandatory.
`opts->feed` is optional; pass `NULL` to disable
telemetry.

Returns an opaque `swim_t *` handle on success, or
`NULL` on failure. On failure, sets `swim_errno` to:
- `SWIM_ERR_INVALID`: `mynodeid` or `opts->name` are
  NULL, empty, or malformed.
- `SWIM_ERR_NOMEM`: Memory allocation failed.

Thread-safe.

---

### `swim_leave`

```c
int swim_leave(swim_t *inst);
```

Performs a graceful leave, stops the background thread,
and deallocates all associated resources. Does not
destroy the caller's feed. The handle is invalid after
this call.

Returns `0` on success, `-1` on failure.

Thread-safe.

---

### `swim_peers`

```c
int swim_peers(swim_t *inst, bool include_dead,
               char **out);
```

Returns a snapshot of current peers as a packed string
buffer. Each peer is formatted as `"host:port"` or
`"host:port/cookie"`; strings are packed consecutively,
each NUL-terminated. The caller must `free(*out)`.
`*out` is NULL when there are no peers.

Set `include_dead` to `true` to include
dead/quarantined entries, or `false` for active nodes
only.

Returns the number of peers on success, or `-1` on
error:
- `SWIM_ERR_INVALID`: `inst` or `out` is NULL.
- `SWIM_ERR_NOMEM`: Memory allocation failed.

Thread-safe.

---

### `swim_hint_alive`

```c
int swim_hint_alive(swim_t *inst, const char *peer);
```

Feeds an out-of-band reachability signal into the
failure detector to cancel suspicion and revive a node.

`peer` is a `"host:port"` or `"host:port/cookie"`
string identifying the target node.

Returns `0` on success. On failure, returns `-1` and
sets `swim_errno` to:
- `SWIM_ERR_INVALID`: `inst` is NULL, or `peer` is
  NULL.
- `SWIM_ERR_BAD_STATE`: `inst` is not a valid running
  instance.

Thread-safe.

---

### Feed API

```c
swim_feed_t *swim_feed_create(void);
void         swim_feed_destroy(swim_feed_t *feed);
void         swim_feed_wakeall(swim_feed_t *feed);
int          swim_feed_get(swim_feed_t *feed, int bufsz,
                           char *buf, int nptr, char **ptr);
int          swim_feed_wait(swim_feed_t *feed,
                            uint64_t timeout_ms);
bool         swim_feed_empty(swim_feed_t *feed);
```

**`swim_feed_create`** — allocates and returns a new
feed. Returns `NULL` on OOM.

**`swim_feed_destroy`** — frees the feed and all
buffered records. See [Shutdown sequence](#shutdown-sequence)
for the required call order.

**`swim_feed_wakeall`** — broadcasts on the feed's
condition variable without writing a record. Use during
shutdown to unblock threads sleeping in `swim_feed_wait`.

**`swim_feed_get`** — copies the next record out of the
feed into `buf` and populates `ptr[0..n-1]` with
pointers to each string. Returns the string count
(≥ 1) on success, `0` if the feed is empty, or `-1` on
error. If the record does not fit in `bufsz` or `nptr`,
returns `-1` and leaves the record in the feed.

**`swim_feed_wait`** — blocks until the feed is
signalled or `timeout_ms` elapses. Returns `0`
immediately if the feed already has unread data. Pass
`SWIM_WAIT_FOREVER` to block without a deadline. Returns
`0` on signal, `1` on timeout, `-1` on error.

**`swim_feed_empty`** — returns `true` if the feed has
no unread records.

---

### Error Handling

```c
int          swim_errno(void);
const char  *swim_errmsg(void);
const char  *swim_strerror(int err);
```

`swim_errno` and `swim_errmsg` return the thread-local
error code and message set by the most recent failed
call. `swim_strerror` returns a static description for
a given error code.

Error codes: `SWIM_OK`, `SWIM_ERR_NOMEM`,
`SWIM_ERR_INVALID`, `SWIM_ERR_FULL`, `SWIM_ERR_TIMEOUT`,
`SWIM_ERR_BAD_STATE`.

---

## Configuration

Options are configured in the `swim_start_opts_t` struct:

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `name` | `const char*` | **required** | Group name — only nodes sharing this name exchange messages |
| `seeds` | `const char**` | `NULL` | NULL-terminated seed list |
| `feed` | `swim_feed_t*` | `NULL` | Telemetry feed; `NULL` disables |
| `protocol_period_ms` | `uint64_t` | `1000` | How often to probe one peer |
| `ping_timeout_ms` | `uint64_t` | `200` | Direct ACK wait time |
| `ping_req_fanout` | `uint32_t` | `3` | Indirect ping relay count |
| `suspicion_timeout_ms` | `uint64_t` | `3000` | Suspect → dead delay |
| `seed_retry_interval_ms` | `uint64_t` | `5000` | Retry interval if no peers |
| `dead_node_expiry_ms` | `uint64_t` | `6000` | How long to keep dead entries |

### Tuning for cluster size

Use `swim_opts_for(n, detect_ms)` to compute all timing
parameters from two inputs: the expected cluster size
and the worst-case failure-detection latency you want.
Then set the identity fields and start:

```c
swim_start_opts_t opts = swim_opts_for(50, 10000);
opts.name  = "my_cluster";
opts.feed  = feed;
swim_start("10.0.0.1:7771", &opts);
```

The defaults (zero-initialized fields) target an 8-node
cluster with ~5-second worst-case detection latency.

---

## Multiple Instances

Run multiple independent SWIM clusters in the same
process by giving each a unique name:

```c
swim_feed_t *feed_a = swim_feed_create();
swim_feed_t *feed_b = swim_feed_create();

swim_start_opts_t opts_a = { .name = "cluster_a", .feed = feed_a };
swim_start_opts_t opts_b = { .name = "cluster_b", .feed = feed_b };

swim_t *inst_a = swim_start("10.0.0.1:7771", &opts_a);
swim_t *inst_b = swim_start("10.0.0.1:7772", &opts_b);

char *p;
int count = swim_peers(inst_a, false, &p);
// iterate p...
free(p);
```

---

## Testing

For unit and integration test implementation details,
see the test suite files in `test/unit/`.
