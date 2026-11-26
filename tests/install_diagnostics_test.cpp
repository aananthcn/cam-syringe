// Host test of the on-target install diagnostics (docs/adr/0020): renders the script against a
// fake /var/opt that fails the way the 0.12.2 bug-report board did, runs it under /bin/sh, and
// checks the log says WHICH file/symbol and WHAT kind of failure.
//   g++ -std=c++17 -fPIC -Isrc $(pkg-config --cflags Qt6Core) tests/install_diagnostics_test.cpp
//       src/net/TargetDiagnostics.cpp src/net/TargetSsh.cpp src/net/PingProbe.cpp src/net/InstallLog.cpp
//       -o /tmp/diag $(pkg-config --libs Qt6Core) && /tmp/diag
#include "net/TargetDiagnostics.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

#include <cstdio>

using namespace camsyringe;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static void put(const QString& path, const QByteArray& data, bool exec = false) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(data);
    f.close();
    if (exec) f.setPermissions(f.permissions() | QFileDevice::ExeOwner);
}

int main() {
    QTemporaryDir tmp;
    const QString d = tmp.path();
    const QString bundle = "qcarcam_injector_bundle_v0.12.0.bin";

    put(d + "/tmp/" + bundle, "#!/bin/sh\n# BUILD_ID=58c95caea51c\n# INSTALL_DIR=/var/opt\n");
    put(d + "/BUILD_ID", "58c95caea51c\n");
    put(d + "/bin/qcarcam_dispatcher", "#!/bin/sh\necho usage\n", true);
    // the failure from the bug report: the loader's own text, exit non-zero
    put(d + "/bin/qcarcam_injector",
        "#!/bin/sh\necho 'unknown symbol: cache_init referenced from qcarcam_injector'\n"
        "echo 'ldd:FATAL: Could not resolve all symbols'\nexit 1\n", true);
    // a missing library
    put(d + "/bin/qcarcam_receiver",
        "#!/bin/sh\necho 'ldd:FATAL: Could not open library libnotthere.so.7'\nexit 1\n", true);
    // qcarcam_viewer is not installed at all
    put(d + "/lib/libold.so.1", "...nothing relevant...");
    put(d + "/lib/libref.so.1", "binary cache_init-and-friends");   // name present: a reference, maybe
    // manifest: one ok-by-presence file, one missing, one mismatching
    put(d + "/MANIFEST.md5",
        "# manifest\n"
        "00000000000000000000000000000000 bin/qcarcam_dispatcher\n"
        "00000000000000000000000000000000 lib/gone.so\n");

    QString script = TargetDiagnostics::script(d, bundle);
    // the fake tree stands in for /var/opt; the real /ifs and /mnt directories are absent here too
    CHECK(script.contains(d)); CHECK(!script.contains("@DIR@")); CHECK(!script.contains("@BUNDLE@"));
    CHECK(!script.contains("printf"));   // the target has no printf binary

    QProcess sh;
    sh.start("/bin/sh", {"-c", script});
    CHECK(sh.waitForFinished(30000));
    CHECK(sh.exitCode() == 0);
    CHECK(sh.readAllStandardError().isEmpty());   // everything goes to the log, nothing leaks

    QFile logf(d + "/tmp/" + TargetDiagnostics::kLogName);
    CHECK(logf.open(QIODevice::ReadOnly));
    const QString log = QString::fromUtf8(logf.readAll());
    std::printf("%s\n", qPrintable(log));

    CHECK(log.contains("58c95caea51c"));                                  // bundle build id
    CHECK(log.contains("MISSING   lib/gone.so"));                         // manifest vs disk
    CHECK(log.contains("SUMMARY: installed file " + d + "/lib/gone.so is missing"));
    CHECK(log.contains("unknown symbol: cache_init referenced from qcarcam_injector"));
    CHECK(log.contains("SUMMARY: qcarcam_injector cannot load: symbol 'cache_init'"));
    CHECK(log.contains("libref.so.1"));                                   // where the name appears
    CHECK(log.contains("SUMMARY: qcarcam_receiver cannot load: library libnotthere.so.7 is not in any search directory"));
    CHECK(log.contains("SUMMARY: qcarcam_viewer is not installed"));
    CHECK(log.contains("MISSING  /ifs/lib64/"));                          // absent search dirs are named
    CHECK(!log.contains("SUMMARY: qcarcam_dispatcher cannot"));           // the healthy one isn't blamed
    CHECK(!QFile::exists(d + "/tmp/.qcarcam_diag_summary"));              // scratch file removed

    // the symbol provided by nothing: no library holds the name
    QFile::remove(d + "/lib/libref.so.1");
    sh.start("/bin/sh", {"-c", script});
    CHECK(sh.waitForFinished(30000));
    QFile again(d + "/tmp/" + TargetDiagnostics::kLogName);
    again.open(QIODevice::ReadOnly);
    CHECK(QString::fromUtf8(again.readAll()).contains("symbol 'cache_init' (needed by qcarcam_injector) is in no library of the search path"));

    std::printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
    return fails;
}
