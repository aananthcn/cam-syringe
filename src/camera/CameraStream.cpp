#include "camera/CameraStream.h"

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

std::string avErrorToString(int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, buf, sizeof(buf));
    return buf;
}

int64_t monotonicNowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

// Scales down (never up) to fit within maxW x maxH, preserving aspect
// ratio, rounded to even dimensions (required for yuv420p).
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

CameraStream::CameraStream(std::string inputPath, std::string destUrl)
    : inputPath_(std::move(inputPath)), destUrl_(std::move(destUrl)) {}

CameraStream::~CameraStream() {
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
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

bool CameraStream::openDecoder(AVCodecParameters* codecpar) {
    const AVCodec* decoder = avcodec_find_decoder(codecpar->codec_id);
    if (!decoder) {
        std::fprintf(stderr, "CameraStream: no decoder available for codec id %d\n",
                     static_cast<int>(codecpar->codec_id));
        return false;
    }
    decCtx_ = avcodec_alloc_context3(decoder);
    if (!decCtx_) {
        std::fprintf(stderr, "CameraStream: failed to allocate decoder context\n");
        return false;
    }
    int ret = avcodec_parameters_to_context(decCtx_, codecpar);
    if (ret < 0) {
        std::fprintf(stderr, "CameraStream: failed to copy decoder parameters: %s\n",
                     avErrorToString(ret).c_str());
        return false;
    }
    decCtx_->pkt_timebase = inputCtx_->streams[videoStreamIndex_]->time_base;
    ret = avcodec_open2(decCtx_, decoder, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "CameraStream: failed to open decoder '%s': %s\n", decoder->name,
                     avErrorToString(ret).c_str());
        return false;
    }
    return true;
}

bool CameraStream::openEncoder() {
    const AVCodec* encoder = avcodec_find_encoder_by_name("libx264");
    if (!encoder) {
        std::fprintf(stderr, "CameraStream: libx264 encoder not available in this FFmpeg build\n");
        return false;
    }
    encCtx_ = avcodec_alloc_context3(encoder);
    if (!encCtx_) {
        std::fprintf(stderr, "CameraStream: failed to allocate encoder context\n");
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
        std::fprintf(stderr, "CameraStream: failed to open libx264 encoder: %s\n",
                     avErrorToString(ret).c_str());
        return false;
    }
    return true;
}

bool CameraStream::openOutput() {
    int ret = avformat_alloc_output_context2(&outputCtx_, nullptr, "rtp_mpegts", destUrl_.c_str());
    if (ret < 0 || !outputCtx_) {
        std::fprintf(stderr, "CameraStream: failed to allocate rtp_mpegts output for '%s': %s\n",
                     destUrl_.c_str(), avErrorToString(ret).c_str());
        return false;
    }

    AVStream* outStream = avformat_new_stream(outputCtx_, nullptr);
    if (!outStream) {
        std::fprintf(stderr, "CameraStream: failed to allocate output stream\n");
        return false;
    }
    ret = avcodec_parameters_from_context(outStream->codecpar, encCtx_);
    if (ret < 0) {
        std::fprintf(stderr, "CameraStream: failed to copy encoder parameters: %s\n",
                     avErrorToString(ret).c_str());
        return false;
    }
    outStream->time_base = encCtx_->time_base;

    if (!(outputCtx_->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&outputCtx_->pb, destUrl_.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            std::fprintf(stderr, "CameraStream: failed to open output '%s': %s\n",
                         destUrl_.c_str(), avErrorToString(ret).c_str());
            return false;
        }
    }

    ret = avformat_write_header(outputCtx_, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "CameraStream: failed to write output header: %s\n",
                     avErrorToString(ret).c_str());
        return false;
    }
    return true;
}

bool CameraStream::open() {
    avformat_network_init();

    int ret = avformat_open_input(&inputCtx_, inputPath_.c_str(), nullptr, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "CameraStream: failed to open '%s': %s\n", inputPath_.c_str(),
                     avErrorToString(ret).c_str());
        return false;
    }

    ret = avformat_find_stream_info(inputCtx_, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "CameraStream: failed to read stream info: %s\n",
                     avErrorToString(ret).c_str());
        return false;
    }

    for (unsigned i = 0; i < inputCtx_->nb_streams; ++i) {
        if (inputCtx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex_ = static_cast<int>(i);
            break;
        }
    }
    if (videoStreamIndex_ < 0) {
        std::fprintf(stderr, "CameraStream: no video stream found in '%s'\n", inputPath_.c_str());
        return false;
    }

    AVCodecParameters* codecpar = inputCtx_->streams[videoStreamIndex_]->codecpar;
    if (!openDecoder(codecpar)) {
        return false;
    }

    computeOutputSize(decCtx_->width, decCtx_->height, kMaxWidth, kMaxHeight, &outWidth_,
                      &outHeight_);
    std::fprintf(stderr, "camsyringe: %dx%d -> %dx%d\n", decCtx_->width, decCtx_->height,
                 outWidth_, outHeight_);

    if (!openEncoder()) {
        return false;
    }

    swsCtx_ = sws_getContext(decCtx_->width, decCtx_->height, decCtx_->pix_fmt, outWidth_,
                              outHeight_, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr,
                              nullptr);
    if (!swsCtx_) {
        std::fprintf(stderr, "CameraStream: failed to create scaler context\n");
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

    streamStartNs_ = monotonicNowNs();
    int64_t frameCounter = 0;
    int passNumber = 1;
    bool fatalError = false;

    while (!fatalError) {
        if (passNumber > 1) {
            std::fprintf(stderr, "camsyringe: looping playback (pass %d)\n", passNumber);
        }

        while (av_read_frame(inputCtx_, pkt) >= 0) {
            if (pkt->stream_index != videoStreamIndex_) {
                av_packet_unref(pkt);
                continue;
            }

            // Pace by DTS (decode order): matches ffmpeg's own `-re` input
            // throttle, which camera-streamer-pc.sh also relies on.
            int64_t dts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
            if (dts != AV_NOPTS_VALUE) {
                int64_t deadlineUs = av_rescale_q(dts, inStream->time_base, AVRational{1, 1000000});
                sleepUntilDeadline(deadlineUs);
            }

            int decSendRet = avcodec_send_packet(decCtx_, pkt);
            av_packet_unref(pkt);
            if (decSendRet < 0 && decSendRet != AVERROR(EAGAIN)) {
                std::fprintf(stderr, "CameraStream: decode send_packet failed: %s\n",
                             avErrorToString(decSendRet).c_str());
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
                    std::fprintf(stderr, "CameraStream: encode send_frame failed: %s\n",
                                 avErrorToString(encSendRet).c_str());
                    fatalError = true;
                    break;
                }

                int encRecvRet;
                while ((encRecvRet = avcodec_receive_packet(encCtx_, encPkt)) == 0) {
                    av_packet_rescale_ts(encPkt, encCtx_->time_base, outStream->time_base);
                    encPkt->stream_index = 0;
                    // av_interleaved_write_frame always unreferences encPkt.
                    int writeRet = av_interleaved_write_frame(outputCtx_, encPkt);
                    if (writeRet < 0) {
                        std::fprintf(stderr, "CameraStream: write_frame failed: %s\n",
                                     avErrorToString(writeRet).c_str());
                        fatalError = true;
                        break;
                    }
                }
                if (fatalError ||
                    (encRecvRet < 0 && encRecvRet != AVERROR(EAGAIN) && encRecvRet != AVERROR_EOF)) {
                    fatalError = true;
                    break;
                }
            }
            if (fatalError) {
                break;
            }
            if (decRecvRet < 0 && decRecvRet != AVERROR(EAGAIN) && decRecvRet != AVERROR_EOF) {
                std::fprintf(stderr, "CameraStream: decode receive_frame failed: %s\n",
                             avErrorToString(decRecvRet).c_str());
                fatalError = true;
                break;
            }
        }
        if (fatalError) {
            break;
        }

        ++passNumber;
        int seekRet = av_seek_frame(inputCtx_, videoStreamIndex_, 0, AVSEEK_FLAG_BACKWARD);
        if (seekRet < 0) {
            std::fprintf(stderr, "CameraStream: failed to seek back to start for loop: %s\n",
                         avErrorToString(seekRet).c_str());
            break;
        }
        avcodec_flush_buffers(decCtx_);
    }

    av_write_trailer(outputCtx_);
    av_packet_free(&pkt);
    av_packet_free(&encPkt);
    av_frame_free(&decFrame);
    av_frame_free(&scaledFrame);
}

} // namespace camsyringe
