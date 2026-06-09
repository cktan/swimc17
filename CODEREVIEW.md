# SWIM Code Review Findings (Since 3bebccb1)

This code review analyzes all commits introduced to the SWIM library since commit `3bebccb185bc99d3d6b36f705bc94b61e6a8ec13` (which refactored `swim_feed` into a paged queue) up to `HEAD` (`ec9fd7748a62414e6c5cc454d887f182c6b536b2`).

## Executive Summary of Changes

* **Removal of Callback System**: Replaced the previous single-callback model (`opts.callback` and `opts.ctx` in `swim_start_opts_t`) and the related event types (`swim_event_t`) with a pull-based telemetry system. The global `swim_read_feed()` API has also been removed.
* **User-Owned Telemetry Feed**: Telemetry is now captured via `swim_feed_t`, whose lifecycle is managed explicitly by the caller using `swim_feed_create()` and `swim_feed_destroy()`. The feed is optionally passed to `swim_start_opts_t` (`opts.feed`), allowing telemetry to be disabled entirely if `NULL`.
* **Blocking Wait Capability**: Added the `swim_feed_wait(feed, timeout_ms)` function to allow reader threads to block until telemetry is written to the feed or a timeout occurs, avoiding the need for busy-polling.
* **Simplification of Internals**: Removed the deferred notification queues (`pending_notify_t`, `notify_batch_t`) and dispatch machinery (`take_notifications`, `dispatch_notifications`). The protocol loop now writes directly to the user-supplied feed under `inst->mutex`, eliminating re-entrancy deadlock risks.
* **Single Header Interface**: Moved the public `swim_feed_t` declarations from `swim_feed.h` into the main public header `swim.h`.

---

## Detailed Findings

### 1. [HIGH] Lost Wakeup / Race Condition in `swim_feed_wait`

#### Description
In [swim_feed_wait](file:///home/sprite/p/swimc17/src/swim_feed.c#L158-L180), the function locks `feed->mutex` and immediately calls `pthread_cond_timedwait` without checking if the feed already contains unread data:

```c
int swim_feed_wait(swim_feed_t *feed, uint64_t timeout_ms) {
  ...
  pthread_mutex_lock(&feed->mutex);
  int rc = pthread_cond_timedwait(&feed->cond, &feed->mutex, &ts);
  pthread_mutex_unlock(&feed->mutex);
  ...
}
```

This creates a classic **lost wakeup** race condition:
1. A reader thread drains the feed via `swim_feed_get` and finds it empty.
2. Before the reader thread calls `swim_feed_wait` and locks the mutex, the protocol worker thread writes a new event to the feed (calling `swim_feed_put`), signals the condition variable `feed->cond`, and unlocks the mutex.
3. The reader thread then locks the mutex and calls `pthread_cond_timedwait`. Since the signal occurred before the reader began waiting, the signal has no effect. The reader blocks and sleeps for the full `timeout_ms`, despite there being unread data in the queue.

Similarly, if data was written to the feed during initialization before the reader thread started its wait loop, the first call to `swim_feed_wait` will block and delay processing of that initial data until a timeout occurs or a new event is written.

#### Impact
**High Severity / Latency Issue.** Telemetry events will be delayed and sit unprocessed in the queue for up to `timeout_ms` (which is typically `1000` ms in the documentation examples), failing to deliver near-real-time telemetry.

#### Recommendation
Check if the feed is empty (under the lock) before calling `pthread_cond_timedwait`. If the feed contains unread data, unlock and return `0` immediately:

```diff
@@ -169,5 +169,10 @@
   }
 
   pthread_mutex_lock(&feed->mutex);
+  bool empty = (feed->head == NULL || feed->head->bot == feed->head->top);
+  if (!empty) {
+    pthread_mutex_unlock(&feed->mutex);
+    return 0;
+  }
   int rc = pthread_cond_timedwait(&feed->cond, &feed->mutex, &ts);
   pthread_mutex_unlock(&feed->mutex);
```

---

### 2. [MEDIUM] Concurrency / Undefined Behavior Hazard on Teardown

#### Description
If a reader thread is blocked inside `swim_feed_wait()` (which yields `feed->mutex` while waiting on `feed->cond`), and another thread calls `swim_feed_destroy()` concurrently, it will call `pthread_cond_destroy` and `pthread_mutex_destroy` while they are still in use:

```c
void swim_feed_destroy(swim_feed_t *feed) {
  if (!feed)
    return;
  pthread_cond_destroy(&feed->cond);
  pthread_mutex_destroy(&feed->mutex);
  ...
}
```

According to POSIX standards, destroying active mutexes or condition variables upon which threads are currently blocked or referenced results in undefined behavior (often causing segment faults or hangs when the waiting thread wakes up and attempts to re-acquire the destroyed mutex).

#### Impact
**Medium Severity.** Potential crashes or deadlocks during application shutdown.

#### Recommendation
**Integration Guidelines for Users:**
The user must ensure the reader thread is joined or terminated *before* calling `swim_feed_destroy()`. To do this cleanly:
1. The reader thread should check a shared atomic shutdown flag before calling `swim_feed_wait`.
2. To prevent the reader thread from blocking for the full timeout during shutdown, the main thread can wake it up immediately by writing a dummy/sentinel record (e.g. `"shutdown"`) to the feed using `swim_feed_put()` prior to joining the thread.

**Suggested API Improvement:**
Introduce a `swim_feed_shutdown()` or `swim_feed_close()` API that sets a internal `closed` flag inside `swim_feed_t`, signals the condition variable, and causes all subsequent `swim_feed_wait` and `swim_feed_get` calls to fail or return 0 immediately. This allows the reader loop to exit cleanly and guarantees synchronization primitives are not in use when `swim_feed_destroy` is called.

---

### 3. [LOW] Integer Overflow and `EINVAL` with Large Timeouts in `swim_feed_wait`

#### Description
In `swim_feed_wait`, the timeout calculation adds `timeout_ms` to `ts.tv_sec` and `ts.tv_nsec` without overflow checks:

```c
  ts.tv_sec  += (time_t)(timeout_ms / 1000);
  ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000L;
  }
```

If a caller passes `UINT64_MAX` or a value close to it to represent an infinite wait, `ts.tv_sec` will overflow (either 32-bit or 64-bit depending on the system's `time_t`). An overflowed/negative `tv_sec` or a value in the past causes `pthread_cond_timedwait` to fail immediately with `EINVAL`, making the wait return `-1` with error `SWIM_ERR_INVALID`.

#### Impact
**Low Severity.** Users cannot perform clean infinite/long waits using large timeout values.

#### Recommendation
Add an overflow check, or support a timeout of `0` (or a special constant like `SWIM_WAIT_FOREVER`) to perform a standard, non-timed `pthread_cond_wait`:

```c
  if (timeout_ms == SWIM_WAIT_FOREVER) {
    pthread_mutex_lock(&feed->mutex);
    int rc = pthread_cond_wait(&feed->cond, &feed->mutex);
    pthread_mutex_unlock(&feed->mutex);
    ...
  }
```

---

## Architectural & Design Observations

### Locking Hierarchy Check
The locking hierarchy remains acyclic and safe:
$$\text{inst}\rightarrow\text{mutex} \implies \text{feed}\rightarrow\text{mutex}$$
No library function holding `feed->mutex` ever calls an API that acquires `inst->mutex`. This guarantees there are no deadlock cycles between protocol actions and feed operations.

### Callback Elimination
Eliminating the callback system is a major architectural improvement:
1. **No Re-entrancy Deadlocks**: The background protocol thread no longer runs arbitrary user code. Previously, if user callbacks called public SWIM functions, it could easily deadlock the worker thread.
2. **Decoupled Lifetimes**: Decoupling feed ownership from the `swim_instance_t` lifetime resolves the previous Use-After-Free (UAF) vulnerabilities on instance teardown. The feed remains allocated and safe to read even if the instance is destroyed.

### Copying Semantics
The design of `swim_feed_get` utilizes copying semantics to copy records from the internal `feed_page_t` structures directly into the user-supplied buffer. This is a very clean memory barrier: the caller does not retain pointers into the feed's internal memory pages, allowing pages to be freed or recycled safely without risking dangling pointers in the calling application.
