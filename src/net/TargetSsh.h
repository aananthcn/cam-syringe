#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QTemporaryDir>

#include <functional>
#include <map>
#include <mutex>

namespace camsyringe {

// Resolves and caches SSH auth for one or more targets, for the lifetime
// of this object -- MainWindow owns one instance spanning the whole app
// session, so InjectorBundleInstaller's install flow and MainWindow's own
// "start the dispatcher if it's not running" retry on Play (see
// DispatcherRemoteControl) don't each separately probe passwordless SSH
// or prompt for credentials: once resolved for a target, every later
// call against that same target reuses it silently, which is the whole
// point -- minimizing how often this app needs a human to type a
// password or SSH in by hand.
//
// Tries passwordless SSH first (root, key/no-password -- the common case
// for a QNX dev target). Only on failure does it ask the caller for
// credentials, via a temporary SSH_ASKPASS script (SSH_ASKPASS_REQUIRE=force,
// OpenSSH 8.4+) rather than an sshpass dependency or a pty this GUI
// process doesn't have.
//
// Thread-safe: the auth cache and askpass script are shared across
// whichever background thread calls in (install runs on its own thread,
// a Play-time dispatcher-start retry runs on another).
class TargetSsh {
public:
    // Shown on the GUI thread only if passwordless SSH fails. Fills
    // *username/*password and returns true to retry with them; false to
    // abort (user cancelled).
    using CredentialsCallback =
        std::function<bool(const QString& target, QString* username, QString* password)>;

    struct Result {
        int exitCode = -1;
        QString stdOut;
        QString stdErr;
        bool ok() const { return exitCode == 0; }
    };

    // Resolves and caches auth for `target` if not already done this
    // session. Safe to call before every run()/scp() -- a fast no-op
    // once resolved (defaultUser is then ignored -- whatever's already
    // cached wins, matching how a credentials-prompt override works the
    // same way). defaultUser is tried passwordless first (e.g.
    // MainWindow's sshUser_, from Configure's "SSH user" field or
    // --target user@host -- "root" if the caller passes empty); only on
    // failure does the credentials callback get a chance to override it.
    // Returns false only if the user cancelled the credentials prompt.
    bool ensureAuth(const QString& target, const QString& defaultUser,
                     const CredentialsCallback& credentials);

    // ensureAuth() for `target` MUST have already succeeded, or these
    // return a -1 "not authenticated" result without running anything.
    Result run(const QString& target, const QString& remoteCommand, int timeoutMs);
    Result scp(const QString& localPath, const QString& target, const QString& remotePath,
               int timeoutMs);

    // The SSH username resolved for `target` (via ensureAuth()), if
    // already known this session -- empty otherwise. DISPLAY only (e.g.
    // showing exactly what "Install Injector" is about to connect as,
    // before its confirmation dialog's destructive action) -- never used
    // to gate anything, callers must still go through ensureAuth().
    QString resolvedUser(const QString& target);

private:
    struct Auth {
        QString user;
        QProcessEnvironment env;
    };

    std::mutex mutex_;
    std::map<QString, Auth> authByTarget_;
    QTemporaryDir askpassDir_;
    bool askpassScriptReady_ = false;

    QString ensureAskpassScript(); // lazily written once; call with mutex_ held
    bool lookupAuth(const QString& target, Auth* out);
};

} // namespace camsyringe
