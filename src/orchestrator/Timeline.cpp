#include "orchestrator/Timeline.h"

#include "util/Clock.h"

namespace camsyringe {

void Timeline::start() {
    originNs_ = monotonicNowNs();
    started_.store(true, std::memory_order_release);
}

} // namespace camsyringe
