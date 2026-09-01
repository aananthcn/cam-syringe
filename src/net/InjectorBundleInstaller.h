#pragma once

#include <QString>

#include <functional>

namespace camsyringe {

class TargetSsh;

// Pushes a qcarcam_injector_bundle_vX.Y.bin to a target over SCP/SSH,
// removes the previous install first (the bundle's own installer is
// deliberately additive -- see
// ~/labs/qnx/qnx_toolkit/release/create-qcarcam-inj-bundle.sh's header
// comment -- so this class does the removal itself), runs the new
// bundle, then restarts qcarcam_dispatcher (via DispatcherRemoteControl)
// so the new binary actually takes effect -- a teammate never needs to
// SSH in and do any of this by hand. Runs entirely on its own background
// thread (via installAsync()'s internal std::thread), same "background
// thread, caller marshals callbacks to the GUI thread" convention as
// DispatcherClient/StreamPool -- EXCEPT confirm/credentials below, which
// the caller must implement as BLOCKING calls back onto the GUI thread
// (e.g. QMetaObject::invokeMethod(..., Qt::BlockingQueuedConnection)
// around a modal dialog's exec()), since this class needs their answer
// before continuing, not just a fire-and-forget notification.
//
// Auth is resolved via the caller-owned TargetSsh (see its own class
// comment) -- shared with MainWindow's Play-time "start the dispatcher if
// it's not running" retry, so a credentials prompt (if the target isn't
// set up for passwordless SSH) only ever happens once per app session,
// not once per feature.
class InjectorBundleInstaller {
public:
    // Shown on the GUI thread before anything destructive happens. Must
    // display *bundlePath (not just target/version) so the user can see
    // exactly which file is about to be pushed -- and may overwrite
    // *bundlePath/*bundleVersion in place (e.g. after the user picks a
    // different file from a "Choose Different File..." option) before
    // returning; the installer re-reads both afterward, so whatever the
    // callback leaves them as is what actually gets removed/transferred/
    // installed. Returns false to abort the whole install.
    using ConfirmCallback =
        std::function<bool(const QString& target, QString* bundlePath, QString* bundleVersion)>;

    using CredentialsCallback = std::function<bool(const QString& target, QString* username,
                                                     QString* password)>;

    using ResultCallback = std::function<void(bool success, QString message)>;

    // Fire-and-forget step updates once the install actually starts doing
    // something (after confirm() succeeds) -- percent is 0-100 for a
    // meaningful value, or -1 to mean "in progress, no percentage
    // available" (the SCP transfer specifically: scp's own progress
    // meter only ever appears on a real tty, which this class -- like
    // TargetSsh generally -- deliberately never allocates, so there's no
    // byte-level progress to report; the caller is expected to show this
    // as an indeterminate/busy indicator instead of a stalled 0%). Same
    // GUI-thread-marshaling convention as ResultCallback.
    using ProgressCallback = std::function<void(int percent, QString label)>;

    // controlPort is only needed for the post-install restart's
    // "wait for the dispatcher to actually come back up" poll. sshUser is
    // passed straight through to ssh.ensureAuth() as the passwordless
    // default (MainWindow's sshUser_ -- see its own comment).
    static void installAsync(TargetSsh& ssh, QString target, int controlPort, QString sshUser,
                              QString bundlePath, QString bundleVersion, ConfirmCallback confirm,
                              CredentialsCallback credentials, ProgressCallback progress,
                              ResultCallback onDone);
};

} // namespace camsyringe
