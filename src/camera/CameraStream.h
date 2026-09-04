#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace camsyringe {

// Transcodes an arbitrary video file to H.264/MPEG-TS-in-RTP
// (`rtp_mpegts`), pacing input reads to the source's native frame rate
// using clock_nanosleep, matching the target's proven reference PC-side
// tool (qcarcam-injector's sender/camera-streamer-pc.sh):
//
//   ffmpeg -re -i in.mp4 -an -vf scale=...  -c:v libx264 -preset medium
//     -tune zerolatency -pix_fmt yuv420p -g 30 -keyint_min 30
//     -x264-params repeat-headers=1 -crf 20 -f rtp_mpegts rtp://host:port
//
// A pure remux of the source's original H.264 bitstream was tried first
// and does not work against this target: qcarcam_receiver's hardware
// decoder pipeline assumes a zero-latency encode (no B-frames, PTS==DTS
// always, short GOP with repeated inline SPS/PPS) and a bounded
// resolution/bitrate -- an arbitrary source file (B-frames, multi-second
// GOP, native 4K) starves/corrupts the decoder's input queue. Transcoding
// to match those assumptions is required, not optional, for this target.
//
// Loops the file indefinitely until requestStop() is called (or a fatal
// error). Output timestamps are a simple incrementing frame counter
// (encoder time_base = 1/fps), so they stay monotonic across any number of
// loops without needing to track the source's own timestamps across passes.
//
// IMPORTANT: qcarcam_receiver only tolerates one continuous stream per
// receiver process run (see qcarcam-injector/ARCHITECTURE.md item 22) --
// a second, independent streaming session (even from a clean restart of
// this sender) can wedge the target's hardware decoder at the OS driver
// level, sometimes recoverable only by a full target power-cycle, not an
// app-level restart. Don't run a second sender session against an
// already-fed receiver process; restart run_qcarcam.sh on the target
// first.
//
// Multi-camera / UI hooks (setStartOrigin/setFrameCallback/setErrorCallback
// /requestStop) must be called before open()/run(); run() is expected to
// execute on its own thread (e.g. owned by StreamPool).
class CameraStream {
public:
    // rgbData points at width*height*3 bytes, row-major, AV_PIX_FMT_RGB24
    // (matches QImage::Format_RGB888 byte order -- no channel swap needed).
    // Valid only for the duration of the callback; the caller must copy it
    // if it needs to keep it. Invoked on this CameraStream's own thread.
    using FrameCallback = std::function<void(const uint8_t* rgbData, int width, int height,
                                              int strideBytes)>;
    using ErrorCallback = std::function<void(const std::string& message)>;

    // forcedWidth/forcedHeight (both 0 by default): the real, target-
    // authored resolution to match EXACTLY (see
    // CameraConfig::targetWidth/Height and net/CameraGeometryResolver.h)
    // -- 0/0 keeps today's cap-only behavior (computeOutputSize() in the
    // .cpp). When set, open() letterboxes the source into a canvas of
    // exactly this size instead (see contentWidth_/Height_ and their own
    // comment).
    CameraStream(std::string inputPath, std::string destUrl, std::string label = {}, int index = 0,
                 int forcedWidth = 0, int forcedHeight = 0);
    ~CameraStream();

    CameraStream(const CameraStream&) = delete;
    CameraStream& operator=(const CameraStream&) = delete;

    // Opens the input file, sets up the decode/scale/encode pipeline, and
    // opens the rtp_mpegts output, writing its header. Returns false
    // (after logging to stderr) on failure.
    bool open();

    // Reads, decodes, rescales, re-encodes and paces frames, looping the
    // file indefinitely, writing each encoded packet to the output.
    // Returns when requestStop() has been called or on a decode/encode/
    // write error. open() must have succeeded first.
    void run();

    // Shares a master clock origin (e.g. from Timeline) instead of
    // self-initializing one at the start of run(). Call before run().
    void setStartOrigin(int64_t originNs);

    // Throttled (~10fps) preview frames, emitted after each frame is handed
    // to the encoder so preview work never delays the real-time encode/send
    // path. Call before run().
    void setFrameCallback(FrameCallback callback);

    // Invoked once, on fatal failure (not on a clean requestStop() exit).
    // Call before run().
    void setErrorCallback(ErrorCallback callback);

    // Thread-safe; asks run() to return promptly (polled once per read-loop
    // iteration, so within one frame's pacing interval).
    void requestStop();

    const std::string& label() const { return label_; }
    int index() const { return index_; }
    const std::string& inputPath() const { return inputPath_; }
    const std::string& destUrl() const { return destUrl_; }

private:
    void sleepUntilDeadline(int64_t deadlineUs);
    bool openDecoder(AVCodecParameters* codecpar);
    bool openEncoder();
    bool openOutput();
    bool openPreviewScaler();
    void emitPreviewFrame(const AVFrame* scaledFrame);
    void reportFpsIfDue(int64_t nowNs);
    std::string logLabel() const;

    std::string inputPath_;
    std::string destUrl_;
    std::string label_;
    int index_ = 0;

    AVFormatContext* inputCtx_ = nullptr;
    AVCodecContext* decCtx_ = nullptr;
    AVCodecContext* encCtx_ = nullptr;
    SwsContext* swsCtx_ = nullptr;
    AVFormatContext* outputCtx_ = nullptr;
    int videoStreamIndex_ = -1;
    int outWidth_ = 0;
    int outHeight_ = 0;
    // forcedWidth_/forcedHeight_: constructor input, see its own comment.
    int forcedWidth_ = 0;
    int forcedHeight_ = 0;
    // The scaled SOURCE content's own sub-rectangle within the full
    // outWidth_ x outHeight_ canvas -- equal to the whole canvas
    // (contentOffsetX_/Y_ == 0) when forcedWidth_/Height_ is 0 (today's
    // unchanged behavior). When forced, this preserves the source's own
    // aspect ratio (may scale UP, unlike computeOutputSize()'s
    // shrink-only cap) and is centered within the canvas, whose margins
    // are filled once with letterbox black (see run()'s own comment) --
    // an exact target match without distorting/stretching the source.
    int contentWidth_ = 0;
    int contentHeight_ = 0;
    int contentOffsetX_ = 0;
    int contentOffsetY_ = 0;

    int64_t streamStartNs_ = 0;
    std::atomic<bool> startOriginSet_{false};
    int64_t externalStartOriginNs_ = 0;

    // Achieved-FPS reporting: how many frames actually got encoded+sent in
    // the last ~1s window, vs. the target frame rate. Printed periodically
    // so multi-camera CPU contention is directly visible, not just inferred.
    int64_t fpsWindowStartNs_ = 0;
    int fpsWindowFrameCount_ = 0;

    std::atomic<bool> stopRequested_{false};

    FrameCallback frameCallback_;
    SwsContext* previewSwsCtx_ = nullptr;
    AVFrame* previewFrame_ = nullptr;
    int previewWidth_ = 0;
    int previewHeight_ = 0;

    ErrorCallback errorCallback_;
    std::string lastErrorMessage_;
};

} // namespace camsyringe
