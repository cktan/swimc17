/*
 * swim_errno.c — Thread-local error state.
 *
 * Stores the most recent error code and message in
 * _Thread_local variables so each thread has its own slot
 * and concurrent API calls do not clobber each other's
 * diagnostics.
 *
 * swim_set_error() always returns -1, so call sites can
 * write `return swim_set_error(...)` as a one-liner without
 * a separate return statement.
 */
#include "swim.h"

#include <stdarg.h>
#include <stdio.h>

static _Thread_local int errnum = SWIM_OK;
static _Thread_local char errbuf[200] = {0};

int swim_errno(void) { return errnum; }

const char *swim_errmsg(void) { return errbuf; }

const char *swim_strerror(int err) {
  switch (err) {
  case SWIM_OK:
    return "Success";
  case SWIM_ERR_NOMEM:
    return "Out of memory";
  case SWIM_ERR_INVALID:
    return "Invalid argument";
  case SWIM_ERR_FULL:
    return "Container is full";
  case SWIM_ERR_TIMEOUT:
    return "Operation timed out";
  case SWIM_ERR_BAD_STATE:
    return "Object in bad state";
  default:
    return "Unknown error";
  }
}

int swim_set_error(int e, const char *fmt, ...) {
  errnum = e;
  if (fmt) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, sizeof(errbuf), fmt, ap);
    va_end(ap);
  } else {
    errbuf[0] = '\0';
  }
  return -1;
}
