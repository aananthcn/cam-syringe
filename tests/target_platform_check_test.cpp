// Host test of the target-platform check (docs/adr/0018):
//   g++ -std=c++17 -fPIC -Isrc $(pkg-config --cflags Qt6Core) tests/target_platform_check_test.cpp
//       src/net/QcxVersionProbe.cpp src/net/TargetSsh.cpp src/net/PingProbe.cpp -o /tmp/tpc $(pkg-config --libs Qt6Core) && /tmp/tpc   (one command)
// The two functions tested here are pure; TargetSsh.cpp is only there to satisfy the linker.
#include "net/QcxVersionProbe.h"

#include <cstdio>

using namespace camsyringe;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    QcxVersionProbe::Result r;
    QcxVersionProbe::classifyOs("QNX 8.0.0\n", &r);
    CHECK(r.osRelease == "QNX 8.0.0"); CHECK(!r.unsupportedOs);

    QcxVersionProbe::Result r71;
    QcxVersionProbe::classifyOs("QNX 7.1.0\n", &r71);
    CHECK(r71.osRelease == "QNX 7.1.0"); CHECK(r71.unsupportedOs);

    QcxVersionProbe::Result other;   // not QNX / unreadable: never blocked
    QcxVersionProbe::classifyOs("Linux 6.1\n", &other);
    CHECK(other.osRelease.isEmpty()); CHECK(!other.unsupportedOs);
    QcxVersionProbe::classifyOs("", &other);
    CHECK(!other.unsupportedOs);

    // the loader text from the 0.11 bench logs
    const QString bad =
        "unknown symbol: _ZN8CameraDB11GetInstanceEv referenced from libqcxplatform.so\n"
        "ldd:FATAL: Could not resolve all symbols \n";
    const QString err = QcxVersionProbe::findLibraryError(bad);
    CHECK(err.contains("CameraDB")); CHECK(err.contains("FATAL"));
    CHECK(QcxVersionProbe::findLibraryError("[shim] input probe: ok\nattached to frame ring\n").isEmpty());

    std::printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
    return fails;
}
