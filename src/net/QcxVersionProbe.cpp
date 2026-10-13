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

void QcxVersionProbe::classifyOs(const QString& unameOutput, Result* result) {
    static const QRegularExpression kQnx(QStringLiteral(R"(\bQNX\s+(\d+)\.(\d+)(?:\.\d+)?)"));
    const auto m = kQnx.match(unameOutput);
    if (!m.hasMatch()) {
        return; // not recognisably QNX: say nothing rather than block a target we can't judge
    }
    result->osRelease = m.captured(0).simplified();
    result->unsupportedOs = m.captured(1).toInt() != kSupportedQnxMajor;
}

QString QcxVersionProbe::findLibraryError(const QString& injectorOutput) {
    static const QRegularExpression kLoader(
        QStringLiteral(R"(unknown symbol|ldd:\s*FATAL|Could not resolve|cannot open|Could not find lib)"));
    QStringList lines;
    for (const QString& line : injectorOutput.split('\n')) {
        if (kLoader.match(line).hasMatch()) {
            lines << line.trimmed();
            if (lines.size() == 2) break;
        }
    }
    return lines.join(QStringLiteral("; "));
}

void QcxVersionProbe::checkAsync(TargetSsh& ssh, QString target, QString sshUser,
                                  QString sshKeyPath, Callback callback) {
    std::thread([&ssh, target = std::move(target), sshUser = std::move(sshUser),
                 sshKeyPath = std::move(sshKeyPath), callback = std::move(callback)]() {
        Result result;
        if (!ssh.ensureAuth(target, sshUser, sshKeyPath, declineCredentials)) {
            callback(result);
            return;
        }
        // Scratch id (99, outside the real 0-16 range) and scratch ring
        // name -- never collides with, or disturbs, an already-running
        // real session. Confirmed live: this reaches the shim's own
        // probeRealInputs() diagnostic output in ~2s, no display-service
        // dependency at all (unlike qcarcam_test). Output is never
        // redirected away, so it flows straight into this ssh.run()
        // call's own captured stdout/stderr, same idiom as
        // Ipv6SupportProbe's own dispatcher probe.
        //
        // `kill -9`, not a plain `kill`: qcarcam_injector ignores SIGTERM
        // (confirmed live on the board -- all its threads survive it), so a
        // plain kill leaked one scratch injector per CamSyringe launch. The
        // scratch ring leaves nothing behind in /dev/shmem after SIGKILL.
        TargetSsh::Result res = ssh.run(
            target,
            "uname -sr; echo ---CSPROBE---; "
            "/var/opt/bin/qcarcam_injector 99 --ring-name /camsyringe_qcxprobe_scratch "
            "--width 64 --height 64 & IP=$!; sleep 2; kill -9 $IP 2>/dev/null; wait $IP 2>/dev/null",
            8000);

        // `uname -sr` comes first, then the marker, then the injector's own output.
        QString combined = res.stdOut + "\n" + res.stdErr;
        const int marker = combined.indexOf(QStringLiteral("---CSPROBE---"));
        if (marker >= 0) {
            classifyOs(combined.left(marker), &result);
            combined = combined.mid(marker);
        }
        result.libraryError = findLibraryError(combined);
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
