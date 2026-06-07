#include "swim_timer.h"
#include "swim_errno.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* One alarm in the delta list. tick is the delay relative to the
 * predecessor element (or to "now" for the head). */
typedef struct entry_t entry_t;
struct entry_t {
  int tick;
  swim_timer_cb_t cb;
  void *ctx;
  void *param;
  entry_t *next;
  char name[128];
};

struct swim_timer_t {
  entry_t *head;
};

swim_timer_t *swim_timer_init(void) {
  swim_timer_t *t = calloc(1, sizeof(*t));
  if (!t) {
    swim_set_error(SWIM_ERR_NOMEM, "Failed to allocate swim_timer_t container");
    return NULL;
  }
  return t;
}

int swim_timer_add(swim_timer_t *t, int ticks, const char *name,
                   swim_timer_cb_t cb, void *ctx, void *param) {
  assert(t);
  assert(ticks >= 1);
  assert(name && strlen(name) < sizeof(((entry_t *)0)->name));
  assert(cb);

  entry_t *nw = calloc(1, sizeof(*nw));
  if (!nw) {
    return swim_set_error(
        SWIM_ERR_NOMEM, "Failed to allocate entry_t node for alarm '%s'", name);
  }
  nw->cb = cb;
  nw->ctx = ctx;
  nw->param = param;
  strcpy(nw->name, name);

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

void swim_timer_cancel(swim_timer_t *t, const char *name) {
  assert(t);
  assert(name);

  entry_t **pp = &t->head;
  while (*pp && strcmp((*pp)->name, name) != 0) {
    pp = &(*pp)->next;
  }
  if (!*pp) {
    return;
  }

  entry_t *victim = *pp;
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
    entry_t *n = t->head;
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
    entry_t *n = t->head;
    t->head = n->next;
    n->cb(n->ctx, SWIM_TIMER_ALARM, n->param);
    free(n);
  }
}
