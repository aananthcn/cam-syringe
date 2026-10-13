#include "net/TargetStatsStream.h"

#include "net/TargetSsh.h"

#include <QElapsedTimer>
#include <QProcessEnvironment>
#include <QTimer>

#include <cstdio>

namespace camsyringe {

namespace {
// Monotonic clock for the parser's rates.
int64_t nowMs() {
    static QElapsedTimer t = [] { QElapsedTimer x; x.start(); return x; }();
    return t.elapsed();
}
} // namespace

TargetStatsStream::TargetStatsStream(TargetSsh& ssh, QString target, int camId, QObject* parent)
    : QObject(parent), ssh_(ssh), target_(std::move(target)), camId_(camId), parser_(camId) {}

TargetStatsStream::~TargetStatsStream() {
    stop();
}

bool TargetStatsStream::start() {
    if (proc_) {
        return true;
    }
    const QString inj = QStringLiteral("/tmp/qcarcam_dispatcher_cam%1_injector.log").arg(camId_);
    const QString rcv = QStringLiteral("/tmp/qcarcam_dispatcher_cam%1_receiver.log").arg(camId_);
    // `exec tail` so the pid it prints IS the tail's (one simple command, no
    // backgrounding: docs/adr/0010). -n 0: only what happens from now on.
    const QString remote =
        QStringLiteral("sh -c 'echo PID $$; exec tail -n 0 -f %1 %2'").arg(inj, rcv);
    QStringList args;
    QProcessEnvironment env;
    if (!ssh_.sshInvocation(target_, remote, 3, &args, &env)) {
        return false;
    }
    proc_ = new QProcess(this);
    proc_->setProcessEnvironment(env);
    proc_->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc_, &QProcess::readyReadStandardOutput, this, &TargetStatsStream::onReadyRead);
    connect(proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            &TargetStatsStream::onFinished);
    stopping_ = false;
    proc_->start("ssh", args);
    proc_->closeWriteChannel();
    return true;
}

void TargetStatsStream::onReadyRead() {
    pending_ += QString::fromUtf8(proc_->readAllStandardOutput());
    int nl;
    while ((nl = pending_.indexOf('\n')) >= 0) {
        const QString line = pending_.left(nl);
        pending_.remove(0, nl + 1);
        handleLine(line);
    }
}

void TargetStatsStream::handleLine(const QString& line) {
    if (remotePid_ == 0 && line.startsWith(QStringLiteral("PID "))) {
        remotePid_ = line.mid(4).trimmed().toLongLong();
        return;
    }
    const QString out = parser_.feed(line, nowMs());
    if (!out.isEmpty()) {
        std::fprintf(stderr, "%s\n", out.toUtf8().constData());
    }
}

TargetStatsSnapshot TargetStatsStream::snapshot() const {
    return parser_.snapshot(nowMs());
}

void TargetStatsStream::onFinished(int exitCode, QProcess::ExitStatus) {
    if (!stopping_) {
        std::fprintf(stderr,
                     "target[cam%d]: live target statistics ended (ssh exit %d) -- the target "
                     "or the connection went away; streaming itself is not affected\n",
                     camId_, exitCode);
    }
}

void TargetStatsStream::stop() {
    if (!proc_) {
        return;
    }
    stopping_ = true;
    QProcess* p = proc_;
    proc_ = nullptr;
    p->disconnect(this);
    p->setParent(nullptr); // outlives this object, so its destructor never waits on the kill
    if (p->state() != QProcess::NotRunning) {
        p->kill(); // asynchronous, never waited for
    }
    // Freed when it has actually exited; if the application quits first it is
    // simply never destroyed, so Qt has no "destroyed while running" to warn about.
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), p,
            &QObject::deleteLater);
    connect(p, &QProcess::errorOccurred, p, &QObject::deleteLater);

    // Best effort: end the remote tail too, by the pid it announced. Without
    // this it lingers on the target until it next writes (and finds the
    // connection closed). A separate short ssh, itself bounded and discarded.
    if (remotePid_ > 0) {
        QStringList args;
        QProcessEnvironment env;
        if (ssh_.sshInvocation(target_, QStringLiteral("kill %1").arg(remotePid_), 3, &args,
                               &env)) {
            auto* killer = new QProcess();
            killer->setProcessEnvironment(env);
            killer->setProcessChannelMode(QProcess::MergedChannels);
            connect(killer, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                    killer, &QObject::deleteLater);
            connect(killer, &QProcess::errorOccurred, killer, &QObject::deleteLater);
            QTimer::singleShot(10000, killer, [killer] {
                if (killer->state() != QProcess::NotRunning) killer->kill();
            });
            killer->start("ssh", args);
            killer->closeWriteChannel();
        }
        remotePid_ = 0;
    }
}

} // namespace camsyringe
