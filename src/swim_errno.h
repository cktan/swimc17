#ifndef SWIM_ERRNO_H
#define SWIM_ERRNO_H

#include "swim.h"

/* Internal: set thread-local error state. Returns -1. */
SWIM_EXTERN int swim_set_error(int e, const char *fmt, ...);

#endif /* SWIM_ERRNO_H */
