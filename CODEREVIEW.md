# Code Review: swimc17

This document contains a comprehensive code review of the
`swimc17` library, a C17 implementation of the SWIM gossip
membership protocol.

---

## 1. Architecture & Design

The library is structured as a collection of decoupled,
cohesive modules:
- **`swim_main`**: Orchestrates the main protocol loop,
  manages instances, and coordinates timing.
- **`swim_membership`**: Maintains the registry of cluster
  members, sorted by node ID to allow $O(\log N)$ lookup.
- **`swim_gossip_queue`**: Tracks membership updates to be
  piggybacked onto outgoing UDP packets.
- **`swim_timer`**: Implements a passive, delta-list timer
  driven by logical ticks rather than wall-clock time.
- **`swim_feed`**: An auto-draining ring buffer for lossy,
  non-blocking telemetry events.
- **`swim_udp`**: Handles binding, sending, and receiving
  non-blocking IPv4 and IPv6 packets.
- **`swim_codec`**: Handles network-byte-order binary
  serialization and deserialization.

### Design Highlights
1. **Logical Timer**: Decoupling the timer from actual
   system threads and clocks via `swim_timer_tick` makes the
   protocol engine highly testable and deterministic.
2. **Decoupled Telemetry**: The lock-free/non-blocking
   telemetry feed ensures that instrumenting events never
   blocks the protocol loop.

---

## 2. Concurrency & Synchronization

The library runs a background thread per instance. Thread
synchronization and safety are well-thought-out:
- The global registry is guarded by `g_instances_mutex`.
- Individual instances are guarded by `inst->mutex`.
- Telemetry feeds have an internal `feed->mutex` lock.

### Deadlock Analysis
Lock ordering is strictly hierarchical and acyclic:
1. `g_instances_mutex` -> `inst->mutex`
2. `g_instances_mutex` -> `feed->mutex` (in `swim_read_feed`)
3. `inst->mutex` -> `feed->mutex` (in protocol thread)

Because `feed->mutex` is always a leaf lock, and the global
mutex is never acquired while holding an instance lock, the
locking model is deadlock-free.

### Teardown Safety
In `swim_leave`, the instance is first removed from
`g_instances` under the global lock. The background thread
is joined (`pthread_join`) before resources are freed.
Because any concurrent API calls must lookup the instance via
`g_instances` first, racing threads are serialized safely.

---

## 3. Code Quality & Memory Safety

The code follows strict C17 standards and incorporates
excellent safety patterns:
- **Safe Allocation**: Resizing functions use temporary
  pointers for `realloc` results before overwriting existing
  pointers, preventing memory leaks on failure.
- **Sanitizers**: AddressSanitizer and
  UndefinedBehaviorSanitizer are enabled in unit tests,
  ensuring no memory corruption goes unnoticed.
- **Thread-Local Errors**: Errors are stored in thread-local
  diagnostics (`_Thread_local`), ensuring concurrent API
  calls do not clobber each other's status.

---

## 4. Key Findings & Recommendations

### Finding 1: Unchecked `strcpy` in `swim_timer.c`
In `swim_timer_add`, the alarm key name is copied using
`strcpy`:
```c
strcpy(nw->name, name);
```
- **Risk**: Although an `assert` checks the name length,
  asserts are compiled out in production (`-DNDEBUG`). A
  malicious or bugged caller passing a long name could
  trigger a buffer overflow.
- **Recommendation**: Use `strncpy` to guarantee bounds
  safety regardless of compile-time definitions.

### Finding 2: Background Socket Errors are Muted
In `swim_udp_send` and `swim_udp_recv`, errors set a
thread-local errno. However:
- **Risk**: The background thread operates on its own
  `_Thread_local` variables. Setting `swim_set_error` in the
  protocol thread has no effect on the user thread's error
  state, leaving background socket errors completely invisible.
- **Recommendation**: Capture UDP send/recv failures and
  write them to the `swim_feed` warning channel so the user
  application can observe connectivity problems.

### Finding 3: Hard-coded Instance Limit
The library maintains active instances in a static array:
```c
static swim_instance_t *g_instances[16] = {0};
```
- **Risk**: Applications requiring more than 16 instances in
  a single process will fail with `SWIM_ERR_FULL`.
- **Recommendation**: Document this limit clearly in the
  public API header, or migrate to a dynamically allocated
  registry.

### Finding 4: Integer Overflow in logical timer
In `swim_timer.c`:
```c
while (*pp && s + (*pp)->tick <= ticks) {
```
- **Risk**: If a caller passes extremely large ticks, `s`
  could overflow.
- **Recommendation**: For safety, cast the addition or
  perform subtraction bounds-checks. However, this is minor
  since timing ticks are typically small.
