#include "blf/BlfReplayer.h"

#include "util/Clock.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

namespace camsyringe {

BlfReplayer::~BlfReplayer() {
    if (socketFd_ >= 0) {
        ::close(socketFd_);
    }
}

void BlfReplayer::setStartOrigin(int64_t originNs) {
    externalStartOriginNs_ = originNs;
    startOriginSet_.store(true, std::memory_order_release);
}

void BlfReplayer::requestStop() { stopRequested_.store(true, std::memory_order_release); }

bool BlfReplayer::open(const std::string& blfPath, const std::string& interfaceName) {
    lastError_.clear();

    frames_ = LoadBlfEthernetFrames(blfPath);
    if (frames_.empty()) {
        // LoadBlfEthernetFrames() already logged the specific reason to
        // stderr; this is the console-less-caller-facing equivalent.
        lastError_ = "'" + blfPath + "' has no Ethernet-frame objects (see console for details)";
        return false;
    }

    // ETH_P_ALL (not a specific EtherType) -- this socket sends complete,
    // already-constructed frames (BlfLoader already put the real
    // destination MAC/EtherType/VLAN tag in place); it doesn't need the
    // kernel to fill in or interpret anything beyond "put these bytes on
    // the wire".
    socketFd_ = ::socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (socketFd_ < 0) {
        std::fprintf(stderr,
                      "BlfReplayer: socket(AF_PACKET, SOCK_RAW) failed: %s (needs CAP_NET_RAW, "
                      "e.g. run as root or `sudo setcap cap_net_raw+ep` on this binary)\n",
                      strerror(errno));
        lastError_ = std::string("raw socket open failed: ") + strerror(errno) +
                     " (needs CAP_NET_RAW -- run `sudo setcap cap_net_raw+ep` on this binary, "
                     "then try again; a rebuild wipes this and it must be re-set)";
        return false;
    }

    unsigned ifIndex = if_nametoindex(interfaceName.c_str());
    if (ifIndex == 0) {
        std::fprintf(stderr, "BlfReplayer: no such interface '%s': %s\n", interfaceName.c_str(),
                      strerror(errno));
        lastError_ = "no such network interface '" + interfaceName + "': " + strerror(errno);
        ::close(socketFd_);
        socketFd_ = -1;
        return false;
    }

    sockaddr_ll addr{};
    addr.sll_family = AF_PACKET;
    addr.sll_protocol = htons(ETH_P_ALL);
    addr.sll_ifindex = static_cast<int>(ifIndex);
    if (::bind(socketFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "BlfReplayer: bind() to '%s' failed: %s\n", interfaceName.c_str(),
                      strerror(errno));
        lastError_ = "bind to interface '" + interfaceName + "' failed: " + strerror(errno);
        ::close(socketFd_);
        socketFd_ = -1;
        return false;
    }

    std::fprintf(stderr, "BlfReplayer: ready -- %zu frame(s) on '%s'\n", frames_.size(),
                 interfaceName.c_str());
    return true;
}

void BlfReplayer::sleepUntilDeadline(int64_t deadlineNs) {
    int64_t absoluteNs = streamStartNs_ + deadlineNs;
    timespec ts{};
    ts.tv_sec = absoluteNs / 1000000000LL;
    ts.tv_nsec = absoluteNs % 1000000000LL;

    int ret;
    do {
        ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
    } while (ret == EINTR);
}

void BlfReplayer::run() {
    streamStartNs_ = startOriginSet_.load(std::memory_order_acquire) ? externalStartOriginNs_
                                                                      : monotonicNowNs();

    for (const auto& frame : frames_) {
        if (stopRequested_.load(std::memory_order_acquire)) {
            break;
        }
        sleepUntilDeadline(frame.timestampNs);
        if (stopRequested_.load(std::memory_order_acquire)) {
            break;
        }
        ssize_t sent = ::send(socketFd_, frame.rawFrame.data(), frame.rawFrame.size(), 0);
        if (sent < 0) {
            std::fprintf(stderr, "BlfReplayer: send() failed: %s (continuing with next frame)\n",
                          strerror(errno));
        }
    }
}

} // namespace camsyringe
