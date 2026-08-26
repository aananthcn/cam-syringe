#include "ui/MainWindow.h"

#include "camera/PortScheme.h"
#include "camera/StreamPool.h"
#include "ui/CameraConfigDialog.h"
#include "ui/CameraWidget.h"

#include <QAction>
#include <QCloseEvent>
#include <QFileInfo>
#include <QGridLayout>
#include <QMenuBar>
#include <QMetaObject>
#include <QStatusBar>
#include <QTimer>
#include <QWidget>

#include <cmath>

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

} // namespace

MainWindow::MainWindow(camsyringe::StreamPool* pool, QString initialTarget, int initialControlPort,
                        bool initialInjectOnly, bool initialQcxBypass, QString initialBlfPath,
                        QString initialBlfInterface, bool startImmediately, QWidget* parent)
    : QMainWindow(parent),
      pool_(pool),
      currentTarget_(std::move(initialTarget)),
      controlPort_(initialControlPort),
      injectOnly_(initialInjectOnly),
      qcxBypass_(initialQcxBypass),
      blfPath_(std::move(initialBlfPath)),
      blfInterface_(std::move(initialBlfInterface)) {
    auto* central = new QWidget(this);
    grid_ = new QGridLayout(central);
    setCentralWidget(central);
    setWindowTitle(tr("CamSyringe"));

    playPauseAction_ = menuBar()->addAction(tr("▶ Play"));
    connect(playPauseAction_, &QAction::triggered, this, &MainWindow::onPlayPauseTriggered);

    stopAction_ = menuBar()->addAction(tr("⏹ Stop"));
    connect(stopAction_, &QAction::triggered, this, &MainWindow::onStopTriggered);

    configureAction_ = menuBar()->addAction(tr("⚙ Configure"));
    connect(configureAction_, &QAction::triggered, this, &MainWindow::onConfigureTriggered);

    rebuildGrid();
    applyState(PlaybackState::Idle);

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
            configureAction_->setEnabled(true);
            break;
        case PlaybackState::Playing:
            playPauseAction_->setText(tr("⏸ Pause"));
            playPauseAction_->setEnabled(true);
            stopAction_->setEnabled(true);
            configureAction_->setEnabled(false);
            break;
        case PlaybackState::Paused:
            playPauseAction_->setText(tr("▶ Play"));
            playPauseAction_->setEnabled(true);
            stopAction_->setEnabled(true);
            configureAction_->setEnabled(false);
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
        statusBar()->setStyleSheet(QString());
        statusBar()->clearMessage();
        if (blfReplayer_.open(blfPath_.toStdString(), blfInterface_.toStdString())) {
            blfReplayer_.setStartOrigin(pool_->timelineOriginNs());
            blfThread_ = std::thread([this] { blfReplayer_.run(); });
        } else {
            // Also logged to stderr by BlfReplayer::open() itself; this is
            // the GUI-visible equivalent, since a GUI session may have no
            // visible console. Disabled for the rest of this session (no
            // retry on a later Pause->Play) -- re-enable via Configure.
            statusBar()->setStyleSheet("color: red;");
            statusBar()->showMessage(
                tr("BLF replay disabled: %1").arg(QString::fromStdString(blfReplayer_.lastError())));
            blfPath_.clear();
        }
    }

    for (auto* widget : cameraWidgets_) {
        widget->showStatus(tr("Confirming target injection…"));
    }

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
        currentTarget_.toStdString(), controlPort_, std::move(declarations), injectOnly_, qcxBypass_,
        [this](std::vector<CameraDeclareOutcome> outcomes, bool connectFailed,
               std::string connectError) {
            QString qConnectError = QString::fromStdString(connectError);
            QMetaObject::invokeMethod(
                this,
                [this, outcomes = std::move(outcomes), connectFailed, qConnectError]() mutable {
                    onDeclareComplete(std::move(outcomes), connectFailed, qConnectError);
                },
                Qt::QueuedConnection);
        });
}

void MainWindow::onDeclareComplete(std::vector<CameraDeclareOutcome> outcomes, bool connectFailed,
                                    QString connectError) {
    if (state_ != PlaybackState::Playing) {
        // Stop/Pause was pressed (or the window closed) while the
        // declaration was in flight -- stopEverything() already called
        // dispatcherClient_.disconnect(), tearing down whatever this call
        // was doing; nothing left to apply.
        return;
    }

    if (connectFailed) {
        // Streaming is ALREADY running locally (see startStreaming()) --
        // a control-channel problem doesn't stop it, it just means the
        // target never got auto-configured for it. Surfaced as an error
        // so it's not silently invisible, but this is not fatal to local
        // operation the way it would have been if this gated startAll().
        for (auto* widget : cameraWidgets_) {
            widget->showError(
                tr("Target control-channel unreachable: %1 (streaming locally regardless -- the "
                   "target won't inject unless it was already configured some other way)")
                    .arg(connectError));
        }
        dispatcherClient_.disconnect(); // joins the now-finished background thread
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
}

void MainWindow::stopEverything() {
    pool_->stopAll();
    dispatcherClient_.disconnect(); // the target-side teardown signal
    blfReplayer_.requestStop();
    if (blfThread_.joinable()) {
        blfThread_.join();
    }
}

void MainWindow::onPlayPauseTriggered() {
    if (state_ == PlaybackState::Playing) {
        stopEverything();
        applyState(PlaybackState::Paused);
        return;
    }
    startStreaming();
}

void MainWindow::onStopTriggered() {
    stopEverything();
    for (auto* widget : cameraWidgets_) {
        widget->resetIdle();
        widget->clearError();
    }
    statusBar()->setStyleSheet(QString());
    statusBar()->clearMessage();
    applyState(PlaybackState::Idle);
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

    CameraConfigDialog dialog(currentTarget_, controlPort_, currentFiles, currentCamIds, injectOnly_,
                               qcxBypass_, blfPath_, blfInterface_, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    currentTarget_ = dialog.target();
    controlPort_ = dialog.controlPort();
    injectOnly_ = dialog.injectOnly();
    qcxBypass_ = dialog.qcxBypass();
    blfPath_ = dialog.blfPath();
    blfInterface_ = dialog.blfInterface();
    QStringList files = dialog.videoFiles();
    std::vector<int> camIds = dialog.camIds();

    pool_->clearCameras();
    for (int i = 0; i < files.size(); ++i) {
        int port = camsyringe::kBasePort + i * camsyringe::kPortStep;
        CameraConfig cfg;
        cfg.inputPath = files[i].toStdString();
        cfg.destUrl = ("rtp://" + currentTarget_ + ":" + QString::number(port)).toStdString();
        cfg.label = ("cam" + QString::number(i)).toStdString();
        cfg.index = i;
        cfg.camId = camIds[static_cast<size_t>(i)];
        cfg.port = port;
        pool_->addCamera(std::move(cfg));
    }

    rebuildGrid();
    applyState(PlaybackState::Idle);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    stopEverything();
    QMainWindow::closeEvent(event);
}

} // namespace camsyringe::ui
