#pragma once

#include <atomic>
#include <cstdint>

namespace camsyringe {

// Captures one CLOCK_MONOTONIC origin per playback session and hands it to
// every CameraStream so all cameras' DTS-paced sleeps share one wall-clock
// zero point ("all threads slave to the same steady_clock origin captured
// at playback start"). Re-callable: start() may be invoked again for a
// fresh session after stop, recapturing the origin each time.
class Timeline {
public:
    void start();
    int64_t originNs() const { return originNs_; }
    bool isStarted() const { return started_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> started_{false};
    int64_t originNs_ = 0;
};

} // namespace camsyringe
