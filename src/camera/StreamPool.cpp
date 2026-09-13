#include "camera/StreamPool.h"

namespace camsyringe {

StreamPool::~StreamPool() { stopAll(); }

void StreamPool::addCamera(CameraConfig config) {
    cameras_.push_back(Entry{std::move(config), nullptr, {}, false, true});
}

void StreamPool::clearCameras() { cameras_.clear(); }

void StreamPool::setEnabled(size_t index, bool enabled) {
    if (index < cameras_.size()) {
        cameras_[index].enabled = enabled;
    }
}

void StreamPool::setTargetGeometry(size_t index, int width, int height) {
    if (index < cameras_.size()) {
        cameras_[index].config.targetWidth = width;
        cameras_[index].config.targetHeight = height;
    }
}

void StreamPool::setPreviewCallback(PreviewCallback callback) {
    previewCallback_ = std::move(callback);
}

void StreamPool::setErrorCallback(ErrorCallback callback) { errorCallback_ = std::move(callback); }

size_t StreamPool::startAll() {
    timeline_.start();
    size_t started = 0;

    for (auto& entry : cameras_) {
        if (!entry.enabled) {
            continue; // e.g. target rejected this camera's control-channel declaration
        }
        entry.stream = std::make_unique<CameraStream>(
            entry.config.inputPath, entry.config.destUrl, entry.config.label, entry.config.index,
            entry.config.targetWidth, entry.config.targetHeight);

        const int idx = entry.config.index;
        if (previewCallback_) {
            entry.stream->setFrameCallback(
                [this, idx](const uint8_t* data, int w, int h, int stride) {
                    previewCallback_(idx, data, w, h, stride);
                });
        }
        if (errorCallback_) {
            entry.stream->setErrorCallback(
                [this, idx](const std::string& message) { errorCallback_(idx, message); });
        }
        entry.stream->setStartOrigin(timeline_.originNs());

        if (!entry.stream->open()) {
            entry.opened = false;
            if (errorCallback_) {
                errorCallback_(idx, "failed to open camera (see stderr)");
            }
            continue;
        }

        entry.opened = true;
        CameraStream* streamPtr = entry.stream.get();
        entry.thread = std::thread([streamPtr] { streamPtr->run(); });
        ++started;
    }

    return started;
}

void StreamPool::stopAll() {
    for (auto& entry : cameras_) {
        if (entry.opened) {
            entry.stream->requestStop();
        }
    }
    for (auto& entry : cameras_) {
        if (entry.thread.joinable()) {
            // NEVER thread.join() HERE, on the caller's thread -- confirmed
            // live, via gdb, as a real GUI freeze: CameraStream::run()'s
            // av_interleaved_write_frame() call can block indefinitely
            // (inside ffmpeg's own internals, poll()) writing an RTP
            // packet to an unreachable destination -- there is no
            // AVIOContext timeout configured, so this has no bound of its
            // own. requestStop()'s flag (set just above) is only checked
            // BETWEEN writes in run()'s own loop, never inside one
            // already in flight, so it does nothing to unblock this.
            // MainWindow::stopEverything() calls stopAll() synchronously
            // on the GUI thread -- including automatically, via
            // autoStopOnUnreachable(), specifically against a target this
            // just confirmed is unreachable -- so a blocking join() here
            // froze the entire window exactly the way
            // DispatcherClient::disconnect()'s own header comment already
            // documents for its own, analogous bug. Same fix: hand the
            // thread off to its own detached cleanup thread instead of
            // blocking here. `stream` (the CameraStream this thread is
            // still running) moves into that same lambda so it stays
            // alive until its own thread actually finishes with it --
            // nothing outside this lambda references either afterward,
            // so this is safe even if StreamPool itself is destroyed
            // (~StreamPool() calls stopAll() too) the moment this
            // returns.
            std::thread(
                [thread = std::move(entry.thread), stream = std::move(entry.stream)]() mutable {
                    thread.join();
                })
                .detach();
        }
    }
    for (auto& entry : cameras_) {
        entry.opened = false;
    }
}

} // namespace camsyringe
