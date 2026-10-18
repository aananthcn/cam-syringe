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
    // Not `tail -f`: the v14.4.1 BSP's tail fails with "xnotify_add failed ...
    // Function not implemented", even on a plain file in /tmp. So poll instead:
    // once a second, count each log's complete lines (wc -l) and print the new
    // ones (sed). Only what is written from now on is printed (the counts are
    // primed first, output discarded), a file that doesn't exist yet counts as
    // 0, and a log that shrinks (the dispatcher truncates it when it respawns the
    // injector) is read again from the top. Whole lines only, so two files'
    // output never interleaves mid-line. The pid it prints is the shell's own
    // (`$$`), so Stop's `kill <pid>` ends it; its `sleep` child exits within a
    // second. The receiver prints the stream size once, when it opens the stream, which
    // is before READY and so before this starts; so the last such line is printed once
    // up front (else the report's size column stays "-"). One simple command to the outer shell, no backgrounding (docs/adr/0010);
    // checked in the board's own ksh, not just bash.
    const QString script = QStringLiteral(
        "echo PID $$; "
        "poll() { eval o=\\${$2}; n=0; if [ -r \"$1\" ]; then n=`wc -l < \"$1\"`; fi; "
        "n=$(($n+0)); if [ $n -lt $o ]; then o=0; fi; "
        "if [ $n -gt $o ]; then sed -n \"$((o+1)),${n}p\" \"$1\"; fi; eval $2=$n; }; "
        "A=%1; B=%2; oa=0; ob=0; poll \"$A\" oa >/dev/null; poll \"$B\" ob >/dev/null; "
        "if [ -r \"$B\" ]; then grep \"FrameReceiver: stream is\" \"$B\" | tail -n 1; fi; "
        "while :; do poll \"$A\" oa; poll \"$B\" ob; sleep 1; done");
    const QString remote = QStringLiteral("sh -c '%1'").arg(script.arg(inj, rcv));
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
