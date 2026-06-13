/* Copyright (c) 2026, CK Tan.
 * https://github.com/cktan/swimc17/blob/main/LICENSE
 */
#ifndef SWIM_TIMER_H
#define SWIM_TIMER_H

#include "swim.h"
#include "swim_node_id.h"

/**
 * A passive delta-list timer.
 *
 * The timer keeps a single ordered list of alarms. Each alarm's
 * delay is stored relative to the one before it, in ticks, where
 * one tick is defined by the application to be 100 ms.
 *
 * The timer is purely logical: it never reads the wall clock,
 * never sleeps, and never touches a socket. The caller owns real
 * time and advances the timer by calling swim_timer_tick() once
 * per elapsed tick. This keeps the timer trivially testable -
 * feed it ticks, observe callbacks.
 *
 * Not thread-safe: intended for a single-threaded event loop.
 */
typedef struct swim_timer_t swim_timer_t;

/**
 * How an alarm's callback was reached. Every alarm invokes its
 * callback exactly once, terminally, with one of these.
 */
typedef enum {
  SWIM_TIMER_ALARM,  /**< the alarm came due */
  SWIM_TIMER_CANCEL, /**< the alarm was cancelled or finalized */
} swim_timer_event_t;

/**
 * Alarm callback.
 *
 * Invoked exactly once per alarm, terminally:
 *   - SWIM_TIMER_ALARM  when the alarm comes due, or
 *   - SWIM_TIMER_CANCEL when it is cancelled (swim_timer_cancel,
 *     swim_timer_cancel_all, or swim_timer_destroy).
 *
 * This single guaranteed call is the owner's one place to release
 * whatever @ctx and @param refer to. The timer treats both as
 * opaque and never dereferences or frees them itself.
 *
 * The callback may safely add or cancel alarms on the same timer
 * (the timer removes the firing alarm before invoking it).
 *
 * @param ctx   opaque context supplied to swim_timer_add().
 * @param ev    why the callback fired (alarm vs cancel).
 * @param param opaque per-alarm datum supplied to swim_timer_add().
 */
typedef void (*swim_timer_cb_t)(void *ctx, swim_timer_event_t ev, void *param);

/**
 * Create an empty timer.
 *
 * @return a new timer; free it with swim_timer_destroy().
 */
SWIM_EXTERN swim_timer_t *swim_timer_create(void);

/**
 * Cancel every pending alarm, firing SWIM_TIMER_CANCEL on each,
 * and empty the list. The timer remains valid and reusable.
 *
 * @param t the timer.
 */
SWIM_EXTERN void swim_timer_cancel_all(swim_timer_t *t);

/**
 * Cancel every pending alarm (as swim_timer_cancel_all) and then
 * destroy the timer. The handle is invalid afterwards.
 *
 * @param t the timer.
 */
SWIM_EXTERN void swim_timer_destroy(swim_timer_t *t);

/**
 * Arm an alarm to fire @ticks ticks from now.
 *
 * When it comes due the callback is invoked as
 * cb(ctx, SWIM_TIMER_ALARM, param) and the alarm is removed.
 * Several alarms due on the same tick fire in the order added.
 *
 * @param t     the timer.
 * @param ticks delay in ticks; must be >= 1.
 * @param name  lookup key for swim_timer_cancel(); copied into the
 *              alarm. Must be non-NULL and at most 384 bytes
 *              including the terminating NUL. Need not be unique.
 * @param cb    callback to fire; must be non-NULL.
 * @param ctx   opaque context passed back to @cb.
 * @param param opaque per-alarm datum passed back to @cb.
 */
SWIM_EXTERN int swim_timer_add(swim_timer_t *t, int ticks, const char *name,
                               swim_timer_cb_t cb, void *ctx, void *param);

/**
 * Cancel the first pending alarm whose name equals @name.
 *
 * The matched alarm is removed and its callback is fired as
 * cb(ctx, SWIM_TIMER_CANCEL, param). Remaining alarms keep their
 * original fire times. If no alarm matches, this is a no-op.
 *
 * @param t    the timer.
 * @param name name passed to swim_timer_add().
 */
SWIM_EXTERN void swim_timer_cancel(swim_timer_t *t, const char *name);

/**
 * Advance the timer by one tick (100 ms) and fire every alarm now
 * due, each as cb(ctx, SWIM_TIMER_ALARM, param). A no-op when no
 * alarms are pending.
 *
 * @param t the timer.
 */
SWIM_EXTERN void swim_timer_tick(swim_timer_t *t);

#endif /* SWIM_TIMER_H */
