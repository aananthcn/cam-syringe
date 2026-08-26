#pragma once

#include "blf/BlfLoader.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace camsyringe {

// Replays a pre-loaded set of captured Ethernet frames (BlfLoader) out a
// raw AF_PACKET socket bound to a real network interface, pacing each
// send to its ORIGINAL inter-frame timing -- same DTS-paced
// clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...) design
// CameraStream already uses (see CameraStream.cpp's sleepUntilDeadline()),
// and able to share the SAME Timeline origin so BLF replay and camera
// streaming stay phase-aligned, matching CONTEXT.md's Architecture
// diagram (one shared Timeline orchestrator feeding both the camera
// streamer and the BLF/ETH replayer).
//
// Verbatim replay only, deliberately: every frame is sent byte-for-byte
// as captured (source/destination MAC, EtherType, VLAN tag, payload all
// untouched) -- no header rewriting. requires CAP_NET_RAW (root, or the
// binary needs that capability set) to open an AF_PACKET SOCK_RAW socket
// at all, same as any raw-socket tool (tcpreplay included).
//
// No looping -- unlike CameraStream's indefinite video loop, a BLF
// capture is played exactly once per run() call; requestStop() (or
// reaching the last frame) ends it. Call open() again for a fresh replay.
class BlfReplayer {
public:
    BlfReplayer() = default;
    ~BlfReplayer();

    BlfReplayer(const BlfReplayer&) = delete;
    BlfReplayer& operator=(const BlfReplayer&) = delete;

    // Loads `blfPath` (LoadBlfEthernetFrames()) and opens a raw AF_PACKET
    // socket bound to `interfaceName` (e.g. "eth0") -- both synchronous,
    // on the caller's thread, before run() is ever spawned. Returns false
    // (logging to stderr, and recording the same reason in lastError()) if
    // either the BLF has zero Ethernet frames or the socket/bind fails
    // (e.g. missing CAP_NET_RAW, or no such interface).
    bool open(const std::string& blfPath, const std::string& interfaceName);

    // Set only when open() just returned false -- the reason, suitable for
    // showing a caller-side UI (e.g. MainWindow's status bar), since
    // stderr isn't visible from a GUI session. Empty otherwise/before the
    // first open() call.
    const std::string& lastError() const { return lastError_; }

    // Shares a master clock origin (e.g. from the same Timeline
    // CameraStream/StreamPool use) instead of self-initializing one at the
    // start of run(). Call before run().
    void setStartOrigin(int64_t originNs);

    // Blocks, replaying every loaded frame in capture order, pacing each
    // send via clock_nanosleep against its original relative timestamp.
    // Returns once every frame has been sent, or requestStop() unblocks
    // it early -- both are a clean exit, not an error. open() must have
    // already succeeded. Expected to run on its own thread.
    void run();

    // Thread-safe; asks run() to return promptly (polled once per frame,
    // so within one frame's pacing interval).
    void requestStop();

    size_t frameCount() const { return frames_.size(); }

private:
    void sleepUntilDeadline(int64_t deadlineNs);

    std::vector<BlfEthernetFrame> frames_;
    int socketFd_ = -1;
    std::string lastError_;

    int64_t streamStartNs_ = 0;
    std::atomic<bool> startOriginSet_{false};
    int64_t externalStartOriginNs_ = 0;

    std::atomic<bool> stopRequested_{false};
};

} // namespace camsyringe
