#include "net/QcxVersionProbe.h"

#include "net/TargetSsh.h"

#include <QRegularExpression>

#include <thread>

namespace camsyringe {

namespace {
// Same convention as Ipv6SupportProbe -- this check runs automatically,
// not from a deliberate user action, so it must never itself prompt for
// credentials.
bool declineCredentials(const QString&, QString*, QString*) { return false; }
}  // namespace

void QcxVersionProbe::checkAsync(TargetSsh& ssh, QString target, QString sshUser,
                                  QString sshKeyPath, Callback callback) {
    std::thread([&ssh, target = std::move(target), sshUser = std::move(sshUser),
                 sshKeyPath = std::move(sshKeyPath), callback = std::move(callback)]() {
        Result result;
        if (!ssh.ensureAuth(target, sshUser, sshKeyPath, declineCredentials)) {
            callback(result);
            return;
        }
        // Scratch id (99, outside the real 1-16 range) and scratch ring
        // name -- never collides with, or disturbs, an already-running
        // real session. Confirmed live: this reaches the shim's own
        // probeRealInputs() diagnostic output in ~2s, no display-service
        // dependency at all (unlike qcarcam_test). Output is never
        // redirected away, so it flows straight into this ssh.run()
        // call's own captured stdout/stderr, same idiom as
        // Ipv6SupportProbe's own dispatcher probe.
        TargetSsh::Result res = ssh.run(
            target,
            "/var/opt/bin/qcarcam_injector 99 --ring-name /camsyringe_qcxprobe_scratch "
            "--width 64 --height 64 & IP=$!; sleep 2; kill $IP 2>/dev/null; wait $IP 2>/dev/null",
            8000);

        const QString combined = res.stdOut + "\n" + res.stdErr;
        result.ok = true;
        result.versionMismatch = combined.contains(QStringLiteral("QCARCAM_RET_UNSUPPORTED"));
        if (result.versionMismatch) {
            static const QRegularExpression kVersionPattern(
                QStringLiteral(R"(QCARCAM_VERSION=(\d+) \(v(\d+\.\d+\.\d+))"));
            auto match = kVersionPattern.match(combined);
            if (match.hasMatch()) {
                result.builtVersion =
                    QStringLiteral("%1 (v%2)").arg(match.captured(1), match.captured(2));
            }
        }
        callback(result);
    }).detach();
}

}  // namespace camsyringe
