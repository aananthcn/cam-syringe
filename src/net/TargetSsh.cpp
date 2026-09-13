#include "net/TargetSsh.h"

#include "net/PingProbe.h"

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
// this whole file works in QString throughout. IMPORTANT: this is an
// scp()-ONLY helper, not a general ssh-destination one -- confirmed via
// `ssh -G user@[ipv6]` vs `ssh -G user@ipv6` (OpenSSH 10.2): scp's
// legacy `user@host:path` syntax genuinely needs the brackets to tell
// an IPv6 literal's own colons apart from the host:path separator, but
// plain ssh's `user@host` destination (no trailing `:port`) does NOT --
// OpenSSH only strips the brackets back off when a `:port` follows
// them, so `ssh user@[fd53::1]` with no port resolves the LITERAL
// string "[fd53::1]" as a hostname and fails with "Could not resolve
// hostname [fd53::1]", while `ssh user@fd53::1` (unbracketed) resolves
// fine -- ssh's destination parser splits only on the first `@`, never
// on colons, so no bracketing is needed there at all. Bitten by this
// for real: run()/ensureAuth() used to call this too and broke every
// IPv6 target. Deliberately never applied to `target` itself where
// it's used as the auth cache key (authByTarget_) -- only to the text
// actually handed to the scp process.
QString bracketIfIPv6(const QString& host) { return host.contains(':') ? "[" + host + "]" : host; }

// Real-world trigger, not a hypothetical: this project's own QNX target
// regenerates its SSH host key on every reboot/reflash (confirmed
// repeatedly, live, this session), so `known_hosts` accumulating a STALE
// entry for the SAME IP is an expected, routine occurrence here, not a
// meaningful security signal -- unlike a genuinely unexpected host-key
// change against, say, a real production server. Before this, a stale
// entry made ensureAuth()'s own passwordless probe fail for a reason
// that has NOTHING to do with credentials, then fall through to
// prompting the user for a username/password that could never actually
// work either (password auth doesn't bypass host-key verification) --
// confirmed as a real, reported bad user experience: CamSyringe asked
// for credentials while the actual fix (`ssh-keygen -R <target>`) needed
// a terminal the user was trying to avoid. `StrictHostKeyChecking=
// accept-new` (see commonSshOpts()) already covers the OTHER case (an
// entirely new/unknown host) automatically; this covers the CHANGED
// case the same way, scoped narrowly to this exact message so a
// genuinely different ssh failure still falls through to the normal
// credentials prompt unchanged.
bool looksLikeHostKeyChanged(const QString& stderrText) {
    return stderrText.contains("REMOTE HOST IDENTIFICATION HAS CHANGED") ||
           stderrText.contains("Host key verification failed");
}

// `ssh-keygen -R` matches by the exact host TEXT as connected to (see
// bracketIfIPv6's own comment on why `target` itself is passed here
// unbracketed) -- best-effort: if this itself fails for some reason
// (e.g. no known_hosts file yet), the retry right after this call
// naturally falls through to the normal credentials prompt just like
// before this fix existed, so there's no new failure mode to handle.
void removeStaleHostKey(const QString& target) {
    QProcess::execute("ssh-keygen", {"-R", target});
}

// Shared by ensureAuth()'s own probe, run(), and scp() -- any of the
// three can hit a stale known_hosts entry (the target can reboot/reflash
// AFTER a session's initial ensureAuth() already succeeded, not just
// before it), so all three get the same one-retry recovery rather than
// only the first.
RunResult runWithHostKeyRetry(const QString& program, const QStringList& args,
                               const QProcessEnvironment& env, int timeoutMs, const QString& target) {
    RunResult r = runProcess(program, args, env, timeoutMs);
    if (r.exitCode != 0 && looksLikeHostKeyChanged(r.stdErr)) {
        removeStaleHostKey(target);
        r = runProcess(program, args, env, timeoutMs);
    }
    return r;
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
                            const QString& defaultKeyPath, const CredentialsCallback& credentials) {
    Auth cached;
    if (lookupAuth(target, &cached)) {
        return true;
    }

    Auth auth;
    auth.user = defaultUser.isEmpty() ? QStringLiteral("root") : defaultUser;
    auth.env = QProcessEnvironment::systemEnvironment();

    // Skip the passwordless probe entirely if it very recently failed for
    // this exact target -- see passwordlessProbeFailedUntil_'s own
    // comment for why (confirmed live: this is what turned one
    // unreachable target into a 20-30s "stuck" UI, several independent
    // features each separately re-timing-out the same probe).
    int probeExitCode = -1;
    bool skipProbe;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = passwordlessProbeFailedUntil_.find(target);
        skipProbe = it != passwordlessProbeFailedUntil_.end() &&
                    std::chrono::steady_clock::now() < it->second;
    }

    // Fast-fail pre-check (see quickPingUnreachable()'s own comment) --
    // only ever turns "going to fail anyway" into a faster failure, never
    // skips the real probe just because ping succeeded.
    if (!skipProbe && quickPingUnreachable(target)) {
        skipProbe = true; // falls straight into the credentials branch below
        std::lock_guard<std::mutex> lock(mutex_);
        passwordlessProbeFailedUntil_[target] = std::chrono::steady_clock::now() +
                                                 std::chrono::milliseconds(kPasswordlessProbeFailureTtlMs);
    }

    if (!skipProbe) {
        // Passwordless probe -- BatchMode=yes here ONLY (it globally
        // disables password/askpass querying too, so it must never be
        // set once we're in password-auth mode below). Covers BOTH "no
        // key needed at all" (default agent/identity already trusted)
        // and "use this specific key" (defaultKeyPath set) -- same probe
        // either way, see keyOpts()'s own comment.
        // No bracketIfIPv6 here -- see that function's own comment:
        // plain ssh destinations must NOT be bracketed (only scp's
        // legacy path syntax needs it), or an IPv6 target fails hostname
        // resolution.
        QStringList probeArgs = commonSshOpts();
        probeArgs << keyOpts(defaultKeyPath) << "-o"
                  << "BatchMode=yes" << (auth.user + "@" + target) << "true";
        // See looksLikeHostKeyChanged()'s own comment: a stale
        // known_hosts entry for this target (routine after a
        // reboot/reflash on this project) makes the probe fail for a
        // reason credentials can never fix -- clear it and retry ONCE
        // before falling through to prompting for a username/password
        // that would just hit the same wall.
        RunResult probe = runWithHostKeyRetry("ssh", probeArgs, auth.env, 8000, target);
        probeExitCode = probe.exitCode;
        if (probeExitCode != 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            passwordlessProbeFailedUntil_[target] =
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(kPasswordlessProbeFailureTtlMs);
        }
    }

    if (probeExitCode == 0) {
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
    // No bracketIfIPv6 here either -- see its own comment (scp()-only).
    QStringList args = commonSshOpts();
    args << keyOpts(auth.keyPath) << (auth.user + "@" + target) << remoteCommand;
    RunResult r = runWithHostKeyRetry("ssh", args, auth.env, timeoutMs, target);
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
    RunResult r = runWithHostKeyRetry("scp", args, auth.env, timeoutMs, target);
    return {r.exitCode, r.stdOut, r.stdErr};
}

} // namespace camsyringe
