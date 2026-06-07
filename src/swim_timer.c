#include "swim_timer.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* One alarm in the delta list. tick is the delay relative to the
 * predecessor element (or to "now" for the head). */
typedef struct node {
  int tick;
  swim_timer_cb_t cb;
  void *ctx;
  void *param;
  struct node *next;
  char name[128];
} node;

struct swim_timer_t {
  node *head;
};

swim_timer_t *swim_timer_init(void) {
  swim_timer_t *t = calloc(1, sizeof(*t));
  return t;
}

void swim_timer_add(swim_timer_t *t, int ticks, const char *name,
                    swim_timer_cb_t cb, void *ctx, void *param) {
  assert(t);
  assert(ticks >= 1);
  assert(name && strlen(name) < sizeof(((node *)0)->name));
  assert(cb);

  node *nw = calloc(1, sizeof(*nw));
  nw->cb = cb;
  nw->ctx = ctx;
  nw->param = param;
  strcpy(nw->name, name);

  /* Walk past every alarm due at or before our target, summing the
   * deltas we step over. Tie (S + cur->tick == ticks) -> we go
   * after the existing alarm, so it fires first. */
  int s = 0;
  node **pp = &t->head;
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
}

void swim_timer_cancel(swim_timer_t *t, const char *name) {
  assert(t);
  assert(name);

  node **pp = &t->head;
  while (*pp && strcmp((*pp)->name, name) != 0) {
    pp = &(*pp)->next;
  }
  if (!*pp) {
    return;
  }

  node *victim = *pp;
  *pp = victim->next;
  if (*pp) {
    /* The successor was measured from victim; restore it relative
     * to victim's predecessor. */
    (*pp)->tick += victim->tick;
  }
  /* List is consistent now, so the callback may reenter safely. */
  victim->cb(victim->ctx, SWIM_TIMER_CANCEL, victim->param);
  free(victim);
}

void swim_timer_cancel_all(swim_timer_t *t) {
  assert(t);

  while (t->head) {
    node *n = t->head;
    t->head = n->next;
    n->cb(n->ctx, SWIM_TIMER_CANCEL, n->param);
    free(n);
  }
}

void swim_timer_final(swim_timer_t *t) {
  if (!t) {
    return;
  }
  swim_timer_cancel_all(t);
  free(t);
}

void swim_timer_tick(swim_timer_t *t) {
  assert(t);

  if (!t->head) {
    return;
  }

  t->head->tick -= 1;
  /* Fire the head and any following alarms whose delta is 0 (i.e.
   * due on this same tick). Pop before firing so a reentrant
   * callback sees a consistent list. */
  while (t->head && t->head->tick == 0) {
    node *n = t->head;
    t->head = n->next;
    n->cb(n->ctx, SWIM_TIMER_ALARM, n->param);
    free(n);
  }
}
