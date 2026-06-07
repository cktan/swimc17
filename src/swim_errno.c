#include "swim_errno.h"

_Thread_local int swim_errno = SWIM_OK;
_Thread_local char swim_errmsg[200] = {0};

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
