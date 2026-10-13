// Host test of the terminal statistics lines (docs/adr/0017). Lines are copied from real target logs.
//   g++ -std=c++17 -fPIC -Isrc $(pkg-config --cflags Qt6Core) tests/target_stats_parser_test.cpp
//       src/net/TargetStatsParser.cpp -o /tmp/tsp $(pkg-config --libs Qt6Core) && /tmp/tsp   (one command)
#include "net/TargetStatsParser.h"

#include <cstdio>

using namespace camsyringe;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
#define SHOW(s) std::printf("   %s\n", (s).toUtf8().constData())

int main() {
    TargetStatsParser p(2);
    // noise and tail's headers print nothing, and nothing is known yet
    CHECK(p.feed("", 0).isEmpty());
    CHECK(p.feed("==> /tmp/qcarcam_dispatcher_cam2_injector.log <==", 0).isEmpty());
    CHECK(p.feed("[qcarcam-receiver] TIMING PROBE: packet size avg=131161B max=283645B (n=742 since decoder open)", 0).isEmpty());
    TargetStatsSnapshot s0 = p.snapshot(0);
    CHECK(!s0.haveReceiver); CHECK(!s0.haveShim);

    // injector: the first sample gives totals only, the second gives rates (2 s, 60 read, 58 delivered)
    CHECK(p.feed("[qcarcam-injector] shim-server: clients=1 frames_read=100 frames_delivered=100", 4000).isEmpty());
    CHECK(!p.snapshot(4000).haveShim);
    CHECK(p.feed("[qcarcam-injector] shim-server: clients=1 frames_read=160 frames_delivered=158", 6000).isEmpty());
    TargetStatsSnapshot s1 = p.snapshot(6000);
    CHECK(s1.haveShim); CHECK(s1.clients == 1);
    CHECK(s1.readFps > 29.9 && s1.readFps < 30.1); CHECK(s1.deliveredFps > 28.9 && s1.deliveredFps < 29.1);
    CHECK(s1.totalRead == 160); CHECK(s1.totalDelivered == 158);
    // counters going backwards (injector restarted): no negative rate
    p.feed("[qcarcam-injector] shim-server: clients=1 frames_read=5 frames_delivered=5", 8000);
    CHECK(p.snapshot(8000).readFps > 0.0); CHECK(p.snapshot(8000).totalRead == 5);

    // receiver: fps from the average publish interval, size from the stream line, corrupt counted
    p.feed("[qcarcam-receiver] TIMING PROBE: publish interval avg=33.33ms min=5.03ms max=72.99ms (n=60) -- real SOURCE-side", 9000);
    p.feed("[mpegts @ 53a5b1f540] Packet corrupt (stream = 0, dts = 6000), dropping it.", 9100);
    p.feed("[mpegts @ 53a5b1f540] Packet corrupt (stream = 0, dts = 15000), dropping it.", 9200);
    p.feed("FrameReceiver: stream is 1920x1080, publishing at 1920x1080 -- plane0 stride=1920", 9300);
    TargetStatsSnapshot s2 = p.snapshot(9400);
    CHECK(s2.haveReceiver); CHECK(s2.receiverFps > 29.9 && s2.receiverFps < 30.1);
    CHECK(s2.streamSize == "1920x1080"); CHECK(s2.corruptTotal == 2);

    // numbers from a source that has gone silent are not shown as current
    CHECK(!p.snapshot(9000 + 20000).haveReceiver); CHECK(!p.snapshot(8000 + 20000).haveShim);

    // a failure on the target is still printed at once
    QString i = p.feed("HwVideoDecoder: START(output) failed", 15000);
    SHOW(i); CHECK(i.startsWith("target[cam2]: HwVideoDecoder: START(output) failed"));

    // per-application lines from the injector (qcarcam-injector docs/adr/0023)
    TargetStatsParser q(2);
    CHECK(!q.snapshot(0).haveApp);
    q.feed("[qcarcam-injector] shim-server: client 1 pid=312 state=started mode=classic buffers=4 delivered=100 released=98 requested=0 missed=1 last_call_ms=12", 1000);
    TargetStatsSnapshot a1 = q.snapshot(1000);
    CHECK(a1.haveApp); CHECK(a1.appCount == 1); CHECK(a1.appPid == 312); CHECK(a1.appMode == "classic");
    CHECK(!a1.haveAppRate);   // one sample is not a rate
    q.feed("[qcarcam-injector] shim-server: client 1 pid=312 state=started mode=classic buffers=4 delivered=250 released=248 requested=0 missed=2 last_call_ms=20", 6000);
    TargetStatsSnapshot a2 = q.snapshot(6000);
    CHECK(a2.haveAppRate); CHECK(a2.appFps > 29.9 && a2.appFps < 30.1); CHECK(a2.appMissed == 2); CHECK(a2.appLastCallMs == 20);
    // a detached application disappears; a silent one ages out
    q.feed("[qcarcam-injector] shim-server: client 1 detached (goodbye; 250 frames delivered)", 7000);
    CHECK(!q.snapshot(7000).haveApp);
    q.feed("[qcarcam-injector] shim-server: client 2 pid=9 state=stopped mode=request buffers=0 delivered=0 released=0 requested=0 missed=0 last_call_ms=5", 7000);
    CHECK(q.snapshot(7000).appState == "stopped"); CHECK(!q.snapshot(7000 + 20000).haveApp);

    // the report table
    TargetStatsSnapshot idle; idle.camId = 8; idle.haveReceiver = true; idle.receiverFps = 30.1;
    idle.haveShim = true; idle.clients = 0; idle.readFps = 30.0; idle.totalRead = 900; idle.streamSize = "1920x1080";
    TargetStatsSnapshot gone; gone.camId = 9;
    TargetStatsSnapshot ok = a2; ok.camId = 2; ok.haveReceiver = true; ok.receiverFps = 30.0; ok.streamSize = "1920x1080";
    TargetStatsSnapshot stalled = ok; stalled.camId = 3; stalled.appLastCallMs = 9000;
    TargetStatsSnapshot starving = ok; starving.camId = 4; starving.appFps = 0.0;
    QString table = formatTargetStatsReport({ok, stalled, starving, idle, gone}, 5.0);
    std::printf("%s", table.toUtf8().constData());
    CHECK(table.contains("injecting")); CHECK(table.contains("idle (no app)")); CHECK(table.contains("no data"));
    CHECK(table.contains("APP STALLED")); CHECK(table.contains("APP NOT CONSUMING"));
    CHECK(table.contains("CamSyringe injection report (5.0 sec)"));

    std::printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
    return fails;
}
