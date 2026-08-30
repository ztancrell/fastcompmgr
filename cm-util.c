
#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 200809L
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "cm-util.h"

#include <time.h>

int64_t
get_time_in_milliseconds(void) {
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }

  return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
