#include "swim_errno.h"

#include <stdarg.h>
#include <stdio.h>

_Thread_local int swim_errno = SWIM_OK;
static _Thread_local char swim_errbuf[200] = {0};

const char *swim_errmsg(void) {
  return swim_errbuf;
}

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

void swim_set_error(int e, const char *fmt, ...) {
  swim_errno = e;
  if (fmt) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(swim_errbuf, sizeof(swim_errbuf), fmt, ap);
    va_end(ap);
  } else {
    swim_errbuf[0] = '\0';
  }
}
