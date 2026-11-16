#include "net/InjectorBundleInstaller.h"

#include "net/DispatcherRemoteControl.h"
#include "net/InstallLog.h"
#include "net/TargetBsp.h"
#include "net/TargetDiagnostics.h"
#include "net/RealRefSync.h"
#include "net/TargetSsh.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>

#include <chrono>
#include <cstdio>
#include <thread>

namespace camsyringe {

namespace {

// Every install step goes to the PC terminal (stderr) with its command, exit code, duration and
// output, so a failed install can be narrowed down from the log alone. Exit code -1 with a
// "did not complete in time" stderr is a timeout, not a remote failure.
void logLine(const QString& text) { installLog("install", text); }

void logResult(const char* step, const TargetSsh::Result& r, qint64 ms) {
    logLine(QString("%1: exit=%2 in %3 ms, stdout=%4 B, stderr=%5 B")
                .arg(step).arg(r.exitCode).arg(ms).arg(r.stdOut.toUtf8().size()).arg(r.stdErr.toUtf8().size()));
    if (!r.stdOut.isEmpty()) logLine(QString("%1 stdout:\n%2").arg(step, r.stdOut.trimmed()));
    if (!r.stdErr.isEmpty()) logLine(QString("%1 stderr:\n%2").arg(step, r.stdErr.trimmed()));
}

TargetSsh::Result loggedRun(TargetSsh& ssh, const QString& target, const char* step,
                            const QString& command, int timeoutMs) {
    logLine(QString("%1: ssh run (timeout %2 ms): %3").arg(step).arg(timeoutMs).arg(command));
    auto t0 = std::chrono::steady_clock::now();
    TargetSsh::Result r = ssh.run(target, command, timeoutMs);
    logResult(step, r, std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0).count());
    return r;
}

// What the user needs to see when a step fails: which step, the exit code, and BOTH streams.
// A remote script's own explanation usually goes to stdout (the bundle installer prints its
// "FAIL <prog> ..." lines there and exits non-zero with an empty stderr).
QString describeFailure(const QString& what, const TargetSsh::Result& r) {
    QString out = QString("%1 (exit code %2)").arg(what).arg(r.exitCode);
    out += "\n\n--- stdout ---\n" + (r.stdOut.trimmed().isEmpty() ? QString("(empty)") : r.stdOut.trimmed());
    out += "\n\n--- stderr ---\n" + (r.stdErr.trimmed().isEmpty() ? QString("(empty)") : r.stdErr.trimmed());
    return out;
}

} // namespace

void InjectorBundleInstaller::installAsync(TargetSsh& ssh, QString target, int controlPort,
                                            QString sshUser, QString sshKeyPath, QString bundlePath,
                                            QString bundleVersion, ConfirmCallback confirm,
                                            CredentialsCallback credentials, ProgressCallback progress,
                                            ResultCallback onDone) {
    std::thread([&ssh, target = std::move(target), controlPort, sshUser = std::move(sshUser),
                 sshKeyPath = std::move(sshKeyPath), bundlePath = std::move(bundlePath),
                 bundleVersion = std::move(bundleVersion), confirm = std::move(confirm),
                 credentials = std::move(credentials), progress = std::move(progress),
                 onDone = std::move(onDone)]() mutable { // mutable: confirm() below rewrites
                                                          // bundlePath/bundleVersion in place
        logLine("log file: ~/.cache/camsyringe/install.log (appended on every install)");
        logLine(describePcEnvironment());
        logLine(QString("start: target=%1 user=%2 key=%3 controlPort=%4")
                    .arg(target, sshUser, sshKeyPath.isEmpty() ? QString("(default)") : sshKeyPath)
                    .arg(controlPort));
        if (!ssh.ensureAuth(target, sshUser, sshKeyPath, credentials)) {
            logLine("ensureAuth: cancelled or failed");
            onDone(false, "Cancelled.");
            return;
        }

        // docs/adr/0020: refuse before anything on the target is touched (the install below deletes
        // the previous one first) if the board's BSP is older than the one the bundle is built for.
        TargetSsh::Result osr =
            loggedRun(ssh, target, "target-os-release", "cat /etc/os-release; uname -a", 15000);
        TargetBsp bsp = TargetBsp::classify(osr.ok() ? osr.stdOut : QString());
        logLine(QString("BSP check: %1 -> verdict %2. %3")
                    .arg(bsp.versionId.isEmpty() ? QString("(no VERSION_ID)") : bsp.versionId)
                    .arg(bsp.verdict == TargetBsp::Verdict::Verified   ? "verified"
                         : bsp.verdict == TargetBsp::Verdict::Untested ? "untested (newer)"
                         : bsp.verdict == TargetBsp::Verdict::TooOld   ? "TOO OLD, refused"
                                                                       : "unknown, not blocked")
                    .arg(bsp.message));
        if (bsp.verdict == TargetBsp::Verdict::TooOld) {
            if (qEnvironmentVariableIntValue("CAMSYRINGE_ALLOW_UNSUPPORTED_BSP") != 0) {
                logLine("BSP check overridden by CAMSYRINGE_ALLOW_UNSUPPORTED_BSP: installing anyway");
            } else {
                onDone(false, "Not installing: unsupported BSP on " + target + ".\n\n" + bsp.message);
                return;
            }
        }

        // Confirm before anything destructive (blocking call back onto
        // the GUI thread) -- may rewrite bundlePath/bundleVersion in
        // place if the user picked a different file from within the
        // confirmation dialog, so everything below must use these
        // (already-local, mutable) variables, not the originals passed
        // into this function.
        if (!confirm(target, &bundlePath, &bundleVersion)) {
            onDone(false, "Cancelled.");
            return;
        }

        {
            QFile f(bundlePath);
            QString md5 = f.open(QIODevice::ReadOnly)
                              ? QString(QCryptographicHash::hash(f.readAll(), QCryptographicHash::Md5).toHex())
                              : QString("(unreadable: %1)").arg(f.errorString());
            logLine(QString("bundle: %1, version %2, %3 bytes, md5 %4")
                        .arg(bundlePath, bundleVersion).arg(QFileInfo(bundlePath).size()).arg(md5));
        }

        // Percentages below are rough, fixed step weights, NOT measured
        // work -- there's no cheap way to know in advance how long each
        // remote step actually takes. The SCP transfer (usually the
        // dominant cost, tens to ~180MB) reports -1/indeterminate instead
        // of a fake percentage climbing at a made-up rate -- see this
        // class's own ProgressCallback comment for why there's no real
        // byte-level number available here.
        progress(5, "Removing previous install on " + target + "...");

        // Remove the previous install -- only these three subdirs, never
        // /var/opt itself (see create-qcarcam-inj-bundle.py's own header
        // comment for why /var/opt is the shared install root and why
        // its own installer only ever touches these three).
        TargetSsh::Result rm = loggedRun(ssh, target, "remove-previous",
                                         "rm -rf /var/opt/bin /var/opt/lib /var/opt/include", 15000);
        if (!rm.ok()) {
            onDone(false, describeFailure("Failed to remove the previous install", rm));
            return;
        }

        QString bundleName = QFileInfo(bundlePath).fileName();
        // docs/adr/0019: the bundle is staged on the persistent disk, never in the target's RAM
        // (/tmp is a 64 MB RAM disk). Earlier bundles go first, including any an older CamSyringe
        // left in /tmp. This must stay outside /var/opt/bin|lib|include, which the step above wipes.
        TargetSsh::Result clean =
            loggedRun(ssh, target, "prepare-staging",
                    "mkdir -p /var/opt/tmp && rm -f /var/opt/tmp/qcarcam_injector_bundle_*.bin "
                    "/tmp/qcarcam_injector_bundle_*.bin",
                    15000);
        if (!clean.ok()) {
            onDone(false, describeFailure("Failed to prepare /var/opt/tmp on the target", clean));
            return;
        }
        progress(-1, "Transferring " + bundleName + " to " + target + "...");
        logLine("transfer: scp " + bundlePath + " -> /var/opt/tmp/ (timeout 300000 ms)");
        auto scpStart = std::chrono::steady_clock::now();
        TargetSsh::Result scp = ssh.scp(bundlePath, target, "/var/opt/tmp/", 300000); // up to 5 min for ~180MB
        logResult("transfer", scp, std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - scpStart).count());
        if (!scp.ok()) {
            onDone(false, describeFailure("Transfer failed", scp));
            return;
        }
        // Confirm what actually landed: a short or missing file would otherwise show up only as an
        // opaque failure of the run below.
        loggedRun(ssh, target, "verify-upload", QString("ls -l /var/opt/tmp/%1").arg(bundleName), 15000);

        progress(70, "Installing " + bundleName + " on " + target + "...");
        // Install -- the bundle is already self-installing/additive, see
        // create-qcarcam-inj-bundle.py; this class only handles the
        // removal step above that installer deliberately never does.
        TargetSsh::Result install =
            loggedRun(ssh, target, "run-bundle",
                      QString("chmod +x /var/opt/tmp/%1 && /var/opt/tmp/%1").arg(bundleName), 60000);
        if (!install.ok()) {
            // docs/adr/0021: write the diagnostic log on the target, and say where it is.
            TargetDiagnostics::Result diag = TargetDiagnostics::collect(ssh, target, bundleName);
            QString msg = describeFailure(QString("Install failed: %1 exited non-zero on %2")
                                              .arg(bundleName, target), install);
            if (!diag.summary.isEmpty()) msg += "\n\n--- what the diagnostics found ---\n" + diag.summary.join("\n");
            const QString where = QString("root@%1:%2").arg(target, diag.path);
            if (diag.written) {
                msg += QString("\n\n=====================================================================\n"
                               " A detailed diagnostic log was written on the target:\n"
                               "   %1\n"
                               " Please copy that file and send it, together with this PC's\n"
                               "   ~/.cache/camsyringe/install.log\n"
                               " To copy it:  scp %1 .\n"
                               "=====================================================================")
                           .arg(where);
            } else {
                msg += "\n\n(Could not write the diagnostic log on the target; the steps above and "
                       "~/.cache/camsyringe/install.log on this PC are all there is.)";
            }
            onDone(false, msg);
            return;
        }

        // The installer has run; the uploaded copy is only taking disk space now (docs/adr/0019).
        // Best-effort and kept on a failed install above, so it can be inspected or re-run by hand.
        loggedRun(ssh, target, "cleanup-bundle", QString("rm -f /var/opt/tmp/%1").arg(bundleName), 15000);

        progress(85, "Refreshing real-camera reference library on " + target + "...");
        // Best-effort, not fatal on failure -- see net/RealRefSync.h's own
        // comment for why this needs to run after every install
        // regardless of the board's current REAL/SHIM state:
        // CameraGeometryResolver's own per-camera resolution discovery
        // depends on real-ref being fresh and board-correct independently
        // of whether the SHIM/REAL toggle has ever been used this
        // session at all. Discovery itself already handles a missing/
        // broken real-ref gracefully (falls back to no resolution rather
        // than crashing, see that class's own comment), so this install
        // flow shouldn't block on it either -- deliberately not checked
        // against ok(), unlike every other step above.
        loggedRun(ssh, target, "real-ref-sync", camsyringe::realRefSyncCommand(), 15000);

        // docs/adr/0019: this project's own logs from earlier runs, so the new run's logs start
        // clean and old ones aren't mistaken for them. Before the restart, which creates
        // qcarcam_dispatcher.log afresh. Best effort, and only these names: the rest of /tmp
        // (camera config, system logs) is not ours.
        loggedRun(ssh, target, "clean-old-logs",
                "rm -f /tmp/qcarcam_dispatcher*.log /tmp/qcarcam_testapp_* "
                "/tmp/qcarcam_test_report.txt",
                15000);

        progress(90, "Restarting qcarcam_dispatcher on " + target + "...");
        // Restart so the new binary actually takes effect -- no separate
        // confirmation; the one above already covers "install vX.Y",
        // which implies wanting it actually running afterward.
        QString restartError;
        if (!DispatcherRemoteControl::restart(ssh, target, controlPort, &restartError)) {
            onDone(false, QString("Installed %1 (v%2) on %3, but restarting qcarcam_dispatcher "
                                   "failed: %4\n\n%5")
                               .arg(bundleName, bundleVersion, target, restartError, install.stdOut));
            return;
        }

        progress(100, "Done.");
        onDone(true, QString("Installed %1 (v%2) on %3 and restarted qcarcam_dispatcher -- ready "
                              "to use, no further steps needed.\n\n%4")
                          .arg(bundleName, bundleVersion, target, install.stdOut));
    }).detach();
}

} // namespace camsyringe
