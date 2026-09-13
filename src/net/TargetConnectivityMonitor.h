#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <vector>

namespace camsyringe {

// Single, shared source of truth for "is the configured target address
// even alive at all" -- ICMP ping only, same narrow question ADR 0003
// established (net/PingProbe.h's quickPingUnreachable()), never a
// service-specific check (SSH, the control port, etc). Owned by
// MainWindow, ticking every ~1s once a target is set.
//
// Exists to remove a real, confirmed redundancy: the SHIM/REAL status
// feature (refreshShimStatus(), via TargetSsh::ensureAuth()'s own
// internal ping pre-check) and the Play-time reachability gate (see ADR
// 0003/0005) used to each run their OWN independent liveness check
// against the same target, on different cadences, for the same
// fundamental fact. Both now depend on THIS one instead: SHIM/REAL's
// BLIND state is derived directly from state() (see
// MainWindow::refreshShimStatus()'s own gating), and Play's reachability
// gate asks probeNow() rather than running a separate one-shot probe.
//
// SHIM/REAL still layers its OWN, strictly stronger check (SSH login +
// a board file query) ON TOP of a Reachable answer here -- this monitor
// only ever answers the narrower, lower-level question; it does not
// replace what SHIM/REAL needs beyond that.
class TargetConnectivityMonitor : public QObject {
    Q_OBJECT

public:
    enum class State { Unknown, Reachable, Unreachable };

    explicit TargetConnectivityMonitor(QObject* parent = nullptr);

    // (Re)starts monitoring `target`: resets state() to Unknown,
    // immediately kicks off a probe (doesn't wait out intervalMs for the
    // first answer), then repeats every intervalMs after. Safe to call
    // again with a different target (e.g. Configure applied against a
    // new one) -- any probe still in flight for whatever was being
    // monitored before is discarded (its eventual result recognized as
    // stale and ignored) via the same token idiom MainWindow's own
    // reachability-check bookkeeping already uses.
    void start(QString target, int intervalMs = 1000);

    // Stops monitoring entirely (e.g. currentTarget_ became empty).
    // Resets state() to Unknown; fires no callback (mirrors
    // refreshShimStatus() going quiet rather than announcing a stop).
    void stop();

    State state() const { return state_; }

    using StateChangedCallback = std::function<void(State)>;
    // Invoked on the GUI thread every time state() actually CHANGES
    // value (never on a tick that just reconfirms the same answer) --
    // set once, before start(). MainWindow uses this to re-run
    // refreshShimStatus() immediately on a connectivity transition,
    // rather than waiting up to a further ~1s for its own timer.
    void setStateChangedCallback(StateChangedCallback callback) { callback_ = std::move(callback); }

    // If state() is already known (Reachable/Unreachable), invokes
    // `callback` immediately and synchronously with that answer -- no
    // new probe. If state() is Unknown (e.g. Play pressed within the
    // first ~1-2s after start(), before its first probe has resolved),
    // ensures a probe is in flight right now (kicks one off immediately
    // if none already is -- never waits out the rest of the regular
    // timer interval) and invokes `callback` once THAT resolves.
    // Multiple overlapping calls while one probe is in flight all get
    // the same eventual answer; none of them starts a second probe.
    void probeNow(std::function<void(State)> callback);

private:
    void tick();

    QString target_;
    State state_ = State::Unknown;
    int token_ = 0;
    bool probeInFlight_ = false;
    QTimer timer_;
    StateChangedCallback callback_;
    std::vector<std::function<void(State)>> pendingOneShots_;
};

} // namespace camsyringe
