#pragma once

#include <QString>

#include <functional>

namespace camsyringe {

class TargetSsh;

// Checks, via a lightweight live functional test over SSH, whether
// `target` would hit this project's known board-level AF_INET6
// socket-creation issue (see docs/adr/
// 0001-interim-ipv4-default-while-board-ipv6-broken.md). Deliberately a
// FUNCTIONAL PROBE, not a version check -- the underlying issue has no
// discovered version signature to check instead (root-caused hard this
// session: it's a client-side rejection inside libsocket.so.4 with no
// visible IPC to the network manager at all, present on some boards'
// flashed OS images and not others, with nothing resembling a queryable
// "network stack version" found anywhere in the process).
//
// The probe itself: briefly starts /var/opt/bin/qcarcam_dispatcher on a
// throwaway scratch control port (never the real one -- never disturbs
// an already-running real session) and checks whether IT prints the
// known failure. Deliberately NOT `ping`/`telnet` -- confirmed hard,
// repeatedly, this session: those are among a small, fixed set of
// PRE-EXISTING base-image binaries immune to this board's issue (they
// always succeed at AF_INET6 regardless), so they'd never actually
// detect it; qcarcam_dispatcher is the real, affected binary itself, so
// this reproduces the exact real symptom directly rather than by proxy.
// See the ADR above for the full diagnostic trail behind why "a
// pre-existing binary" and "a freshly-run one" behave differently on
// this board at all. Any OTHER outcome (starts fine, SSH itself fails,
// binary not found -- bundle never installed) is treated as "not
// detected": this is a best-effort nicety, never a hard blocker, and a
// false "still broken" warning (e.g. once the board is eventually
// fixed, or against some other target this issue was never confirmed
// on) is worse than occasionally staying quiet.
//
// Passwordless-only, same convention as MainWindow's own
// refreshShimStatus() -- NEVER prompts for credentials, since this is an
// automatic check triggered by setting a target, not a deliberate user
// action.
class Ipv6SupportProbe {
public:
    // ok=true means the probe reached a conclusive result; `broken`
    // reflects what it found. ok=false means inconclusive (no
    // passwordless auth, SSH failure, etc.) -- callers must NOT warn in
    // that case, `broken` is meaningless when ok is false.
    using Callback = std::function<void(bool ok, bool broken)>;

    // Runs on its own background thread; callback fires from that same
    // thread, NOT marshaled to the GUI thread -- callers must do that
    // themselves (same convention as DispatcherVersionProbe::queryAsync
    // and DispatcherRemoteControl's own callers).
    static void checkAsync(TargetSsh& ssh, QString target, QString sshUser, QString sshKeyPath,
                            Callback callback);
};

}  // namespace camsyringe
