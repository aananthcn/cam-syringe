#pragma once

#include <cstdint>
#include <ctime>

namespace camsyringe {

// CLOCK_MONOTONIC in nanoseconds. Shared by Timeline and CameraStream so
// "now" means exactly the same thing everywhere a start origin is compared
// against it.
inline int64_t monotonicNowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

} // namespace camsyringe
