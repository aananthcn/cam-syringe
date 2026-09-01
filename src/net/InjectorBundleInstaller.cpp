#include "net/InjectorBundleInstaller.h"

#include "net/DispatcherRemoteControl.h"
#include "net/TargetSsh.h"

#include <QFileInfo>

#include <thread>

namespace camsyringe {

void InjectorBundleInstaller::installAsync(TargetSsh& ssh, QString target, int controlPort,
                                            QString sshUser, QString bundlePath, QString bundleVersion,
                                            ConfirmCallback confirm, CredentialsCallback credentials,
                                            ProgressCallback progress, ResultCallback onDone) {
    std::thread([&ssh, target = std::move(target), controlPort, sshUser = std::move(sshUser),
                 bundlePath = std::move(bundlePath), bundleVersion = std::move(bundleVersion),
                 confirm = std::move(confirm), credentials = std::move(credentials),
                 progress = std::move(progress),
                 onDone = std::move(onDone)]() mutable { // mutable: confirm() below rewrites
                                                          // bundlePath/bundleVersion in place
        if (!ssh.ensureAuth(target, sshUser, credentials)) {
            onDone(false, "Cancelled.");
            return;
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

        // Percentages below are rough, fixed step weights, NOT measured
        // work -- there's no cheap way to know in advance how long each
        // remote step actually takes. The SCP transfer (usually the
        // dominant cost, tens to ~180MB) reports -1/indeterminate instead
        // of a fake percentage climbing at a made-up rate -- see this
        // class's own ProgressCallback comment for why there's no real
        // byte-level number available here.
        progress(5, "Removing previous install on " + target + "...");

        // Remove the previous install -- only these three subdirs, never
        // /var/opt itself (see create-qcarcam-inj-bundle.sh's own header
        // comment for why /var/opt is the shared install root and why
        // its own installer only ever touches these three).
        TargetSsh::Result rm = ssh.run(target, "rm -rf /var/opt/bin /var/opt/lib /var/opt/include", 15000);
        if (!rm.ok()) {
            onDone(false, "Failed to remove previous install: " + rm.stdErr);
            return;
        }

        QString bundleName = QFileInfo(bundlePath).fileName();
        progress(-1, "Transferring " + bundleName + " to " + target + "...");
        TargetSsh::Result scp = ssh.scp(bundlePath, target, "/tmp/", 300000); // up to 5 min for ~180MB
        if (!scp.ok()) {
            onDone(false, "Transfer failed: " + scp.stdErr);
            return;
        }

        progress(70, "Installing " + bundleName + " on " + target + "...");
        // Install -- the bundle is already self-installing/additive, see
        // create-qcarcam-inj-bundle.sh; this class only handles the
        // removal step above that installer deliberately never does.
        TargetSsh::Result install =
            ssh.run(target, QString("chmod +x /tmp/%1 && /tmp/%1").arg(bundleName), 60000);
        if (!install.ok()) {
            onDone(false, "Install failed: " + install.stdErr);
            return;
        }

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
