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

All public functions accept a `name` argument (defaults to
`"swim"` if `NULL` is passed) to specify the named instance:

- `swim_start`
- `swim_members`
- `swim_subscribe`
- `swim_unsubscribe`
- `swim_hint_alive`
- `swim_leave`

### Query membership

```c
swim_member_t members[64];
int count = swim_members("my_cluster", members, 64, false);
if (count >= 0) {
    for (int i = 0; i < count; i++) {
        char host_buf[384];
        swim_node_id_format(&members[i].id, host_buf, sizeof(host_buf));
        printf("Member: %s status: %c incarnation: %llu\n",
               host_buf, members[i].status, members[i].incarnation);
    }
}
```

Each member is a `swim_member_t` struct where `status` is
`SWIM_STATUS_ALIVE` (`'A'`), `SWIM_STATUS_SUSPECT` (`'S'`),
or `SWIM_STATUS_DEAD` (`'D'`).

### Subscribe to events

Register a callback function to receive events when nodes
join (`SWIM_NODE_UP`), are suspected (`SWIM_NODE_SUSPECT`),
or are declared dead/leave (`SWIM_NODE_DOWN`).

```c
void my_event_cb(void *ctx, swim_event_t event, const swim_node_id_t *node) {
    char host_buf[384];
    swim_node_id_format(node, host_buf, sizeof(host_buf));

    switch (event) {
        case SWIM_NODE_UP:
            printf("Node UP: %s\n", host_buf);
            break;
        case SWIM_NODE_SUSPECT:
            printf("Node SUSPECT: %s\n", host_buf);
            break;
        case SWIM_NODE_DOWN:
            printf("Node DOWN: %s\n", host_buf);
            break;
    }
}

// Subscribe
swim_subscribe("my_cluster", my_event_cb, NULL);

// Unsubscribe when done
swim_unsubscribe("my_cluster", my_event_cb, NULL);
```

### Hint that a node is alive

When your application already communicates with peers over
another channel (e.g., a successful TCP/HTTP request), this
exchange is first-hand proof of reachability. Use the hint
API to feed this evidence into the failure detector:

```c
swim_node_id_t peer;
swim_node_id_parse(&peer, "10.0.0.2:7771:c1");
swim_hint_alive("my_cluster", &peer);
```

This hint is asynchronous and advisory:
- A **suspected** peer is restored to alive locally, its
  suspicion timer cancelled, and an `alive` event is
  re-gossiped.
- An **alive** peer has no status change.
- A **dead** peer is not revived.
- In-flight probes to that target are cancelled to
  avoid false-positive suspicion triggers.

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
