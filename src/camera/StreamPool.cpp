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
            entry.thread.join();
        }
    }
    for (auto& entry : cameras_) {
        entry.opened = false;
    }
}

} // namespace camsyringe
