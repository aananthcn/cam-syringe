#pragma once

#include <cstdint>
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
// Loops the file indefinitely (Ctrl+C to stop). Output timestamps are a
// simple incrementing frame counter (encoder time_base = 1/fps), so they
// stay monotonic across any number of loops without needing to track the
// source's own timestamps across passes.
//
// IMPORTANT: qcarcam_receiver only tolerates one continuous stream per
// receiver process run (see qcarcam-injector/ARCHITECTURE.md item 22) --
// a second, independent streaming session (even from a clean restart of
// this sender) can wedge the target's hardware decoder at the OS driver
// level, sometimes recoverable only by a full target power-cycle, not an
// app-level restart. Don't run a second sender session against an
// already-fed receiver process; restart run_qcarcam.sh on the target
// first.
class CameraStream {
public:
    CameraStream(std::string inputPath, std::string destUrl);
    ~CameraStream();

    CameraStream(const CameraStream&) = delete;
    CameraStream& operator=(const CameraStream&) = delete;

    // Opens the input file, sets up the decode/scale/encode pipeline, and
    // opens the rtp_mpegts output, writing its header. Returns false
    // (after logging to stderr) on failure.
    bool open();

    // Reads, decodes, rescales, re-encodes and paces frames, looping the
    // file indefinitely, writing each encoded packet to the output.
    // Returns only on a decode/encode/write error or Ctrl+C. open() must
    // have succeeded first.
    void run();

private:
    void sleepUntilDeadline(int64_t deadlineUs);
    bool openDecoder(AVCodecParameters* codecpar);
    bool openEncoder();
    bool openOutput();

    std::string inputPath_;
    std::string destUrl_;
    AVFormatContext* inputCtx_ = nullptr;
    AVCodecContext* decCtx_ = nullptr;
    AVCodecContext* encCtx_ = nullptr;
    SwsContext* swsCtx_ = nullptr;
    AVFormatContext* outputCtx_ = nullptr;
    int videoStreamIndex_ = -1;
    int outWidth_ = 0;
    int outHeight_ = 0;
    int64_t streamStartNs_ = 0;
};

} // namespace camsyringe
