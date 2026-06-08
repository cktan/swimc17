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
| `seed_retry_interval` | 5000 ms | fixed, not exponential |
| `dead_node_expiry` | 6000 ms | `2 × suspicion_timeout` |

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
  their transmit count reaches the current multiplier.

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

```c
// Initialize / start the library with startup options:
swim_start_opts_t opts = {
  .host = "10.0.0.1",                  // required
  .port = 7771,                        // required
  .name = "my_cluster",                // required
  .cookie = "c1",                      // optional
  .seed_list = seeds,                  // optional
  .seed_count = 1,
  .protocol_period_ms = 1000,
  .ping_timeout_ms = 200,
  .ping_req_fanout = 3,
  .suspicion_timeout_ms = 3000,
  .seed_retry_interval_ms = 5000,
  .dead_node_expiry_ms = 6000
};
swim_start(&opts);
```

Except for `swim_start`, which takes a `swim_start_opts_t`
struct pointer containing the unique instance name, all
public functions take a mandatory `name` argument to target
a specific instance. The name cannot be `NULL` or empty.

### Querying membership

```c
swim_node_id_t peers[32];
int count = swim_peers("my_cluster", peers, 32, false);
```

### Event subscription

Membership changes are delivered through a registered
callback:

```c
swim_subscribe("my_cluster", callback, ctx);   // register
swim_unsubscribe("my_cluster", callback, ctx); // deregister
```

The callback has the signature:

```c
void callback(void *ctx, swim_event_t event,
              const swim_node_id_t *node);
```

The opaque `ctx` pointer is passed back unchanged on every
invocation. The callback is invoked from the protocol loop
when a membership change is adopted, so it must be cheap and
non-blocking; offload any heavy work to another thread.
`unsubscribe` removes the matching `(callback, ctx)` pair.

### Liveness hint

```c
swim_hint_alive("my_cluster", &peer);
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

The library emits telemetry through a per-instance **feed**: a
fixed-size (4 KB) FIFO buffer of records that the protocol thread
writes and the application drains. There is no logging in the
library itself — the feed is the only output channel.

### Reading the feed

```c
int swim_get_event(const char *name, int bufsz, char *buf, int nptr, char **ptr);
```

The application pulls events by calling `swim_get_event`
repeatedly: it returns the number of strings copied (>= 1)
on success, `0` when the feed is empty, or `-1` on error.
A buffer of size 4096 (`bufsz`) and a pointer array of
size 10 (`nptr`) are recommended.

Example:

```c
char buf[4096];
char *ptr[10];
int n;
while ((n = swim_get_event("my_cluster", sizeof(buf), buf, 10, ptr)) > 0) {
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
- Telemetry is lossy: when the fixed buffer fills, the oldest
  records are dropped rather than block the protocol.

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
  partitions (see `QA.md` for details).
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
