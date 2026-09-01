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
                            const CredentialsCallback& credentials) {
    Auth cached;
    if (lookupAuth(target, &cached)) {
        return true;
    }

    Auth auth;
    auth.user = defaultUser.isEmpty() ? QStringLiteral("root") : defaultUser;
    auth.env = QProcessEnvironment::systemEnvironment();

    // Passwordless probe -- BatchMode=yes here ONLY (it globally disables
    // password/askpass querying too, so it must never be set once we're
    // in password-auth mode below).
    QStringList probeArgs = commonSshOpts();
    probeArgs << "-o"
              << "BatchMode=yes" << (auth.user + "@" + target) << "true";
    RunResult probe = runProcess("ssh", probeArgs, auth.env, 8000);

    if (probe.exitCode != 0) {
        QString username = auth.user, password;
        if (!credentials(target, &username, &password)) {
            return false;
        }
        auth.user = username;
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

TargetSsh::Result TargetSsh::run(const QString& target, const QString& remoteCommand,
                                  int timeoutMs) {
    Auth auth;
    if (!lookupAuth(target, &auth)) {
        return {-1, QString(), "ensureAuth() was not called (or failed) for " + target};
    }
    QStringList args = commonSshOpts();
    args << (auth.user + "@" + target) << remoteCommand;
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
    args << localPath << (auth.user + "@" + target + ":" + remotePath);
    RunResult r = runProcess("scp", args, auth.env, timeoutMs);
    return {r.exitCode, r.stdOut, r.stdErr};
}

} // namespace camsyringe
