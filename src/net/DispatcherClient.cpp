#include "net/DispatcherClient.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace camsyringe {

namespace {

bool sendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Byte-at-a-time line reader -- this is the client-side counterpart of
// qcarcam_dispatcher's own readLine() (main_dispatcher.cpp), same
// protocol, same "control messages are a handful of short lines, never a
// performance path" reasoning for why simplicity wins over efficiency
// here. Returns false on EOF/error, including when disconnect() unblocks
// a pending recv() by shutdown()-ing the socket from another thread.
bool readLine(int fd, std::string& out) {
    out.clear();
    for (;;) {
        char c;
        ssize_t n = ::recv(fd, &c, 1, 0);
        if (n <= 0) return false;
        if (c == '\n') return true;
        if (c != '\r') out.push_back(c);
        if (out.size() > 256) return false; // malformed/oversized line -- bail
    }
}

} // namespace

DispatcherClient::DispatcherClient() = default;

DispatcherClient::~DispatcherClient() { disconnect(); }

void DispatcherClient::declareAsync(std::string target, int controlPort,
                                     std::vector<CameraDeclaration> cameras, bool injectOnly,
                                     bool qcxBypass, DeclareCallback callback) {
    // Self-cleaning: joins any previous session's thread first (whether it
    // finished on its own after a connect failure, or is still holding a
    // connection open) -- assigning a new std::thread over a still-
    // joinable one calls std::terminate(), so this isn't optional. Callers
    // don't need to remember to call disconnect() themselves between uses.
    disconnect();
    thread_ = std::thread(&DispatcherClient::threadFunc, this, std::move(target), controlPort,
                           std::move(cameras), injectOnly, qcxBypass, std::move(callback));
}

void DispatcherClient::disconnect() {
    int fd = socketFd_.exchange(-1);
    if (fd >= 0) {
        // shutdown() from this (GUI) thread unblocks whatever blocking
        // socket call threadFunc() is currently in (connect/select, or a
        // recv() inside readLine()) on the background thread -- the
        // standard, if imperfect, technique for cancelling a blocking
        // socket op from another thread. Acceptable here given this
        // class only ever has ONE declaration/connection in flight at a
        // time (a single desktop GUI app, not a high-concurrency server).
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void DispatcherClient::threadFunc(std::string target, int controlPort,
                                   std::vector<CameraDeclaration> cameras, bool injectOnly,
                                   bool qcxBypass, DeclareCallback callback) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    std::string portStr = std::to_string(controlPort);
    int rc = ::getaddrinfo(target.c_str(), portStr.c_str(), &hints, &result);
    if (rc != 0 || result == nullptr) {
        callback({}, true, "failed to resolve '" + target + "': " + gai_strerror(rc));
        return;
    }

    int fd = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd < 0) {
        std::string err = std::string("socket() failed: ") + strerror(errno);
        freeaddrinfo(result);
        callback({}, true, err);
        return;
    }
    socketFd_.store(fd);

    // Non-blocking connect + bounded select(), not a plain blocking
    // connect() -- an unreachable (but IP-routable) target can otherwise
    // hang for the OS's own much longer default TCP connect timeout (this
    // project's own history has a documented case of exactly this
    // "unreachable target looks like a hang" symptom, see CONTEXT.md).
    // 8s is generous for a LAN target, short enough for prompt UI
    // feedback.
    const int origFlags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, origFlags | O_NONBLOCK);

    rc = ::connect(fd, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);
    if (rc != 0 && errno != EINPROGRESS) {
        std::string err = std::string("connect to ") + target + ":" + portStr +
                           " failed: " + strerror(errno);
        int expected = fd;
        if (socketFd_.compare_exchange_strong(expected, -1)) {
            ::close(fd);
        }
        callback({}, true, err);
        return;
    }
    if (rc != 0) { // EINPROGRESS -- wait for it to complete or time out
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        timeval tv{8, 0};
        int sel = select(fd + 1, nullptr, &wfds, nullptr, &tv);
        int soerr = 0;
        socklen_t soerrLen = sizeof(soerr);
        if (sel <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &soerrLen) != 0 || soerr != 0) {
            std::string err = sel == 0 ? "connect to " + target + ":" + portStr + " timed out"
                                        : std::string("connect to ") + target + ":" + portStr +
                                              " failed: " + strerror(soerr != 0 ? soerr : errno);
            int expected = fd;
            if (socketFd_.compare_exchange_strong(expected, -1)) {
                ::close(fd);
            }
            callback({}, true, err);
            return;
        }
    }
    fcntl(fd, F_SETFL, origFlags); // back to blocking for the rest of the protocol

    std::string declaration;
    for (const auto& cam : cameras) {
        declaration += "CAM " + std::to_string(cam.camId) + " " + std::to_string(cam.port) + "\n";
    }
    if (injectOnly || qcxBypass) {
        declaration += "FLAGS";
        if (injectOnly) declaration += " --inject-only";
        if (qcxBypass) declaration += " --qcx-bypass";
        declaration += "\n";
    }
    declaration += "END\n";

    if (!sendAll(fd, declaration)) {
        std::string err = std::string("failed to send declaration: ") + strerror(errno);
        int expected = fd;
        if (socketFd_.compare_exchange_strong(expected, -1)) {
            ::close(fd);
        }
        callback({}, true, err);
        return;
    }

    std::vector<CameraDeclareOutcome> outcomes;
    while (outcomes.size() < cameras.size()) {
        std::string line;
        if (!readLine(fd, line)) {
            int expected = fd;
            if (socketFd_.compare_exchange_strong(expected, -1)) {
                ::close(fd);
            }
            callback({}, true, "connection closed before every camera reported READY/ERROR");
            return;
        }
        if (line.rfind("READY ", 0) == 0) {
            CameraDeclareOutcome outcome;
            outcome.camId = atoi(line.c_str() + 6);
            outcome.result = DeclareResult::Ready;
            outcomes.push_back(std::move(outcome));
        } else if (line.rfind("ERROR ", 0) == 0) {
            CameraDeclareOutcome outcome;
            outcome.camId = atoi(line.c_str() + 6);
            outcome.result = DeclareResult::Error;
            size_t sp = line.find(' ', 6);
            outcome.errorReason = sp == std::string::npos ? std::string() : line.substr(sp + 1);
            outcomes.push_back(std::move(outcome));
        }
        // Any other/unrecognized line (shouldn't happen against a real
        // qcarcam_dispatcher) is ignored, not fatal -- forward-compatible
        // with a future line this client doesn't understand yet.
    }

    // Expect "DONE" next -- not fatal if this fails or the line doesn't
    // match: every camera's real outcome is already known from the loop
    // above, and the "hold connection open" loop right after this handles
    // an early/unexpected close the same way regardless.
    std::string doneLine;
    readLine(fd, doneLine);

    callback(outcomes, false, std::string());

    // Hold the connection open, silently, as the teardown signal -- see
    // class comment. Returns as soon as disconnect() shuts the socket
    // down from the GUI thread, or the dispatcher itself closes it first
    // (e.g. target-side crash/process exit).
    std::string discard;
    while (readLine(fd, discard)) {
        // ignore
    }
    int expected = fd;
    if (socketFd_.compare_exchange_strong(expected, -1)) {
        ::close(fd);
    }
}

} // namespace camsyringe
