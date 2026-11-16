#include "net/InstallLog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QSysInfo>

#include <cstdio>
#include <mutex>

namespace camsyringe {

void installLog(const QString& tag, const QString& text) {
    static std::mutex mu;
    const QString line = QString("[%1 %2] %3\n")
                             .arg(tag, QDateTime::currentDateTime().toString("HH:mm:ss.zzz"), text);
    std::lock_guard<std::mutex> lock(mu);
    std::fputs(qPrintable(line), stderr);
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (base.isEmpty()) return;
    const QString dir = base + "/camsyringe";
    QDir().mkpath(dir);
    QFile f(dir + "/install.log");
    if (f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        f.write(QDateTime::currentDateTime().toString("yyyy-MM-dd ").toUtf8() + line.toUtf8());
    }
}

namespace {

QString capture(const QString& program, const QStringList& args) {
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(program, args);
    if (!p.waitForStarted(3000) || !p.waitForFinished(5000)) {
        p.kill();
        return QString("(could not run %1)").arg(program);
    }
    return QString::fromUtf8(p.readAll()).trimmed().replace('\n', " | ");
}

QString readFirstLine(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readLine()).trimmed() : QString();
}

} // namespace

QString describePcEnvironment() {
    const QString procVersion = readFirstLine("/proc/version");
    const bool wsl = procVersion.contains("microsoft", Qt::CaseInsensitive) ||
                     procVersion.contains("WSL", Qt::CaseInsensitive);
    QString out;
    out += QString("pc: %1, kernel %2 %3, wsl=%4\n")
               .arg(QSysInfo::prettyProductName(), QSysInfo::kernelType(), QSysInfo::kernelVersion(),
                    wsl ? "yes" : "no");
    if (!procVersion.isEmpty()) out += "pc: /proc/version: " + procVersion + "\n";
    out += QString("pc: user=%1 home=%2 ssh-agent=%3\n")
               .arg(qEnvironmentVariable("USER"), QDir::homePath(),
                    qEnvironmentVariableIsSet("SSH_AUTH_SOCK") ? "set" : "unset");
    out += "pc: ssh -V: " + capture("ssh", {"-V"}) + "\n";
    out += "pc: ssh path: " + QStandardPaths::findExecutable("ssh") + "\n";
    out += "pc: scp path: " + QStandardPaths::findExecutable("scp") + "\n";
    return out.trimmed();
}

} // namespace camsyringe
