# SWIM Code Review Findings (Since 55c93143)

This code review analyzes all commits introduced to the SWIM library since commit `55c931431f115cfe869e00646759b50300980baf` up to `1e557ea42f29ac9e1d734f807afe39d958aaea48`.

## Executive Summary of Changes
- **Single-Callback Model**: Replaced the dynamic list of up to 16 subscribers (`swim_subscribe` / `swim_unsubscribe`) with a single callback and context registered at startup time in `swim_start_opts_t` (`opts.callback` and `opts.ctx`).
- **Telemetry Feed (`SWIM_FEED`)**: Added the `SWIM_FEED` event to `swim_event_t`. When new entries are written to the telemetry feed, the background protocol loop notifies the callback with a `SWIM_FEED` event, instructing the subscriber to drain the feed using `swim_read_feed()`.
- **Simplification**: Removed the internal subscription arrays from `swim_instance_t` and simplified `notify_batch_t`.
- **Validation**: Added a safety assert inside `swim_gossip_queue_pack` to ensure correctness during entry squeezing.

---

## Detailed Findings

### 1. [CRITICAL] Race Condition and Use-After-Free (UAF) in `swim_hint_alive`

#### Description
In [swim_hint_alive](file:///home/sprite/p/swimc17/src/swim_main.c#L1159-L1206), the code unlocks the instance mutex before dispatching notifications to avoid deadlocks:
```c
  notify_batch_t batch;
  take_notifications(inst, &batch);
  swim_callback_t cb = inst->callback;
  void *ctx = inst->ctx;
  pthread_mutex_unlock(&inst->mutex);

  dispatch_notifications(&batch, cb, ctx, inst->feed);
```
However, the pointer `inst->feed` is passed to [dispatch_notifications](file:///home/sprite/p/swimc17/src/swim_main.c#L538-L552), which accesses it without holding `inst->mutex`:
```c
static void dispatch_notifications(notify_batch_t *batch, swim_callback_t cb,
                                   void *ctx, swim_feed_t *feed) {
  if (cb) {
    ...
    if (!swim_feed_empty(feed))
      cb(ctx, SWIM_FEED, NULL);
  }
  ...
}
```
If [swim_leave](file:///home/sprite/p/swimc17/src/swim_main.c#L1037-L1108) is called concurrently on another thread:
1. `swim_leave` removes the instance from the registry.
2. It locks `inst->mutex` (blocking until `swim_hint_alive` unlocks it).
3. Once unlocked, `swim_leave` acquires the lock, performs cleanup, destroys the feed structure (`swim_feed_destroy(inst->feed)`), and frees `inst`.
4. Meanwhile, `swim_hint_alive` resumes on the first thread and runs `dispatch_notifications(&batch, cb, ctx, inst->feed)`.
5. Since `inst` and `inst->feed` have been freed and destroyed, calling `swim_feed_empty(feed)` causes a use-after-free (UAF) bug.

#### Impact
**High Risk / Critical Severity.** Can cause segment faults, memory corruption, or unpredictable behavior when hints and instance teardowns run concurrently.

#### Recommendation
Do not pass or dereference `inst->feed` after unlocking the instance mutex. Instead, check if the feed is empty while still holding the mutex, store the result in a boolean flag `feed_has_data`, and pass that flag to `dispatch_notifications`:

```diff
-static void dispatch_notifications(notify_batch_t *batch, swim_callback_t cb,
-                                   void *ctx, swim_feed_t *feed) {
+static void dispatch_notifications(notify_batch_t *batch, swim_callback_t cb,
+                                   void *ctx, bool feed_has_data) {
   if (cb) {
     for (int i = 0; i < batch->count; i++) {
       char node_str[350];
       swim_node_id_format(&batch->items[i].node, node_str, sizeof(node_str));
       cb(ctx, batch->items[i].event, node_str);
     }
-    if (!swim_feed_empty(feed))
+    if (feed_has_data)
       cb(ctx, SWIM_FEED, NULL);
   }
   free(batch->items);
   batch->items = NULL;
   batch->count = 0;
 }
```

In `swim_protocol_loop` and `swim_hint_alive`:
```diff
       take_notifications(instance, &batch);
+      bool feed_has_data = !swim_feed_empty(instance->feed);
       pthread_mutex_unlock(&instance->mutex);
-      dispatch_notifications(&batch, instance->callback, instance->ctx,
-                             instance->feed);
+      dispatch_notifications(&batch, instance->callback, instance->ctx,
+                             feed_has_data);
```

---

### 2. [LOW] Potential NULL Pointer Dereference in `swim_feed_empty`

#### Description
The public library function [swim_feed_empty](file:///home/sprite/p/swimc17/src/swim_feed.c#L285-L290) immediately locks `feed->mutex` without checking if `feed` is NULL:
```c
bool swim_feed_empty(swim_feed_t *feed) {
  pthread_mutex_lock(&feed->mutex);
  bool empty = (feed->read_off == feed->write_off);
  pthread_mutex_unlock(&feed->mutex);
  return empty;
}
```
If a caller accidentally passes a `NULL` feed, this function will segfault.

#### Impact
**Low Severity.** Most internal calls are protected by verifying the parent instance, but public APIs should always be robust against `NULL` pointers.

#### Recommendation
Add a defensive NULL check at the beginning of the function:
```c
bool swim_feed_empty(swim_feed_t *feed) {
  if (!feed) {
    return true;
  }
  pthread_mutex_lock(&feed->mutex);
  bool empty = (feed->read_off == feed->write_off);
  pthread_mutex_unlock(&feed->mutex);
  return empty;
}
```

---

### 3. [MEDIUM] Non-portable VLA Initialization in `swim_gossip_queue_pack`

#### Description
In [swim_gossip_queue_pack](file:///home/sprite/p/swimc17/src/swim_gossip_queue.c#L178-L235), the code allocates a variable-length array (VLA) `keep` and initializes it via `memset`:
```c
  bool keep[queue->count];
  memset(keep, 1, queue->count);
```
Specifying `queue->count` as the byte-size argument to `memset` assumes that `sizeof(bool) == 1`. While this is common on modern systems/compilers, the C standard does not guarantee it. If compiled on an architecture where `sizeof(bool) > 1`, `memset` will leave the majority of the `keep` array uninitialized, causing undefined behavior in the subsequent loop.

#### Impact
**Medium Severity / Portability Issue.** Can lead to hard-to-debug failures on non-standard compilers or architectures.

#### Recommendation
Use `sizeof(keep)` to guarantee that the entire array is initialized regardless of the platform's representation of `bool`:
```diff
-  memset(keep, 1, queue->count);
+  memset(keep, 1, sizeof(keep));
```
Alternatively, initialize it using a standard loop:
```c
  for (int i = 0; i < queue->count; i++) {
    keep[i] = true;
  }
```

---

## Architectural & Design Observations

### Locking Hierarchy Check
The locking hierarchy is consistent and acyclic:
1. `g_instances_mutex` (Global Registry Lock) -> `inst->mutex` (Instance Lock) -> `feed->mutex` (Feed Ring Buffer Lock)
This prevents deadlock scenarios between concurrent `swim_start`, `swim_leave`, `swim_read_feed`, and protocol operations.

### Callback Execution Semantics
Because the callback registered in `swim_start_opts_t` is called directly from the background worker thread, any long-running or blocking user code in the callback will halt the protocol tick loop, potentially resulting in delayed pings/probes and false-suspect node failure detections. 
The documentation in `DESIGN.md` correctly warns the user about this constraint:
> "It must be non-blocking; offload heavy work to another thread."
This documentation warning is sufficient and crucial.
