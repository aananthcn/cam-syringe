#include "ui/MainWindow.h"

#include "camera/PortScheme.h"
#include "camera/StreamPool.h"
#include "net/DispatcherRemoteControl.h"
#include "net/DispatcherVersionProbe.h"
#include "net/InjectorBundleInstaller.h"
#include "ui/CameraConfigDialog.h"
#include "ui/CameraWidget.h"
#include "ui/SshCredentialsDialog.h"
#include "util/InjectorBundleFinder.h"

#include <QAction>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
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
                        QString initialSshUser, bool initialInjectOnly, bool initialQcxBypass,
                        QString initialBlfPath, QString initialBlfInterface, bool startImmediately,
                        QWidget* parent)
    : QMainWindow(parent),
      pool_(pool),
      currentTarget_(std::move(initialTarget)),
      controlPort_(initialControlPort),
      sshUser_(std::move(initialSshUser)),
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

    // Target-maintenance action, independent of Play/Pause/Idle state --
    // enabled whenever a target is configured, not gated to Idle the way
    // configureAction_ is.
    installInjectorAction_ = menuBar()->addAction(tr("⇪ Install Injector"));
    connect(installInjectorAction_, &QAction::triggered, this, &MainWindow::onInstallInjectorTriggered);
    installInjectorAction_->setEnabled(!currentTarget_.isEmpty());

    QMenu* helpMenu = menuBar()->addMenu(tr("Help"));
    QAction* aboutAction = helpMenu->addAction(tr("About"));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onAboutTriggered);

    // Bottom of the main window (status bar), permanent widget so it sits
    // to the right of showMessage()'s own text instead of being replaced
    // by it -- hidden except during an active install, see
    // runInjectorInstall()'s progress callback.
    installProgressBar_ = new QProgressBar(this);
    installProgressBar_->setFixedWidth(160);
    installProgressBar_->setRange(0, 100);
    installProgressBar_->setTextVisible(true);
    installProgressBar_->setVisible(false);
    statusBar()->addPermanentWidget(installProgressBar_);

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
    // Captured before applyState() below overwrites state_. True only for
    // an actual Pause->Play resume where the control connection (and thus
    // the target's whole per-camera session) is CONFIRMED still alive --
    // exactly the case stopEverything(false) was built for, see its own
    // comment. Deliberately re-checks isConnected() rather than assuming
    // "Paused implies still connected": if the connection somehow died on
    // its own during the pause (target crash/reboot, network drop), this
    // falls through to a full redeclare below instead of resuming into a
    // dead connection and streaming into nothing.
    const bool resuming = state_ == PlaybackState::Paused && dispatcherClient_.isConnected();

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

    if (resuming) {
        // Resuming into the SAME still-alive target session, deliberately
        // untouched -- no redeclare, no version re-probe. Confirmed for
        // real this matters, not just tidiness: redeclaring here would
        // run DispatcherClient::declareAsync()'s own disconnect()-then-
        // reconnect against the very connection stopEverything(false)
        // just went out of its way to keep open, tearing down and
        // respawning the target's receiver/injector/viewer for no reason
        // other than resuming -- visible on the target's own physical
        // panel as a brief blank-then-recover glitch (real-hardware bug
        // report). checkInjectorVersion() is skipped for the same
        // reason: its own short-lived probe connection can only ever
        // succeed by racing into the brief gap that reconnect creates --
        // main_dispatcher.cpp accepts exactly one connection at a time,
        // see its own accept() loop -- which is also why that notice
        // showed up specifically ON a resume, not a coincidence.
        return;
    }

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
        currentTarget_.toStdString(), controlPort_, std::move(declarations), injectOnly_, qcxBypass_,
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

void MainWindow::tryStartDispatcherThenRetry() {
    QString target = currentTarget_;
    int controlPort = controlPort_;
    QString sshUser = sshUser_;
    std::thread([this, target, controlPort, sshUser]() {
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
        if (sshSession_.ensureAuth(target, sshUser, credentialsCb)) {
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
        statusBar()->setStyleSheet("color: red;");
        statusBar()->showMessage(
            tr("Target's local preview failed for cam id(s) %1 -- injection itself is fine, only "
               "the target's own on-panel display for %2 %3 affected (see "
               "/tmp/qcarcam_dispatcher_previewer.log on target).")
                .arg(ids.join(", "))
                .arg(ids.size() == 1 ? tr("that camera") : tr("those cameras"))
                .arg(ids.size() == 1 ? tr("is") : tr("are")));
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
    statusBar()->setStyleSheet(QString());
    statusBar()->clearMessage();
    applyState(PlaybackState::Idle);
    if (!currentTarget_.isEmpty()) {
        killTargetProcesses();
    }
}

void MainWindow::killTargetProcesses() {
    QString target = currentTarget_;
    QString sshUser = sshUser_;
    std::thread([this, target, sshUser]() {
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

        if (!sshSession_.ensureAuth(target, sshUser, credentialsCb)) {
            return; // user cancelled the credentials prompt -- nothing more to do
        }
        QString error;
        if (!camsyringe::DispatcherRemoteControl::stopAll(sshSession_, target, &error)) {
            QMetaObject::invokeMethod(
                this,
                [this, error]() {
                    statusBar()->setStyleSheet("color: red;");
                    statusBar()->showMessage(tr("Couldn't stop target processes: %1").arg(error));
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

    CameraConfigDialog dialog(currentTarget_, controlPort_, sshUser_, currentFiles, currentCamIds,
                               injectOnly_, qcxBypass_, blfPath_, blfInterface_, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    currentTarget_ = dialog.target();
    installInjectorAction_->setEnabled(!currentTarget_.isEmpty());
    controlPort_ = dialog.controlPort();
    sshUser_ = dialog.sshUser();
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
    statusBar()->setStyleSheet(QString());
    statusBar()->showMessage(tr("Preparing injector install on %1…").arg(currentTarget_));
    runInjectorInstall(bundlePath, version);
}

void MainWindow::runInjectorInstall(const QString& bundlePath, const QString& bundleVersion) {
    camsyringe::InjectorBundleInstaller::installAsync(
        sshSession_, currentTarget_, controlPort_, sshUser_, bundlePath, bundleVersion,
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
                    for (;;) {
                        QMessageBox box(QMessageBox::Question, tr("Install Injector Bundle"),
                                         tr("Target: %1\nUser: %2\n\n"
                                            "This will remove the existing injector install at "
                                            "%1:/var/opt (bin/lib/include), install v%3, and "
                                            "restart qcarcam_dispatcher.\n\nBundle file:\n%4")
                                             .arg(target, user, *bundleVersion, *bundlePath),
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
                        statusBar()->setStyleSheet(QString());
                        statusBar()->showMessage(
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
                    statusBar()->setStyleSheet(QString());
                    statusBar()->showMessage(label);
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
                    statusBar()->setStyleSheet(success || cancelled ? QString() : "color: red;");
                    statusBar()->showMessage(summary);
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

} // namespace camsyringe::ui
