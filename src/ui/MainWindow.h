#pragma once

#include <QDateTime>
#include <QImage>
#include <QMainWindow>
#include <QMap>
#include <QString>

#include <thread>
#include <vector>

#include "blf/BlfReplayer.h"
#include "net/CameraGeometryResolver.h"
#include "net/DispatcherClient.h"
#include "net/TargetConnectivityMonitor.h"
#include "net/TargetSsh.h"

class QGridLayout;
class QAction;
class QCloseEvent;
class QEvent;
class QLabel;
class QMenu;
class QObject;
class QProgressBar;
class QTimer;
class QWidget;

namespace camsyringe {
class StreamPool;
}

namespace camsyringe::ui {

class CameraWidget;

// Owns (a pointer to) a StreamPool and shows one CameraWidget per
// configured camera in a grid. Three menu-bar controls: Play/Pause,
// Stop, and a "Settings" menu with two submenus -- "CamSyringe" (opens
// CameraConfigDialog, the session settings this class comment otherwise
// still calls "Configure") and "Camera" (opens CameraSettingsDialog, a
// read-only viewer of each configured camera's real target-authored
// resolution -- see onCameraSettingsTriggered()) -- see CONTEXT.md's
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
// Play FROM PAUSED (a resume) ALSO calls declareToTarget()/
// checkInjectorVersion(), same as a fresh Play -- an earlier version of
// this code skipped both whenever the control connection was still alive,
// specifically to avoid a visible blank-then-recover glitch on the
// target's own physical panel (redeclaring reconnects via
// DispatcherClient::declareAsync()'s own disconnect()-then-reconnect,
// which makes qcarcam_dispatcher tear down and respawn the target's
// receiver/injector/viewer). That optimization was REMOVED after a
// real-hardware bug report: Pause (stopEverything(false), below) stops
// local RTP send but deliberately leaves the target's receiver process --
// and its open hardware H.264 decoder session -- running and idle for the
// whole pause; resuming into that SAME session feeds a brand-new RTP/
// MPEGTS stream (fresh SPS/PPS) into an already-open, gone-idle decoder,
// which was confirmed live to wedge the target's Venus/vidc hardware
// decoder badly enough that no software restart (qcarcam_test, CamSyringe,
// even a full dispatcher Stop/Play) recovered it -- only a target power
// cycle did. Redeclaring on every resume gives the old receiver a
// graceful SIGINT (main_dispatcher.cpp's stopPid(), called from
// stopSession()) before a fresh one opens a new decoder session, avoiding
// the wedge; the reintroduced cosmetic glitch (and checkInjectorVersion()'s
// probe racing the reconnect gap, same as a fresh Play) is strictly
// preferable to a hardware hang that needs a power cycle to clear.
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
                         QString initialSshUser, QString initialSshKeyPath, bool initialInjectOnly,
                         QString initialBlfPath, QString initialBlfInterface,
                         bool startImmediately, QWidget* parent = nullptr);
    ~MainWindow() override = default;

protected:
    void closeEvent(QCloseEvent* event) override;
    // Two unrelated uses: (1) catches a double-click on shimStatusLabel_
    // (a plain QLabel has no such signal of its own -- see
    // toggleShimStatus()), (2) catches statusBar()'s own resize events to
    // keep statusIndicatorArea_ pinned to its true right edge (see its
    // own comment) -- see this override's own definition and each
    // widget's installEventFilter(this) call in the constructor.
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onPlayPauseTriggered();
    void onStopTriggered();
    void onConfigureTriggered();
    void onCameraSettingsTriggered();
    void onInstallInjectorTriggered();
    void onAboutTriggered();

private:
    enum class PlaybackState { Idle, Playing, Paused };
    // Mirrors the target's own `/mnt/lib64/camera/libqcxclient.so` state --
    // see qcarcam-injector/ARCHITECTURE.md item 37. Blind ("target not
    // connected", shown as dark gray "BLIND") covers "never queried yet",
    // "target unreachable/no target configured", and "a query or toggle
    // is currently in flight" -- shimStatusLabel_ never guesses REAL by
    // default, and (see toggleShimStatus()) double-clicks are ignored
    // entirely while in this state -- there's nothing confirmed to toggle
    // relative to yet.
    enum class ShimState { Blind, Real, Shim };

    void rebuildGrid();
    void applyState(PlaybackState state); // updates state_ + all actions' text/enabled
    // Idle/Paused -> Playing: starts pool_ (local streaming) unconditionally
    // and immediately, same as always -- see class comment for why that
    // can never wait on the target. The target-communication half
    // (declareToTarget()/checkInjectorVersion()) is gated behind
    // beginReachabilityCheck() instead of running immediately -- see its
    // own comment.
    void startStreaming();
    // Hard, dedicated pre-flight gate in front of declareToTarget()/
    // checkInjectorVersion() (called from startStreaming(), and nowhere
    // else) -- NOT in front of pool_->startAll(), which already ran by the
    // time this is called. Asks connectivityMonitor_.probeNow() (a ping,
    // see its own class comment for why NOT a control-port connect --
    // shared with refreshShimStatus(), see ADR 0005) and starts
    // reachabilityTimeoutTimer_ (kReachabilityTimeoutMs) as a belt-and-
    // suspenders UI-facing cutoff for it; shows a "searching" status-bar
    // message for the whole window (see showGeneralStatus()). Whichever
    // comes first -- the probe's own result (onReachabilityCheckResult())
    // or the timer elapsing (calls autoStopOnUnreachable() directly) --
    // decides the outcome; the other is then stale and ignored (see
    // reachabilityCheckToken_'s own comment). probeNow() answers
    // synchronously, on this same call stack, whenever connectivityMonitor_
    // already has a confirmed answer (the common case -- it's been
    // ticking since launch/Configure) -- the "searching" phase is only
    // ever actually visible for the rare Unknown case (Play pressed
    // within ~1-2s of launch/Configure, before its first probe resolved).
    void beginReachabilityCheck();
    // GUI thread only -- invoked (via connectivityMonitor_.probeNow()'s
    // own callback, itself always GUI-thread-marshaled) once a
    // connectivity answer is available, immediately or otherwise.
    // Discards a stale result (see reachabilityCheckToken_'s own comment)
    // rather than acting on it. reachable==false hands off to
    // autoStopOnUnreachable() and returns without ever calling
    // declareToTarget()/checkInjectorVersion().
    void onReachabilityCheckResult(bool reachable, int token);
    // Disables (busy=true) or restores (busy=false) every menu action this
    // window has, for the duration of a reachability check -- called from
    // beginReachabilityCheck()/onReachabilityCheckResult()/
    // cancelReachabilityCheck() so nothing is clickable while the search
    // is in flight. Replaces an earlier approach (an app-wide event
    // filter intercepting clicks and popping a "do you want me to stop?"
    // confirmation) that caused two confirmed, reproducible GUI hangs
    // this session -- disabling the controls outright removes the
    // trigger for both instead of working around it. busy=false restores
    // playPauseAction_/stopAction_/camSyringeSettingsAction_ via
    // applyState(state_) (their normal, PlaybackState-driven formula) and
    // installInjectorAction_/cameraSettingsAction_ via their own
    // independent !currentTarget_.isEmpty() formula (same one
    // onConfigureTriggered() uses) -- NOT a plain "re-enable everything",
    // since either action set legitimately stays disabled in some states.
    void setControlsBusyForReachabilityCheck(bool busy);
    // Silently ends an in-flight reachability check -- called only from
    // stopEverything() (a no-op if no check is active, so safe
    // unconditionally on every Stop/Pause/window-close). Reaching this
    // WHILE a check is still active only happens via autoStopOnUnreachable()'s
    // own onStopTriggered() call (user-facing Play/Pause/Stop are all
    // disabled for the check's duration, see setControlsBusyForReachabilityCheck())
    // or a window close mid-search; either way there is no status message
    // to show here -- autoStopOnUnreachable() shows its own, and a window
    // close shows nothing at all. Bumps reachabilityCheckToken_ so the
    // probe's own eventual callback (still running in the background --
    // there is no live handle to cancel it with, same limitation
    // TargetSsh's SSH-based probes already have) is recognized as stale
    // and ignored when it does arrive.
    void cancelReachabilityCheck();
    // Runs a full Stop (onStopTriggered() -- local streaming, target
    // connection, and target-side processes all torn down) when the
    // reachability check concludes that the target isn't there at all,
    // then re-shows `message` as a status-bar error (onStopTriggered()'s
    // own clearGeneralStatus() would otherwise silently wipe it). Called
    // from onReachabilityCheckResult() (the probe itself says
    // unreachable) and the 2s timeout timer (neither answer arrived in
    // time) -- user-specified: getting the target address wrong at Play
    // time should tear the session down automatically, the same as
    // pressing Stop by hand, rather than leaving local streaming running
    // against a target that was never there. Deliberately NOT the same
    // policy onDeclareComplete()'s connectFailed branch uses (streams
    // locally regardless) -- that branch is for a target confirmed
    // reachable at Play time whose actual declaration then fails, a
    // materially different, later situation.
    void autoStopOnUnreachable(const QString& message);
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
    // Fires from onConfigureTriggered() once Configure is accepted --
    // resolves each configured camera id's REAL, target-authored
    // resolution (see net/CameraGeometryResolver.h) for whichever ids
    // aren't already in geometryCache_[currentTarget_] ("once per
    // target", per its own comment). Never blocks Configure's own
    // dialog/Idle-state flow -- same never-block philosophy as
    // checkInjectorVersion(). No-op if currentTarget_ is empty or every
    // configured id is already cached for it.
    void resolveCameraGeometry();
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
    // Sets generalStatusLabel_'s text/color -- the single place every
    // status-bar-message call site in this file goes through now (see
    // generalStatusLabel_'s own comment for why this replaced plain
    // statusBar()->showMessage()/setStyleSheet()). isError selects red vs.
    // the label's normal (unstyled) text color; the border stays the same
    // either way.
    void showGeneralStatus(const QString& message, bool isError = false);
    // Equivalent of the old statusBar()->clearMessage() -- empties
    // generalStatusLabel_ and resets its color, keeping the border.
    void clearGeneralStatus();
    // Best-effort SSH query of the target's current SHIM/REAL state (see
    // ShimState above) -- passwordless-only, NEVER prompts for
    // credentials (unlike toggleShimStatus() below): this can fire from
    // window-show or a Configure apply, neither of which is a deliberate
    // "talk to the target now" action the way a double-click is, so it
    // must never pop a credentials dialog unprompted. A passwordless
    // failure just leaves the label at Blind ("BLIND", dark gray) --
    // note a toggle click can't fix that itself, since toggleShimStatus()
    // ignores clicks entirely while Blind; recovering needs a working
    // automatic refresh (Configure re-apply, or relaunching). No-op
    // (leaves state at Blind) if currentTarget_ is empty or a
    // query/toggle is already in flight (shimBusy_).
    void refreshShimStatus();
    // Best-effort, passwordless-only (same convention as
    // refreshShimStatus() just above -- an automatic check, not a
    // deliberate user action, must never prompt for credentials) live
    // functional check of whether `target` would hit this board's known
    // AF_INET6 socket-creation issue (net/Ipv6SupportProbe.h, docs/adr/
    // 0001-interim-ipv4-default-while-board-ipv6-broken.md) -- only runs
    // at all when `target` looks like an IPv6 literal (contains ':'),
    // since the known issue is IPv6-specific and an IPv4 target can
    // never hit it. Fire-and-forget: posts a status-bar warning via
    // showGeneralStatus() if the probe conclusively finds the issue,
    // otherwise does nothing (no "all clear" message either -- silence
    // is the normal, expected state). A stale in-flight probe from a
    // target the user has since changed away from is discarded (checked
    // against currentTarget_ when the result comes back), never shown
    // against the wrong target.
    void maybeWarnAboutIpv6(const QString& target);
    // Same convention as maybeWarnAboutIpv6() just above -- best-effort,
    // passwordless-only, fire-and-forget proactive check, this time for
    // the board's real QCX driver reporting an API version incompatible
    // with this build (net/QcxVersionProbe.h, qcarcam-injector's docs/
    // adr/0002-qcx-client-api-version-must-match-board.md). Runs
    // regardless of the target's family (unlike the IPv6 check, this
    // isn't IPv6-specific) and regardless of whether any configured
    // camera's resolution flow happens to touch the real driver at all
    // -- deliberately NOT folded into resolveCameraGeometry()'s own
    // per-camera "unknown" reporting, which can legitimately stay silent
    // even when this mismatch exists (a camera with a static XML
    // resolution never needs the live query that would hit it).
    // Warns, does not block Play -- confirmed live that this mismatch
    // does not affect the actual injection pipeline (the shim's own
    // QCarCamInitialize() ignores apiVersion for every caller); see the
    // ADR for the full reasoning and when blocking WOULD be correct.
    void maybeWarnAboutQcxVersion(const QString& target);
    // Double-click handler for shimStatusLabel_ (see eventFilter()) --
    // a no-op while shimState_ is Blind (nothing confirmed to toggle
    // relative to -- explicit requirement, not an oversight). Otherwise
    // performs the OPPOSITE-direction rename on the target from
    // shimState_ and updates the label from the actual confirmed result,
    // not an optimistic guess (any failure along the way -- auth
    // cancelled, SSH error, shim payload missing -- leaves the label at
    // Blind, not a guessed "unchanged"). Unlike Install Injector, no
    // confirmation dialog -- a rename is cheap to reverse with a second
    // double-click, and the whole point of this control is one
    // frictionless click per validation session. DOES prompt for SSH
    // credentials on passwordless failure (via sshSession_, same
    // SshCredentialsDialog convention as killTargetProcesses()) -- this
    // IS a deliberate user action. No PC<->target file transfer happens
    // during a toggle, in either direction -- REAL->SHIM copies from
    // kShimOnTargetPath (in the .cpp), where the ONE injector bundle
    // already installs the shim alongside the real binaries; if that
    // bundle was never installed on this target, the toggle fails with a
    // status-bar message pointing at Install Injector rather than trying
    // to source the file from this PC.
    void toggleShimStatus();
    // GUI-thread only -- sets shimState_ and shimStatusLabel_'s
    // text/color together, the only place either is touched, so they
    // can never disagree.
    void applyShimState(ShimState state);

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
    // Optional SSH private key path, passed to every TargetSsh::ensureAuth()
    // call alongside sshUser_ -- empty (the default) means "no explicit
    // key, use ssh's own default identity/agent", exactly today's
    // behavior. Set from Configure's own "SSH key" field or --ssh-key;
    // same editable-only-through-Configure convention as sshUser_.
    QString sshKeyPath_;
    bool injectOnly_ = false;
    // Empty blfPath_ means BLF/Ethernet replay is disabled this session --
    // startStreaming() checks that, not a separate enabled bool, same
    // convention as CameraConfigDialog::blfPath().
    QString blfPath_;
    QString blfInterface_;
    // Last known result of maybeWarnAboutQcxVersion()'s probe for
    // currentTarget_ -- reset to false at the START of every Configure
    // apply (before the fresh async probe for the new target even
    // starts), so a stale true from a PREVIOUS target can never block
    // Play against a new one before its own probe has had a chance to
    // run. Explicit REQUIREMENT (not just a warning): with injectOnly_
    // true, this blocks startStreaming() outright, and actively stops an
    // already-Playing/Paused session the moment the probe confirms it --
    // see qcarcam-injector's docs/adr/0002-qcx-client-api-version-must-
    // match-board.md's recorded blocking policy for why this is
    // conditioned on injectOnly_ specifically (with it unchecked, the
    // target's own local Screen/EGL preview render is a still-valid,
    // independent reason to keep going).
    bool qcxVersionMismatch_ = false;
    PlaybackState state_ = PlaybackState::Idle;
    camsyringe::DispatcherClient dispatcherClient_;
    // Owned here (not local to a function) so auth resolved once -- for
    // the Play-time dispatcher-start retry OR an "Install Injector" run,
    // whichever happens first -- is reused by the other for the rest of
    // this app session, instead of each separately probing passwordless
    // SSH or prompting for credentials. See TargetSsh's own class comment.
    camsyringe::TargetSsh sshSession_;
    // Single shared "is currentTarget_ even alive" fact, owned here --
    // both refreshShimStatus() (BLIND is now DERIVED from this, see its
    // own comment) and beginReachabilityCheck() depend on it instead of
    // each running an independent liveness probe (see the class's own
    // header comment, and ADR 0005). Started/restarted in the
    // constructor and onConfigureTriggered() whenever currentTarget_ is
    // set.
    camsyringe::TargetConnectivityMonitor connectivityMonitor_;
    // Reset at the start of every startStreaming() call (see there) --
    // caps tryStartDispatcherThenRetry() at one attempt per Play press.
    bool dispatcherStartAttempted_ = false;
    // Hard UI-facing budget for beginReachabilityCheck()'s probe --
    // user-specified: a genuinely correct target answers well under a
    // second, so 2s is generous for "is this even the right IP" while
    // still being fast enough that a wrong one is never mistaken for a
    // hang. Deliberately much shorter than DispatcherClient's own 8s
    // connect timeout or TargetSsh's per-call SSH timeouts (5-45s) --
    // those stay as-is for the (already-confirmed-reachable) target
    // they're bounding.
    static constexpr int kReachabilityTimeoutMs = 2000;
    // True only between beginReachabilityCheck() starting and its outcome
    // being decided (onReachabilityCheckResult() or
    // cancelReachabilityCheck()) -- i.e. exactly the window in which
    // declareToTarget()/checkInjectorVersion() have NOT yet been allowed
    // to run for this Play press.
    bool reachabilityCheckActive_ = false;
    // Bumped by every beginReachabilityCheck() (a fresh check starting)
    // and cancelReachabilityCheck() (an in-flight one ending early) --
    // lets onReachabilityCheckResult() and the timeout timer's own
    // callback recognize a result/timeout belonging to a check that is no
    // longer the current one (superseded by a newer Play press, or
    // already resolved/cancelled) and discard it, rather than a second
    // bool that could itself race against reachabilityCheckActive_. Same
    // "stale callback" idiom as currentTarget_ comparisons elsewhere in
    // this file.
    int reachabilityCheckToken_ = 0;
    QTimer* reachabilityTimeoutTimer_ = nullptr;
    // Per-target, per-camera-id cache of resolveCameraGeometry()'s own
    // results -- "once per target" (its own comment): kept for the
    // app's lifetime, keyed by target address, so re-applying Configure
    // for the SAME target doesn't re-query ids it already resolved
    // (adding a NEW camera id to an already-resolved target only
    // resolves that one id, not everything again).
    QMap<QString, QMap<int, camsyringe::ResolvedCameraGeometry>> geometryCache_;
    // Per-target timestamp of the last time geometryCache_[target] was
    // populated/refreshed -- by resolveCameraGeometry()'s own automatic
    // run (see its call site) OR by a "Settings > Camera" dialog's
    // explicit Read button (see onCameraSettingsTriggered()). Shown at
    // that dialog's bottom-left; QDateTime() (invalid/default) means
    // "never read for this target".
    QMap<QString, QDateTime> geometryReadTimestamps_;
    // Owned directly here (not inside StreamPool) since it's a single,
    // session-wide replay independent of camera count -- BlfReplayer.h's
    // own class comment covers why this needs its own thread the same way
    // StreamPool manages one thread per camera internally.
    camsyringe::BlfReplayer blfReplayer_;
    std::thread blfThread_;

    QGridLayout* grid_ = nullptr;
    QAction* playPauseAction_ = nullptr;
    QAction* stopAction_ = nullptr;
    QMenu* settingsMenu_ = nullptr;
    QAction* camSyringeSettingsAction_ = nullptr; // "Settings > CamSyringe" -- opens CameraConfigDialog
    QAction* cameraSettingsAction_ = nullptr;      // "Settings > Camera" -- opens CameraSettingsDialog
    QAction* installInjectorAction_ = nullptr;
    // The status bar (its full width, bottom of the window) shows exactly
    // two visible rectangles, same height, side by side -- but they are
    // NOT two adjacent layout items, and NOT nested in each other either
    // (two earlier revisions tried both -- see the long comment in the
    // constructor for the full, screenshot-confirmed story of why each
    // one left a real gap). generalStatusLabel_ is the status bar's ONLY
    // addWidget()-managed widget, spanning its ENTIRE width by itself;
    // statusIndicatorArea_ (below) is a plain child of statusBar() ITSELF
    // (not of generalStatusLabel_), manually positioned to overlay
    // generalStatusLabel_'s rightmost portion while living in the bar's
    // own coordinate space -- which, unlike generalStatusLabel_'s, has no
    // baked-in inset on any side. See CONTEXT.md's "UI requirements" for
    // the full spec.
    //
    // General status message box -- every general status/error message
    // this class shows (previously plain
    // statusBar()->showMessage()/setStyleSheet() calls) now goes through
    // showGeneralStatus()/clearGeneralStatus(), the only two places that
    // touch this label's text/style. Deliberately draws NO border of its
    // own -- see showGeneralStatus()'s own comment for why.
    QLabel* generalStatusLabel_ = nullptr;
    // Tracks whether generalStatusLabel_ currently shows an error/warning
    // -- see showGeneralStatus()'s own comment for why a routine update
    // checks this before overwriting.
    bool generalStatusIsError_ = false;
    // Fixed-width box, parented to statusBar() DIRECTLY (not to
    // generalStatusLabel_) so its coordinates are in the bar's own
    // inset-free space -- eventFilter() watches statusBar()'s own
    // QEvent::Resize to keep it pinned to the bar's true right edge
    // (repositioning is necessary here specifically because, unlike a
    // layout-managed widget, a manually-positioned child doesn't move on
    // its own when its parent resizes). Holds BOTH shimStatusLabel_ and
    // installProgressBar_ stacked in the SAME geometry -- deliberately
    // overlapping, not side-by-side. The two are mutually exclusive in
    // practice (a bundle install and a shim toggle are never both
    // meaningful at the same moment), so the overlap is harmless;
    // installProgressBar_ is raised above shimStatusLabel_ (see the
    // constructor) so it wins the z-order on the rare chance both are
    // visible at once. statusIndicatorArea_ itself IS explicitly raised
    // above generalStatusLabel_ too (a sibling now, not a child of it) --
    // see qcarcam-injector/ARCHITECTURE.md item 37 and CONTEXT.md's "UI
    // requirements" for the full spec.
    QWidget* statusIndicatorArea_ = nullptr;
    // "SHIM" bold red / "REAL" normal green / "…" (Unknown) -- see
    // ShimState and applyShimState(). installEventFilter(this) in the
    // constructor routes its double-clicks to toggleShimStatus() (see
    // eventFilter()).
    QLabel* shimStatusLabel_ = nullptr;
    ShimState shimState_ = ShimState::Blind;
    // True while a refreshShimStatus()/toggleShimStatus() SSH round-trip
    // is in flight -- guards against a second click/refresh piling another
    // one on top before the first resolves (including shimStatusTimer_'s
    // own tick below, which is itself just a refreshShimStatus() call and
    // subject to the same guard).
    bool shimBusy_ = false;
    // Periodic re-check (1s, see the constructor) -- without this, a
    // target going away mid-session left the label showing a stale
    // REAL/SHIM answer indefinitely (only a toggle attempt or a Configure
    // re-apply ever re-queried), and worse, coming back online had NO
    // path back to a confirmed state at all: toggleShimStatus() ignores
    // clicks entirely while Blind (see its own comment), so a target that
    // reconnected just sat on "BLIND" forever with no user action able to
    // fix it. Both were real, reported behavior, not theoretical -- this
    // timer's refreshShimStatus() calls are what actually notice a target
    // coming back, closing the loop double-clicking alone couldn't.
    QTimer* shimStatusTimer_ = nullptr;
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
