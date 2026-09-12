#include "net/Ipv6SupportProbe.h"

#include "net/TargetSsh.h"

#include <thread>

namespace camsyringe {

namespace {
// Same convention as MainWindow's own refreshShimStatus() -- this check
// runs automatically, not from a deliberate user action, so it must
// never itself prompt for credentials.
bool declineCredentials(const QString&, QString*, QString*) { return false; }
}  // namespace

void Ipv6SupportProbe::checkAsync(TargetSsh& ssh, QString target, QString sshUser,
                                   QString sshKeyPath, Callback callback) {
    std::thread([&ssh, target = std::move(target), sshUser = std::move(sshUser),
                 sshKeyPath = std::move(sshKeyPath), callback = std::move(callback)]() {
        if (!ssh.ensureAuth(target, sshUser, sshKeyPath, declineCredentials)) {
            callback(false, false);
            return;
        }
        // Scratch port (59991, nowhere near the real control port or any
        // RTP port) -- never collides with, or disturbs, an
        // already-running real session. Runs the binary directly, not
        // via run_qcarcam.sh -- socket()/bind()/listen() happens at the
        // very top of main(), before any display-service dependency, so
        // this is fast and self-contained. Backgrounded, killed after
        // 1s, then waited on so the whole command line (and this SSH
        // call) completes promptly either way; its stdout/stderr are
        // never redirected away, so they flow straight into this
        // ssh.run() call's own captured output, same as every other
        // brief dispatcher-probe command used throughout this project's
        // own diagnostic history.
        TargetSsh::Result res = ssh.run(
            target,
            "/var/opt/bin/qcarcam_dispatcher --control-port 59991 & DP=$!; sleep 1; "
            "kill $DP 2>/dev/null; wait $DP 2>/dev/null",
            8000);
        bool broken = res.stdOut.contains("Address family not supported") ||
                      res.stdErr.contains("Address family not supported");
        callback(true, broken);
    }).detach();
}

}  // namespace camsyringe
