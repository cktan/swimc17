# swim — Design Specification

This is SWIM membership library:
  - C17
  - no dependency on any libraries other than standard C and UDP socket

---

## 1. Project Identity

- **Package name:** `swim`
- **Language:** c17
- **Algorithm:** See `ALGORITHM.md` for the step-by-step
  protocol; this document covers the design decisions.
  


---

## 2. Protocol

- **Variant:** SWIM+INF+Susp (incarnation numbers +
  suspicion period before declaring dead)

### Failure detection flow

```
A → B        ping(seq)              # no ack within ping_timeout
A → [C,D,E]  ping_req(seq, B)       # indirect probe via k peers
C → B → C → A                       # B answers, relayed back to A
# still no ack → A gossips suspect(B), then dead(B) after timeout
```

Relay-ack refutation, self-refutation, and the full
step detail are in `ALGORITHM.md` (procedures A, B, F).

---

## 3. Node Identity

- **Wire and API type:** plain 3-tuple `{"host", port, "cookie"}`
  - `host` is a string (IP address or hostname)
  - `port` is an integer
  - `cookie` is a string (user-defined opaque datum). It is optionally specified by the user, defaulting to `""`.
- **Identity key:** the full `{"host", port, "cookie"}` tuple
- **Assigned by:** caller
- **Stability:** stable across restarts (same tuple, new incarnation number). A change in cookie signifies a different node instance even if host/port are reused.

### Node Identification with Cookie

While a node is reachable via its network location (`host` and `port`), the protocol incorporates a `cookie` into the node's strict identity (`{host, port, cookie}`). This cookie allows the application to distinguish between different instances, sessions, or restarts of a node running on the exact same host and port. 

The cookie is optionally specified by the user during node startup or when defining seeds. If not explicitly provided, it defaults to the empty string `""`.

---

## 4. Transport

- **Protocol:** UDP only
- **IP versions:** IPv4 + IPv6
- **Port:** UDP port (required, configurable)
- **MTU:** 1400 bytes (conservative, safe everywhere)
- **Overflow behaviour:** return error — caller
  re-encodes with fewer piggyback events
- **Internal abstraction:** an internal `Transport`
  interface with a UDP implementation shipped. An
  in-memory implementation lives in the test tree
  only and is not part of the public API. This
  boundary allows fault injection (packet loss,
  delay, reorder) in tests without exposing a
  pluggable transport contract to callers.

---

## 5. Wire Encoding

- **Format:** a compact binary serialization of the
  message terms (a custom or runtime-native encoding).
- Not polyglot — acceptable for a single-runtime
  library.
- **Message shape:**

```
# ping
{ping, sender :: {host, port, cookie}, seq :: non_neg_integer,
       events :: [event]}

# ack
{ack, sender :: {host, port, cookie}, seq :: non_neg_integer,
      events :: [event]}

# ping_req (A asks C to ping B)
{ping_req, sender :: {host, port, cookie}, seq :: non_neg_integer,
           target :: {host, port, cookie}, events :: [event]}

# forwarded ack (C tells A that B responded)
{fwd_ack, sender :: {host, port, cookie}, seq :: non_neg_integer,
          source :: {host, port, cookie}, events :: [event]}

# leave
{leave, sender :: {host, port, cookie}, seq :: non_neg_integer}
```

- **Event shape:**

```
{alive,   {host, port, cookie}, incarnation :: non_neg_integer}
{suspect, {host, port, cookie}, incarnation :: non_neg_integer}
{dead,    {host, port, cookie}, incarnation :: non_neg_integer}
```

---

## 6. Security

None. Trust the network (VPC / firewall handles
perimeter). Deserialization is safe here because
only trusted cluster nodes communicate on the gossip
port.

---

## 7. Failure Detection Parameters

All parameters are runtime-configurable via start
opts. Defaults are tuned for an 8-node cluster.

| Parameter | Default | Formula / notes |
|-----------|---------|-----------------|
| `protocol_period` | 1000 ms | `T` |
| `ping_timeout` | 200 ms | `T / 5` |
| `ping_req_fanout` | 3 | paper default; `k` |
| `suspicion_timeout` | 3000 ms | `log2(N) × T` at N=8 |
| `seed_retry_interval` | 5000 ms | `5 × T` |
| `dead_node_expiry` | 6000 ms | `2 × suspicion_timeout` |

### Deriving T from detection latency

The worst-case failure-detection path is:

1. Node dies right after being probed — waits up to
   one full period `T` before the next probe fires.
2. Direct ping times out after `T/5`.
3. Indirect ping-req times out after another `T/5`.
4. Suspicion timer runs for `ceil(log2(N)) × T`.

Total worst-case latency:

```
detect = T + 2×(T/5) + ceil(log2(N))×T
       = T × (1.4 + ceil(log2(N)))
```

Solving for `T` given a target detection latency and
cluster size `N`:

```
T = detect_ms / (1.4 + ceil(log2(N)))
```

`swim_opts_for(n, detect_ms)` performs this inversion
automatically (see §11).

---

## 8. Dissemination

The packing and expiry flow is specified in
`ALGORITHM.md` (procedure D); the parameters below are
the design choices.

- **Transmit multiplier:** `ceil(log₂(N+1)) × 3` where
  `N` = count of alive + suspect nodes. Recalculated
  dynamically as membership changes.
- **Piggyback budget:** fill outgoing messages up to the 1400-byte
  MTU. Since headers are packed first and the remaining buffer space
  is computed dynamically, the gossip payload fills the remaining
  space directly without exceeding the 1400-byte limit.
- **Event types:** `alive`, `suspect`, `dead`.
  No user-defined event type.
- **Priority ordering:** dead → suspect → alive.
  Within the same priority, events with the lowest
  transmit count are packed first.
- **Supersession:** when a higher-incarnation event
  for the same node arrives, the old event is replaced
  in the queue immediately.
- **Expiry:** events are dropped from the queue once
  their transmit count reaches the transmit limit
  (which is the base limit scaled by the event's
  own multiplier).

---

## 9. Membership State Machine

The `alive → suspect → dead` transitions and the
incarnation acceptance rules are specified in
`ALGORITHM.md` (procedures B and E). This section
records the design decisions behind them.

- **States:** `alive`, `suspect`, `dead`
- **Graceful leave:** treated as `dead`
  (no separate `left` state)
- **Incarnation numbers:** always included.
  Seeded at startup with the wall-clock time in
  milliseconds.
- **Dead → alive transition:** only via gossip with
  `inc > dead_inc`. Direct pings or acks from a dead
  node do not revive it; the node must restart with a
  higher incarnation and let that propagate via gossip.

> **Caveat:** if the system clock steps backward
> (NTP correction) between a node's death and its
> restart, the restarted node's incarnation may be
> lower than the stale dead event. The cluster will
> ignore its alive announcements until the incarnation
> value overtakes the stale one. This window is
> typically milliseconds. Mitigate by ensuring NTP
> is configured with `makestep` rather than slewing.

- **Dead node GC:** dead entries are retained
  internally for `dead_node_expiry` (default 6000 ms)
  to reject stale alive events. After expiry a
  rejoining node with the same identity is treated as
  a fresh member.

- **Membership list capacity:** unbounded. The array
  starts at 16 slots and doubles on each realloc.
  There is no hard cap. This is reasonable for the
  intended scale (tens to hundreds of nodes); at
  1000 members the list is ~340 KB. The risk is
  runaway growth if a bug or misconfiguration causes
  the node to accept membership for many fake peers —
  there is no defence against that today.

---

## 10. Bootstrap / Join

Join and seed handling are specified in `ALGORITHM.md`
(procedures G and F). Design decisions:

- **Discovery:** caller provides a seed node list.
- **Self-announcement:** on startup the node gossips
  its own `alive` so peers learn its real incarnation
  early.
- **Seed unreachable:** start as a single-node
  cluster and retry seeds every `seed_retry_interval`
  (default 5000 ms, fixed interval).
- **Join mode:** async — the node starts immediately
  and membership converges in the background.

---

## 11. API

### Initialization

Use `swim_opts_for(n, detect_ms)` to compute timing
parameters from cluster size and a desired worst-case
failure-detection latency, then fill in the identity
fields and call `swim_start()`:

```c
const char *seeds[] = { "10.0.0.1:7771/c1", NULL };
swim_start_opts_t opts = swim_opts_for(50, 10000);
opts.self  = "10.0.0.2:7771/c1";
opts.name  = "my_cluster";
opts.seeds = seeds;
swim_start(&opts);
```

Or zero-initialize and set all fields manually:

```c
swim_start_opts_t opts = {
  .self  = "10.0.0.1:7771/c1",
  .name  = "my_cluster",
  .seeds = seeds,
  .protocol_period_ms    = 1000,
  .ping_timeout_ms       = 200,
  .ping_req_fanout       = 3,
  .suspicion_timeout_ms  = 3000,
  .seed_retry_interval_ms = 5000,
  .dead_node_expiry_ms   = 6000
};
swim_start(&opts);
```

Except for `swim_start`, which takes a `swim_start_opts_t`
struct pointer containing the unique instance name, all
public functions take a mandatory `name` argument to target
a specific instance. The name cannot be `NULL` or empty.

### Querying membership

```c
int count;
char *p = swim_peers("my_cluster", false, &count);
// iterate: char *s = p; for (int i = 0; i < count; i++) { ...; s += strlen(s)+1; }
free(p);
```

### Event subscription

A single callback is registered at start time via
`swim_start_opts_t`:

```c
opts.callback = my_callback;
opts.ctx      = my_ctx;
```

The callback has the signature:

```c
void callback(void *ctx, swim_event_t event,
              const char *node);
```

`event` is one of:

- `SWIM_NODE_UP` / `SWIM_NODE_SUSPECT` / `SWIM_NODE_DOWN`:
  membership transition; `node` is the affected peer string.
- `SWIM_FEED`: the telemetry feed has records; `node` is
  NULL. The callback should drain with `swim_read_feed()`
  until it returns 0.

The callback is invoked from the protocol loop with no locks
held, so it may re-enter the public API. It must be
non-blocking; offload heavy work to another thread.

### Liveness hint

```c
swim_hint_alive("my_cluster", "10.0.0.2:7772");
```

Feeds out-of-band evidence that `peer` is alive into the
failure detector — semantically equivalent to having
received a SWIM ack from it. Intended for applications
that already communicate with peers over another channel
(e.g. an HTTP request *from* `peer`) and want that
first-hand reachability signal to suppress false-positive
suspicion. SWIM probes run over UDP; this lets a
successful exchange over TCP count as liveness evidence.

Semantics — local and advisory:

- If `peer` is `suspect`, its suspicion timer is
  cancelled and an `alive` event is re-disseminated, so
  this node will not declare `peer` dead.
- If `peer` is already `alive`, there is no membership
  change.
- A `dead` `peer` is not revived — revival requires a
  higher incarnation from the peer itself (see §9).
- In all cases, any in-flight *direct or indirect* probe
  this node has outstanding to `peer` is cancelled, so a
  probe already doomed to time out cannot re-suspect
  `peer` (and gossip false `suspect`) moments after the
  hint. Relay probes (`ping_req` work performed for
  another node) are left untouched — they never trigger
  suspicion here, and dropping one would only deny that
  node a confirmation path.

It cannot override a same-incarnation `suspect` already
circulating elsewhere; only the peer's own self-refutation
(a higher incarnation) is authoritative cluster-wide. The
call is asynchronous (non-blocking) and never blocks the
caller.

### Graceful leave

```c
swim_leave("my_cluster");
```

1. Increments own incarnation number.
2. Broadcasts a leave message directly to
   `max(⌈N×0.25⌉, 8)` random peers (not via gossip
   queue), where `N` is the number of alive + suspect
   peers.
3. Stops the protocol task. A supervising owner may
   restart it; perform a full shutdown afterward for a
   complete teardown.

For a silent stop (no dead broadcast), shut down the
library directly without calling `swim_leave`.

---

## 12. Observability

The library emits telemetry through a per-instance **feed**:
a growable FIFO queue of 4 KB pages that the protocol thread
writes and the application drains. There is no logging in
the library itself — the feed is the only output channel.

### Reading the feed

```c
int swim_read_feed(const char *name, int bufsz, char *buf, int nptr, char **ptr);
```

The application pulls events by calling `swim_read_feed`
repeatedly: it returns the number of strings copied (>= 1)
on success, `0` when the feed is empty, or `-1` on error.
A buffer of size 4096 (`bufsz`) and a pointer array of
size 10 (`nptr`) are recommended.

Example:

```c
char buf[4096];
char *ptr[10];
int n;
while ((n = swim_read_feed("my_cluster", sizeof(buf), buf, 10, ptr)) > 0) {
  // Process event in ptr[0 ... n-1]
}
```

### Events

```
"node"    "up"      "host:port[:cookie]"   # a peer became alive
"node"    "suspect" "host:port[:cookie]"   # a peer became suspect
"node"    "down"    "host:port[:cookie]"   # a peer was declared dead
"ping"    "rtt"     "host:port[:cookie]" "<ms>"  # direct probe round-trip
"cluster" "size"    "<n>"                  # alive+suspect count, on change
"warning" "message dropped to host:port[:cookie]"  # outbound drop
```

- A node renders as `host:port[:cookie]`, with IPv6 hosts
  bracketed (via `swim_node_id_format`). The cookie is carried
  as its own record string, so no escaping is needed.
- `ping rtt` is reported only for a clean **direct** probe
  round-trip (a direct ack to our ping); indirect/relayed
  resolutions produce no rtt event.
- `cluster size` is emitted only when the count changes.
- Telemetry is lossless under normal conditions. A new 4 KB
  page is allocated whenever the tail page is full. Under
  OOM, the oldest pages are freed to make room; if no pages
  can be freed (feed empty, malloc still failing), the write
  returns an error and that record is lost.

---

## 13. Ping Target Selection

The node probes one peer per period using
**round-robin with periodic shuffle** (see
`ALGORITHM.md` procedure A1). The design rationale:
this bounds worst-case detection latency to
`(N-1) × T`, avoiding the unlucky streaks possible
with pure random selection.

---

## 14. Testing Strategy

- **Unit tests:** encode/decode roundtrip, state
  machine transitions, gossip queue ordering, and
  gossip piggybacking in relay packets.
- **Integration tests:** multiple library instances
  in the same process on different ports.
- **Simulated network:** the in-memory transport
  (test-only) supports injecting packet loss, delay,
  and reorder for protocol correctness tests.
- **Large-scale Stress tests:** 64-node clusters
  testing staged startup, churn, and network
  partitions.
- **Property tests:**
  1. Encoding roundtrip: `∀ msg → encode → decode = msg`
  2. State machine: no invalid transitions regardless
     of event sequence and incarnation values
  3. Gossip queue: packed output always respects
     dead > suspect > alive priority regardless of
     insertion order
- **CI:** run locally with the test runner.

---

## 15. Non-Goals

- Data replication / CRDTs
- Leader election
- Distributed locking
- Service discovery beyond membership
- Reliable message delivery
- Cross-datacenter topology awareness
- Lifeguard / local health multiplier
- Pluggable public transport API
- Polyglot wire compatibility
