// Names the calling thread (/proc/<pid>/task/<tid>/comm). Like the original's direct
// prctl(PR_SET_NAME) calls, names longer than 15 characters are truncated; pthread_setname_np
// would reject them instead.
#pragma once

#include <sys/prctl.h>

inline void SetThreadName(const char* name)
{
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg): prctl() is variadic by definition
  prctl(PR_SET_NAME, name);
}
