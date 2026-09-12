#include "net/DispatcherRemoteControl.h"

#include "net/TargetSsh.h"
#include "net/TcpConnect.h"

#include <unistd.h>

namespace camsyringe {

namespace {

bool pollControlPort(const QString& target, int controlPort, int totalTimeoutMs) {
    const int stepMs = 500;
    int waited = 0;
    while (waited < totalTimeoutMs) {
        std::string err;
        int fd = connectWithTimeout(target.toStdString(), controlPort, 1, &err);
        if (fd >= 0) {
            ::close(fd);
            return true;
        }
        usleep(static_cast<useconds_t>(stepMs) * 1000);
        waited += stepMs;
    }
    return false;
}

// Idempotent: only spawns if qcarcam_dispatcher isn't already in `pidin`'s
// output, detached so it survives this SSH connection closing -- see
// run_qcarcam.sh's own header for what it actually does (ensure the
// display service is up, then start qcarcam_dispatcher and wait).
//
// CONFIRMED FOR REAL against the live target (a prior version of this
// comment described a DIFFERENT, incorrect theory about env.sh's own fds
// -- corrected here after isolating the actual cause with a live SSH
// session): this shell (QNX's own minimal ksh) does NOT properly detach a
// COMPOUND command backgrounded via `&`, even trivially (`( true &&
// sleep 5 ) &`, `( true; sleep 5 ) &`, or a `for` loop all measurably
// blocked the ssh client for the command's full duration; a single
// SIMPLE command backgrounded the same way detached in well under a
// second every time). `. /var/opt/env.sh && nohup run_qcarcam.sh` is a
// compound command, which is exactly what broke it. Separately, `A || B &`
// ALSO failed to detach `B` on this shell even when B was already a
// single simple command -- only `A ; B &` or `if ! A; then B & fi` did.
// Fixed by combining both workarounds: an `if`/`then` instead of `||`
// before the backgrounded part, and wrapping the compound "source env.sh
// then exec run_qcarcam.sh" in a NESTED `sh -c '...'` invocation so the
// OUTER shell only ever sees ONE simple command to background (`sh`,
// with two arguments) -- verified on the live target to return in ~0.2s
// either way (dispatcher already running, or freshly started) instead of
// blocking for its full run_qcarcam.sh startup time (which itself can
// run well past this function's own ssh timeout on a cold display-
// service start -- see ensureRunning()'s own comment for why a timeout
// here still isn't treated as fatal, kept as defense in depth even after
// this fix). `exec` (not a second `&&`-joined command) hands off the
// nested shell's own process to run_qcarcam.sh directly, avoiding one
// extra layer of process -- not required for the detach fix itself, just
// tidier. (No `disown` needed: no pty is ever allocated for this ssh
// invocation -- see commonSshOpts(), no `-t` -- so there's no
// controlling terminal to send SIGHUP on session close in the first
// place; `nohup` alone is defensive insurance, not load-bearing here.)
// `--ipv4` is appended whenever `target` doesn't look like an IPv6
// literal (same "contains a ':'" check as TcpConnect.h's
// bracketHostIfIPv6/TargetSsh.cpp's bracketIfIPv6, deliberately not
// shared into a named predicate here since this is the only spot that
// needs the family for a *decision* rather than for string formatting)
// -- so run_qcarcam.sh/qcarcam_dispatcher always bind the same family
// CamSyringe is about to connect the control channel on. This is an
// interim workaround for this board's IPv6 socket creation being broken
// at the platform level (confirmed not a qcarcam_dispatcher/CamSyringe
// bug -- see run_qcarcam.sh's own header); once fixed, IPv6 targets go
// back to needing nothing special here at all, since that was always
// qcarcam_dispatcher's own default.
QString buildStartCommand(const QString& target) {
    QString runCmd = target.contains(':') ? "run_qcarcam.sh" : "run_qcarcam.sh --ipv4";
    return "if ! pidin | grep -qi qcarcam_dispatcher; then "
           "sh -c '. /var/opt/env.sh && exec " +
           runCmd +
           "' "
           ">/tmp/qcarcam_dispatcher.log 2>&1 </dev/null & "
           "fi";
}

// All four binaries -- a running dispatcher may have spawned
// qcarcam_injector/qcarcam_receiver per camera plus a qcarcam_viewer
// previewer, none of which die just because their parent did. `pidin`,
// not pgrep/pkill (unconfirmed on this image) or `slay -f` (confirmed
// unreliable for these processes) -- see
// qcarcam-injector/ARCHITECTURE.md's "Remote process management on the
// target", mirroring qcarcam-injector/tests/deploy_and_run.sh's own
// remote_kill_by_name().
const char* kKillAllCommand =
    "for n in qcarcam_dispatcher qcarcam_injector qcarcam_receiver qcarcam_viewer; do "
    "for p in $(pidin | grep -i \"$n\" | awk '{print $1}' | sort -u); do "
    "kill -9 $p 2>/dev/null; done; done; sleep 1";

} // namespace

bool DispatcherRemoteControl::ensureRunning(TargetSsh& ssh, const QString& target, int controlPort,
                                             QString* error) {
    TargetSsh::Result r = ssh.run(target, buildStartCommand(target), 15000);
    // Poll the control port regardless of r.ok() -- confirmed for real
    // that the ssh client itself can time out here ("ssh did not
    // complete in time") even though the remote start actually
    // succeeded: run_qcarcam.sh waits up to 30s for the target's display
    // service to come up before it even starts the dispatcher, and never
    // returns after that (by design, see its own header) -- if anything
    // about that chain doesn't fully detach from the ssh session's own
    // pipes (e.g. the target's specific ksh handling backgrounding
    // slightly differently than expected), ssh ends up waiting on it
    // instead of returning right away, and a cold display-service start
    // can easily outlast this function's own 15s ssh timeout. The
    // control port coming up is the only thing that actually matters --
    // an ssh-side timeout alone must never be reported as failure while
    // the dispatcher is demonstrably fine.
    // 40s, not the 10s this used to be: CONFIRMED live as a real bug, not
    // just theoretical -- a fresh Install Injector's post-install restart
    // reported failure ("didn't come up within 10s") while qcarcam_dispatcher
    // was, moments later, demonstrably running fine and its control port
    // accepting connections. Root cause, read directly from
    // run_qcarcam.sh's own source: on a cold start (display service not
    // already up -- exactly the case right after a fresh flash/reboot,
    // which an Install is disproportionately likely to follow) it waits
    // up to a HARD 30s for /dev/openwfd_server_0 before it EVER starts
    // qcarcam_dispatcher at all, so a 10s poll here can, and did, give up
    // while the dispatcher hadn't even been spawned yet. 40s covers that
    // 30s worst case plus margin for the dispatcher itself to actually
    // bind its socket afterward.
    if (pollControlPort(target, controlPort, 40000)) {
        return true;
    }
    if (error) {
        *error = r.ok() ? "qcarcam_dispatcher didn't come up within 40s of starting it."
                         : "Couldn't start qcarcam_dispatcher on the target: " + r.stdErr;
    }
    return false;
}

bool DispatcherRemoteControl::restart(TargetSsh& ssh, const QString& target, int controlPort,
                                       QString* error) {
    ssh.run(target, kKillAllCommand, 10000); // best-effort -- nothing running is fine too
    return ensureRunning(ssh, target, controlPort, error);
}

bool DispatcherRemoteControl::stopAll(TargetSsh& ssh, const QString& target, QString* error) {
    TargetSsh::Result r = ssh.run(target, kKillAllCommand, 10000);
    if (!r.ok()) {
        if (error) *error = "Couldn't stop qcarcam processes on the target: " + r.stdErr;
        return false;
    }
    return true;
}

} // namespace camsyringe
