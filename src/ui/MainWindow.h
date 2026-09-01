#pragma once

#include <QImage>
#include <QMainWindow>
#include <QString>

#include <thread>
#include <vector>

#include "blf/BlfReplayer.h"
#include "net/DispatcherClient.h"
#include "net/TargetSsh.h"

class QGridLayout;
class QAction;
class QCloseEvent;
class QProgressBar;

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
// Play (from Idle, or after Stop) ALSO declares every configured camera
// (its QCarCam id + RTP port) to the target's qcarcam_dispatcher over a
// control-channel connection (DispatcherClient, see
// qcarcam-injector/ARCHITECTURE.md items 29/30 for the target-side half
// of this) -- but streaming itself (StreamPool::startAll()) starts
// FIRST, immediately, not gated on the target's response. This is
// deliberate, not a shortcut: the target's own receiver can't report
// readiness until it has actually received and probed real RTP data, so
// gating startAll() on the declaration completing would deadlock both
// sides waiting on each other (confirmed for real, the hard way, in this
// feature's own first end-to-end test -- see git history/ARCHITECTURE.md).
// The declaration therefore runs ASYNCHRONOUSLY alongside already-live
// streaming; once it resolves, a camera id the target rejected (ERROR)
// gets an error banner on its tile and StreamPool::setEnabled(i, false)
// (so a later Pause->Play resume within the same session skips
// restarting it) -- its already-running local encode/preview is
// deliberately left alone rather than torn down mid-stream (StreamPool
// has no per-camera stop, only stopAll()). If the control channel itself
// is unreachable, streaming still proceeds locally regardless -- this
// feature only adds target-side coordination on top of what already
// worked before it existed, never blocks it.
//
// Play FROM PAUSED (a resume) is deliberately DIFFERENT: startStreaming()
// skips declareToTarget()/checkInjectorVersion() entirely whenever the
// control connection is still alive (see its own `resuming` check) --
// Pause never closes that connection (see stopEverything() below)
// specifically so the target's whole per-camera session stays alive and
// untouched across a pause, and a resume just lets local streaming
// continue into it unchanged. Redeclaring here anyway (an earlier version
// of this code did) reconnects via DispatcherClient::declareAsync()'s own
// disconnect()-then-reconnect, which tears down and respawns the target's
// receiver/injector/viewer for no reason other than resuming -- confirmed
// for real as a visible blank-then-recover glitch on the target's own
// physical panel, plus a race with checkInjectorVersion()'s own probe
// connection (main_dispatcher.cpp accepts exactly one connection at a
// time, so that probe can only succeed by sneaking into the brief
// reconnect gap -- not a coincidence that the version notice used to
// appear specifically on a resume).
//
// The control connection is held open for the whole Playing/Paused
// session -- closing it (on Stop or window close; NOT Pause, see above)
// is itself the target-side teardown signal for that connection's own
// spawned per-camera injector/receiver/viewer processes (see
// main_dispatcher.cpp's stopSession()). Stop specifically (not Pause, not
// window close) goes further and kills qcarcam_dispatcher itself too, via
// SSH (see killTargetProcesses()/DispatcherRemoteControl) -- so nothing
// qcarcam-injector-related is left running on the target after Stop. The
// next Play then finds the control channel unreachable, and
// tryStartDispatcherThenRetry() SSHes back in to start it fresh before
// retrying -- the whole point being that a teammate never has to SSH into
// the target by hand for any of this.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // startImmediately: true for --playall (requires pool already have at
    // least one camera configured) -- starts streaming as soon as the
    // window is shown, no Play click needed. When pool->cameraCount() == 0
    // (no CLI video files given) and startImmediately is false, the
    // Camera dialog is opened automatically once the window is shown.
    explicit MainWindow(camsyringe::StreamPool* pool, QString initialTarget, int initialControlPort,
                         QString initialSshUser, bool initialInjectOnly, bool initialQcxBypass,
                         QString initialBlfPath, QString initialBlfInterface, bool startImmediately,
                         QWidget* parent = nullptr);
    ~MainWindow() override = default;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onPlayPauseTriggered();
    void onStopTriggered();
    void onConfigureTriggered();
    void onInstallInjectorTriggered();
    void onAboutTriggered();

private:
    enum class PlaybackState { Idle, Playing, Paused };

    void rebuildGrid();
    void applyState(PlaybackState state); // updates state_ + all actions' text/enabled
    void startStreaming();                // Idle/Paused -> Playing (starts pool_ AND declares, see above)
    // Playing/Paused -> Idle (or Playing -> Paused, see disconnectFromTarget):
    // always stops pool_ + BLF replay locally. disconnectFromTarget=true
    // (Stop, window close) additionally closes the control connection --
    // the target-side teardown signal for that connection's spawned
    // per-camera processes (main_dispatcher.cpp's stopSession()).
    // false (Pause) deliberately leaves it open: closing it would tear
    // down the target's injection session, which showed up as the
    // target's display going BLANK on Pause instead of freezing on its
    // last frame (the whole point of Pause) -- local streaming stopping
    // is what freezes camsyringe's own preview; the target should freeze
    // the same way, which only happens if its session stays alive.
    void stopEverything(bool disconnectFromTarget);
    void onPreviewFrame(int index, const QImage& frame);   // GUI thread only
    void onCameraError(int index, const QString& message); // GUI thread only
    // Builds the current declarations from pool_ and calls
    // dispatcherClient_.declareAsync() -- split out from startStreaming()
    // so onDeclareComplete()'s dispatcher-not-running retry (see below)
    // can call it again without duplicating this.
    void declareToTarget();
    // GUI thread only -- invoked (via QMetaObject::invokeMethod from
    // DispatcherClient's background-thread callback) once the target has
    // responded to every camera's declaration, or connecting itself failed.
    // Streaming is ALREADY running by the time this fires -- see class
    // comment for why this can't gate startAll() instead. previewIssues
    // (see PreviewIssue's own comment) never affects outcome handling
    // below -- surfaced separately via the status bar, since it's about
    // the TARGET's own on-panel preview, not injection itself.
    void onDeclareComplete(std::vector<CameraDeclareOutcome> outcomes,
                            std::vector<PreviewIssue> previewIssues, bool connectFailed,
                            QString connectError);
    // Called once (per Play press, see dispatcherStartAttempted_) from
    // onDeclareComplete() when the control channel is unreachable --
    // SSHes in (via sshSession_, prompting for credentials only if
    // passwordless fails) and starts qcarcam_dispatcher if it isn't
    // already running (DispatcherRemoteControl::ensureRunning(), which is
    // itself idempotent), then retries declareToTarget() exactly once
    // regardless of whether the SSH attempt itself succeeded -- so a
    // genuinely unreachable target (wrong IP, no SSH at all) still ends
    // up at the same "streaming locally, target unreachable" error banner
    // as before, just after one extra automatic attempt to fix it first.
    void tryStartDispatcherThenRetry();
    // Fire-and-forget: SSHes in (via sshSession_) and kills all four
    // qcarcam-injector binaries on the target -- called from
    // onStopTriggered() only (not Pause, not window close), so Stop
    // actually stops everything on the target too, not just locally.
    void killTargetProcesses();
    // Best-effort: fires alongside declareAsync() in startStreaming(), see
    // its own call site. Never blocks or fails Play -- see
    // DispatcherVersionProbe's own class comment.
    void checkInjectorVersion();
    // Runs InjectorBundleInstaller::installAsync() against `bundlePath`,
    // showing its confirm/credentials prompts via Qt::BlockingQueuedConnection
    // callbacks (the confirm dialog names the exact bundle file and lets
    // the user swap it for a different one right there, looping back to
    // itself until Install or Cancel), driving installProgressBar_ +
    // status bar text from its progress callback once real work starts,
    // and reporting its result: a status bar message either way, plus --
    // real failure only, not a plain Cancel -- a modal with the full
    // detail (selectable/copyable) so it isn't lost if the user moves on
    // to Play/Pause/Stop before reading it.
    void runInjectorInstall(const QString& bundlePath, const QString& bundleVersion);

    camsyringe::StreamPool* pool_ = nullptr;
    QString currentTarget_;
    int controlPort_ = 5000;
    // SSH username used for every TargetSsh::ensureAuth() call against
    // currentTarget_ (install, Play-time dispatcher-start retry, Stop's
    // target-process kill) -- set from `--target user@host` at launch or
    // Configure's own "SSH User" field, defaulting to "root" either way.
    // Editable ONLY through Configure (or a fresh launch), same as
    // currentTarget_ itself -- the Install confirmation dialog shows it
    // but deliberately can't change it, see runInjectorInstall()'s own
    // confirm callback.
    QString sshUser_ = "root";
    bool injectOnly_ = false;
    bool qcxBypass_ = false;
    // Empty blfPath_ means BLF/Ethernet replay is disabled this session --
    // startStreaming() checks that, not a separate enabled bool, same
    // convention as CameraConfigDialog::blfPath().
    QString blfPath_;
    QString blfInterface_;
    PlaybackState state_ = PlaybackState::Idle;
    camsyringe::DispatcherClient dispatcherClient_;
    // Owned here (not local to a function) so auth resolved once -- for
    // the Play-time dispatcher-start retry OR an "Install Injector" run,
    // whichever happens first -- is reused by the other for the rest of
    // this app session, instead of each separately probing passwordless
    // SSH or prompting for credentials. See TargetSsh's own class comment.
    camsyringe::TargetSsh sshSession_;
    // Reset at the start of every startStreaming() call (see there) --
    // caps tryStartDispatcherThenRetry() at one attempt per Play press.
    bool dispatcherStartAttempted_ = false;
    // Owned directly here (not inside StreamPool) since it's a single,
    // session-wide replay independent of camera count -- BlfReplayer.h's
    // own class comment covers why this needs its own thread the same way
    // StreamPool manages one thread per camera internally.
    camsyringe::BlfReplayer blfReplayer_;
    std::thread blfThread_;

    QGridLayout* grid_ = nullptr;
    QAction* playPauseAction_ = nullptr;
    QAction* stopAction_ = nullptr;
    QAction* configureAction_ = nullptr;
    QAction* installInjectorAction_ = nullptr;
    // Lives in the status bar (addPermanentWidget(), bottom-right, next to
    // the message text set alongside it) -- hidden except during an
    // active "Install Injector" run. Determinate (fixed step percentages,
    // see InjectorBundleInstaller::ProgressCallback) except during the
    // SCP transfer step, shown as indeterminate/busy (range(0,0)) since
    // there's no real byte-level progress available without a tty.
    QProgressBar* installProgressBar_ = nullptr;
    std::vector<CameraWidget*> cameraWidgets_;
};

} // namespace camsyringe::ui
