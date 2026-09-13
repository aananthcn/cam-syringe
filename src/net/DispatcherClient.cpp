#include "net/DispatcherClient.h"

#include "net/TcpConnect.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

namespace camsyringe {

namespace {
void shutdownSocket(std::atomic<int>& socketFd) {
    int fd = socketFd.exchange(-1);
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
}
}  // namespace

DispatcherClient::DispatcherClient() = default;

DispatcherClient::~DispatcherClient() {
    // Deliberately NOT a plain disconnect() call, and MUST join()
    // synchronously here, unlike disconnect() itself -- this object is
    // about to be destroyed, and threadFunc() runs as a member function
    // bound to `this` (declareAsync()'s std::thread constructor call).
    // disconnect()'s own non-blocking hand-off is safe mid-session
    // because the object lives on and a late callback is already
    // tolerated (see its own comment) -- neither holds here, where a
    // still-running detached thread would end up dereferencing a
    // dangling `this` the moment this destructor returned and the
    // object's memory was freed. A real use-after-free, not just a
    // theoretical one -- caught specifically while fixing disconnect()'s
    // own GUI-freeze bug below, so this needed calling out explicitly
    // rather than just inheriting disconnect()'s new behavior blindly.
    shutdownSocket(socketFd_);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void DispatcherClient::declareAsync(std::string target, int controlPort,
                                     std::vector<CameraDeclaration> cameras, bool injectOnly,
                                     DeclareCallback callback) {
    // Self-cleaning: joins any previous session's thread first (whether it
    // finished on its own after a connect failure, or is still holding a
    // connection open) -- assigning a new std::thread over a still-
    // joinable one calls std::terminate(), so this isn't optional. Callers
    // don't need to remember to call disconnect() themselves between uses.
    disconnect();
    thread_ = std::thread(&DispatcherClient::threadFunc, this, std::move(target), controlPort,
                           std::move(cameras), injectOnly, std::move(callback));
}

void DispatcherClient::disconnect() {
    // shutdown() from this (GUI) thread unblocks whatever blocking
    // socket call threadFunc() is currently in (a recv() inside
    // readLine(), most commonly -- the fd is only ever stored here once
    // connectWithTimeout() has ALREADY succeeded, see socketFd_'s own
    // comment) -- the standard, if imperfect, technique for cancelling a
    // blocking socket op from another thread. Acceptable here given this
    // class only ever has ONE declaration/connection in flight at a time
    // (a single desktop GUI app, not a high-concurrency server).
    shutdownSocket(socketFd_);
    // NEVER thread_.join() HERE -- see this function's own header
    // comment (DispatcherClient.h) for the real bug this caused: if
    // threadFunc() is still inside connectWithTimeout() itself (target
    // unreachable, fd not stored yet, so the shutdown() above had
    // nothing to act on), a synchronous join() on the CALLING thread
    // blocks for as long as that connect attempt takes -- and
    // MainWindow::onStopTriggered() calls disconnect() directly on the
    // GUI thread, so this froze the entire window. Hand the thread off
    // to its own detached cleanup thread instead; thread_ itself is left
    // non-joinable immediately (via the move), so declareAsync() can
    // safely start a new one right after this returns, same contract as
    // before.
    if (thread_.joinable()) {
        std::thread([t = std::move(thread_)]() mutable { t.join(); }).detach();
    }
}

void DispatcherClient::threadFunc(std::string target, int controlPort,
                                   std::vector<CameraDeclaration> cameras, bool injectOnly,
                                   DeclareCallback callback) {
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
    if (injectOnly) {
        declaration += "FLAGS --inject-only\n";
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
