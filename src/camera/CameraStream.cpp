#include "camera/CameraStream.h"

#include "util/Clock.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <ctime>

extern "C" {
#include <libavutil/opt.h>
}

namespace camsyringe {

namespace {

// Matches qcarcam-injector/sender/camera-streamer-pc.sh's defaults: keeps
// both the PC-side encode and the target's hardware decode real-time-
// capable, and gives the decoder short, self-describing GOPs to join on.
constexpr int kMaxWidth = 1920;
constexpr int kMaxHeight = 1080;
constexpr int kGopSize = 30;
constexpr const char* kPreset = "medium";
constexpr const char* kCrf = "20";

// Preview tap: small and throttled so it can never meaningfully compete
// with the real-time encode/send path for CPU or cause frame-pacing jitter.
constexpr int kPreviewMaxWidth = 640;
constexpr int kPreviewMaxHeight = 360;
constexpr int kPreviewFrameInterval = 3; // ~10fps preview at a 30fps source

constexpr int64_t kFpsReportIntervalNs = 1000000000LL; // report achieved fps roughly once/second

std::string avErrorToString(int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, buf, sizeof(buf));
    return buf;
}

// Scales down (never up) to fit within maxW x maxH, preserving aspect
// ratio, rounded to even dimensions (required for yuv420p/rgb24 planes).
void computeOutputSize(int inW, int inH, int maxW, int maxH, int* outW, int* outH) {
    double scale = 1.0;
    if (inW > maxW) {
        scale = std::min(scale, static_cast<double>(maxW) / inW);
    }
    if (inH > maxH) {
        scale = std::min(scale, static_cast<double>(maxH) / inH);
    }
    int w = static_cast<int>(inW * scale) & ~1;
    int h = static_cast<int>(inH * scale) & ~1;
    *outW = std::max(w, 2);
    *outH = std::max(h, 2);
}

AVRational inputFrameRate(AVFormatContext* fmt, int streamIndex) {
    AVStream* st = fmt->streams[streamIndex];
    if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) {
        return st->avg_frame_rate;
    }
    if (st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0) {
        return st->r_frame_rate;
    }
    return AVRational{30, 1};
}

} // namespace

CameraStream::CameraStream(std::string inputPath, std::string destUrl, std::string label, int index)
    : inputPath_(std::move(inputPath)),
      destUrl_(std::move(destUrl)),
      label_(std::move(label)),
      index_(index) {}

CameraStream::~CameraStream() {
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
    }
    if (previewSwsCtx_) {
        sws_freeContext(previewSwsCtx_);
    }
    if (previewFrame_) {
        av_frame_free(&previewFrame_);
    }
    if (decCtx_) {
        avcodec_free_context(&decCtx_);
    }
    if (encCtx_) {
        avcodec_free_context(&encCtx_);
    }
    if (outputCtx_) {
        if (outputCtx_->pb && !(outputCtx_->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&outputCtx_->pb);
        }
        avformat_free_context(outputCtx_);
    }
    if (inputCtx_) {
        avformat_close_input(&inputCtx_);
    }
    avformat_network_deinit();
}

std::string CameraStream::logLabel() const {
    return label_.empty() ? ("cam" + std::to_string(index_)) : label_;
}

void CameraStream::setStartOrigin(int64_t originNs) {
    externalStartOriginNs_ = originNs;
    startOriginSet_.store(true, std::memory_order_release);
}

void CameraStream::setFrameCallback(FrameCallback callback) { frameCallback_ = std::move(callback); }

void CameraStream::setErrorCallback(ErrorCallback callback) { errorCallback_ = std::move(callback); }

void CameraStream::requestStop() { stopRequested_.store(true, std::memory_order_release); }

bool CameraStream::openDecoder(AVCodecParameters* codecpar) {
    const AVCodec* decoder = avcodec_find_decoder(codecpar->codec_id);
    if (!decoder) {
        std::fprintf(stderr, "CameraStream[%s]: no decoder available for codec id %d\n",
                     logLabel().c_str(), static_cast<int>(codecpar->codec_id));
        return false;
    }
    decCtx_ = avcodec_alloc_context3(decoder);
    if (!decCtx_) {
        std::fprintf(stderr, "CameraStream[%s]: failed to allocate decoder context\n",
                     logLabel().c_str());
        return false;
    }
    int ret = avcodec_parameters_to_context(decCtx_, codecpar);
    if (ret < 0) {
        std::fprintf(stderr, "CameraStream[%s]: failed to copy decoder parameters: %s\n",
                     logLabel().c_str(), avErrorToString(ret).c_str());
        return false;
    }
    decCtx_->pkt_timebase = inputCtx_->streams[videoStreamIndex_]->time_base;
    // AVCodecContext's compiled-in default is thread_count=1 (single-
    // threaded), NOT 0/auto -- unlike ffmpeg's CLI, which explicitly
    // negotiates auto-threading per decoder. Left unset, a 4K source
    // decodes on exactly one core; this was the actual cause of cam0's
    // ~10fps ceiling (the encoder's own thread count was never the
    // bottleneck -- see CONTEXT.md's "Open investigation" section).
    decCtx_->thread_count = 0;
    ret = avcodec_open2(decCtx_, decoder, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "CameraStream[%s]: failed to open decoder '%s': %s\n",
                     logLabel().c_str(), decoder->name, avErrorToString(ret).c_str());
        return false;
    }
    return true;
}

bool CameraStream::openEncoder() {
    const AVCodec* encoder = avcodec_find_encoder_by_name("libx264");
    if (!encoder) {
        std::fprintf(stderr, "CameraStream[%s]: libx264 encoder not available in this FFmpeg build\n",
                     logLabel().c_str());
        return false;
    }
    encCtx_ = avcodec_alloc_context3(encoder);
    if (!encCtx_) {
        std::fprintf(stderr, "CameraStream[%s]: failed to allocate encoder context\n",
                     logLabel().c_str());
        return false;
    }

    AVRational frameRate = inputFrameRate(inputCtx_, videoStreamIndex_);
    encCtx_->width = outWidth_;
    encCtx_->height = outHeight_;
    encCtx_->pix_fmt = AV_PIX_FMT_YUV420P;
    encCtx_->time_base = av_inv_q(frameRate);
    encCtx_->framerate = frameRate;
    encCtx_->gop_size = kGopSize;
    encCtx_->max_b_frames = 0;

    // Matches camera-streamer-pc.sh: zero-latency (no B-frames, PTS==DTS),
    // a short GOP the target's decoder can join on quickly, and inline
    // SPS/PPS repeated before every keyframe (no SDP/extradata needed).
    av_opt_set(encCtx_->priv_data, "preset", kPreset, 0);
    av_opt_set(encCtx_->priv_data, "tune", "zerolatency", 0);
    av_opt_set(encCtx_->priv_data, "crf", kCrf, 0);
    av_opt_set(encCtx_->priv_data, "x264-params", "repeat-headers=1", 0);

    int ret = avcodec_open2(encCtx_, encoder, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "CameraStream[%s]: failed to open libx264 encoder: %s\n",
                     logLabel().c_str(), avErrorToString(ret).c_str());
        return false;
    }
    return true;
}

bool CameraStream::openOutput() {
    int ret = avformat_alloc_output_context2(&outputCtx_, nullptr, "rtp_mpegts", destUrl_.c_str());
    if (ret < 0 || !outputCtx_) {
        std::fprintf(stderr, "CameraStream[%s]: failed to allocate rtp_mpegts output for '%s': %s\n",
                     logLabel().c_str(), destUrl_.c_str(), avErrorToString(ret).c_str());
        return false;
    }

    AVStream* outStream = avformat_new_stream(outputCtx_, nullptr);
    if (!outStream) {
        std::fprintf(stderr, "CameraStream[%s]: failed to allocate output stream\n",
                     logLabel().c_str());
        return false;
    }
    ret = avcodec_parameters_from_context(outStream->codecpar, encCtx_);
    if (ret < 0) {
        std::fprintf(stderr, "CameraStream[%s]: failed to copy encoder parameters: %s\n",
                     logLabel().c_str(), avErrorToString(ret).c_str());
        return false;
    }
    outStream->time_base = encCtx_->time_base;

    if (!(outputCtx_->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&outputCtx_->pb, destUrl_.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            std::fprintf(stderr, "CameraStream[%s]: failed to open output '%s': %s\n",
                         logLabel().c_str(), destUrl_.c_str(), avErrorToString(ret).c_str());
            return false;
        }
    }

    ret = avformat_write_header(outputCtx_, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "CameraStream[%s]: failed to write output header: %s\n",
                     logLabel().c_str(), avErrorToString(ret).c_str());
        return false;
    }
    return true;
}

bool CameraStream::openPreviewScaler() {
    computeOutputSize(outWidth_, outHeight_, kPreviewMaxWidth, kPreviewMaxHeight, &previewWidth_,
                      &previewHeight_);
    previewSwsCtx_ = sws_getContext(outWidth_, outHeight_, AV_PIX_FMT_YUV420P, previewWidth_,
                                     previewHeight_, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr,
                                     nullptr, nullptr);
    if (!previewSwsCtx_) {
        std::fprintf(stderr, "CameraStream[%s]: failed to create preview scaler context\n",
                     logLabel().c_str());
        return false;
    }
    previewFrame_ = av_frame_alloc();
    previewFrame_->format = AV_PIX_FMT_RGB24;
    previewFrame_->width = previewWidth_;
    previewFrame_->height = previewHeight_;
    av_frame_get_buffer(previewFrame_, 0);
    return true;
}

bool CameraStream::open() {
    avformat_network_init();

    int ret = avformat_open_input(&inputCtx_, inputPath_.c_str(), nullptr, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "CameraStream[%s]: failed to open '%s': %s\n", logLabel().c_str(),
                     inputPath_.c_str(), avErrorToString(ret).c_str());
        return false;
    }

    ret = avformat_find_stream_info(inputCtx_, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "CameraStream[%s]: failed to read stream info: %s\n",
                     logLabel().c_str(), avErrorToString(ret).c_str());
        return false;
    }

    for (unsigned i = 0; i < inputCtx_->nb_streams; ++i) {
        if (inputCtx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex_ = static_cast<int>(i);
            break;
        }
    }
    if (videoStreamIndex_ < 0) {
        std::fprintf(stderr, "CameraStream[%s]: no video stream found in '%s'\n",
                     logLabel().c_str(), inputPath_.c_str());
        return false;
    }

    AVCodecParameters* codecpar = inputCtx_->streams[videoStreamIndex_]->codecpar;
    if (!openDecoder(codecpar)) {
        return false;
    }

    computeOutputSize(decCtx_->width, decCtx_->height, kMaxWidth, kMaxHeight, &outWidth_,
                      &outHeight_);
    std::fprintf(stderr, "camsyringe[%s]: %dx%d -> %dx%d\n", logLabel().c_str(), decCtx_->width,
                 decCtx_->height, outWidth_, outHeight_);

    if (!openEncoder()) {
        return false;
    }

    swsCtx_ = sws_getContext(decCtx_->width, decCtx_->height, decCtx_->pix_fmt, outWidth_,
                              outHeight_, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr,
                              nullptr);
    if (!swsCtx_) {
        std::fprintf(stderr, "CameraStream[%s]: failed to create scaler context\n",
                     logLabel().c_str());
        return false;
    }

    if (!openPreviewScaler()) {
        return false;
    }

    if (!openOutput()) {
        return false;
    }

    return true;
}

void CameraStream::sleepUntilDeadline(int64_t deadlineUs) {
    int64_t deadlineNs = streamStartNs_ + deadlineUs * 1000;
    timespec ts{};
    ts.tv_sec = deadlineNs / 1000000000LL;
    ts.tv_nsec = deadlineNs % 1000000000LL;

    int ret;
    do {
        ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
    } while (ret == EINTR);
}

void CameraStream::emitPreviewFrame(const AVFrame* scaledFrame) {
    sws_scale(previewSwsCtx_, scaledFrame->data, scaledFrame->linesize, 0, outHeight_,
              previewFrame_->data, previewFrame_->linesize);
    frameCallback_(previewFrame_->data[0], previewWidth_, previewHeight_,
                    previewFrame_->linesize[0]);
}

void CameraStream::reportFpsIfDue(int64_t nowNs) {
    ++fpsWindowFrameCount_;
    int64_t elapsedNs = nowNs - fpsWindowStartNs_;
    if (elapsedNs < kFpsReportIntervalNs) {
        return;
    }
    double achievedFps = fpsWindowFrameCount_ / (static_cast<double>(elapsedNs) / 1e9);
    double targetFps = av_q2d(encCtx_->framerate);
    std::fprintf(stderr, "camsyringe[%s]: %.1f fps (target %.1f fps)\n", logLabel().c_str(),
                 achievedFps, targetFps);
    fpsWindowFrameCount_ = 0;
    fpsWindowStartNs_ = nowNs;
}

void CameraStream::run() {
    AVStream* inStream = inputCtx_->streams[videoStreamIndex_];
    AVStream* outStream = outputCtx_->streams[0];

    AVPacket* pkt = av_packet_alloc();
    AVPacket* encPkt = av_packet_alloc();
    AVFrame* decFrame = av_frame_alloc();
    AVFrame* scaledFrame = av_frame_alloc();
    scaledFrame->format = AV_PIX_FMT_YUV420P;
    scaledFrame->width = outWidth_;
    scaledFrame->height = outHeight_;
    av_frame_get_buffer(scaledFrame, 0);

    streamStartNs_ = startOriginSet_.load(std::memory_order_acquire) ? externalStartOriginNs_
                                                                      : monotonicNowNs();
    fpsWindowStartNs_ = streamStartNs_;
    fpsWindowFrameCount_ = 0;
    int64_t frameCounter = 0;
    int passNumber = 1;
    bool fatalError = false;
    // Tracks the most recent DTS-derived deadline (relative to
    // streamStartNs_, matching sleepUntilDeadline()'s own units) seen this
    // pass -- needed to advance streamStartNs_ by one pass's real duration
    // at each loop restart, see the loop-back block below for why.
    int64_t lastDeadlineUs = 0;

    while (!fatalError && !stopRequested_.load(std::memory_order_acquire)) {
        if (passNumber > 1) {
            std::fprintf(stderr, "camsyringe[%s]: looping playback (pass %d)\n", logLabel().c_str(),
                         passNumber);
        }

        while (!stopRequested_.load(std::memory_order_acquire) &&
               av_read_frame(inputCtx_, pkt) >= 0) {
            if (pkt->stream_index != videoStreamIndex_) {
                av_packet_unref(pkt);
                continue;
            }

            // Pace by DTS (decode order): matches ffmpeg's own `-re` input
            // throttle, which camera-streamer-pc.sh also relies on.
            int64_t dts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
            if (dts != AV_NOPTS_VALUE) {
                int64_t deadlineUs = av_rescale_q(dts, inStream->time_base, AVRational{1, 1000000});
                lastDeadlineUs = deadlineUs;
                sleepUntilDeadline(deadlineUs);
            }

            int decSendRet = avcodec_send_packet(decCtx_, pkt);
            av_packet_unref(pkt);
            if (decSendRet < 0 && decSendRet != AVERROR(EAGAIN)) {
                lastErrorMessage_ = "decode send_packet failed: " + avErrorToString(decSendRet);
                std::fprintf(stderr, "CameraStream[%s]: %s\n", logLabel().c_str(),
                             lastErrorMessage_.c_str());
                fatalError = true;
                break;
            }

            int decRecvRet;
            while ((decRecvRet = avcodec_receive_frame(decCtx_, decFrame)) == 0) {
                sws_scale(swsCtx_, decFrame->data, decFrame->linesize, 0, decCtx_->height,
                          scaledFrame->data, scaledFrame->linesize);
                av_frame_unref(decFrame);
                // Encoder timestamps are a plain incrementing frame counter
                // (encCtx_->time_base == 1/fps) rather than carried over
                // from the source: with zerolatency (no B-frame reorder)
                // this is simpler than tracking source PTS across loops
                // and stays monotonic across any number of loop passes.
                scaledFrame->pts = frameCounter++;

                int encSendRet = avcodec_send_frame(encCtx_, scaledFrame);
                if (encSendRet < 0) {
                    lastErrorMessage_ = "encode send_frame failed: " + avErrorToString(encSendRet);
                    std::fprintf(stderr, "CameraStream[%s]: %s\n", logLabel().c_str(),
                                 lastErrorMessage_.c_str());
                    fatalError = true;
                    break;
                }

                if (frameCallback_ && (frameCounter % kPreviewFrameInterval == 0)) {
                    emitPreviewFrame(scaledFrame);
                }
                reportFpsIfDue(monotonicNowNs());

                int encRecvRet;
                while ((encRecvRet = avcodec_receive_packet(encCtx_, encPkt)) == 0) {
                    av_packet_rescale_ts(encPkt, encCtx_->time_base, outStream->time_base);
                    encPkt->stream_index = 0;
                    // av_interleaved_write_frame always unreferences encPkt.
                    int writeRet = av_interleaved_write_frame(outputCtx_, encPkt);
                    if (writeRet < 0) {
                        lastErrorMessage_ = "write_frame failed: " + avErrorToString(writeRet);
                        std::fprintf(stderr, "CameraStream[%s]: %s\n", logLabel().c_str(),
                                     lastErrorMessage_.c_str());
                        fatalError = true;
                        break;
                    }
                }
                if (fatalError ||
                    (encRecvRet < 0 && encRecvRet != AVERROR(EAGAIN) && encRecvRet != AVERROR_EOF)) {
                    if (!fatalError) {
                        lastErrorMessage_ =
                            "encode receive_packet failed: " + avErrorToString(encRecvRet);
                        std::fprintf(stderr, "CameraStream[%s]: %s\n", logLabel().c_str(),
                                     lastErrorMessage_.c_str());
                    }
                    fatalError = true;
                    break;
                }
            }
            if (fatalError) {
                break;
            }
            if (decRecvRet < 0 && decRecvRet != AVERROR(EAGAIN) && decRecvRet != AVERROR_EOF) {
                lastErrorMessage_ = "decode receive_frame failed: " + avErrorToString(decRecvRet);
                std::fprintf(stderr, "CameraStream[%s]: %s\n", logLabel().c_str(),
                             lastErrorMessage_.c_str());
                fatalError = true;
                break;
            }
        }
        if (fatalError || stopRequested_.load(std::memory_order_acquire)) {
            break;
        }

        ++passNumber;
        int seekRet = av_seek_frame(inputCtx_, videoStreamIndex_, 0, AVSEEK_FLAG_BACKWARD);
        if (seekRet < 0) {
            lastErrorMessage_ = "failed to seek back to start for loop: " + avErrorToString(seekRet);
            std::fprintf(stderr, "CameraStream[%s]: %s\n", logLabel().c_str(),
                         lastErrorMessage_.c_str());
            fatalError = true;
            break;
        }
        avcodec_flush_buffers(decCtx_);
        // BUG FIX: streamStartNs_ is the origin sleepUntilDeadline() adds
        // every DTS-derived deadline to -- but after av_seek_frame() rewinds
        // to the start, the NEXT pass's packets have DTS values starting
        // near 0 again too. Without advancing the origin here, every
        // deadline on pass 2+ would compute to streamStartNs_ + (a small
        // value) -- a wall-clock time already WELL IN THE PAST (real time
        // has moved on by this pass's actual duration), so
        // clock_nanosleep(TIMER_ABSTIME, ...) returns immediately forever
        // after, i.e. ZERO pacing on every loop after the first. Confirmed
        // for real: reported fps jumped from a correct ~30fps on pass 1 to
        // 80-115fps on every subsequent pass, and the receiving end saw
        // sustained "Packet corrupt" (the RTP/MPEG-TS stream arriving in an
        // unthrottled burst, not paced to real time). Advancing the origin
        // by this pass's own last DTS-derived deadline keeps pass 2's
        // deadlines anchored to wall-clock time correctly, the same way
        // frameCounter (the encoder's own PTS) already stays monotonic
        // across passes without needing the source's timestamps.
        streamStartNs_ += lastDeadlineUs * 1000;
        lastDeadlineUs = 0;
    }

    av_write_trailer(outputCtx_);
    av_packet_free(&pkt);
    av_packet_free(&encPkt);
    av_frame_free(&decFrame);
    av_frame_free(&scaledFrame);

    if (fatalError && errorCallback_) {
        errorCallback_(lastErrorMessage_);
    }
}

} // namespace camsyringe
