// Headless test of how CamSyringe remembers its configuration, through the REAL code: the launch-time merge
// (camsyringe::mergeLaunchConfig), the real MainWindow, and the real Settings > Camera Injection dialog
// (docs/adr/0013). Nothing here contacts a target: the target is 127.0.0.1 and every file lives in a temporary
// directory (CAMSYRINGE_CONFIG_DIRS). Build and run:
//   cmake --build build --target camsyringe_gui_test && QT_QPA_PLATFORM=offscreen build/camsyringe_gui_test
#include "camera/StreamPool.h"
#include "ui/CameraConfigDialog.h"
#include "ui/MainWindow.h"
#include "util/LaunchConfig.h"
#include "util/SessionConfigStore.h"

#include <cstdio>
#include <functional>
#include <memory>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>

#include <unistd.h>

using namespace camsyringe;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static QByteArray readBytes(const QString& path) { QFile f(path); f.open(QIODevice::ReadOnly); return f.readAll(); }
static void touch(const QString& path) { QFile f(path); f.open(QIODevice::WriteOnly); f.write("x"); }

static LaunchDefaults defaults()
{
    LaunchDefaults d;
    d.target = "192.168.1.1"; d.sshUser = "root"; d.controlPort = 5000; d.blfInterface = "enp6s0";
    return d;
}

static SessionConfig saved(const QString& video)
{
    SessionConfig c;
    c.valid = true; c.target = "127.0.0.1"; c.controlPort = 5001; c.sshUser = "tester"; c.sshKeyPath = "/keys/k";
    c.videoFiles = {video}; c.camIds = {2}; c.injectOnly = false; c.blfInterface = "eth7";
    return c;
}

// ---- A. the launch-time merge: command line > saved > default --------------------------------------------
static void testMerge(const QString& video)
{
    const SessionConfig s = saved(video);
    LaunchInput none;

    LaunchConfig a = mergeLaunchConfig(none, s, defaults());                       // nothing on the command line
    CHECK(a.target == "127.0.0.1"); CHECK(a.controlPort == 5001); CHECK(a.sshUser == "tester");
    CHECK(a.sshKeyPath == "/keys/k"); CHECK(!a.injectOnly); CHECK(a.blfInterface == "eth7");
    CHECK(a.videoFiles == std::vector<std::string>{video.toStdString()}); CHECK((a.camIds == std::vector<int>{2}));
    CHECK(a.camerasFromSaved);

    LaunchConfig b = mergeLaunchConfig(none, SessionConfig(), defaults());         // no saved file: the defaults
    CHECK(b.target == "192.168.1.1"); CHECK(b.controlPort == 5000); CHECK(b.sshUser == "root"); CHECK(b.injectOnly);
    CHECK(b.videoFiles.empty()); CHECK(!b.camerasFromSaved);

    LaunchInput withPort; withPort.controlPort = 6000; withPort.controlPortGiven = true;
    CHECK(mergeLaunchConfig(withPort, s, defaults()).controlPort == 6000);        // given option wins
    CHECK(mergeLaunchConfig(withPort, s, defaults()).target == "127.0.0.1");      // the rest still from saved

    LaunchInput other; other.target = "10.9.9.9";                                  // a DIFFERENT target
    LaunchConfig c = mergeLaunchConfig(other, s, defaults());
    CHECK(c.target == "10.9.9.9"); CHECK(c.sshUser == "root"); CHECK(c.sshKeyPath.empty());   // saved user/key don't follow it
    CHECK(c.camerasFromSaved);                                                      // but the cameras do

    LaunchInput same; same.target = "tester2@127.0.0.1";                           // same machine, user given
    LaunchConfig d = mergeLaunchConfig(same, s, defaults());
    CHECK(d.target == "127.0.0.1"); CHECK(d.sshUser == "tester2"); CHECK(d.sshKeyPath == "/keys/k");

    LaunchInput files; files.videoFiles = {"/cli/a.mp4"};                          // files on the command line
    LaunchConfig e = mergeLaunchConfig(files, s, defaults());
    CHECK(e.videoFiles == std::vector<std::string>{"/cli/a.mp4"}); CHECK(!e.camerasFromSaved); CHECK(e.camIds.empty());

    LaunchInput ids; ids.camIdsGiven = true;                                       // ids alone also block saved cameras
    CHECK(!mergeLaunchConfig(ids, s, defaults()).camerasFromSaved);

    LaunchInput flag; flag.injectOnlyGiven = true;                                 // --inject-only turns it back on
    CHECK(mergeLaunchConfig(flag, s, defaults()).injectOnly);

    SessionConfig noCams = s; noCams.videoFiles.clear(); noCams.camIds.clear();    // saved without cameras
    CHECK(!mergeLaunchConfig(none, noCams, defaults()).camerasFromSaved);
}

// ---- B. the real dialog ------------------------------------------------------------------------------
using Inspect = std::function<void(ui::CameraConfigDialog*)>;

// Opens Settings > Camera Injection and, once the dialog is up, runs `inspect` and presses `button`.
static void openDialog(ui::MainWindow& window, const Inspect& inspect, QDialogButtonBox::StandardButton button)
{
    QAction* action = nullptr;
    for (QAction* top : window.menuBar()->actions())
        if (top->menu())
            for (QAction* a : top->menu()->actions())
                if (a->text().contains("Camera Injection")) action = a;
    CHECK(action != nullptr);
    if (action == nullptr) return;

    bool ran = false;
    QTimer::singleShot(400, [&] {
        auto* dlg = qobject_cast<ui::CameraConfigDialog*>(QApplication::activeModalWidget());
        CHECK(dlg != nullptr);
        if (dlg == nullptr) return;
        inspect(dlg);
        ran = true;
        dlg->findChild<QDialogButtonBox*>()->button(button)->click();
    });
    action->trigger();   // blocks in the dialog's own event loop until the button closes it
    CHECK(ran);
}

static void expectOriginal(ui::CameraConfigDialog* d, const QString& video)
{
    CHECK(d->target() == "127.0.0.1"); CHECK(d->controlPort() == 5001); CHECK(d->sshUser() == "tester");
    CHECK(d->sshKeyPath() == "/keys/k"); CHECK(d->videoFiles() == QStringList({video}));
    CHECK((d->camIds() == std::vector<int>{2})); CHECK(!d->injectOnly());
}

static std::unique_ptr<ui::MainWindow> launch(StreamPool& pool, const SessionConfig& savedCfg)
{
    LaunchConfig l = mergeLaunchConfig(LaunchInput(), savedCfg, defaults());
    populatePool(pool, l, l.camIds);
    return std::make_unique<ui::MainWindow>(&pool, QString::fromStdString(l.target), l.controlPort,
                                            QString::fromStdString(l.sshUser), QString::fromStdString(l.sshKeyPath),
                                            l.injectOnly, QString::fromStdString(l.blfFile),
                                            QString::fromStdString(l.blfInterface), false);
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("camsyringe");

    QTemporaryDir tmp;
    qputenv("CAMSYRINGE_CONFIG_DIRS", (tmp.path() + "/install:" + tmp.path() + "/cache").toLocal8Bit());
    const QString video = tmp.path() + "/drive.mp4", other = tmp.path() + "/other.mp4";
    touch(video); touch(other);
    const QString file = tmp.path() + "/install/camera_injection.json";

    std::printf("-- A. launch merge (command line > saved > default)\n");
    testMerge(video);

    std::printf("-- B. the real dialog\n");
    // The state a previous run left behind: what the user accepted last time.
    CHECK(SessionConfigStore::save(saved(video)));
    const QByteArray before = readBytes(file);

    StreamPool pool;
    auto window = launch(pool, SessionConfigStore::load());    // like main(): load, merge, build the pool, open the window
    CHECK(pool.cameraCount() == 1);

    std::printf("   1. first open shows the saved configuration; Cancel\n");
    openDialog(*window, [&](ui::CameraConfigDialog* d) { expectOriginal(d, video); }, QDialogButtonBox::Cancel);
    CHECK(readBytes(file) == before);                           // Cancel writes nothing

    std::printf("   2. reopen after Cancel: nothing was lost\n");
    openDialog(*window, [&](ui::CameraConfigDialog* d) { expectOriginal(d, video); }, QDialogButtonBox::Cancel);

    std::printf("   3. edit fields, then Cancel: the edits are discarded and the saved file is untouched\n");
    openDialog(*window, [&](ui::CameraConfigDialog* d) {
        for (auto* e : d->findChildren<QLineEdit*>()) if (e->text() == video) e->setText(other);
        for (auto* c : d->findChildren<QCheckBox*>()) if (c->text().contains("Inject only")) c->setChecked(true);
        CHECK(d->videoFiles() == QStringList({other}));         // the edit really took inside the dialog
    }, QDialogButtonBox::Cancel);
    CHECK(readBytes(file) == before);
    openDialog(*window, [&](ui::CameraConfigDialog* d) { expectOriginal(d, video); }, QDialogButtonBox::Cancel);

    std::printf("   4. edit, then OK: it is saved, and applies to this session\n");
    openDialog(*window, [&](ui::CameraConfigDialog* d) {
        for (auto* e : d->findChildren<QLineEdit*>()) if (e->text() == video) e->setText(other);
    }, QDialogButtonBox::Ok);
    SessionConfig after = SessionConfigStore::load();
    CHECK(after.valid); CHECK(after.videoFiles == QStringList({other})); CHECK((after.camIds == std::vector<int>{2}));
    CHECK(after.target == "127.0.0.1"); CHECK(after.controlPort == 5001); CHECK(after.sshUser == "tester");
    openDialog(*window, [&](ui::CameraConfigDialog* d) { CHECK(d->videoFiles() == QStringList({other})); },
               QDialogButtonBox::Cancel);

    std::printf("   5. RELAUNCH: a new window from the saved file shows what was accepted\n");
    window.reset();
    StreamPool pool2;
    auto window2 = launch(pool2, SessionConfigStore::load());
    CHECK(pool2.cameraCount() == 1);
    openDialog(*window2, [&](ui::CameraConfigDialog* d) {
        CHECK(d->videoFiles() == QStringList({other})); CHECK((d->camIds() == std::vector<int>{2}));
        CHECK(d->target() == "127.0.0.1"); CHECK(d->controlPort() == 5001); CHECK(d->sshUser() == "tester");
        CHECK(d->sshKeyPath() == "/keys/k"); CHECK(!d->injectOnly());
    }, QDialogButtonBox::Cancel);

    std::printf("   6. a REINSTALL wipes the install location: the cache copy still restores everything\n");
    window2.reset();
    QDir(tmp.path() + "/install").removeRecursively();
    StreamPool pool3;
    auto window3 = launch(pool3, SessionConfigStore::load());
    CHECK(pool3.cameraCount() == 1);
    openDialog(*window3, [&](ui::CameraConfigDialog* d) { CHECK(d->videoFiles() == QStringList({other})); },
               QDialogButtonBox::Cancel);

    std::printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
    std::fflush(stdout);
    _exit(fails ? 1 : 0);   // the window's background probes are detached threads; skip the orderly teardown
}
