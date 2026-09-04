#pragma once

#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

namespace camsyringe {

class TargetSsh;

// One resolved (or attempted) camera's real, target-authored geometry --
// see ResolvedCameraGeometry's own use in CameraGeometryResolver below.
struct ResolvedCameraGeometry {
    int camId = 0;
    bool ok = false;          // false: nothing usable resolved for this id at all
    bool wasFallback = false; // true: borrowed from an earlier id's successful result
                              // (see resolveAsync()'s own comment) rather than resolved
                              // for this id directly
    uint32_t width = 0;
    uint32_t height = 0;
};

// Discovers each configured camera's REAL, vehicle-engineer-authored
// resolution from the target itself, instead of guessing or requiring a
// human to manually match env vars -- see qcarcam-injector/
// ARCHITECTURE.md items 38/39 for the "display renders blank" bug this
// exists to prevent: at least one real camera ID's display pipe
// (confirmed live via slog2's WFD_PIPELINE_SCALE_RANGE trace) supports
// NO scaling at all, so the injected content must be pixel-dimension-
// identical to what that ID's real camera actually negotiates.
//
// For each camera id, in the ORDER given (ascending -- the caller's
// responsibility, see resolveAsync()'s own param comment):
//   1. Determine which /mnt/scripts/1cam_*.xml template /mnt/scripts/
//      camtest would pick for THIS id, by mirroring its own board-variant
//      (`uname -m`) + `case $CAM in` branching verbatim (see
//      xmlFilenameForCamId() in the .cpp -- a literal, commented port,
//      not a guess; re-derive it from camtest's own source directly if
//      that script's branching ever changes in a future release).
//   2. Fetch that template (with camtest's own `sed s/CAM_INPUT/<id>/g`
//      substitution already applied) and look for a declared
//      <output_setting width="N" height="M" .../> -- if present, that's
//      this id's resolved geometry, no live query needed (e.g. id 13's
//      1cam_ifcc.xml: 2560x1984).
//   3. If not declared (e.g. ids 8/9/10's 1cam_728.xml -- confirmed live
//      to have no width/height at all), run a brief LIVE query against
//      the REAL libqcxclient.so: mirrors camtest's own preamble
//      (`. start-qcxserver.sh`, `start_server`, kill any already-running
//      qcarcam_test -- same side effect camtest itself already has every
//      time it's invoked, not a new risk this class introduces) then runs
//      qcarcam_test with LD_LIBRARY_PATH pointed at the bundle's dedicated
//      always-real reference copy (/var/opt/lib/real-ref/libqcxclient.so
//      -- confirmed via readelf that qcarcam_test has no RPATH/RUNPATH,
//      so this is honored regardless of whatever the SHIM/REAL toggle
//      currently has active at /mnt/lib64/camera/libqcxclient.so),
//      bounded by a real timeout -- just long enough to reach its own
//      "--- QCarCam Queried Inputs ----" dump, never long enough to
//      actually start streaming. Parses that dump's negotiated WxH.
//   4. If step 3 also fails (camera not physically connected on this
//      bench, real hardware error, etc.): reuse the most recent
//      SUCCESSFUL result from an earlier id in this same call (general
//      "last success carried forward" rule -- NOT a fixed id pairing).
//      If no earlier id has succeeded yet, this id's result has
//      ok=false -- reported plainly, never guessed further.
//
// Runs entirely on its own background thread (detached), using the
// caller-owned TargetSsh the same way InjectorBundleInstaller does --
// callback is invoked on THAT thread, not the GUI thread; the caller
// must marshal it (QMetaObject::invokeMethod), same convention as
// DispatcherClient/DispatcherVersionProbe. Never prompts for
// credentials (passwordless-only, same convention as
// MainWindow::refreshShimStatus()) -- this can fire from a Configure
// apply, not a deliberate "talk to the target now" action, so a target
// needing credentials just fails every id's discovery rather than
// popping a prompt unprompted.
class CameraGeometryResolver {
public:
    using Callback = std::function<void(std::vector<ResolvedCameraGeometry> results)>;
    // completed: how many of camIds have finished (successfully or not),
    // including the one that just finished -- 1..camIds.size(). Optional:
    // this can take anywhere from under a second (declared-XML ids) to
    // tens of seconds per id (the live-query fallback's worst case, a
    // cold display-service start), so a caller reading many ids at once
    // (e.g. CameraSettingsDialog's full 1-16 sweep) needs SOME feedback
    // that it's still working, not just a single callback at the very
    // end. Invoked on the SAME background thread as Callback -- same
    // marshaling requirement.
    using ProgressCallback = std::function<void(int completed, int total)>;

    // camIds: ASCENDING order is the caller's responsibility -- resolution
    // order IS the fallback order (see class comment's step 4).
    // progressCallback: optional (defaults to a no-op), see its own comment.
    static void resolveAsync(TargetSsh& ssh, QString target, QString sshUser, QString sshKeyPath,
                              std::vector<int> camIds, Callback callback,
                              ProgressCallback progressCallback = nullptr);
};

} // namespace camsyringe
