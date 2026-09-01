#include "net/DispatcherClient.h"

#include "net/TcpConnect.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

namespace camsyringe {

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
    // 8s is generous for a LAN target, short enough for prompt UI feedback.
    std::string connectErr;
    int fd = connectWithTimeout(target, controlPort, 8, &connectErr);
    if (fd < 0) {
        callback({}, {}, true, connectErr);
        return;
    }
    socketFd_.store(fd);

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
        callback({}, {}, true, err);
        return;
    }

    // Read lines until DONE, rather than "read exactly cameras.size()
    // READY/ERROR lines, then one more expected to be DONE" -- the old
    // fixed count couldn't correctly handle PREVIEW_ERROR lines (see
    // PreviewIssue's own comment; qcarcam_dispatcher sends zero or more
    // of them between the READY/ERROR block and DONE) without either
    // miscounting them as a camera outcome or, worse, treating one as the
    // expected DONE line and silently discarding every line after it
    // (including a real subsequent PREVIEW_ERROR). This also incidentally
    // fixes a latent edge case: a whole-declaration rejection sends a
    // single "ERROR 0 ..." + DONE regardless of how many cameras were
    // actually declared, which the old fixed count could misread as a
    // dropped connection once more than one camera was in the
    // declaration (it would keep waiting for a 2nd READY/ERROR line that
    // was never coming).
    std::vector<CameraDeclareOutcome> outcomes;
    std::vector<PreviewIssue> previewIssues;
    for (;;) {
        std::string line;
        if (!readLine(fd, line)) {
            int expected = fd;
            if (socketFd_.compare_exchange_strong(expected, -1)) {
                ::close(fd);
            }
            callback({}, {}, true, "connection closed before every camera reported READY/ERROR");
            return;
        }
        if (line == "DONE") {
            break;
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
        } else if (line.rfind("PREVIEW_ERROR ", 0) == 0) {
            PreviewIssue issue;
            issue.camId = atoi(line.c_str() + 14);
            size_t sp = line.find(' ', 14);
            issue.reason = sp == std::string::npos ? std::string() : line.substr(sp + 1);
            previewIssues.push_back(std::move(issue));
        }
        // Any other/unrecognized line (shouldn't happen against a real
        // qcarcam_dispatcher) is ignored, not fatal -- forward-compatible
        // with a future line this client doesn't understand yet.
    }

    callback(outcomes, previewIssues, false, std::string());

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
