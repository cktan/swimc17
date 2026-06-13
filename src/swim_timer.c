/*
 * swim_timer.c — Delta-list tick timer.
 *
 * Implements a logical (non-wall-clock) timer driven by
 * explicit swim_timer_tick() calls from the protocol loop.
 * The caller invokes swim_timer_tick() once every 100 ms.
 * One tick equals 100 ms; the default protocol period is 1000 ms (10 ticks).
 *
 * Alarms are stored in a singly-linked list where each
 * node's tick field is the delay *relative to its
 * predecessor* (delta encoding). This means advancing time
 * by one tick costs O(1): decrement the head's tick by 1
 * and fire all entries whose tick reaches zero. Insertion
 * is O(n) but the list stays short in practice.
 *
 * Cancelled and fired entries are not freed immediately;
 * they are pushed onto a free_list slab and reused on the
 * next swim_timer_add(). Memory is only returned to the
 * heap in swim_timer_destroy().
 *
 * Callbacks receive SWIM_TIMER_ALARM on normal fire and
 * SWIM_TIMER_CANCEL when cancelled. The entry is unlinked
 * from the list before the callback is invoked, so the
 * callback may safely call swim_timer_add() or
 * swim_timer_cancel() without corrupting the list.
 */
#include "swim_timer.h"
#include "swim_errno.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct entry_t entry_t;
struct entry_t {
  int tick;           /* ticks remaining relative to predecessor (or now for head) */
  swim_timer_cb_t cb; /* callback invoked on fire or cancel */
  void *ctx;          /* opaque context passed to cb */
  void *param;        /* per-alarm parameter passed to cb */
  entry_t *next;      /* next entry in delta list or free_list */
  char name[384];     /* human-readable label for debugging */
};

struct swim_timer_t {
  entry_t *head;      /* front of the delta list; smallest remaining tick */
  entry_t *free_list; /* slab of recycled entries available for reuse */
};

// Create an empty timer. Free it with swim_timer_destroy().
swim_timer_t *swim_timer_create(void) {
  swim_timer_t *t = calloc(1, sizeof(*t));
  if (!t) {
    swim_set_error(SWIM_ERR_NOMEM, "Failed to allocate swim_timer_t container");
    return NULL;
  }
  return t;
}

// Arm an alarm to fire @ticks ticks from now. Several alarms due on the same
// tick fire in the order added. @name is the cancel key (copied); @cb is
// called as cb(ctx, SWIM_TIMER_ALARM, param) when due.
int swim_timer_add(swim_timer_t *t, int ticks, const char *name,
                   swim_timer_cb_t cb, void *ctx, void *param) {
  assert(t);
  assert(ticks >= 1);
  assert(name && strlen(name) < sizeof(((entry_t *)0)->name));
  assert(cb);

  // any time the free list is empty, refill it with
  // 8 new entries in a batch
  entry_t *nw;
  if (!t->free_list) {
    for (int i = 0; i < 8; i++) {
      nw = calloc(1, sizeof(*nw));
      if (!nw) {
        return swim_set_error(SWIM_ERR_NOMEM,
                              "Failed to allocate entry_t node for alarm '%s'",
                              name);
      }
      nw->next = t->free_list;
      t->free_list = nw;
    }
  }

  // pop an entry from free_list
  assert(t->free_list);
  nw = t->free_list;
  t->free_list = nw->next;
  memset(nw, 0, sizeof(*nw));

  // initialize the new entry
  nw->cb = cb;
  nw->ctx = ctx;
  nw->param = param;
  /* All names are internally generated: fixed literals or suspect_key()
   * output (max ~333 bytes: 255 host + 5 port + 63 cookie + overhead).
   * The 384-byte buffer is always sufficient; strcpy is safe here. */
  strcpy(nw->name, name);

  // Hook nw into the timer list at the right place, and adjust
  // the tick of the successor.

  /* Walk past every alarm due at or before our target, summing the
   * deltas we step over. Tie (S + cur->tick == ticks) -> we go
   * after the existing alarm, so it fires first. */
  int s = 0;
  entry_t **pp = &t->head;
  while (*pp && s + (*pp)->tick <= ticks) {
    s += (*pp)->tick;
    pp = &(*pp)->next;
  }

  nw->tick = ticks - s;
  if (*pp) {
    /* The successor's delta was relative to our predecessor; it is
     * now relative to us. */
    (*pp)->tick -= nw->tick;
  }
  nw->next = *pp;
  *pp = nw;

  return 0;
}

// Cancel the first pending alarm whose name equals @name, firing
// SWIM_TIMER_CANCEL on it. No-op if no match.
void swim_timer_cancel(swim_timer_t *t, const char *name) {
  assert(t);
  assert(name);

  // locate an entry matching name
  entry_t **pp = &t->head;
  while (*pp && strcmp((*pp)->name, name) != 0) {
    pp = &(*pp)->next;
  }
  // if not found, done!
  if (!*pp) {
    return;
  }

  // unlink victim from the list
  entry_t *victim = *pp;
  *pp = victim->next;
  victim->next = NULL;
  if (*pp) {
    /* The successor was measured from victim; restore it relative
     * to victim's predecessor. */
    (*pp)->tick += victim->tick;
  }
  /* List is consistent now, so the callback may reenter safely. */
  victim->cb(victim->ctx, SWIM_TIMER_CANCEL, victim->param);

  // Insert victim into free_list.
  victim->next = t->free_list;
  t->free_list = victim;
}

// Cancel every pending alarm, firing SWIM_TIMER_CANCEL on each. Timer remains
// valid and reusable.
void swim_timer_cancel_all(swim_timer_t *t) {
  assert(t);

  // keep firing cancel until list is empty
  while (t->head) {
    entry_t *n = t->head;
    t->head = n->next;
    n->cb(n->ctx, SWIM_TIMER_CANCEL, n->param);
    n->next = t->free_list;
    t->free_list = n;
  }
}

// Cancel all alarms (swim_timer_cancel_all) then destroy the timer. Handle is
// invalid afterwards.
void swim_timer_destroy(swim_timer_t *t) {
  if (!t) {
    return;
  }
  swim_timer_cancel_all(t);
  while (t->free_list) {
    entry_t *n = t->free_list;
    t->free_list = n->next;
    free(n);
  }
  free(t);
}

// Advance the timer by one tick (100 ms) and fire every alarm now due as
// cb(ctx, SWIM_TIMER_ALARM, param). No-op when no alarms are pending.
void swim_timer_tick(swim_timer_t *t) {
  assert(t);

  // No pending timer?
  if (!t->head) {
    return;
  }

  // Deduct 1 tick
  t->head->tick -= 1;
  
  /* Fire the head and any following alarms whose delta is 0 (i.e.
   * due on this same tick). Pop before firing so a reentrant
   * callback sees a consistent list. */
  while (t->head && t->head->tick <= 0) {
    entry_t *n = t->head;
    t->head = n->next;
    n->cb(n->ctx, SWIM_TIMER_ALARM, n->param);
    n->next = t->free_list;
    t->free_list = n;
  }
}
