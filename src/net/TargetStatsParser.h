#pragma once

#include <QString>

#include <cstdint>
#include <map>
#include <vector>

namespace camsyringe {

// Turns lines from the target's own per-camera logs (qcarcam_injector's
// /tmp/qcarcam_dispatcher_cam<id>_injector.log and qcarcam_receiver's
// ..._receiver.log) into the short, human-readable status lines printed on
// the PC terminal (docs/adr/0017). Pure: no I/O, no clock -- the caller passes
// the arrival time, so it can be tested with canned lines.
//
// What each source says, as confirmed on the bench target:
//   injector  "shim-server: clients=N frames_read=R frames_delivered=D"
//             clients   = applications attached to the SHIM for this camera,
//             frames_read      = frames the injector took from the ring,
//             frames_delivered = frames handed on to an application.
//   receiver  "TIMING PROBE: publish interval avg=33.33ms ..." = how fast the
//             decoded frames are published into the ring (the rate actually
//             received from the PC and decoded on the target),
//             "FrameReceiver: stream is WxH, publishing at WxH",
//             "Packet corrupt ..." for damaged input.
// The latest numbers for one camera, as shown in the periodic report.
struct TargetStatsSnapshot {
    int camId = 0;
    bool haveReceiver = false;   // a publish-rate line arrived recently
    double receiverFps = 0.0;    // frames/s decoded on the target and published into the ring
    QString streamSize;          // "1920x1080", "" until the receiver says
    bool haveShim = false;       // an injector counter line arrived recently
    int clients = 0;             // applications attached to the SHIM
    double readFps = 0.0;        // injector reading from the ring
    double deliveredFps = 0.0;   // handed on to applications
    uint64_t totalRead = 0;
    uint64_t totalDelivered = 0;
    uint64_t corruptTotal = 0;
    // What the attached application(s) did through the SHIM, from the injector's per-client
    // lines (qcarcam-injector docs/adr/0023). haveApp is false with an older injector.
    bool haveApp = false;
    int appCount = 0;
    int appPid = 0;               // the first application's pid
    QString appState;             // "started", "paused" or "stopped" (first application)
    QString appMode;              // "classic" or "request"
    bool haveAppRate = false;     // two samples seen, so appFps is meaningful
    double appFps = 0.0;          // frames/s the applications handed back (ReleaseFrame)
    uint64_t appMissed = 0;
    uint64_t appLastCallMs = 0;   // the most recent call from any application
};

// One table, in the style of qcarcam_test's "FPS Report", for all cameras.
QString formatTargetStatsReport(const std::vector<TargetStatsSnapshot>& cams, double windowSecs);

class TargetStatsParser {
public:
    explicit TargetStatsParser(int camId) : camId_(camId) {}

    // Absorbs one log line into the running numbers. Returns a line to print
    // at once only for something that went wrong on the target (empty
    // otherwise): the steady numbers go in the periodic report instead.
    // `nowMs` is any monotonic millisecond clock, used for the injector's
    // rates and to age out stale numbers.
    QString feed(const QString& line, int64_t nowMs);

    // The numbers as of `nowMs`; a source that has been silent for a few
    // seconds reads as absent rather than showing its last value.
    TargetStatsSnapshot snapshot(int64_t nowMs) const;

private:
    QString prefix() const { return QStringLiteral("target[cam%1]: ").arg(camId_); }

    int camId_;
    bool haveInjectorSample_ = false;
    int64_t lastInjectorMs_ = 0;
    uint64_t lastRead_ = 0;
    uint64_t lastDelivered_ = 0;
    uint64_t corruptTotal_ = 0;
    double receiverFps_ = 0.0;
    int64_t receiverMs_ = -1;
    QString streamSize_;
    int clients_ = 0;
    double readFps_ = 0.0;
    double deliveredFps_ = 0.0;
    int64_t shimMs_ = -1;

    struct AppClient {
        int pid = 0;
        QString state, mode;
        uint64_t released = 0, missed = 0, lastCallMs = 0;
        double fps = 0.0;
        bool haveRate = false;
        int64_t seenMs = 0;
    };
    std::map<int, AppClient> apps_;
};

} // namespace camsyringe
