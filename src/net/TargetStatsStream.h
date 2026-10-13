#pragma once

#include "net/TargetStatsParser.h"

#include <QObject>
#include <QProcess>
#include <QString>

#include <memory>

namespace camsyringe {

class TargetSsh;

// Streams one camera's target-side logs to this PC's terminal while a session
// runs (docs/adr/0017): ONE long-lived `ssh ... tail -f` of the camera's
// injector and receiver logs, each line parsed by TargetStatsParser and
// printed on stderr as it arrives, in real time.
//
// GUI thread only, and non-blocking throughout (docs/adr/0004): start() and
// stop() never wait on the network or on the remote process.
class TargetStatsStream : public QObject {
    Q_OBJECT
public:
    TargetStatsStream(TargetSsh& ssh, QString target, int camId, QObject* parent = nullptr);
    ~TargetStatsStream() override;

    // False (silently) when ssh auth isn't already resolved for the target:
    // this is an informational extra and must never prompt for credentials.
    bool start();

    // Ends the stream. Kills the local ssh at once and, best effort, the
    // remote tail by its pid (read from the stream's first line), so no tail is
    // left running on the target after the session.
    void stop();

    TargetStatsSnapshot snapshot() const;

private:
    void onReadyRead();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void handleLine(const QString& line);

    TargetSsh& ssh_;
    QString target_;
    int camId_;
    TargetStatsParser parser_;
    QProcess* proc_ = nullptr;
    QString pending_;        // a partial line carried between reads
    qint64 remotePid_ = 0;   // the remote `exec tail`'s pid, from the first line
    bool stopping_ = false;
};

} // namespace camsyringe
