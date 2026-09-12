#include "ui/MainWindow.h"

#include "camera/PortScheme.h"
#include "camera/StreamPool.h"
#include "net/DispatcherRemoteControl.h"
#include "net/DispatcherVersionProbe.h"
#include "net/InjectorBundleInstaller.h"
#include "net/Ipv6SupportProbe.h"
#include "net/RealRefSync.h"
#include "net/TcpConnect.h"
#include "ui/CameraConfigDialog.h"
#include "ui/CameraSettingsDialog.h"
#include "ui/CameraWidget.h"
#include "ui/SshCredentialsDialog.h"
#include "util/CameraConfigStore.h"
#include "util/InjectorBundleFinder.h"

#include <QAction>
#include <QCloseEvent>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGridLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QWidget>

#include <thread>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace camsyringe::ui {

namespace {

// destUrl is "rtp://target:port" -- pull just the port for the tile title.
QString portFromDestUrl(const std::string& destUrl) {
    auto pos = destUrl.rfind(':');
    if (pos == std::string::npos) {
        return QString();
    }
    return QString::fromStdString(destUrl.substr(pos + 1));
}

// See qcarcam-injector/ARCHITECTURE.md item 37 for the target-side
// contract these constants/helpers encode.
constexpr const char* kRealLibPath = "/mnt/lib64/camera/libqcxclient.so";
constexpr const char* kRealLibBackupPath = "/mnt/lib64/camera/libqcxclient.so.real";
// Where the ONE injector bundle (create-qcarcam-inj-bundle.sh) already
// installs the shim on the target, alongside the real binaries --
// InjectorBundleInstaller's existing /var/opt convention (see its own
// "rm -rf /var/opt/bin /var/opt/lib /var/opt/include" comment). The
// REAL->SHIM toggle direction copies FROM here, entirely on the target's
// own filesystem -- explicitly no PC<->target file transfer of any kind
// during a toggle (an earlier revision did its own separate PC-side
// discovery + scp of a loose file; removed per explicit direction: the
// shim should travel ONLY as part of the one bundle install, not via a
// second, independent delivery path).
constexpr const char* kShimOnTargetPath = "/var/opt/lib/libqcxclient_shim.so";
// Distinguishes "the bundle was never installed here" from a generic SSH
// failure in the toggle's own result handling below -- see there.
constexpr const char* kShimMissingMarker = "CAMSYRINGE_SHIM_NOT_INSTALLED";
// Idempotent -- confirmed live on the real target that /mnt is mount-flag
// read-only (not verity-protected) and this always succeeds; safe to
// prefix onto every toggle rather than tracking mount state separately.
constexpr const char* kRemountRw = "mount -uvw /mnt >/dev/null 2>&1";

// refreshShimStatus() must NEVER itself prompt for credentials -- see its
// own header comment for why. Declining unconditionally means ensureAuth()
// only ever succeeds passwordless for this call.
bool declineCredentials(const QString&, QString*, QString*) { return false; }

// Opt-in (unset by default -- silent for normal use): prints each
// refreshShimStatus() attempt's outcome and wall-clock duration to
// stderr. Added to diagnose a real report of the SHIM/REAL/BLIND
// indicator staying stuck at BLIND for ~2 minutes after a target came
// back online, far longer than this function's own ~1s poll interval +
// ~8-10s worst-case SSH timeout should ever allow -- set
// CAMSYRINGE_DEBUG_SHIM_STATUS=1 to see exactly where an attempt is
// actually spending its time (ensureAuth() vs. the status query itself)
// next time this reproduces.
bool shimStatusDebugEnabled() {
    static const bool en = (std::getenv("CAMSYRINGE_DEBUG_SHIM_STATUS") != nullptr);
    return en;
}

} // namespace

MainWindow::MainWindow(camsyringe::StreamPool* pool, QString initialTarget, int initialControlPort,
                        QString initialSshUser, QString initialSshKeyPath, bool initialInjectOnly,
                        QString initialBlfPath, QString initialBlfInterface,
                        bool startImmediately, QWidget* parent)
    : QMainWindow(parent),
      pool_(pool),
      currentTarget_(std::move(initialTarget)),
      controlPort_(initialControlPort),
      sshUser_(std::move(initialSshUser)),
      sshKeyPath_(std::move(initialSshKeyPath)),
      injectOnly_(initialInjectOnly),
      blfPath_(std::move(initialBlfPath)),
      blfInterface_(std::move(initialBlfInterface)) {
    // Restores whatever a previous run already read from a target (see
    // CameraConfigStore's own class comment for the file format/location)
    // -- otherwise every fresh launch started from a completely empty
    // geometryCache_/geometryReadTimestamps_ regardless of the target's
    // own configuration not having changed, a real reported gap.
    camsyringe::CameraConfigStore::load(&geometryCache_, &geometryReadTimestamps_);

    auto* central = new QWidget(this);
    grid_ = new QGridLayout(central);
    setCentralWidget(central);
    setWindowTitle(tr("CamSyringe"));

    playPauseAction_ = menuBar()->addAction(tr("▶ Play"));
    connect(playPauseAction_, &QAction::triggered, this, &MainWindow::onPlayPauseTriggered);

    stopAction_ = menuBar()->addAction(tr("⏹ Stop"));
    connect(stopAction_, &QAction::triggered, this, &MainWindow::onStopTriggered);

    settingsMenu_ = menuBar()->addMenu(tr("⚙ Settings"));
    camSyringeSettingsAction_ = settingsMenu_->addAction(tr("Camera Injection"));
    connect(camSyringeSettingsAction_, &QAction::triggered, this, &MainWindow::onConfigureTriggered);
    cameraSettingsAction_ = settingsMenu_->addAction(tr("Camera Configs"));
    connect(cameraSettingsAction_, &QAction::triggered, this, &MainWindow::onCameraSettingsTriggered);
    cameraSettingsAction_->setEnabled(!currentTarget_.isEmpty());

    // Target-maintenance action, independent of Play/Pause/Idle state --
    // enabled whenever a target is configured, not gated to Idle the way
    // camSyringeSettingsAction_ is.
    installInjectorAction_ = menuBar()->addAction(tr("⇪ Install Injector"));
    connect(installInjectorAction_, &QAction::triggered, this, &MainWindow::onInstallInjectorTriggered);
    installInjectorAction_->setEnabled(!currentTarget_.isEmpty());

    QMenu* helpMenu = menuBar()->addMenu(tr("Help"));
    QAction* aboutAction = helpMenu->addAction(tr("About"));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onAboutTriggered);

    // Bottom of the main window: a long, fixed-height status bar rectangle
    // (standard text height + a few px of top/bottom padding, NOT
    // whatever height its content happens to want -- otherwise it visibly
    // changes height between "empty", "showing a message", and "showing
    // the progress bar", which is what a plain default QStatusBar does).
    // Split into exactly two visible boxes, same height, side by side:
    // generalStatusLabel_ (left, stretches with the window) and
    // statusIndicatorArea_ (right, fixed width) -- see each one's own
    // header comment.
    const QFontMetrics fm = fontMetrics();
    const int kStatusBarVPadding = 4;  // "standard text size + a few pixels" -- user-specified
    const int statusBarHeight = fm.height() + 2 * kStatusBarVPadding;
    // Full status bar height, not shorter -- user-specified: the
    // shim/deploy rectangle (statusIndicatorArea_) must be exactly as
    // tall as its parent (QStatusBar). An earlier revision shrank this by
    // 2px so each box's own border wouldn't touch the bar's top/bottom
    // edge -- no longer needed now that generalStatusLabel_ draws no
    // border of its own (see showGeneralStatus()'s own comment); left in
    // by then-stale habit, which is exactly what made statusIndicatorArea_
    // visibly short of the bar's own top/bottom edge (confirmed via
    // screenshot) even though nothing needed it to be anymore.
    const int boxHeight = statusBarHeight;
    statusBar()->setFixedHeight(statusBarHeight);
    // No size grip in the corner -- it would otherwise claim the exact
    // bottom-right pixels statusIndicatorArea_ is meant to occupy,
    // pushing it slightly left of the true corner (user-specified: right-
    // bottom aligned with the main window, not "somewhere near" it).
    statusBar()->setSizeGripEnabled(false);
    // QStatusBar bakes in a small, non-overridable offset around every
    // item added via addWidget()/addPermanentWidget() -- confirmed, the
    // hard way, self-verified offscreen (QT_QPA_PLATFORM=offscreen +
    // QWidget::grab(), no window manager/X11 involved at all, ruling
    // those out entirely) with an exact parent-chain geometry dump: a
    // widget added that way is reparented to be a DIRECT CHILD OF
    // QStatusBar itself, positioned at a baked-in (2,3) offset within it
    // that NONE of the public APIs that look like they should control it
    // actually touch -- setContentsMargins() on the bar itself,
    // setContentsMargins()/setSpacing() on whatever statusBar()->layout()
    // itself returns, and a "QStatusBar::item { border: none; }"
    // stylesheet override were ALL tried and confirmed, by that same
    // geometry dump, to have zero effect on it. This offset is
    // QStatusBar's own private item-management, not exposed for
    // override.
    //
    // Rather than fight that (an earlier revision tried reparenting
    // generalStatusLabel_ directly to statusBar() with fully manual,
    // resize-synced geometry -- reverted as unnecessary complexity once
    // this simpler alternative was pointed out): don't draw
    // generalStatusLabel_'s OWN border at all. Its baked-in inset is
    // invisible as long as nothing is drawing a border AT that inset --
    // it just becomes equivalent to a few extra pixels of padding, same
    // as its own contentsMargins below. The visible "general status"
    // rectangle is instead QStatusBar's OWN outer border (see the
    // setStyleSheet() call below) -- QStatusBar itself sits flush at
    // (0,0) against the window with no inset of its own (confirmed by
    // the same geometry dump); only its MANAGED ITEMS get the baked-in
    // offset, so styling the bar's own frame sidesteps the whole problem.
    statusBar()->setContentsMargins(0, 0, 0, 0);
    statusBar()->setStyleSheet("QStatusBar { border: 1px solid gray; } QStatusBar::item { border: none; }");

    generalStatusLabel_ = new QLabel(this);
    generalStatusLabel_->setFixedHeight(boxHeight);
    generalStatusLabel_->setContentsMargins(6, 0, 6, 0);
    // Left-aligned (user-specified) -- contrasts with shimStatusLabel_'s
    // own AlignCenter: a short fixed word ("SHIM"/"REAL"/"BLIND") reads
    // better centered in its small box, while a general status message is
    // often a full sentence that should start flush at the box's left
    // edge, not centered.
    generalStatusLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // Selectable/copyable (user-specified) -- a plain QLabel is inert by
    // default. Deliberately NOT applied to shimStatusLabel_: that one's
    // double-click is its own custom toggle (see eventFilter()), and
    // Qt's own text-selection also uses double-click (to select a word)
    // -- turning this on there would fight that, not just add a feature.
    generalStatusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                                  Qt::TextSelectableByKeyboard);
    generalStatusLabel_->setCursor(Qt::IBeamCursor);
    clearGeneralStatus();
    statusBar()->addWidget(generalStatusLabel_, /*stretch=*/1);

    // One fixed-width slot holding BOTH shimStatusLabel_ and
    // installProgressBar_ stacked in the SAME geometry, not side-by-side
    // -- see statusIndicatorArea_'s own header comment for why the
    // overlap is fine. No layout on the container itself (a layout
    // manager won't let children overlap); both are given identical
    // manual geometry below instead. Parented DIRECTLY to statusBar()
    // (not to generalStatusLabel_, unlike an earlier revision) --
    // confirmed the hard way (offscreen pixel dump) that
    // generalStatusLabel_ has its OWN baked-in top inset from the same
    // QStatusBar per-item mechanism discussed above, so nesting inside it
    // inherited that inset vertically (a real, screenshot-confirmed gap
    // above statusIndicatorArea_, even after forcing its height to the
    // full bar height -- it just overflowed generalStatusLabel_'s own
    // bottom and got silently clipped there instead of the gap closing).
    // statusBar() itself has no inset of its own (see above), so working
    // directly in ITS coordinate space avoids the problem entirely rather
    // than inheriting it a second time.
    statusIndicatorArea_ = new QWidget(statusBar());
    // Ordinary sibling of the addWidget()-managed generalStatusLabel_ now
    // (not a child of it) -- explicit raise() needed for z-order, same
    // reasoning as installProgressBar_/shimStatusLabel_ below.
    statusIndicatorArea_->raise();
    // Catches statusBar()'s own QEvent::Resize directly (rather than
    // generalStatusLabel_'s, which has its own inset and is no longer
    // this widget's coordinate reference) to keep statusIndicatorArea_
    // pinned to the bar's true right edge -- see eventFilter().
    statusBar()->installEventFilter(this);

    shimStatusLabel_ = new QLabel(statusIndicatorArea_);
    shimStatusLabel_->setAlignment(Qt::AlignCenter);
    // A plain QLabel has no double-click signal of its own -- caught via
    // eventFilter() instead (see its own definition and the class
    // comment on toggleShimStatus()). The label fills the box's full
    // geometry (set below), so a double-click ANYWHERE inside the visible
    // rectangle lands on it -- not just on the text glyphs themselves,
    // per the explicit requirement that the whole box be clickable.
    shimStatusLabel_->installEventFilter(this);
    applyShimState(ShimState::Blind);

    installProgressBar_ = new QProgressBar(statusIndicatorArea_);
    installProgressBar_->setRange(0, 100);
    installProgressBar_->setTextVisible(true);
    installProgressBar_->setVisible(false);

    // Fixed width -- unlike generalStatusLabel_'s stretch-to-fill box,
    // this one stays a constant size regardless of window size (per
    // explicit direction). Edit this literal directly to resize it.
    const int areaWidth = 160;
    statusIndicatorArea_->setFixedSize(areaWidth, boxHeight);
    // Initial position -- corrected to the bar's actual final width by
    // the first real resize event (see eventFilter()); statusBar()'s own
    // width isn't final yet at this point in the constructor.
    statusIndicatorArea_->move(statusBar()->width() - areaWidth, 0);
    shimStatusLabel_->setGeometry(0, 0, areaWidth, boxHeight);
    installProgressBar_->setGeometry(0, 0, areaWidth, boxHeight);
    // Higher z-order than shimStatusLabel_ (see statusIndicatorArea_'s own
    // comment): raise() moves it to the front among its siblings so it
    // paints on top on the rare chance both are visible at once.
    installProgressBar_->raise();
    // No need to raise() statusIndicatorArea_ itself above
    // generalStatusLabel_ -- it's a CHILD of it, and Qt always paints a
    // widget's children on top of the widget's own content already,
    // regardless of add order. Only SIBLING z-order (the line above)
    // needs an explicit raise(). Initial position is (0,0) here
    // (corrected to the true right edge by the first real resize event
    // eventFilter() below handles, which fires before this window is
    // ever actually shown/painted).

    rebuildGrid();
    applyState(PlaybackState::Idle);
    if (!currentTarget_.isEmpty()) {
        refreshShimStatus();
    }

    // Periodic re-check -- see shimStatusTimer_'s own comment for why
    // this exists at all (a target going away/coming back otherwise had
    // no path back to a confirmed state). 1s, user-specified -- cheap
    // when nothing's wrong (declineCredentials means a dead/unreachable
    // target fails fast, no password prompt ever fires from this timer),
    // and shimBusy_ (already checked inside refreshShimStatus() itself)
    // means a tick that lands while a previous check or a toggle is still
    // in flight is just a no-op, never piles up concurrent SSH attempts.
    shimStatusTimer_ = new QTimer(this);
    connect(shimStatusTimer_, &QTimer::timeout, this, &MainWindow::refreshShimStatus);
    shimStatusTimer_->start(1000);

    pool_->setPreviewCallback([this](int index, const uint8_t* rgb, int w, int h, int stride) {
        QImage frame(rgb, w, h, stride, QImage::Format_RGB888);
        QImage copy = frame.copy(); // deep copy: rgb buffer is only valid for this callback
        QMetaObject::invokeMethod(
            this, [this, index, copy]() { onPreviewFrame(index, copy); }, Qt::QueuedConnection);
    });
    pool_->setErrorCallback([this](int index, const std::string& message) {
        QString qmessage = QString::fromStdString(message);
        QMetaObject::invokeMethod(
            this, [this, index, qmessage]() { onCameraError(index, qmessage); },
            Qt::QueuedConnection);
    });

    if (startImmediately) {
        startStreaming();
    } else if (pool_->cameraCount() == 0) {
        // Defer to the next event-loop iteration so the main window is
        // already shown (main.cpp calls show() before app.exec()) when the
        // dialog pops up.
        QTimer::singleShot(0, this, &MainWindow::onConfigureTriggered);
    }
}

void MainWindow::rebuildGrid() {
    for (auto* widget : cameraWidgets_) {
        grid_->removeWidget(widget);
        widget->deleteLater();
    }
    cameraWidgets_.clear();

    const size_t n = pool_->cameraCount();
    const int cols = n == 0 ? 1 : static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));

    for (size_t i = 0; i < n; ++i) {
        const CameraConfig& cfg = pool_->configAt(i);
        QString fileName = QFileInfo(QString::fromStdString(cfg.inputPath)).fileName();
        QString title = QString("%1 · id %2 · :%3 · %4")
                             .arg(QString::fromStdString(cfg.label))
                             .arg(cfg.camId)
                             .arg(portFromDestUrl(cfg.destUrl))
                             .arg(fileName);

        auto* widget = new CameraWidget(static_cast<int>(i), title, this);
        cameraWidgets_.push_back(widget);
        grid_->addWidget(widget, static_cast<int>(i) / cols, static_cast<int>(i) % cols);
    }
}

void MainWindow::onPreviewFrame(int index, const QImage& frame) {
    if (index >= 0 && index < static_cast<int>(cameraWidgets_.size())) {
        cameraWidgets_[static_cast<size_t>(index)]->clearError();
        cameraWidgets_[static_cast<size_t>(index)]->updateFrame(frame);
    }
}

void MainWindow::onCameraError(int index, const QString& message) {
    if (index >= 0 && index < static_cast<int>(cameraWidgets_.size())) {
        cameraWidgets_[static_cast<size_t>(index)]->showError(message);
    }
}

void MainWindow::applyState(PlaybackState state) {
    state_ = state;
    switch (state_) {
        case PlaybackState::Idle:
            playPauseAction_->setText(tr("▶ Play"));
            playPauseAction_->setEnabled(pool_->cameraCount() > 0);
            stopAction_->setEnabled(false);
            camSyringeSettingsAction_->setEnabled(true);
            break;
        case PlaybackState::Playing:
            playPauseAction_->setText(tr("⏸ Pause"));
            playPauseAction_->setEnabled(true);
            stopAction_->setEnabled(true);
            camSyringeSettingsAction_->setEnabled(false);
            break;
        case PlaybackState::Paused:
            playPauseAction_->setText(tr("▶ Play"));
            playPauseAction_->setEnabled(true);
            stopAction_->setEnabled(true);
            camSyringeSettingsAction_->setEnabled(false);
            break;
    }
}

void MainWindow::startStreaming() {
    if (state_ == PlaybackState::Idle) {
        for (auto* widget : cameraWidgets_) {
            widget->resetIdle();
            widget->clearError();
        }
    }

    // Every camera enabled by default -- a target-side ERROR (known only
    // once the async declaration below resolves) disables it for any
    // FUTURE startAll() (e.g. a later Pause->Play in this same session);
    // see class comment for why this doesn't also tear down its
    // already-running local stream immediately.
    for (size_t i = 0; i < pool_->cameraCount(); ++i) {
        pool_->setEnabled(i, true);
    }
    // Streaming starts NOW, unconditionally -- see class comment for why
    // this can't wait on the target's response first (the target's own
    // receiver can't become ready until real RTP data is already
    // arriving, so waiting here would deadlock both sides).
    pool_->startAll();
    applyState(PlaybackState::Playing);

    // BLF/Ethernet replay, if configured -- shares pool_'s own Timeline
    // origin (just captured by the startAll() call above) so it stays
    // phase-aligned with the camera streams, matching CONTEXT.md's
    // Architecture diagram (one shared Timeline orchestrator). open() does
    // the (synchronous, on this GUI thread) BLF parse + raw socket
    // open/bind -- both fast (BLF parse is a local file read, no network
    // wait, unlike the camera declaration below) so this doesn't need the
    // same "start first, confirm async" treatment DispatcherClient gets.
    if (!blfPath_.isEmpty()) {
        clearGeneralStatus();
        if (blfReplayer_.open(blfPath_.toStdString(), blfInterface_.toStdString())) {
            blfReplayer_.setStartOrigin(pool_->timelineOriginNs());
            blfThread_ = std::thread([this] { blfReplayer_.run(); });
        } else {
            // Also logged to stderr by BlfReplayer::open() itself; this is
            // the GUI-visible equivalent, since a GUI session may have no
            // visible console. Disabled for the rest of this session (no
            // retry on a later Pause->Play) -- re-enable via Configure.
            showGeneralStatus(
                tr("BLF replay disabled: %1").arg(QString::fromStdString(blfReplayer_.lastError())),
                /*isError=*/true);
            blfPath_.clear();
        }
    }

    // Every Play press -- including a Pause->Play resume -- redeclares to
    // the target, unconditionally. This used to be skipped on resume (see
    // git history) to avoid a brief blank-then-recover glitch on the
    // target's own physical preview panel: redeclaring runs
    // DispatcherClient::declareAsync()'s disconnect()-then-reconnect,
    // which makes qcarcam_dispatcher's handleConnection() call
    // stopSession() (tearing down the previous receiver/injector/viewer)
    // before spawning fresh ones. That teardown-and-respawn is now the
    // POINT, not a side effect to avoid: real-hardware bug report --
    // stopEverything(false) (Pause) stops local RTP send but deliberately
    // leaves the target's receiver process (and its open hardware H.264
    // decoder session) running and idle; resuming into that SAME session
    // feeds a brand-new RTP/MPEGTS stream (fresh SPS/PPS) into an
    // already-open decoder that had gone idle mid-stream, which was
    // confirmed live to wedge the target's Venus/vidc hardware decoder
    // block badly enough that NO software-side restart (qcarcam_test,
    // CamSyringe, even a full dispatcher Stop/Play) recovers it -- only a
    // full target power cycle did. A fresh declare's stopSession() sends
    // the old receiver a graceful SIGINT first (see
    // main_dispatcher.cpp's stopPid()), giving its HwVideoDecoder a real
    // chance to close before the next session's receiver opens a new one,
    // which avoids the wedge. The cosmetic on-panel glitch this
    // reintroduces is strictly preferable to a hardware hang that needs a
    // power cycle to clear.
    for (auto* widget : cameraWidgets_) {
        widget->showStatus(tr("Confirming target injection…"));
    }

    dispatcherStartAttempted_ = false; // fresh retry budget for this Play press
    declareToTarget();
    checkInjectorVersion();
}

void MainWindow::declareToTarget() {
    std::vector<CameraDeclaration> declarations;
    for (size_t i = 0; i < pool_->cameraCount(); ++i) {
        const CameraConfig& cfg = pool_->configAt(i);
        declarations.push_back({cfg.camId, cfg.port});
    }

    // declareAsync() runs on DispatcherClient's own background thread --
    // this lambda is invoked THERE, not on the GUI thread, so it must
    // marshal back via QMetaObject::invokeMethod before touching any
    // widget/pool_ state, same convention as StreamPool's own preview/
    // error callbacks above.
    dispatcherClient_.declareAsync(
        currentTarget_.toStdString(), controlPort_, std::move(declarations), injectOnly_,
        [this](std::vector<CameraDeclareOutcome> outcomes, std::vector<PreviewIssue> previewIssues,
               bool connectFailed, std::string connectError) {
            QString qConnectError = QString::fromStdString(connectError);
            QMetaObject::invokeMethod(
                this,
                [this, outcomes = std::move(outcomes), previewIssues = std::move(previewIssues),
                 connectFailed, qConnectError]() mutable {
                    onDeclareComplete(std::move(outcomes), std::move(previewIssues), connectFailed,
                                      qConnectError);
                },
                Qt::QueuedConnection);
        });
}

void MainWindow::checkInjectorVersion() {
    camsyringe::DispatcherVersionProbe::queryAsync(
        currentTarget_.toStdString(), controlPort_, [this](bool ok, std::string version) {
            if (!ok || version.empty()) {
                return; // unknown (old dispatcher, unreachable) -- nothing to compare, no notice
            }
            QString qVersion = QString::fromStdString(version);
            QMetaObject::invokeMethod(
                this,
                [this, qVersion]() {
                    auto localBundle = camsyringe::InjectorBundleFinder::findBundle();
                    if (!localBundle) {
                        return; // nothing known locally to compare against
                    }
                    QString localVersion = camsyringe::InjectorBundleFinder::extractVersion(*localBundle);
                    if (localVersion.isEmpty() || localVersion == qVersion) {
                        return;
                    }
                    QMessageBox::information(
                        this, tr("Injector Version"),
                        tr("Target is running qcarcam_dispatcher v%1; the injector bundle "
                           "available locally is v%2. CamSyringe will try to continue regardless.")
                            .arg(qVersion, localVersion));
                },
                Qt::QueuedConnection);
        });
}

void MainWindow::resolveCameraGeometry() {
    if (currentTarget_.isEmpty()) {
        return;
    }

    const QMap<int, camsyringe::ResolvedCameraGeometry>& cached = geometryCache_[currentTarget_];
    std::vector<int> needed;
    for (size_t i = 0; i < pool_->cameraCount(); ++i) {
        int camId = pool_->configAt(i).camId;
        if (!cached.contains(camId) &&
            std::find(needed.begin(), needed.end(), camId) == needed.end()) {
            needed.push_back(camId);
        }
    }
    if (needed.empty()) {
        return; // every configured id already resolved for this target
    }
    std::sort(needed.begin(), needed.end()); // ascending -- the fallback order, see
                                              // CameraGeometryResolver::resolveAsync()'s own comment

    QString target = currentTarget_;
    camsyringe::CameraGeometryResolver::resolveAsync(
        sshSession_, target, sshUser_, sshKeyPath_, needed,
        [this, target](std::vector<camsyringe::ResolvedCameraGeometry> results) {
            // Runs on CameraGeometryResolver's own background thread --
            // marshal before touching geometryCache_/pool_/any widget,
            // same convention as DispatcherClient/DispatcherVersionProbe.
            QMetaObject::invokeMethod(
                this,
                [this, target, results = std::move(results)]() {
                    QStringList summary;
                    geometryReadTimestamps_[target] = QDateTime::currentDateTime();
                    for (const auto& r : results) {
                        geometryCache_[target][r.camId] = r;
                        if (!r.ok) {
                            summary << tr("cam %1: unknown").arg(r.camId);
                            continue;
                        }
                        summary << (r.wasFallback ? tr("cam %1: %2x%3 (fallback)")
                                                         .arg(r.camId)
                                                         .arg(r.width)
                                                         .arg(r.height)
                                                   : tr("cam %1: %2x%3")
                                                         .arg(r.camId)
                                                         .arg(r.width)
                                                         .arg(r.height));
                        // Only meaningful if currentTarget_ is STILL this
                        // target (the user could have re-run Configure
                        // against a different one while this was in
                        // flight) -- pool_'s own camIds are looked up
                        // fresh here rather than assumed unchanged.
                        if (target == currentTarget_) {
                            for (size_t i = 0; i < pool_->cameraCount(); ++i) {
                                if (pool_->configAt(i).camId == r.camId) {
                                    pool_->setTargetGeometry(i, static_cast<int>(r.width),
                                                              static_cast<int>(r.height));
                                }
                            }
                        }
                    }
                    if (target == currentTarget_ && !summary.isEmpty()) {
                        showGeneralStatus(
                            tr("Camera resolution: %1").arg(summary.join(QStringLiteral("; "))));
                    }
                    camsyringe::CameraConfigStore::save(geometryCache_, geometryReadTimestamps_);
                },
                Qt::QueuedConnection);
        });
}

void MainWindow::tryStartDispatcherThenRetry() {
    QString target = currentTarget_;
    int controlPort = controlPort_;
    QString sshUser = sshUser_;
    QString sshKeyPath = sshKeyPath_;
    std::thread([this, target, controlPort, sshUser, sshKeyPath]() {
        auto credentialsCb = [this, sshUser](const QString& t, QString* username, QString* password) {
            bool accepted = false;
            QMetaObject::invokeMethod(
                this,
                [this, t, sshUser, username, password, &accepted]() {
                    ui::SshCredentialsDialog dlg(t, sshUser, this);
                    if (dlg.exec() == QDialog::Accepted) {
                        *username = dlg.username();
                        *password = dlg.password();
                        accepted = true;
                    }
                },
                Qt::BlockingQueuedConnection);
            return accepted;
        };

        // Best-effort either way -- if SSH itself fails (unreachable
        // target, wrong credentials, no SSH at all), just retry anyway;
        // onDeclareComplete()'s second connectFailed (dispatcherStartAttempted_
        // now true) falls through to the ordinary error banner, same as
        // if this whole retry never existed.
        if (sshSession_.ensureAuth(target, sshUser, sshKeyPath, credentialsCb)) {
            QString error;
            camsyringe::DispatcherRemoteControl::ensureRunning(sshSession_, target, controlPort,
                                                                &error);
        }

        QMetaObject::invokeMethod(
            this,
            [this]() {
                if (state_ == PlaybackState::Playing) {
                    declareToTarget();
                }
                // else: Stop/Pause/window-close happened while this was in
                // flight -- nothing to retry into.
            },
            Qt::QueuedConnection);
    }).detach();
}

void MainWindow::onDeclareComplete(std::vector<CameraDeclareOutcome> outcomes,
                                    std::vector<PreviewIssue> previewIssues, bool connectFailed,
                                    QString connectError) {
    if (state_ != PlaybackState::Playing) {
        // Stop or window-close was pressed while the declaration was in
        // flight -- stopEverything(true) already called
        // dispatcherClient_.disconnect(), tearing down whatever this call
        // was doing; nothing left to apply. (A plain Pause doesn't
        // disconnect -- see stopEverything()'s own comment -- but also
        // can't race this: Pause is only reachable from Playing, and this
        // callback firing IS what would have kept it there.)
        return;
    }

    if (connectFailed) {
        dispatcherClient_.disconnect(); // joins the now-finished background thread

        // One automatic attempt to fix this ourselves before giving up:
        // qcarcam_dispatcher is very likely just not running (a prior
        // Stop kills it -- see onStopTriggered()) rather than genuinely
        // unreachable. See tryStartDispatcherThenRetry()'s own comment.
        if (!dispatcherStartAttempted_) {
            dispatcherStartAttempted_ = true;
            for (auto* widget : cameraWidgets_) {
                widget->showStatus(
                    tr("Target control-channel unreachable -- trying to start "
                       "qcarcam_dispatcher on the target…"));
            }
            tryStartDispatcherThenRetry();
            return;
        }

        // Streaming is ALREADY running locally (see startStreaming()) --
        // a control-channel problem doesn't stop it, it just means the
        // target never got auto-configured for it. Surfaced as an error
        // so it's not silently invisible, but this is not fatal to local
        // operation the way it would have been if this gated startAll().
        for (auto* widget : cameraWidgets_) {
            widget->showError(
                tr("Target control-channel unreachable: %1 (tried starting "
                   "qcarcam_dispatcher on the target automatically, still couldn't connect -- "
                   "streaming locally regardless)")
                    .arg(connectError));
        }
        return;
    }

    // outcomes is parallel to declarations, which was built by iterating
    // pool_->configAt(i) in order -- same order/indices here.
    for (size_t i = 0; i < outcomes.size() && i < pool_->cameraCount(); ++i) {
        const auto& outcome = outcomes[i];
        if (outcome.result == DeclareResult::Error) {
            cameraWidgets_[i]->showError(
                tr("Target rejected cam id %1: %2 (still streaming locally, but the target isn't "
                   "injecting it)")
                    .arg(outcome.camId)
                    .arg(QString::fromStdString(outcome.errorReason)));
            pool_->setEnabled(i, false);
        } else {
            cameraWidgets_[i]->clearError();
        }
    }

    // Best-effort, independent of the outcomes above -- injection for
    // these cameras is fine (they're READY, not in `outcomes` as an
    // Error); only the TARGET's own on-panel preview quadrant for them
    // failed (see PreviewIssue's own comment). Previously silent
    // (real-hardware bug report: 3 of 4 quadrants rendered on the
    // target's physical panel, no error anywhere in CamSyringe) --
    // qcarcam_dispatcher now reports it explicitly (PREVIEW_ERROR,
    // qcarcam-injector/ARCHITECTURE.md item 33), surfaced here via the
    // status bar rather than a popup, matching this app's own "no modal
    // for a non-fatal target-side notice" convention.
    if (!previewIssues.empty()) {
        QStringList ids;
        for (const auto& issue : previewIssues) {
            ids << QString::number(issue.camId);
        }
        showGeneralStatus(
            tr("Target's local preview failed for cam id(s) %1 -- injection itself is fine, only "
               "the target's own on-panel display for %2 %3 affected (see "
               "/tmp/qcarcam_dispatcher_previewer.log on target).")
                .arg(ids.join(", "))
                .arg(ids.size() == 1 ? tr("that camera") : tr("those cameras"))
                .arg(ids.size() == 1 ? tr("is") : tr("are")),
            /*isError=*/true);
    }
}

void MainWindow::stopEverything(bool disconnectFromTarget) {
    pool_->stopAll();
    if (disconnectFromTarget) {
        dispatcherClient_.disconnect(); // the target-side teardown signal
    }
    blfReplayer_.requestStop();
    if (blfThread_.joinable()) {
        blfThread_.join();
    }
}

void MainWindow::onPlayPauseTriggered() {
    if (state_ == PlaybackState::Playing) {
        stopEverything(false); // keep the target session alive -- see stopEverything()'s own comment
        applyState(PlaybackState::Paused);
        return;
    }
    startStreaming();
}

void MainWindow::onStopTriggered() {
    stopEverything(true);
    for (auto* widget : cameraWidgets_) {
        widget->resetIdle();
        widget->clearError();
    }
    clearGeneralStatus();
    applyState(PlaybackState::Idle);
    if (!currentTarget_.isEmpty()) {
        killTargetProcesses();
    }
}

void MainWindow::killTargetProcesses() {
    QString target = currentTarget_;
    QString sshUser = sshUser_;
    QString sshKeyPath = sshKeyPath_;
    std::thread([this, target, sshUser, sshKeyPath]() {
        auto credentialsCb = [this, sshUser](const QString& t, QString* username, QString* password) {
            bool accepted = false;
            QMetaObject::invokeMethod(
                this,
                [this, t, sshUser, username, password, &accepted]() {
                    ui::SshCredentialsDialog dlg(t, sshUser, this);
                    if (dlg.exec() == QDialog::Accepted) {
                        *username = dlg.username();
                        *password = dlg.password();
                        accepted = true;
                    }
                },
                Qt::BlockingQueuedConnection);
            return accepted;
        };

        if (!sshSession_.ensureAuth(target, sshUser, sshKeyPath, credentialsCb)) {
            return; // user cancelled the credentials prompt -- nothing more to do
        }
        QString error;
        if (!camsyringe::DispatcherRemoteControl::stopAll(sshSession_, target, &error)) {
            QMetaObject::invokeMethod(
                this,
                [this, error]() {
                    showGeneralStatus(tr("Couldn't stop target processes: %1").arg(error),
                                      /*isError=*/true);
                },
                Qt::QueuedConnection);
        }
    }).detach();
}

void MainWindow::onConfigureTriggered() {
    if (state_ != PlaybackState::Idle) {
        return; // defensive; the action is disabled outside Idle anyway
    }

    QStringList currentFiles;
    std::vector<int> currentCamIds;
    for (size_t i = 0; i < pool_->cameraCount(); ++i) {
        currentFiles << QString::fromStdString(pool_->configAt(i).inputPath);
        currentCamIds.push_back(pool_->configAt(i).camId);
    }

    CameraConfigDialog dialog(currentTarget_, controlPort_, sshUser_, sshKeyPath_, currentFiles,
                               currentCamIds, injectOnly_, blfPath_, blfInterface_,
                               geometryCache_.value(currentTarget_), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    currentTarget_ = dialog.target();
    installInjectorAction_->setEnabled(!currentTarget_.isEmpty());
    cameraSettingsAction_->setEnabled(!currentTarget_.isEmpty());
    controlPort_ = dialog.controlPort();
    sshUser_ = dialog.sshUser();
    sshKeyPath_ = dialog.sshKeyPath();
    injectOnly_ = dialog.injectOnly();
    // Configure may have pointed at a different target entirely -- last
    // known SHIM/REAL state belonged to the PREVIOUS target, not this one.
    // Drop it and query fresh rather than showing a stale answer against
    // the wrong box.
    applyShimState(ShimState::Blind);
    refreshShimStatus();
    maybeWarnAboutIpv6(currentTarget_);
    blfPath_ = dialog.blfPath();
    blfInterface_ = dialog.blfInterface();
    QStringList files = dialog.videoFiles();
    std::vector<int> camIds = dialog.camIds();

    const QMap<int, camsyringe::ResolvedCameraGeometry>& cachedForTarget =
        geometryCache_[currentTarget_]; // creates an empty entry if new -- fine, lookups below just miss

    pool_->clearCameras();
    for (int i = 0; i < files.size(); ++i) {
        int port = camsyringe::kBasePort + i * camsyringe::kPortStep;
        CameraConfig cfg;
        cfg.inputPath = files[i].toStdString();
        cfg.destUrl = "rtp://" +
                      camsyringe::bracketHostIfIPv6(currentTarget_.toStdString()) + ":" +
                      std::to_string(port);
        cfg.label = ("cam" + QString::number(i)).toStdString();
        cfg.index = i;
        cfg.camId = camIds[static_cast<size_t>(i)];
        cfg.port = port;
        // Already resolved for this target in a previous Configure apply
        // (see resolveCameraGeometry()) -- seed it now rather than
        // waiting for a fresh (never-run, since it's cached) resolve.
        auto it = cachedForTarget.find(cfg.camId);
        if (it != cachedForTarget.end() && it->ok) {
            cfg.targetWidth = static_cast<int>(it->width);
            cfg.targetHeight = static_cast<int>(it->height);
        }
        pool_->addCamera(std::move(cfg));
    }

    rebuildGrid();
    applyState(PlaybackState::Idle);
    resolveCameraGeometry();
}

void MainWindow::onCameraSettingsTriggered() {
    if (currentTarget_.isEmpty()) {
        return; // defensive; disabled without a target anyway
    }

    // Every QCarCam id the chip can address over GMSL2 (camsyringe::
    // kMinCamId/kMaxCamId, 1-16) -- deliberately NOT just pool_'s
    // currently-configured injection camera ids. Explicit requirement:
    // this dialog reads the target's real configuration for every camera
    // the hardware could have, not only whichever ones this session
    // happens to be injecting into right now.
    std::vector<int> camIds;
    camIds.reserve(camsyringe::kMaxCamId - camsyringe::kMinCamId + 1);
    for (int id = camsyringe::kMinCamId; id <= camsyringe::kMaxCamId; ++id) {
        camIds.push_back(id);
    }

    ui::CameraSettingsDialog dialog(sshSession_, currentTarget_, sshUser_, sshKeyPath_,
                                     std::move(camIds), geometryCache_.value(currentTarget_),
                                     geometryReadTimestamps_.value(currentTarget_), this);
    // Folds a fresh Read back into MainWindow's own cache/timestamp (same
    // storage resolveCameraGeometry() populates) so it benefits a later
    // Configure/Play too, not just this dialog's own table.
    connect(&dialog, &ui::CameraSettingsDialog::geometryResolved, this,
            [this](const QString& target, const std::vector<camsyringe::ResolvedCameraGeometry>& results,
                   const QDateTime& when) {
                geometryReadTimestamps_[target] = when;
                for (const auto& r : results) {
                    geometryCache_[target][r.camId] = r;
                    if (r.ok && target == currentTarget_) {
                        for (size_t i = 0; i < pool_->cameraCount(); ++i) {
                            if (pool_->configAt(i).camId == r.camId) {
                                pool_->setTargetGeometry(i, static_cast<int>(r.width),
                                                          static_cast<int>(r.height));
                            }
                        }
                    }
                }
                camsyringe::CameraConfigStore::save(geometryCache_, geometryReadTimestamps_);
            });
    dialog.exec();
}

void MainWindow::onInstallInjectorTriggered() {
    if (currentTarget_.isEmpty()) {
        return; // defensive; the action is disabled without a target anyway
    }

    QString bundlePath;
    if (auto found = camsyringe::InjectorBundleFinder::findBundle()) {
        bundlePath = *found;
    } else {
        QStringList candidates = camsyringe::InjectorBundleFinder::candidateDirs();
        QString startDir = candidates.isEmpty() ? QDir::homePath() : candidates.first();
        bundlePath = QFileDialog::getOpenFileName(this, tr("Select Injector Bundle"), startDir,
                                                   tr("Injector bundles (qcarcam_injector_bundle_v*.bin)"));
        if (bundlePath.isEmpty()) {
            return; // cancelled
        }
        camsyringe::InjectorBundleFinder::rememberDir(QFileInfo(bundlePath).absolutePath());
    }

    QString version = camsyringe::InjectorBundleFinder::extractVersion(bundlePath);
    showGeneralStatus(tr("Preparing injector install on %1…").arg(currentTarget_));
    runInjectorInstall(bundlePath, version);
}

void MainWindow::runInjectorInstall(const QString& bundlePath, const QString& bundleVersion) {
    camsyringe::InjectorBundleInstaller::installAsync(
        sshSession_, currentTarget_, controlPort_, sshUser_, sshKeyPath_, bundlePath, bundleVersion,
        // Confirm -- called on the install's own background thread;
        // blocks it until the GUI thread's modal dialog is answered.
        // Shows exactly which file was picked (so the user isn't left
        // guessing, see this feature's own bug report) and offers a way
        // to pick a different one right from here instead of having to
        // cancel and restart the whole menu action -- looping the dialog
        // back up after a reselect until the user actually commits
        // (Install) or backs out (Cancel).
        [this](const QString& target, QString* bundlePath, QString* bundleVersion) {
            bool accepted = false;
            QMetaObject::invokeMethod(
                this,
                [this, target, bundlePath, bundleVersion, &accepted]() {
                    // Plain (non-editable) text, not an input field -- this
                    // dialog only ever confirms what's already configured;
                    // changing target/user means going back to Configure
                    // (the same place currentTarget_ itself comes from),
                    // not overriding it from here.
                    QString user = sshSession_.resolvedUser(target);
                    QString keyPath = sshSession_.resolvedKeyPath(target);
                    QString keyLine =
                        keyPath.isEmpty() ? QString() : tr("Key: %1\n").arg(keyPath);
                    for (;;) {
                        QMessageBox box(QMessageBox::Question, tr("Install Injector Bundle"),
                                         tr("Target: %1\nUser: %2\n%3\n"
                                            "This will remove the existing injector install at "
                                            "%1:/var/opt (bin/lib/include), install v%4, and "
                                            "restart qcarcam_dispatcher.\n\nBundle file:\n%5")
                                             .arg(target, user, keyLine, *bundleVersion, *bundlePath),
                                         QMessageBox::NoButton, this);
                        QPushButton* installBtn =
                            box.addButton(tr("Install"), QMessageBox::AcceptRole);
                        QPushButton* chooseBtn = box.addButton(tr("Choose Different File…"),
                                                                QMessageBox::ActionRole);
                        box.addButton(QMessageBox::Cancel);
                        box.exec();
                        if (box.clickedButton() == installBtn) {
                            accepted = true;
                            break;
                        }
                        if (box.clickedButton() != chooseBtn) {
                            accepted = false; // Cancel (or dialog dismissed)
                            break;
                        }
                        QString newPath = QFileDialog::getOpenFileName(
                            this, tr("Select Injector Bundle"), QFileInfo(*bundlePath).absolutePath(),
                            tr("Injector bundles (qcarcam_injector_bundle_v*.bin)"));
                        if (!newPath.isEmpty()) {
                            *bundlePath = newPath;
                            *bundleVersion = camsyringe::InjectorBundleFinder::extractVersion(newPath);
                            camsyringe::InjectorBundleFinder::rememberDir(
                                QFileInfo(newPath).absolutePath());
                        }
                        // else: picker cancelled -- loop back to the same
                        // confirmation dialog with the file unchanged.
                    }
                    if (accepted) {
                        showGeneralStatus(
                            tr("Installing injector v%1 on %2… (this runs in the background -- "
                               "Play/Pause/Stop still work meanwhile)")
                                .arg(*bundleVersion, target));
                    }
                },
                Qt::BlockingQueuedConnection);
            return accepted;
        },
        // Credentials -- same blocking convention, shown only if
        // passwordless SSH failed.
        [this](const QString& target, QString* username, QString* password) {
            bool accepted = false;
            QMetaObject::invokeMethod(
                this,
                [this, target, username, password, &accepted]() {
                    ui::SshCredentialsDialog dlg(target, sshUser_, this);
                    if (dlg.exec() == QDialog::Accepted) {
                        *username = dlg.username();
                        *password = dlg.password();
                        accepted = true;
                    }
                },
                Qt::BlockingQueuedConnection);
            return accepted;
        },
        // Progress -- fire-and-forget, drives installProgressBar_ (bottom
        // of the main window, see its own comment) plus the status bar's
        // text right alongside it, so a long step (the SCP transfer
        // especially) doesn't look like nothing's happening. percent < 0
        // means "no real percentage available" (see ProgressCallback's
        // own comment) -- shown as an indeterminate/busy bar instead of a
        // fake number.
        [this](int percent, QString label) {
            QMetaObject::invokeMethod(
                this,
                [this, percent, label]() {
                    installProgressBar_->setVisible(true);
                    if (percent < 0) {
                        installProgressBar_->setRange(0, 0);
                    } else {
                        installProgressBar_->setRange(0, 100);
                        installProgressBar_->setValue(percent);
                    }
                    showGeneralStatus(label);
                },
                Qt::QueuedConnection);
        },
        // Result -- fire-and-forget, no answer needed back. On success,
        // just the status bar (non-blocking, matches the "no popups when
        // things just work" preference this feature was built around) --
        // but the whole install can take a while (SCP of a ~30MB+ bundle,
        // SSH, restart, poll), so by the time this fires the user has
        // often moved on to Play/Pause/Stop, which post their OWN status
        // bar messages (dispatcher retry, target-process kill, etc.) --
        // those can overwrite a plain status bar failure summary within
        // seconds, which is exactly what looked like the message
        // "vanishing" during testing. So on FAILURE specifically, also
        // pop a modal with the full detail (remote stdout/stderr
        // included) and selectable text, so it isn't lost to whatever the
        // user does next and can be copied into a bug report.
        [this, bundleVersion](bool success, QString message) {
            fprintf(stderr, "%s\n", qPrintable(message)); // full detail, including remote stdout
            // A plain user-initiated Cancel (confirm dialog's Cancel
            // button, or backing out of the credentials prompt) reports
            // through this exact same path -- it's not a failure worth a
            // "Failed" modal, just a quieter status bar note.
            bool cancelled = !success && message == QLatin1String("Cancelled.");
            QString firstLine = message.section('\n', 0, 0);
            QString summary = success  ? tr("Injector v%1 installed and dispatcher restarted.")
                                              .arg(bundleVersion)
                               : cancelled ? tr("Injector install cancelled.")
                                           : tr("Injector install failed: %1").arg(firstLine);
            QMetaObject::invokeMethod(
                this,
                [this, success, cancelled, summary, message]() {
                    installProgressBar_->setVisible(false);
                    showGeneralStatus(summary, /*isError=*/!success && !cancelled);
                    if (!success && !cancelled) {
                        QMessageBox box(QMessageBox::Critical, tr("Injector Install Failed"), message,
                                         QMessageBox::Ok, this);
                        box.setTextInteractionFlags(Qt::TextSelectableByMouse);
                        box.exec();
                    }
                },
                Qt::QueuedConnection);
        });
}

void MainWindow::onAboutTriggered() {
    QMessageBox::about(this, tr("About CamSyringe"),
                        tr("<b>CamSyringe</b> v%1 (%2)"
                           "<br><br>Author: Aananth C N, Cariad India</i>"
                           "<br>💻 99% AI Generated Code!")
                            .arg(QStringLiteral(CAMSYRINGE_VERSION), QStringLiteral(__DATE__)));
}

void MainWindow::closeEvent(QCloseEvent* event) {
    stopEverything(true);
    QMainWindow::closeEvent(event);
}

void MainWindow::showGeneralStatus(const QString& message, bool isError) {
    generalStatusLabel_->setText(message);
    // No border here (deliberately) -- QStatusBar's own baked-in item
    // inset (see the constructor's own long comment on this) means a
    // border drawn on generalStatusLabel_ ITSELF sits a few px shy of the
    // window's true edge. statusBar()'s own frame (set once, in the
    // constructor) is the visible "general status" rectangle instead --
    // it sits flush with no inset of its own, sidestepping the problem
    // entirely rather than fighting it.
    generalStatusLabel_->setStyleSheet(isError ? "color: red;" : QString());
}

void MainWindow::clearGeneralStatus() { showGeneralStatus(QString(), /*isError=*/false); }

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == shimStatusLabel_ && event->type() == QEvent::MouseButtonDblClick) {
        toggleShimStatus();
        return true;
    }
    // Keep statusIndicatorArea_ (a plain child of statusBar() itself, not
    // layout-managed -- see the constructor's own comment on why it's
    // parented there directly rather than to generalStatusLabel_) pinned
    // to the bar's true right edge whenever the bar resizes.
    if (watched == statusBar() && event->type() == QEvent::Resize) {
        statusIndicatorArea_->move(statusBar()->width() - statusIndicatorArea_->width(), 0);
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::applyShimState(ShimState state) {
    shimState_ = state;
    QString text;
    QString colorStyle;
    switch (state) {
        case ShimState::Shim:
            text = tr("SHIM");
            colorStyle = "color: red; font-weight: bold;";
            break;
        case ShimState::Real:
            text = tr("REAL");
            colorStyle = "color: green; font-weight: normal;";
            break;
        case ShimState::Blind:
        default:
            text = tr("BLIND");
            colorStyle = "color: #555555; font-weight: normal;"; // dark gray
            break;
    }
    shimStatusLabel_->setText(text);
    // The border IS the visible "rectangular box" -- shimStatusLabel_
    // already fills statusIndicatorArea_'s full geometry (see the
    // constructor), so this border traces exactly the region a
    // double-click anywhere inside registers on, not just the glyphs.
    // Sharp corners (no border-radius) -- see showGeneralStatus()'s own
    // comment for why: a rounded corner's unpainted corner pixel read as
    // a spurious gap at a flush window edge.
    shimStatusLabel_->setStyleSheet(QString("border: 1px solid gray; %1").arg(colorStyle));
}

void MainWindow::refreshShimStatus() {
    if (currentTarget_.isEmpty() || shimBusy_) {
        if (shimStatusDebugEnabled()) {
            std::fprintf(stderr, "[shimstatus] tick skipped (target empty=%d, busy=%d)\n",
                         currentTarget_.isEmpty() ? 1 : 0, shimBusy_ ? 1 : 0);
        }
        return;
    }
    shimBusy_ = true;
    QString target = currentTarget_;
    QString sshUser = sshUser_;
    QString sshKeyPath = sshKeyPath_;
    std::thread([this, target, sshUser, sshKeyPath]() {
        const bool debug = shimStatusDebugEnabled();
        const auto t0 = std::chrono::steady_clock::now();
        ShimState result = ShimState::Blind;
        // declineCredentials, not the real prompt -- see this function's
        // own header comment.
        const bool authed = sshSession_.ensureAuth(target, sshUser, sshKeyPath, declineCredentials);
        const auto t1 = std::chrono::steady_clock::now();
        bool queryOk = false;
        QString queryOut;
        if (authed) {
            auto res = sshSession_.run(
                target,
                QString("test -e %1 && echo SHIM || echo REAL").arg(kRealLibBackupPath),
                5000);
            queryOk = res.ok();
            queryOut = res.stdOut.trimmed();
            if (queryOk) {
                if (queryOut == "SHIM") {
                    result = ShimState::Shim;
                } else if (queryOut == "REAL") {
                    result = ShimState::Real;
                }
            }
        }
        if (debug) {
            const auto t2 = std::chrono::steady_clock::now();
            auto ms = [](auto a, auto b) -> long long {
                return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
            };
            std::fprintf(stderr,
                         "[shimstatus] target=%s ensureAuth=%s (%lldms) query=%s out=\"%s\" "
                         "(%lldms) total=%lldms -> %s\n",
                         target.toUtf8().constData(), authed ? "ok" : "FAILED", ms(t0, t1),
                         authed ? (queryOk ? "ok" : "FAILED") : "skipped",
                         queryOut.toUtf8().constData(), authed ? ms(t1, t2) : 0, ms(t0, t2),
                         result == ShimState::Shim ? "SHIM" : result == ShimState::Real ? "REAL" : "BLIND");
        }
        QMetaObject::invokeMethod(
            this,
            [this, result]() {
                shimBusy_ = false;
                applyShimState(result);
            },
            Qt::QueuedConnection);
    }).detach();
}

void MainWindow::maybeWarnAboutIpv6(const QString& target) {
    if (!target.contains(':')) {
        return; // not an IPv6 literal -- this known issue is IPv6-specific
    }
    camsyringe::Ipv6SupportProbe::checkAsync(
        sshSession_, target, sshUser_, sshKeyPath_,
        [this, target](bool ok, bool broken) {
            QMetaObject::invokeMethod(
                this,
                [this, target, ok, broken]() {
                    if (currentTarget_ != target) {
                        return; // stale -- user has since changed the target again
                    }
                    if (ok && broken) {
                        showGeneralStatus(
                            tr("Warning: %1 appears to hit this board's known IPv6 "
                               "socket-creation issue -- qcarcam_dispatcher's control channel "
                               "will likely fail to start. Consider an IPv4 target (Configure's "
                               "\"Force IPv4\") until the board is fixed.")
                                .arg(target),
                            /*isError=*/true);
                    }
                },
                Qt::QueuedConnection);
        });
}

void MainWindow::toggleShimStatus() {
    // Explicit requirement: double-clicks are ignored entirely while
    // Blind -- not connected, never queried, or a query/toggle is
    // already in flight (that in-flight window itself shows Blind, see
    // below), so there's nothing confirmed to toggle relative to.
    if (shimState_ == ShimState::Blind) {
        return;
    }
    if (currentTarget_.isEmpty() || shimBusy_) {
        return;
    }
    bool toShim = (shimState_ == ShimState::Real);

    shimBusy_ = true;
    applyShimState(ShimState::Blind); // "BLIND" while the toggle is in flight -- also blocks a second click
    QString target = currentTarget_;
    QString sshUser = sshUser_;
    QString sshKeyPath = sshKeyPath_;
    std::thread([this, target, sshUser, sshKeyPath, toShim]() {
        auto credentialsCb = [this, sshUser](const QString& t, QString* username, QString* password) {
            bool accepted = false;
            QMetaObject::invokeMethod(
                this,
                [this, t, sshUser, username, password, &accepted]() {
                    ui::SshCredentialsDialog dlg(t, sshUser, this);
                    if (dlg.exec() == QDialog::Accepted) {
                        *username = dlg.username();
                        *password = dlg.password();
                        accepted = true;
                    }
                },
                Qt::BlockingQueuedConnection);
            return accepted;
        };

        QString error;
        // Blind on failure, NOT "assume unchanged" -- a failed toggle
        // means the target's actual state is no longer confirmed (it may
        // have partially applied, e.g. the backup rename succeeded but
        // the cp didn't), so guessing the pre-toggle state back would
        // risk showing a confident answer that's wrong. Only flips to a
        // real answer once the remote command actually reports success.
        ShimState finalState = ShimState::Blind;
        if (!sshSession_.ensureAuth(target, sshUser, sshKeyPath, credentialsCb)) {
            error = tr("SSH authentication cancelled");
        } else if (toShim) {
            // No PC-side file/scp at all -- the shim travels ONLY as part
            // of the one injector bundle now (see
            // qcarcam-injector/release/create-qcarcam-inj-bundle.sh),
            // landing at kShimOnTargetPath when that bundle is installed
            // (InjectorBundleInstaller, same /var/opt convention it
            // already uses). This toggle just copies it from there into
            // place -- if it's missing, the fix is "install the bundle",
            // not "find/build a loose file on this PC", so that's what
            // the error says. Back up the real file ONLY if it hasn't
            // been already -- never overwrite an existing backup with a
            // second rename (would otherwise clobber the one true REAL
            // copy if this ever ran twice in a row without a REAL toggle
            // between). Then refresh the real-ref reference copy from
            // whichever board file is genuine right now (net/RealRefSync.h)
            // -- unconditional every toggle, unlike the backup rename,
            // since this one has no "clobbering the one true copy" risk.
            QString cmd = QString("%1; [ -e %2 ] || { echo %3; exit 1; }; "
                                   "[ -e %4 ] || mv %5 %4; "
                                   "%6; "
                                   "cp %2 %5 && chmod 555 %5")
                               .arg(kRemountRw, kShimOnTargetPath, kShimMissingMarker, kRealLibBackupPath,
                                    kRealLibPath, camsyringe::realRefSyncCommand());
            auto res = sshSession_.run(target, cmd, 15000);
            if (res.ok()) {
                finalState = ShimState::Shim;
            } else if (res.stdOut.contains(kShimMissingMarker)) {
                error = tr("libqcxclient_shim.so not found on target at %1 -- install the injector "
                           "bundle first (⇪ Install Injector).")
                            .arg(kShimOnTargetPath);
            } else {
                error = res.stdErr;
            }
        } else {
            // SHIM -> REAL: one atomic rename, removes the shim file as a
            // side effect -- see qcarcam-injector/ARCHITECTURE.md item 37.
            QString cmd =
                QString("%1; mv %2 %3").arg(kRemountRw, kRealLibBackupPath, kRealLibPath);
            auto res = sshSession_.run(target, cmd, 10000);
            if (res.ok()) {
                finalState = ShimState::Real;
            } else {
                error = res.stdErr;
            }
        }

        QMetaObject::invokeMethod(
            this,
            [this, finalState, error]() {
                shimBusy_ = false;
                applyShimState(finalState);
                if (!error.isEmpty()) {
                    showGeneralStatus(tr("SHIM/REAL toggle failed: %1").arg(error), /*isError=*/true);
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

} // namespace camsyringe::ui
