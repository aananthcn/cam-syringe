#pragma once

#include <QImage>
#include <QMainWindow>
#include <QString>

#include <vector>

#include "net/DispatcherClient.h"

class QGridLayout;
class QAction;
class QCloseEvent;

namespace camsyringe {
class StreamPool;
}

namespace camsyringe::ui {

class CameraWidget;

// Owns (a pointer to) a StreamPool and shows one CameraWidget per
// configured camera in a grid. Three menu-bar actions are the only
// controls: Play/Pause, Stop, and Configure -- see CONTEXT.md's
// "UI requirements" for the exact state model.
//
// Play ALSO declares every configured camera (its QCarCam id + RTP port)
// to the target's qcarcam_dispatcher over a control-channel connection
// (DispatcherClient, see qcarcam-injector/ARCHITECTURE.md items 29/30 for
// the target-side half of this) -- but streaming itself (StreamPool::
// startAll()) starts FIRST, immediately, not gated on the target's
// response. This is deliberate, not a shortcut: the target's own receiver
// can't report readiness until it has actually received and probed real
// RTP data, so gating startAll() on the declaration completing would
// deadlock both sides waiting on each other (confirmed for real, the hard
// way, in this feature's own first end-to-end test -- see git history/
// ARCHITECTURE.md). The declaration therefore runs ASYNCHRONOUSLY
// alongside already-live streaming; once it resolves, a camera id the
// target rejected (ERROR) gets an error banner on its tile and
// StreamPool::setEnabled(i, false) (so a later Pause->Play cycle within
// the same session skips restarting it) -- its already-running local
// encode/preview is deliberately left alone rather than torn down
// mid-stream (StreamPool has no per-camera stop, only stopAll()). If the
// control channel itself is unreachable, streaming still proceeds
// locally regardless -- this feature only adds target-side coordination
// on top of what already worked before it existed, never blocks it.
// The control connection is held open for the whole Playing/Paused
// session -- closing it (on Stop, on Pause, or on window close) is itself
// the target-side teardown signal.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // startImmediately: true for --playall (requires pool already have at
    // least one camera configured) -- starts streaming as soon as the
    // window is shown, no Play click needed. When pool->cameraCount() == 0
    // (no CLI video files given) and startImmediately is false, the
    // Camera dialog is opened automatically once the window is shown.
    explicit MainWindow(camsyringe::StreamPool* pool, QString initialTarget, int initialControlPort,
                         bool initialInjectOnly, bool initialQcxBypass, bool startImmediately,
                         QWidget* parent = nullptr);
    ~MainWindow() override = default;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onPlayPauseTriggered();
    void onStopTriggered();
    void onConfigureTriggered();

private:
    enum class PlaybackState { Idle, Playing, Paused };

    void rebuildGrid();
    void applyState(PlaybackState state); // updates state_ + all actions' text/enabled
    void startStreaming();                // Idle/Paused -> Playing (starts pool_ AND declares, see above)
    void stopEverything();                // Playing/Paused -> Idle: pool + dispatcher client
    void onPreviewFrame(int index, const QImage& frame);   // GUI thread only
    void onCameraError(int index, const QString& message); // GUI thread only
    // GUI thread only -- invoked (via QMetaObject::invokeMethod from
    // DispatcherClient's background-thread callback) once the target has
    // responded to every camera's declaration, or connecting itself failed.
    // Streaming is ALREADY running by the time this fires -- see class
    // comment for why this can't gate startAll() instead.
    void onDeclareComplete(std::vector<CameraDeclareOutcome> outcomes, bool connectFailed,
                            QString connectError);

    camsyringe::StreamPool* pool_ = nullptr;
    QString currentTarget_;
    int controlPort_ = 5000;
    bool injectOnly_ = false;
    bool qcxBypass_ = false;
    PlaybackState state_ = PlaybackState::Idle;
    camsyringe::DispatcherClient dispatcherClient_;

    QGridLayout* grid_ = nullptr;
    QAction* playPauseAction_ = nullptr;
    QAction* stopAction_ = nullptr;
    QAction* configureAction_ = nullptr;
    std::vector<CameraWidget*> cameraWidgets_;
};

} // namespace camsyringe::ui
