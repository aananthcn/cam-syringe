#include "net/TargetReachabilityProbe.h"

#include "net/PingProbe.h"

#include <QString>

#include <thread>

namespace camsyringe {

void TargetReachabilityProbe::checkAsync(std::string target, Callback callback) {
    std::thread([target = std::move(target), callback = std::move(callback)]() {
        bool reachable = !quickPingUnreachable(QString::fromStdString(target));
        callback(reachable);
    }).detach();
}

} // namespace camsyringe
