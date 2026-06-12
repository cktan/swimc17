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

- **Wire and API type:** address string
  `"host:port/cookie"`
  - `host` is a string (IP address or hostname)
  - `port` is a decimal integer
  - `cookie` is a string (user-defined opaque
    datum). Optionally specified by the caller;
    defaults to `""` (empty string).
- **Identity key:** the full `host:port/cookie` string
- **Assigned by:** caller
- **Stability:** stable across restarts (same
  address string, new incarnation number). A
  change in cookie signifies a different node
  instance even if host/port are reused.

### Node Identification with Cookie

While a node is reachable via its network
location (`host` and `port`), the protocol
incorporates a `cookie` into the node's strict
identity (`host:port/cookie`). This cookie allows
the application to distinguish between different
instances, sessions, or restarts of a node running
on the exact same host and port.

The cookie is optionally specified by the caller
during node startup or when defining seeds. If not
explicitly provided, it defaults to `""`.

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
{ping, sender :: host:port/cookie, seq :: non_neg_integer,
       updates :: [update]}

# ack
{ack, sender :: host:port/cookie, seq :: non_neg_integer,
      updates :: [update]}

# ping_req (A asks C to ping B)
{ping_req, sender :: host:port/cookie, seq :: non_neg_integer,
           target :: host:port/cookie, updates :: [update]}

# forwarded ack (C tells A that B responded)
{fwd_ack, sender :: host:port/cookie, seq :: non_neg_integer,
          source :: host:port/cookie, updates :: [update]}

# leave
{leave, sender :: host:port/cookie, seq :: non_neg_integer}
```

- **Update shape:**

```
{alive,   host:port/cookie, incarnation :: non_neg_integer}
{suspect, host:port/cookie, incarnation :: non_neg_integer}
{dead,    host:port/cookie, incarnation :: non_neg_integer}
```

---

## 6. Security

Every UDP packet carries a 12-byte auth header
prepended before the message payload:

```
[tval: 4 bytes BE] [hval: 8 bytes] [message ...]
```

- **tval** — sender's wall-clock time as a
  `uint32_t` Unix seconds, big-endian. The receiver
  rejects the packet if `|tval − now| > 10 s`.
- **hval** — SipHash-2-4 over
  `tval_be4 || message_bytes`, keyed on the first
  16 bytes of `name` (zero-padded). Covers the full
  message body, so any mutation is detected.
- **Shared secret** — `swim_start_opts_t.name` is
  the cluster secret. Every node in the cluster must
  use the same value. Packets from nodes with a
  different (or absent) secret are silently dropped.

### What this provides

- **Cluster isolation** — nodes that don't know the
  secret cannot inject gossip or trigger membership
  changes.
- **Message integrity** — hval covers the full
  message body; any in-flight mutation is rejected.
- **Replay resistance** — combining a keyed MAC with
  a ±10 s timestamp window makes replays expire
  quickly, and the MAC prevents forging a fresh
  timestamp without the key.

### What this does not provide

- **Confidentiality** — packets are plaintext; use
  a network-layer encryption (VPC, WireGuard) if
  needed.
- **Insider protection** — all nodes share one
  secret, so any cluster member can forge packets
  from any other member.
- **Key entropy guarantee** — short or low-entropy
  names (e.g. `"prod"`) weaken the SipHash key.
  Use a sufficiently random string in production.

### Implementation

SipHash-2-4 is implemented inline in `swim_main.c`
(~50 lines, no new dependency). The auth header is
prepended in `send_message` and validated in
`recv_message`; the codec layer (`swim_codec.c`) is
unchanged. The 12-byte header is taken from the
existing 1400-byte MTU budget, leaving 1388 bytes
for the message payload.

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
opts.name  = "my_cluster_secret"; // shared by all nodes
opts.seeds = seeds;
swim_start(&opts);
```

`opts.name` is the **shared cluster secret** (see §6).
Every node in the cluster must supply the same value.
Packets from nodes with a different name are silently
dropped.

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
swim_t *inst = swim_start(&opts);
```

All instance functions take the opaque `swim_t *` handle
returned by `swim_start`.

### Querying membership

```c
int count;
char *p = swim_peers(inst, false, &count);
// iterate: char *s = p; for (int i = 0; i < count; i++) { ...; s += strlen(s)+1; }
free(p);
```

### Telemetry feed

Create a `swim_feed_t`, pass it to `swim_start_opts_t`,
then read records from a separate thread:

```c
swim_feed_t *feed = swim_feed_create();
opts.feed = feed;  // caller owns; NULL disables telemetry
swim_start(&opts);

// reader thread:
char buf[SWIM_FEED_MAX_RECORD_SIZE];
char *ptr[SWIM_FEED_MAX_ELEMENTS];
for (;;) {
    swim_feed_wait(feed, 1000); // returns 0 or 1 (timeout)
    int n;
    while ((n = swim_feed_get(feed, sizeof(buf), buf,
                              SWIM_FEED_MAX_ELEMENTS, ptr)) > 0) {
        // process ptr[0..n-1]
    }
}

swim_leave(inst);
swim_feed_destroy(feed);
```

`swim_feed_wait` blocks until the feed is signalled or
the timeout elapses. Returning 0 does **not** guarantee a
record is present — always follow with `swim_feed_get`.
The feed is optional; pass `opts.feed = NULL` to disable
telemetry.

### Liveness hint

```c
swim_hint_alive(inst, "10.0.0.2:7772");
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
swim_leave(inst);
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

The library emits telemetry through a caller-supplied
**feed**: a growable FIFO queue of 4 KB pages that the
protocol thread writes and the application drains. There
is no logging in the library itself — the feed is the only
output channel. Telemetry is optional; pass `opts.feed =
NULL` to disable it entirely.

### Feed ownership

The caller creates the feed, passes it to `swim_start`,
and destroys it after `swim_leave` returns:

```c
swim_feed_t *feed = swim_feed_create();
opts.feed = feed;
swim_start(&opts);
// ... run ...
swim_leave(inst);
swim_feed_destroy(feed);
```

### Reading the feed

```c
char buf[SWIM_FEED_MAX_RECORD_SIZE];
char *ptr[SWIM_FEED_MAX_ELEMENTS];
int n;
while ((n = swim_feed_get(feed, sizeof(buf), buf,
                          SWIM_FEED_MAX_ELEMENTS, ptr)) > 0) {
  // Process event in ptr[0 ... n-1]
}
```

Use `swim_feed_wait(feed, timeout_ms)` to block until
data arrives rather than busy-polling.

### Events

```
"node"    "up"      "host:port/cookie"   # a peer became alive
"node"    "suspect" "host:port/cookie"   # a peer became suspect
"node"    "down"    "host:port/cookie"   # a peer was declared dead
"ping"    "rtt"     "host:port/cookie" "<ms>"  # direct probe round-trip
"cluster" "size"    "<n>"               # alive+suspect count, on change
"warning" "message dropped to host:port/cookie"  # outbound drop
```

- A node renders as `host:port/cookie`, with IPv6
  hosts bracketed (via `swim_node_id_format`). The
  cookie is carried as its own record string, so no
  escaping is needed.
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


## 16. Error Codes

| Code | Meaning |
|------|---------|
| `SWIM_OK` | No error |
| `SWIM_ERR_NOMEM` | Out of memory |
| `SWIM_ERR_INVALID` | Invalid argument or malformed packet |
| `SWIM_ERR_FULL` | Gossip queue or feed full |
| `SWIM_ERR_TIMEOUT` | Operation timed out (e.g., `swim_feed_wait`) |
| `SWIM_ERR_BAD_STATE` | Object in an illegal state |

## 17. Resource Usage

- **Membership list** – starts with 16 entries, doubles on growth. For 1000 nodes the list is ~340 KB. No hard cap.
- **Feed** – 4 KB pages; each record ≤ `SWIM_FEED_MAX_RECORD_SIZE` (1024 B) plus a 4‑byte length header.
- **Packet size** – maximum 1400 bytes (`SWIM_MAX_PACKET_SIZE`). Larger payloads cause `SWIM_ERR_INVALID`.
- **Memory overhead** – each node contributes at most a few kilobytes (membership entry + feed page).

## 18. Node Identity Limits

- Cookie length is limited to `SWIM_NODE_ID_MAX_COOKIE` (64 bytes). Empty cookie is allowed.

## 19. Thread‑Safety

All public functions are thread‑safe **except** the feed API:
- `swim_feed_create`, `swim_feed_destroy`, `swim_feed_put`, `swim_feed_get`, `swim_feed_wait`, `swim_feed_wakeall`, `swim_feed_empty` can be called concurrently from multiple threads because they internally protect the data structures with a mutex.
- The library itself does not perform any global locking; each `swim_t` instance owns its own mutex.
## 20. Protocol Limits

| Constant | Description |
|----------|-------------|
| `SWIM_MAX_PACKET_SIZE` | Maximum UDP payload size (1400 bytes). |
| `SWIM_MAX_EVENTS` | Maximum gossip updates that can be decoded from a packet. |
| `SWIM_FEED_MAX_RECORD_SIZE` | Maximum size of a telemetry record (1024 bytes). |
| `SWIM_MAX_PACKET_SIZE` is enforced in `swim_pack_message`; exceeding it returns `SWIM_ERR_INVALID`. |
| `SWIM_MAX_EVENTS` caps the loop in `swim_unpack_message`. |

## 21. Secret Handling

The cluster secret (`opts.name`) is used as the SipHash key. The implementation:
- Truncates the string to the first **16 bytes** if longer.
- Zero‑pads the string to exactly 16 bytes if shorter.
Thus only the first 16 bytes affect MAC verification.

## 22. Seed Retry Behavior

If all seeds are unreachable the node:
- Starts as a single‑node cluster.
- Retries seeds **indefinitely** at `seed_retry_interval_ms` (default 5000 ms).
- No exponential back‑off or max‑retry limit is applied.

## 23. `swim_opts_for` fallback

When called with:
- `n <= 1`, or
- `detect_ms == 0`, or
- the derived period `T` rounds to zero,
the function returns the **default timing parameters** (T = 1000 ms, etc.) without error.

## 24. Glossary

- **tval** – 4‑byte big‑endian Unix timestamp prepended to every packet.
- **hval** – 8‑byte SipHash‑2‑4 MAC over `tval || message`.
- **Incarnation** – monotonic counter incremented on each change of a node’s state.
- **Transmit multiplier** – `ceil(log₂(N+1)) × 3` where `N` is the number of alive + suspect nodes.
- **Ping target selection** – round‑robin with periodic shuffle (procedure A1).

## 25. Feed Record Format

Each feed record consists of:
```
int n          // number of strings (1 .. SWIM_FEED_MAX_ELEMENTS)
char str0\0
char str1\0
...
char str(n‑1)\0
```
The total size must not exceed `SWIM_FEED_MAX_RECORD_SIZE` (including the 4‑byte length header). The `swim_feed_get` API copies the strings into the caller‑provided buffer and returns the count `n`.
