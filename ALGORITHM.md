# The SWIM Algorithm as Realized in swim

This is a top-down sketch of the SWIM+INF+Susp protocol as
implemented here. It starts with the overall framework and
then refers down into the individual procedures (A, B, C,
…). Each procedure is described at the level you need to
understand the algorithm — not every branch in the code.
Names match the source (`src/`).

---

## Framework

Every node runs one protocol instance, driven by a single
event loop. Two recurring timers drive it, and incoming
packets feed it:

```
every protocol_period (T):   procedure A  (probe one peer)
every seed_retry_interval:   procedure G  (find/heal cluster)
on each received packet:     procedure C  (handle message)
on user leave call:          procedure H  (announce + stop)
```

A telemetry feed reports membership and RTT events
(procedure I).

The heart is the **probe cycle** (A → B), which detects
failures. Everything a node learns — about itself or
others — is spread by **gossip piggybacked on the probe
traffic** (procedure D), and interpreted through the
**membership + incarnation rules** (procedure E).

The lifecycle of any peer's status:

```
        probe fails (A,B)            suspicion_timeout (B4)
 alive ──────────────────► suspect ──────────────────► dead
   ▲                          │
   └──── ack / refute (B,F) ──┘
```

Constants and their defaults (from the configuration):

| Name | Default | Role |
| :--- | :--- | :--- |
| `protocol_period` (`T`) | 1000 ms | one probe per period |
| `ping_timeout` | 200 ms | wait for a (direct/indirect) ack |
| `ping_req_fanout` (`k`) | 3 | indirect probers |
| `suspicion_timeout` | 3000 ms | suspect → dead delay |
| `seed_retry_interval` | 5000 ms | re-contact seeds |
| `dead_node_expiry` | 6000 ms | keep dead entries this long |

---

## A. Probe a peer (each `protocol_period`)

A1. Pick one `target` from the alive/suspect members,
    excluding self, using **round-robin with periodic
    shuffle**: iterate the member list in order, reshuffle
    when exhausted. This bounds detection latency to
    `(N-1) × T`.

A2. Send a direct `ping(seq)` to `target`, carrying gossip
    (procedure D), and arm a `ping_timeout`.

A3. Run failure detection on the outcome (procedure B).

(Before A1 each period, expired dead entries are GC'd —
see procedure E.)

---

## B. Failure detection for the probed `target`

B1. **Direct ack received** within `ping_timeout`: target
    is alive. Cancel timers, mark it alive (procedure E),
    done.

B2. **No direct ack**: ask `ping_req_fanout` (`k`) random
    alive peers to probe `target` on our behalf
    (`ping_req`). Each relay pings `target`; if `target`
    answers, the relay forwards the ack back to us
    (`fwd_ack`). Arm another `ping_timeout`.

B3. **An ack arrives** (directly or via `fwd_ack`) before
    that timeout: target is alive. Done.

B4. **Still no ack**: declare `target` **suspect** — record
    it, gossip `suspect(target, inc)` (procedure D), write a
    telemetry event (procedure I), and start a `suspicion_timeout` timer
    (procedure E governs the timer).

B5. **`suspicion_timeout` fires** and `target` is still
    suspect at the same incarnation: declare it **dead** —
    gossip `dead(target, inc)` and write a telemetry event (procedure I).

At any moment, an `alive` rumor with a high enough
incarnation (procedure F) cancels suspicion and revives the
node.

---

## C. Handle an incoming message

Each packet is decoded, its piggybacked gossip is applied
(procedure E), and then it is dispatched by type:

- C1. **`ping`** → mark the sender alive (procedure E) and
  reply with an `ack` (which itself carries gossip).
- C2. **`ack`** → cancel the matching pending probe
  (procedure B1/B3).
- C3. **`ping_req`** (we are a relay) → ping the requested
  `target`; if it answers, send a `fwd_ack` back to the
  original requester.
- C4. **`fwd_ack`** → the relay tells us `target` answered;
  cancel our pending indirect probe.
- C5. **`leave`** → transition the sender node to dead and
  cancel its suspicion timer.

---

## D. Gossip dissemination

Status changes are spread by piggybacking events
(`alive` / `suspect` / `dead`) on the `ping`, `ack`,
`ping_req`, and `fwd_ack` packets the node already sends.

D1. **Enqueue**: when a node adopts a status change, it
    puts the event in a per-node gossip queue. Fresh
    self-refutation rumors (where the node refutes a false
    accusation about itself) get extra transmit slots
    (refutation_multiplier = 2, vs 1 for other updates).

D2. **Supersession**: a newer event for the same node
    (higher incarnation, or more urgent status at the same
    incarnation) replaces the queued one.

D3. **Pack**: when sending a packet, fill it up to the MTU
    (1400 bytes, less a 128-byte margin) with queued events
    in priority order **dead > suspect > alive**, least-sent
    first.

D4. **Expiry**: each event is dropped once it has been sent
    about `ceil(log2(N+1)) × 3` times (the transmit limit
    for cluster size `N`), so each rumor reaches the whole
    cluster with high probability and then stops.

---

## E. Membership and incarnation rules

Each member is `{status, incarnation, dead_at}`. An
**incarnation** is a per-node version number, seeded at
startup from the wall-clock time in milliseconds. Incoming
events are accepted by these rules:

- E1. **`alive(node, inc)`**: accepted for an unknown node
  (a join, recorded at that incarnation), or when `inc`
  beats the current incarnation. A `dead` node is revived
  only by a strictly **higher** incarnation.
- E2. **`suspect(node, inc)`**: accepted if `inc` is at
  least the current incarnation and the node is alive or
  suspect; starts/keeps a suspicion timer (procedure B4).
- E3. **`dead(node, inc)`**: accepted if `inc` is at least
  the current incarnation and the node is not already dead.
- E4. **Same incarnation** ties break by status precedence
  **dead > suspect > alive**.

Whenever an event is *adopted* (it actually changed local
state), it is re-gossiped (procedure D) and a telemetry event is
written (procedure I).

**Dead-node retention**: dead entries are kept for
`dead_node_expiry` to reject stale `alive` rumors, then
garbage-collected; a later rejoin is then treated as fresh.

---

## F. Self-refutation

If a node receives `suspect(self)` or `dead(self)` with an
incarnation `>=` its own, it **bumps its incarnation** to
one above the rumor and gossips `alive(self, new_inc)`. By
E1, this higher incarnation overrides the false rumor
everywhere. (The node also self-announces `alive` once at
startup so peers learn its real incarnation early.)

---

## G. Join and seeds

G1. The node starts immediately and converges in the
    background (async join).

G2. Every `seed_retry_interval`: if the node is **alone**
    (no members), it pings **all** seeds to discover the
    cluster; if it is **already in a cluster**, it pings
    **one** random seed, to detect and heal a partition.

A seed ping is an ordinary probe (procedure A2), so the
reply pulls the node into the gossip mesh.

---

## H. Graceful leave

H1. Bump own incarnation to the current wall-clock time in
    milliseconds (so it outranks any rumor about the node).

H2. Send a `leave` message **directly** (not via the
    gossip queue) to a fanout of `max(ceil(N × 0.25), 8)`
    random peers, then stop the instance.

For a silent stop, just shut down the library instead.

---

## I. Telemetry feed (membership and instrumentation events)

If a telemetry feed is attached, the node writes status transitions and
other instrumentation records to it.

I1. Telemetry records are written to a growable FIFO queue of 4 KB pages
    (the feed). If memory is exhausted, the oldest unread pages are
    dropped to make room for new records.

I2. When a node status change is adopted (procedure E), the node writes
    an event record: `["node", "up" | "suspect" | "down", target_id]`
    to the feed.

I3. On a successful direct probe ack (procedure B1), the node writes a
    latency record: `["ping", "rtt", target_id, latency_ms]` to the feed.

I4. Outbound message drops or cluster size changes also write records
    (e.g., `["cluster", "size", count]`, `["warning", message]`).
