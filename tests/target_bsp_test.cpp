// Host test of the BSP check (docs/adr/0020):
//   g++ -std=c++17 -fPIC -Isrc $(pkg-config --cflags Qt6Core) tests/target_bsp_test.cpp src/net/TargetBsp.cpp
//       -o /tmp/bsp $(pkg-config --libs Qt6Core) && /tmp/bsp
#include "net/TargetBsp.h"

#include <cstdio>

using namespace camsyringe;
using V = TargetBsp::Verdict;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static QString osRelease(const char* versionId) {
    return QString("NAME=\"CARIAD OneBSP\"\nPRETTY_NAME=\"CARIAD OneBSP %1\"\nVARIANT=monaco_qdrive\n"
                   "VERSION=%1-CS1.1-r00005.1\nVERSION_ID=%1\nBUILD_ID=\"2026_09_17-20_52\"\n"
                   "QNX_SDK_VERSION=QNX-patched-EXP_QHS80_20260305\n").arg(versionId);
}

int main() {
    // the two boards from the 0.12.2 bug report
    TargetBsp bench = TargetBsp::classify(osRelease("v14.4.1"));
    CHECK(bench.verdict == V::Verified); CHECK(bench.versionId == "v14.4.1");
    CHECK(bench.prettyName == "CARIAD OneBSP v14.4.1"); CHECK(bench.buildId == "2026_09_17-20_52");

    TargetBsp old = TargetBsp::classify(osRelease("v14.2.6"));
    CHECK(old.verdict == V::TooOld);
    CHECK(old.message.contains("v14.2.6")); CHECK(old.message.contains("v14.4.1"));
    CHECK(old.message.contains("Nothing was changed")); CHECK(old.message.contains("CAMSYRINGE_ALLOW_UNSUPPORTED_BSP"));

    CHECK(TargetBsp::classify(osRelease("v14.3.9")).verdict == V::TooOld);
    CHECK(TargetBsp::classify(osRelease("v13.9.9")).verdict == V::TooOld);
    CHECK(TargetBsp::classify(osRelease("v14.4.0")).verdict == V::TooOld);
    CHECK(TargetBsp::classify(osRelease("v14.4.2")).verdict == V::Untested);
    CHECK(TargetBsp::classify(osRelease("v14.10.0")).verdict == V::Untested);   // numeric, not text, compare
    CHECK(TargetBsp::classify(osRelease("v15.0.0")).verdict == V::Untested);
    CHECK(TargetBsp::classify(osRelease("14.4.1")).verdict == V::Verified);     // no leading v

    // not blocked when it can't be judged
    CHECK(TargetBsp::classify("").verdict == V::Unknown);
    CHECK(TargetBsp::classify("cat: /etc/os-release: No such file or directory\n").verdict == V::Unknown);
    CHECK(TargetBsp::classify("VERSION_ID=banana\n").verdict == V::Unknown);
    CHECK(TargetBsp::classify("VERSION_ID=\n").verdict == V::Unknown);

    // text after os-release (the installer appends `uname -a`) and CRLF don't matter
    CHECK(TargetBsp::classify(osRelease("v14.2.6") + "QNX localhost 8.0.0 aarch64le\n").verdict == V::TooOld);
    CHECK(TargetBsp::classify("VERSION_ID=v14.4.1\r\nPRETTY_NAME=x\r\n").verdict == V::Verified);

    std::printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
    return fails;
}
