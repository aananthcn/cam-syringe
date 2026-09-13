#include "net/PingProbe.h"

#include <QProcess>
#include <QStringList>

namespace camsyringe {

bool quickPingUnreachable(const QString& host) {
    QStringList args;
    QString program;
#ifdef Q_OS_WIN
    program = host.contains(':') ? QStringLiteral("ping") : QStringLiteral("ping");
    args = host.contains(':') ? QStringList{"-6", "-n", "1", "-w", "1000", host}
                               : QStringList{"-n", "1", "-w", "1000", host};
#else
    // ping6 (not `ping -6`) is the portable form across the Linux distros
    // and macOS this project targets -- plain `ping` on an IPv6 literal
    // fails outright on some of them. -W is the per-reply deadline in
    // seconds on Linux; macOS/BSD ping6 uses -x (ms) instead, but lacking
    // it here just means a slower (not incorrect) fall-through on those
    // platforms, never a wrong answer -- see this function's own
    // "inconclusive falls through" contract above.
    program = host.contains(':') ? QStringLiteral("ping6") : QStringLiteral("ping");
    args = host.contains(':') ? QStringList{"-c", "1", "-W", "1", host}
                               : QStringList{"-c", "1", "-W", "1", host};
#endif
    QProcess proc;
    proc.start(program, args);
    if (!proc.waitForStarted(1000)) {
        return false; // ping/ping6 itself unavailable -- inconclusive
    }
    if (!proc.waitForFinished(2000)) {
        proc.kill();
        proc.waitForFinished(500);
        return false; // no clear answer within the bound -- inconclusive
    }
    return proc.exitCode() != 0;
}

} // namespace camsyringe
