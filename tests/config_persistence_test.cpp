// Host test of how CamSyringe remembers its configuration across reinstalls and releases (docs/adr/0013):
//   g++ -std=c++17 -fPIC -Isrc $(pkg-config --cflags Qt6Core) tests/config_persistence_test.cpp
//       src/util/ConfigFiles.cpp src/util/SessionConfigStore.cpp src/util/CameraConfigStore.cpp
//       -o /tmp/config_persistence_test $(pkg-config --libs Qt6Core) && /tmp/config_persistence_test   (one command, wrapped)
// (one command; wrapped here). Uses CAMSYRINGE_CONFIG_DIRS to point the store at throwaway directories.
#include "util/CameraConfigStore.h"
#include "util/ConfigFiles.h"
#include "util/SessionConfigStore.h"

#include <cstdio>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

using namespace camsyringe;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static SessionConfig sample(const QString& target)
{
    SessionConfig c;
    c.valid = true; c.target = target; c.controlPort = 5001; c.sshUser = "tester"; c.sshKeyPath = "/k";
    c.videoFiles = {"/v/a.mp4", "/v/b.mp4"}; c.camIds = {3, 9}; c.injectOnly = false;
    c.blfPath = "/x.blf"; c.blfInterface = "eth9";
    return c;
}
static void writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path); f.open(QIODevice::WriteOnly | QIODevice::Truncate); f.write(bytes);
}
static QJsonObject readJson(const QString& path)
{
    QFile f(path); f.open(QIODevice::ReadOnly);
    return QJsonDocument::fromJson(f.readAll()).object();
}
static void removeAll(const QString& dir) { QDir(dir).removeRecursively(); }

int main(int argc, char** argv)
{
    // --- 0. REGRESSION (the 2026-10-12 "dialog opens empty" bug): main() reads the saved configuration
    //        BEFORE the application object exists. Locating the files must not depend on it.
    {
        QTemporaryDir fakeHome;
        qunsetenv("CAMSYRINGE_CONFIG_DIRS");
        qputenv("HOME", fakeHome.path().toLocal8Bit());
        qunsetenv("XDG_CACHE_HOME");
        writeFile(fakeHome.path() + "/.cache/camsyringe/camera_injection.json",
                  R"({"target":"192.168.1.1","cameras":[{"videoFile":"/v/drive.mp4","camId":2}]})");
        CHECK(QCoreApplication::instance() == nullptr);
        SessionConfig early = SessionConfigStore::load();   // no QCoreApplication yet
        CHECK(early.valid); CHECK(early.target == "192.168.1.1");
        CHECK(early.videoFiles == QStringList({"/v/drive.mp4"})); CHECK((early.camIds == std::vector<int>{2}));
        // ...and a save made before it exists lands in the same place a later load finds.
        early.target = "10.1.1.1";
        CHECK(SessionConfigStore::save(early));
        CHECK(readJson(fakeHome.path() + "/.cache/camsyringe/camera_injection.json").value("target").toString() == "10.1.1.1");
    }

    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    const QString A = tmp.path() + "/install", B = tmp.path() + "/cache";
    qputenv("CAMSYRINGE_CONFIG_DIRS", (A + ":" + B).toLocal8Bit());
    const QString fA = A + "/camera_injection.json", fB = B + "/camera_injection.json";

    // --- 1. a save lands in BOTH locations, and loads back
    QStringList written;
    CHECK(SessionConfigStore::save(sample("192.168.1.1"), &written));
    CHECK(written.size() == 2); CHECK(QFile::exists(fA)); CHECK(QFile::exists(fB));
    SessionConfig r = SessionConfigStore::load();
    CHECK(r.valid); CHECK(r.target == "192.168.1.1"); CHECK(r.videoFiles == QStringList({"/v/a.mp4", "/v/b.mp4"}));
    CHECK((r.camIds == std::vector<int>{3, 9})); CHECK(!r.injectOnly); CHECK(r.blfInterface == "eth9");
    CHECK(readJson(fA).value("schemaVersion").toInt() == 1);

    // --- 2. a REINSTALL deletes the install location: the cache copy still has everything
    removeAll(A);
    r = SessionConfigStore::load();
    CHECK(r.valid); CHECK(r.target == "192.168.1.1"); CHECK(r.camIds.size() == 2);
    // ...and the next save puts it back in the install location.
    CHECK(SessionConfigStore::save(r)); CHECK(QFile::exists(fA));

    // --- 3. the install location is read-only: the save still succeeds, via the cache location
    removeAll(A); QDir().mkpath(A); QFile::setPermissions(A, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    written.clear();
    QStringList errors;
    CHECK(SessionConfigStore::save(sample("10.0.0.7"), &written, &errors));
    CHECK(written.size() == 1); CHECK(written[0] == fB); CHECK(!errors.isEmpty());   // the failure is reported, not hidden
    CHECK(SessionConfigStore::load().target == "10.0.0.7");
    QFile::setPermissions(A, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    // --- 4. nowhere writable: save reports failure (so the UI can say so), nothing crashes
    QFile::setPermissions(A, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    QFile::setPermissions(B, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    errors.clear();
    CHECK(!SessionConfigStore::save(sample("1.2.3.4"), nullptr, &errors));
    CHECK(errors.size() == 2);
    QFile::setPermissions(A, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QFile::setPermissions(B, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    // --- 5. two copies that differ: the NEWEST wins
    removeAll(A); removeAll(B);
    writeFile(fA, R"({"target":"older"})");
    { QFile f(fA); f.open(QIODevice::ReadWrite); f.setFileTime(QDateTime::currentDateTime().addSecs(-3600), QFileDevice::FileModificationTime); }
    writeFile(fB, R"({"target":"newer"})");
    CHECK(SessionConfigStore::load().target == "newer");

    // --- 6. the newest copy is corrupt: fall back to the older one instead of forgetting everything
    writeFile(fB, "{not json");
    { QFile f(fB); f.open(QIODevice::ReadWrite); f.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime); }
    r = SessionConfigStore::load();
    CHECK(r.valid); CHECK(r.target == "older");

    // --- 7. a file from an OLDER release (fewer fields) loads, missing fields take defaults
    removeAll(A); removeAll(B);
    writeFile(fA, R"({"target":"192.168.5.5","controlPort":5002})");
    r = SessionConfigStore::load();
    CHECK(r.valid); CHECK(r.target == "192.168.5.5"); CHECK(r.controlPort == 5002);
    CHECK(r.injectOnly); CHECK(r.sshUser.isEmpty()); CHECK(r.videoFiles.isEmpty());

    // --- 8. a file from a NEWER release (extra keys, a higher schema) loads, and an older release saving
    //        over it keeps those keys and doesn't lower the version
    removeAll(A); removeAll(B);
    writeFile(fA, R"({"schemaVersion":7,"target":"192.168.9.9","futureSetting":{"a":[1,2]},"cameras":[]})");
    r = SessionConfigStore::load();
    CHECK(r.valid); CHECK(r.target == "192.168.9.9");
    SessionConfig edited = sample("192.168.9.10");
    CHECK(SessionConfigStore::save(edited));
    const QJsonObject after = readJson(fA);
    CHECK(after.value("futureSetting").toObject().value("a").toArray().size() == 2);   // kept
    CHECK(after.value("schemaVersion").toInt() == 7);                                  // not lowered
    CHECK(after.value("target").toString() == "192.168.9.10");                         // and ours applied

    // --- 9. camera_configs.json (ADR 0011) follows the same rules: reinstall, and keeps unknown keys
    QMap<QString, QMap<int, ResolvedCameraGeometry>> cache;
    ResolvedCameraGeometry g; g.camId = 8; g.ok = true; g.width = 3840; g.height = 2160;
    cache["192.168.1.1"][8] = g;
    QMap<QString, QDateTime> stamps; stamps["192.168.1.1"] = QDateTime::currentDateTime();
    removeAll(A); removeAll(B);
    CameraConfigStore::save(cache, stamps);
    CHECK(QFile::exists(A + "/camera_configs.json")); CHECK(QFile::exists(B + "/camera_configs.json"));
    removeAll(A);                                                       // a reinstall
    QMap<QString, QMap<int, ResolvedCameraGeometry>> back; QMap<QString, QDateTime> backStamps;
    CameraConfigStore::load(&back, &backStamps);
    CHECK(back.value("192.168.1.1").value(8).width == 3840);
    CHECK(backStamps.contains("192.168.1.1"));

    // --- 10. nothing anywhere: load says "no config", never crashes
    removeAll(A); removeAll(B);
    CHECK(!SessionConfigStore::load().valid);
    QMap<QString, QMap<int, ResolvedCameraGeometry>> none; QMap<QString, QDateTime> noStamps;
    CameraConfigStore::load(&none, &noStamps); CHECK(none.isEmpty());

    std::printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
    return fails;
}
