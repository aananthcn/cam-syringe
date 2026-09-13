#include "net/TargetConnectivityMonitor.h"

#include "net/PingProbe.h"

#include <QMetaObject>

#include <thread>

namespace camsyringe {

TargetConnectivityMonitor::TargetConnectivityMonitor(QObject* parent) : QObject(parent) {
    connect(&timer_, &QTimer::timeout, this, &TargetConnectivityMonitor::tick);
}

void TargetConnectivityMonitor::start(QString target, int intervalMs) {
    target_ = std::move(target);
    ++token_; // any probe still in flight for the previous target is now stale, see tick()
    probeInFlight_ = false; // don't let a stale in-flight probe block this target's own first tick()
    state_ = State::Unknown;
    timer_.start(intervalMs);
    tick(); // probe right away rather than waiting out the first interval
}

void TargetConnectivityMonitor::stop() {
    timer_.stop();
    ++token_;
    probeInFlight_ = false;
    target_.clear();
    state_ = State::Unknown;
}

void TargetConnectivityMonitor::probeNow(std::function<void(State)> callback) {
    if (state_ != State::Unknown) {
        callback(state_);
        return;
    }
    pendingOneShots_.push_back(std::move(callback));
    if (!probeInFlight_) {
        tick(); // don't wait out the rest of the regular interval
    }
    // else: a probe (the regular timer's, or a previous probeNow()'s) is
    // already in flight -- this callback fires once it resolves, same as
    // every other pending one-shot queued against it.
}

void TargetConnectivityMonitor::tick() {
    if (probeInFlight_ || target_.isEmpty()) {
        return; // never pile up concurrent pings, same guard MainWindow's own shimBusy_ follows
    }
    probeInFlight_ = true;
    const int token = token_;
    QString target = target_;
    std::thread([this, token, target]() {
        bool unreachable = quickPingUnreachable(target);
        QMetaObject::invokeMethod(
            this,
            [this, token, unreachable]() {
                if (token != token_) {
                    return; // stale -- start()/stop() moved on since this began; leave current
                            // bookkeeping (including whatever NEWER probe already owns
                            // probeInFlight_) alone
                }
                probeInFlight_ = false;
                State newState = unreachable ? State::Unreachable : State::Reachable;
                bool changed = (newState != state_);
                state_ = newState;

                auto oneShots = std::move(pendingOneShots_);
                pendingOneShots_.clear();
                for (auto& cb : oneShots) {
                    cb(state_);
                }
                if (changed && callback_) {
                    callback_(state_);
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

} // namespace camsyringe
