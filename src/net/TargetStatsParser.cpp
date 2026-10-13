#include "net/TargetStatsParser.h"

#include <QRegularExpression>

#include <algorithm>

namespace camsyringe {

QString TargetStatsParser::feed(const QString& rawLine, int64_t nowMs) {
    const QString line = rawLine.trimmed();
    if (line.isEmpty() || line.startsWith(QStringLiteral("==>"))) {
        return QString(); // tail's own "==> file <==" header between two files
    }

    static const QRegularExpression kInjector(
        QStringLiteral(R"(shim-server: clients=(\d+) frames_read=(\d+) frames_delivered=(\d+))"));
    static const QRegularExpression kPublish(
        QStringLiteral(R"(publish interval avg=([\d.]+)ms)"));
    static const QRegularExpression kStream(
        QStringLiteral(R"(stream is (\d+x\d+), publishing at (\d+x\d+))"));

    static const QRegularExpression kClient(QStringLiteral(
        R"(shim-server: client (\d+) pid=(\d+) state=(\w+) mode=(\w+) buffers=\d+ delivered=\d+ )"
        R"(released=(\d+) requested=\d+ missed=(\d+) last_call_ms=(\d+))"));
    static const QRegularExpression kDetached(QStringLiteral(R"(shim-server: client (\d+) detached)"));

    if (auto m = kClient.match(line); m.hasMatch()) {
        AppClient& a = apps_[m.captured(1).toInt()];
        const uint64_t released = m.captured(5).toULongLong();
        if (a.seenMs > 0 && released >= a.released && nowMs > a.seenMs) {
            a.fps = static_cast<double>(released - a.released) /
                    (static_cast<double>(nowMs - a.seenMs) / 1000.0);
            a.haveRate = true;
        } else {
            a.haveRate = false; // first sample, or the counters restarted
        }
        a.pid = m.captured(2).toInt();
        a.state = m.captured(3);
        a.mode = m.captured(4);
        a.released = released;
        a.missed = m.captured(6).toULongLong();
        a.lastCallMs = m.captured(7).toULongLong();
        a.seenMs = nowMs > 0 ? nowMs : 1;
        return QString();
    }
    if (auto m = kDetached.match(line); m.hasMatch()) {
        apps_.erase(m.captured(1).toInt());
        return QString();
    }

    if (auto m = kInjector.match(line); m.hasMatch()) {
        clients_ = m.captured(1).toInt();
        const uint64_t read = m.captured(2).toULongLong();
        const uint64_t delivered = m.captured(3).toULongLong();
        const bool restarted = read < lastRead_ || delivered < lastDelivered_;
        if (haveInjectorSample_ && !restarted && nowMs > lastInjectorMs_) {
            const double secs = static_cast<double>(nowMs - lastInjectorMs_) / 1000.0;
            readFps_ = static_cast<double>(read - lastRead_) / secs;
            deliveredFps_ = static_cast<double>(delivered - lastDelivered_) / secs;
            shimMs_ = nowMs; // rates known only from the second sample on
        }
        haveInjectorSample_ = true;
        lastInjectorMs_ = nowMs;
        lastRead_ = read;
        lastDelivered_ = delivered;
        return QString();
    }

    if (auto m = kPublish.match(line); m.hasMatch()) {
        const double avgMs = m.captured(1).toDouble();
        if (avgMs > 0.0) {
            receiverFps_ = 1000.0 / avgMs;
            receiverMs_ = nowMs;
        }
        return QString();
    }

    if (auto m = kStream.match(line); m.hasMatch()) {
        streamSize_ = m.captured(2);
        return QString();
    }

    if (line.contains(QStringLiteral("Packet corrupt"))) {
        ++corruptTotal_;
        return QString();
    }

    // Anything else that says something went wrong on the target is shown as is.
    static const QRegularExpression kProblem(
        QStringLiteral(R"(fail|error|cannot|can't|refus|timeout|timed out|no such file)"),
        QRegularExpression::CaseInsensitiveOption);
    if (kProblem.match(line).hasMatch()) {
        return prefix() + (line.size() > 200 ? line.left(200) + QStringLiteral("...") : line);
    }
    return QString();
}

TargetStatsSnapshot TargetStatsParser::snapshot(int64_t nowMs) const {
    constexpr int64_t kStaleMs = 15000; // the logs report every ~2 s
    TargetStatsSnapshot s;
    s.camId = camId_;
    s.haveReceiver = receiverMs_ >= 0 && nowMs - receiverMs_ <= kStaleMs;
    s.receiverFps = receiverFps_;
    s.streamSize = streamSize_;
    s.haveShim = shimMs_ >= 0 && nowMs - shimMs_ <= kStaleMs;
    s.clients = clients_;
    s.readFps = readFps_;
    s.deliveredFps = deliveredFps_;
    s.totalRead = lastRead_;
    s.totalDelivered = lastDelivered_;
    s.corruptTotal = corruptTotal_;
    bool first = true;
    bool allRates = true;
    uint64_t lastCall = UINT64_MAX;
    for (const auto& kv : apps_) {
        const AppClient& a = kv.second;
        if (nowMs - a.seenMs > kStaleMs) {
            continue; // stopped reporting: detached, or the injector is gone
        }
        ++s.appCount;
        if (first) {
            s.appPid = a.pid;
            s.appState = a.state;
            s.appMode = a.mode;
            first = false;
        }
        allRates = allRates && a.haveRate;
        s.appFps += a.fps;
        s.appMissed += a.missed;
        lastCall = std::min(lastCall, a.lastCallMs + static_cast<uint64_t>(nowMs - a.seenMs));
    }
    s.haveApp = s.appCount > 0;
    s.haveAppRate = s.haveApp && allRates;
    s.appLastCallMs = s.haveApp ? lastCall : 0;
    return s;
}

QString formatTargetStatsReport(const std::vector<TargetStatsSnapshot>& cams, double windowSecs) {
    auto fps = [](bool have, double v) {
        return have ? QString::number(v, 'f', 1) : QStringLiteral("-");
    };
    auto num = [](bool have, uint64_t v) {
        return have ? QString::number(v) : QStringLiteral("-");
    };
    QString out;
    out += QStringLiteral("---------------- CamSyringe injection report (%1 sec) ----------------\n")
               .arg(windowSecs, 0, 'f', 1);
    out += QStringLiteral("| cid | state              | rx fps | size      | read fps | deliv fps | "
                          "app pid | mode    | app fps | missed | last call | corrupt |\n");
    for (const auto& c : cams) {
        QString state;
        if (!c.haveReceiver) {
            state = QStringLiteral("no data");
        } else if (c.haveApp) {
            if (c.appState == QStringLiteral("stopped")) {
                state = QStringLiteral("app not started");
            } else if (c.appState == QStringLiteral("paused")) {
                state = QStringLiteral("app paused");
            } else if (c.appLastCallMs > 3000) {
                state = QStringLiteral("APP STALLED");
            } else if (c.haveAppRate && c.appFps <= 0.0) {
                state = QStringLiteral("APP NOT CONSUMING");
            } else {
                state = QStringLiteral("injecting");
            }
        } else if (!c.haveShim) {
            state = QStringLiteral("receiving");
        } else if (c.clients == 0) {
            state = QStringLiteral("idle (no app)");
        } else {
            state = QStringLiteral("app attached");
        }
        const QString pid = c.haveApp
            ? QString::number(c.appPid) + (c.appCount > 1 ? QStringLiteral("+%1").arg(c.appCount - 1) : QString())
            : QStringLiteral("-");
        out += QStringLiteral("| %1 | %2 | %3 | %4 | %5 | %6 | %7 | %8 | %9 | %10 | %11 | %12 |\n")
                   .arg(c.camId, 3)
                   .arg(state, -18)
                   .arg(fps(c.haveReceiver, c.receiverFps), 6)
                   .arg(c.streamSize.isEmpty() ? QStringLiteral("-") : c.streamSize, -9)
                   .arg(fps(c.haveShim, c.readFps), 8)
                   .arg(fps(c.haveShim && c.clients > 0, c.deliveredFps), 9)
                   .arg(pid, 7)
                   .arg(c.haveApp ? c.appMode : QStringLiteral("-"), -7)
                   .arg(fps(c.haveAppRate, c.appFps), 7)
                   .arg(num(c.haveApp, c.appMissed), 6)
                   .arg(c.haveApp ? QStringLiteral("%1 ms").arg(c.appLastCallMs) : QStringLiteral("-"), 9)
                   .arg(c.corruptTotal, 7);
    }
    return out;
}

} // namespace camsyringe
