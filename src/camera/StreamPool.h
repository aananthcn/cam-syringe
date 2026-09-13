#pragma once

#include "camera/CameraStream.h"
#include "orchestrator/Timeline.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace camsyringe {

struct CameraConfig {
    std::string inputPath;
    std::string destUrl;
    std::string label;
    int index = 0;
    // QCarCam id (1-16) this camera's stream should be injected into on
    // the target, and the RTP port destUrl actually points at (kept as a
    // plain int alongside destUrl's own "rtp://host:port" string so
    // callers don't need to re-parse it) -- both travel with the rest of
    // this config purely so MainWindow can hand them to DispatcherClient's
    // control-channel declaration; CameraStream itself never looks at
    // either field.
    int camId = 0;
    int port = 0;
    // The real, target-authored resolution this camera's real vehicle
    // camera/panel actually expects for camId (see
    // net/CameraGeometryResolver.h) -- 0/0 means "not resolved yet, or
    // discovery failed" -- CameraStream then falls back to its own
    // cap-only behavior (see CameraStream.cpp's computeOutputSize()) as
    // before this existed. When non-zero, CameraStream resizes EXACTLY
    // to this size (letterboxed, not stretched) instead of just capping
    // an oversized source -- required for at least one real camera ID's
    // display pipe, confirmed live to support no scaling at all.
    int targetWidth = 0;
    int targetHeight = 0;
};

// Qt-free: owns N CameraStream instances, each running on its own
// std::thread, all sharing one Timeline origin so their DTS-paced sleeps
// stay phase-aligned ("simultaneous sync"). UI-specific thread marshaling
// (e.g. QMetaObject::invokeMethod) belongs in the caller's callbacks, not
// here.
class StreamPool {
public:
    StreamPool() = default;
    ~StreamPool();

    StreamPool(const StreamPool&) = delete;
    StreamPool& operator=(const StreamPool&) = delete;

    // Call before startAll(). Not safe while cameras are running -- caller
    // (e.g. MainWindow, only while the Configure dialog is enabled/state is
    // Idle) is responsible for calling stopAll() first if anything is
    // running.
    void addCamera(CameraConfig config);

    // Discards the configured camera list (not the running threads --
    // caller must stopAll() first if anything is running). Used when the
    // Configure dialog replaces the camera list.
    void clearCameras();

    // Excludes/includes camera `index` from the NEXT startAll() call --
    // e.g. MainWindow disables a camera whose target-side control-channel
    // declaration came back ERROR (no receiver listening on target for
    // it, so streaming to it would just go into the void). Indices stay
    // stable either way (unlike clearCameras()), so the UI's tile-to-
    // camera mapping never shifts. Every camera defaults to enabled; a
    // fresh addCamera() (e.g. after clearCameras()) resets to enabled too.
    void setEnabled(size_t index, bool enabled);

    // Updates camera `index`'s CameraConfig::targetWidth/Height in place
    // (see their own comment) -- e.g. MainWindow applying a
    // CameraGeometryResolver result that only became available after
    // this camera was already added. Takes effect on the NEXT startAll()
    // (CameraStream is constructed fresh there each time, see
    // StreamPool.cpp) -- safe to call regardless of running state, same
    // convention as setEnabled().
    void setTargetGeometry(size_t index, int width, int height);

    using PreviewCallback =
        std::function<void(int index, const uint8_t* rgbData, int width, int height, int strideBytes)>;
    using ErrorCallback = std::function<void(int index, const std::string& message)>;

    // Call before startAll().
    void setPreviewCallback(PreviewCallback callback);
    void setErrorCallback(ErrorCallback callback);

    // Recaptures the Timeline origin, opens every configured camera
    // (synchronously, on the caller's thread), and spawns one thread per
    // camera that opened successfully. A camera whose open() fails is
    // skipped (reported via ErrorCallback) -- the others still start.
    // Safe to call again after stopAll() for a fresh session.
    size_t startAll();

    // Asks every running camera to stop. Does NOT block waiting for them
    // to actually finish -- see stopAll()'s own comment for the real,
    // confirmed GUI freeze a synchronous join() here caused (a camera
    // thread can be stuck inside ffmpeg's own blocking write to an
    // unreachable RTP destination, with no bound of its own). Each
    // thread (and the CameraStream it's still running) is handed off to
    // its own detached cleanup thread instead; startAll() can safely be
    // called again immediately after this returns regardless -- it
    // always constructs a fresh CameraStream per camera anyway. Safe to
    // call repeatedly, from the destructor, or with nothing running.
    void stopAll();

    size_t cameraCount() const { return cameras_.size(); }
    const CameraConfig& configAt(size_t i) const { return cameras_.at(i).config; }

    // The CLOCK_MONOTONIC origin startAll() just captured -- callers with
    // their own separately-paced real-time thread (e.g. BlfReplayer) that
    // needs to stay phase-aligned with these cameras call this right after
    // startAll() and feed it into their own setStartOrigin(), matching
    // CONTEXT.md's Architecture diagram (one shared Timeline orchestrator
    // feeding both the camera streamer and the BLF/ETH replayer). Only
    // meaningful after startAll() has been called at least once.
    int64_t timelineOriginNs() const { return timeline_.originNs(); }

private:
    struct Entry {
        CameraConfig config;
        std::unique_ptr<CameraStream> stream;
        std::thread thread;
        bool opened = false;
        bool enabled = true;
    };

    std::vector<Entry> cameras_;
    Timeline timeline_;
    PreviewCallback previewCallback_;
    ErrorCallback errorCallback_;
};

} // namespace camsyringe
