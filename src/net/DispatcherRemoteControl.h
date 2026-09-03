#pragma once

#include <QString>

namespace camsyringe {

class TargetSsh;

// Starts/restarts qcarcam_dispatcher on the target over SSH (via
// TargetSsh -- ensureAuth() must have already succeeded for `target`),
// so a teammate never has to SSH in and run run_qcarcam.sh (or kill a
// stale process) by hand. Two callers:
//   - MainWindow's Play-time retry, when the control-channel connection
//     to the target fails outright (dispatcher not running at all).
//   - InjectorBundleInstaller, after a fresh bundle install -- the new
//     binary needs a restart to actually take effect.
//
// Uses `pidin`, NOT `pgrep`/`pkill` -- see
// qcarcam-injector/ARCHITECTURE.md's "Remote process management on the
// target" section: pgrep/pkill aren't confirmed present on this QNX
// image, and QNX's own `slay -f <name>` is confirmed UNRELIABLE for this
// project's processes specifically (leaves them running, causing a
// "text file busy" scp failure later) -- this mirrors
// qcarcam-injector/tests/deploy_and_run.sh's own remote_kill_by_name().
class DispatcherRemoteControl {
public:
    // Idempotent: does nothing beyond a quick pidin check (fast) if
    // qcarcam_dispatcher is already running. Otherwise starts it detached
    // (whole start subshell redirected+backgrounded, not just the final
    // command -- see kStartCommand's own comment in the .cpp for why that
    // scope matters, confirmed for real) via /var/opt/env.sh +
    // run_qcarcam.sh. Success/failure is decided ENTIRELY by polling the
    // control port for up to ~10s afterward, not by whether the ssh
    // command itself returned cleanly -- confirmed for real that ssh can
    // time out here (a cold target-side display-service start can take
    // close to/past this function's own ssh timeout) while the dispatcher
    // still comes up fine; only report failure if the port never does.
    static bool ensureRunning(TargetSsh& ssh, const QString& target, int controlPort,
                               QString* error);

    // Kills all four qcarcam-injector binaries (a running dispatcher may
    // have spawned qcarcam_injector/qcarcam_receiver per camera plus a
    // qcarcam_viewer previewer -- these don't die just because their
    // parent dispatcher did), then calls ensureRunning() -- used after a
    // fresh bundle install so the new binary actually takes effect
    // instead of the old process(es) just continuing to run.
    static bool restart(TargetSsh& ssh, const QString& target, int controlPort, QString* error);

    // Kills all four qcarcam-injector binaries and leaves them stopped --
    // used on MainWindow's Stop action, so pressing Stop actually stops
    // everything on the target too, not just the local UI/streams (the
    // control-channel disconnect alone only tears down the spawned
    // per-camera injector/receiver/viewer processes -- see
    // main_dispatcher.cpp's handleConnection()/stopSession() -- it
    // deliberately leaves the long-lived dispatcher LISTENER itself
    // running, waiting for the next connection; this kills that too).
    static bool stopAll(TargetSsh& ssh, const QString& target, QString* error);
};

} // namespace camsyringe
