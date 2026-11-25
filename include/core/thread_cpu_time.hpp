#pragma once

// Processor time the calling thread has used, split into time in the program
// and its libraries (user) and time in the kernel on its behalf (system).
//
// A wall-clock stage timer cannot tell a call that waits (for the display's
// refresh, for the GPU, for a reply from the window system) from one that
// computes. Reading this before and after the call can: a 13 ms present with
// 0.3 ms of processor time waited, one with 13 ms of user time copied or spun
// inside the driver, one with 13 ms of system time was in the kernel.
//
// Linux only (getrusage(RUSAGE_THREAD)); elsewhere `available` is false and
// the times are zero, and callers report nothing.

#if defined(__linux__)
#include <sys/resource.h>
#endif

namespace wowee::core {

struct ThreadCpuTime {
    double userMs = 0.0;
    double systemMs = 0.0;
    bool available = false;
};

inline ThreadCpuTime threadCpuTime() {
#if defined(__linux__)
    rusage usage{};
    if (getrusage(RUSAGE_THREAD, &usage) != 0) return {};
    return {.userMs = usage.ru_utime.tv_sec * 1000.0 + usage.ru_utime.tv_usec / 1000.0,
            .systemMs = usage.ru_stime.tv_sec * 1000.0 + usage.ru_stime.tv_usec / 1000.0,
            .available = true};
#else
    return {};
#endif
}

} // namespace wowee::core
