#include "net/TargetSsh.h"

#include <QFile>
#include <QFileDevice>
#include <QProcess>

namespace camsyringe {

namespace {

struct RunResult {
    int exitCode = -1;
    QString stdOut;
    QString stdErr;
};

RunResult runProcess(const QString& program, const QStringList& args,
                      const QProcessEnvironment& env, int timeoutMs) {
    QProcess proc;
    proc.setProcessEnvironment(env);
    proc.start(program, args);
    RunResult result;
    if (!proc.waitForStarted(5000) || !proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(2000);
        result.stdErr = QString("%1 did not complete in time").arg(program);
        return result;
    }
    result.exitCode = proc.exitCode();
    result.stdOut = QString::fromUtf8(proc.readAllStandardOutput());
    result.stdErr = QString::fromUtf8(proc.readAllStandardError());
    return result;
}

QStringList commonSshOpts() {
    return {"-o", "ConnectTimeout=8", "-o", "StrictHostKeyChecking=accept-new"};
}

// -i <path> plus IdentitiesOnly=yes when a specific key is configured --
// see ensureAuth()'s own comment for why IdentitiesOnly matters (avoids
// ssh trying every other default/agent identity before this one, which
// risks a server-side "too many authentication failures" lockout).
// Empty keyPath (the common case -- no explicit key configured) adds
// nothing, leaving ssh's own default identity/agent behavior untouched.
QStringList keyOpts(const QString& keyPath) {
    if (keyPath.isEmpty()) return {};
    return {"-i", keyPath, "-o", "IdentitiesOnly=yes"};
}

// Same logic as TcpConnect.h's bracketHostIfIPv6, QString-native since
// this whole file works in QString throughout -- needed wherever
// `target` gets concatenated into a "user@host" or "user@host:path"
// ssh/scp ARGUMENT: an IPv6 literal's own colons are otherwise
// indistinguishable from the host:port/host:path separator (scp's
// legacy syntax in particular cannot be parsed at all without this for
// an IPv6 target). Deliberately never applied to `target` itself where
// it's used as the auth cache key (authByTarget_) -- only to the text
// actually handed to the ssh/scp process.
QString bracketIfIPv6(const QString& host) { return host.contains(':') ? "[" + host + "]" : host; }

} // namespace

QString TargetSsh::ensureAskpassScript() {
    if (!askpassScriptReady_) {
        QString path = askpassDir_.filePath("askpass.sh");
        QFile script(path);
        script.open(QIODevice::WriteOnly | QIODevice::Text);
        script.write("#!/bin/sh\necho \"$CAMSYRINGE_SSH_ASKPASS_PASSWORD\"\n");
        script.close();
        script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                               QFileDevice::ExeOwner);
        askpassScriptReady_ = true;
    }
    return askpassDir_.filePath("askpass.sh");
}

bool TargetSsh::lookupAuth(const QString& target, Auth* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = authByTarget_.find(target);
    if (it == authByTarget_.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

bool TargetSsh::ensureAuth(const QString& target, const QString& defaultUser,
                            const QString& defaultKeyPath, const CredentialsCallback& credentials) {
    Auth cached;
    if (lookupAuth(target, &cached)) {
        return true;
    }

    Auth auth;
    auth.user = defaultUser.isEmpty() ? QStringLiteral("root") : defaultUser;
    auth.env = QProcessEnvironment::systemEnvironment();

    // Passwordless probe -- BatchMode=yes here ONLY (it globally disables
    // password/askpass querying too, so it must never be set once we're
    // in password-auth mode below). Covers BOTH "no key needed at all"
    // (default agent/identity already trusted) and "use this specific
    // key" (defaultKeyPath set) -- same probe either way, see keyOpts()'s
    // own comment.
    QStringList probeArgs = commonSshOpts();
    probeArgs << keyOpts(defaultKeyPath) << "-o"
              << "BatchMode=yes" << (auth.user + "@" + bracketIfIPv6(target)) << "true";
    RunResult probe = runProcess("ssh", probeArgs, auth.env, 8000);

    if (probe.exitCode == 0) {
        auth.keyPath = defaultKeyPath;
    } else {
        QString username = auth.user, password;
        if (!credentials(target, &username, &password)) {
            return false;
        }
        auth.user = username;
        // auth.keyPath left empty -- this is the password/askpass path,
        // no key involved.
        QString askpassPath;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            askpassPath = ensureAskpassScript();
        }
        // SSH_ASKPASS_REQUIRE=force (OpenSSH 8.4+) forces askpass use
        // even when a tty looks available, so this never falls back to a
        // terminal prompt CamSyringe has no way to answer.
        auth.env.insert("SSH_ASKPASS", askpassPath);
        auth.env.insert("SSH_ASKPASS_REQUIRE", "force");
        auth.env.insert("CAMSYRINGE_SSH_ASKPASS_PASSWORD", password);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    authByTarget_[target] = auth;
    return true;
}

QString TargetSsh::resolvedUser(const QString& target) {
    Auth cached;
    if (lookupAuth(target, &cached)) {
        return cached.user;
    }
    return QString();
}

QString TargetSsh::resolvedKeyPath(const QString& target) {
    Auth cached;
    if (lookupAuth(target, &cached)) {
        return cached.keyPath;
    }
    return QString();
}

TargetSsh::Result TargetSsh::run(const QString& target, const QString& remoteCommand,
                                  int timeoutMs) {
    Auth auth;
    if (!lookupAuth(target, &auth)) {
        return {-1, QString(), "ensureAuth() was not called (or failed) for " + target};
    }
    QStringList args = commonSshOpts();
    args << keyOpts(auth.keyPath) << (auth.user + "@" + bracketIfIPv6(target)) << remoteCommand;
    RunResult r = runProcess("ssh", args, auth.env, timeoutMs);
    return {r.exitCode, r.stdOut, r.stdErr};
}

TargetSsh::Result TargetSsh::scp(const QString& localPath, const QString& target,
                                  const QString& remotePath, int timeoutMs) {
    Auth auth;
    if (!lookupAuth(target, &auth)) {
        return {-1, QString(), "ensureAuth() was not called (or failed) for " + target};
    }
    QStringList args = commonSshOpts();
    args << keyOpts(auth.keyPath) << localPath
         << (auth.user + "@" + bracketIfIPv6(target) + ":" + remotePath);
    RunResult r = runProcess("scp", args, auth.env, timeoutMs);
    return {r.exitCode, r.stdOut, r.stdErr};
}

} // namespace camsyringe
