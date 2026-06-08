#include "swim_protocol.h"

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
