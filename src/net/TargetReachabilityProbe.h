#pragma once

#include <functional>
#include <string>

namespace camsyringe {

// Fast, dedicated "is this even a plausible address at all" check for a
// Play press -- deliberately answers a NARROWER question than it looks
// like it should: NOT "is qcarcam_dispatcher up and answering on the
// control port" (that is, and stays, declareToTarget()/
// onDeclareComplete()'s job, including the existing tryStartDispatcherThenRetry()
// SSH-in-and-start-it recovery -- see MainWindow's own comments), only
// "does this address even belong to a real, live host". Confirmed live
// this session as a real, reported bug: an earlier version of this class
// did a raw TCP connect to the CONTROL PORT specifically, which made a
// perfectly correct target -- reachable, SSH-able, just with
// qcarcam_dispatcher not started yet (an ordinary, previously-recoverable
// state) -- get misreported as "wrong IP" and auto-stopped, entirely
// bypassing tryStartDispatcherThenRetry()'s own recovery. Ping-based
// instead now, same host-liveness question TargetSsh's own passwordless-
// probe pre-check already answers (net/PingProbe.h) -- deliberately
// reusing that exact logic rather than a second, differently-tuned
// reimplementation.
class TargetReachabilityProbe {
public:
    using Callback = std::function<void(bool reachable)>;

    // Pings `target` on a background thread, bounded to a couple of
    // seconds (see PingProbe.h) -- callback fires exactly once, from THAT
    // thread (caller must marshal to the GUI thread itself via
    // QMetaObject::invokeMethod, same convention as DispatcherClient/
    // DispatcherVersionProbe). reachable is false ONLY when the ping
    // conclusively reports the host unreachable; an inconclusive ping
    // (missing binary, ICMP blocked, no clear answer in time) reports
    // reachable=true -- same "never turn a working target into a false
    // negative" contract PingProbe.h's own quickPingUnreachable()
    // documents, since this is a fast pre-check, not the final word.
    static void checkAsync(std::string target, Callback callback);
};

} // namespace camsyringe
