#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace camsyringe {

// One camera's QCarCam id + the RTP port CamSyringe will stream it to --
// exactly what a "CAM <id> <port>" declaration line needs.
struct CameraDeclaration {
    int camId = 0;
    int port = 0;
};

enum class DeclareResult { Ready, Error };

struct CameraDeclareOutcome {
    int camId = 0;
    DeclareResult result = DeclareResult::Error;
    std::string errorReason; // empty when result == Ready
};

// Talks to qcarcam_dispatcher's control-channel protocol on the target
// (see qcarcam-injector/ARCHITECTURE.md items 29/30 for the target-side
// half of this): declares which QCarCam id each camera's RTP stream
// should be injected into, waits for a READY/ERROR verdict per camera,
// then HOLDS THE CONNECTION OPEN for the rest of the session -- closing
// it (disconnect()) is itself the target-side teardown signal
// (main_dispatcher.cpp tears down everything a connection declared the
// moment it closes), not a separate message. This class's lifetime should
// therefore span Play through Stop/Pause, not just the initial handshake
// -- see MainWindow's own state machine.
//
// Plain POSIX sockets on a background std::thread, not QTcpSocket --
// matches this project's existing StreamPool convention (background
// std::thread + a callback the caller marshals to the GUI thread via
// QMetaObject::invokeMethod) rather than introducing a second, Qt-Network-
// based async style alongside it.
class DispatcherClient {
public:
    // Called from the background thread once every camera's READY/ERROR
    // is known, or immediately with connectFailed=true if the initial TCP
    // connect (or the declaration send) itself failed -- the caller MUST
    // marshal to the GUI thread itself (e.g. QMetaObject::invokeMethod),
    // same convention as StreamPool::PreviewCallback/ErrorCallback.
    using DeclareCallback = std::function<void(std::vector<CameraDeclareOutcome> outcomes,
                                                bool connectFailed, std::string connectError)>;

    DispatcherClient();
    ~DispatcherClient();

    DispatcherClient(const DispatcherClient&) = delete;
    DispatcherClient& operator=(const DispatcherClient&) = delete;

    // Connects to target:controlPort, sends one "CAM <id> <port>" line per
    // entry in `cameras` plus an optional "FLAGS ..." line (only emitted
    // if injectOnly or qcxBypass is true) then "END", reads back exactly
    // cameras.size() READY/ERROR lines followed by DONE, and invokes
    // `callback` -- exactly once, either with the per-camera outcomes or
    // with connectFailed set. After the callback fires, the connection
    // stays open (silently) until disconnect() is called -- see class
    // comment. Call disconnect() before calling this again (starting a
    // second declaration while one is already active/connected is not
    // supported).
    void declareAsync(std::string target, int controlPort, std::vector<CameraDeclaration> cameras,
                       bool injectOnly, bool qcxBypass, DeclareCallback callback);

    // Closes the connection -- the target-side teardown signal. Safe to
    // call even if declareAsync() was never called, already completed, or
    // is still in flight (unblocks the background thread's blocking
    // socket calls, which then exits promptly). Joins the background
    // thread before returning, so it's safe to call declareAsync() again
    // immediately after.
    void disconnect();

private:
    void threadFunc(std::string target, int controlPort, std::vector<CameraDeclaration> cameras,
                     bool injectOnly, bool qcxBypass, DeclareCallback callback);

    std::thread thread_;
    // Set once the socket is created (even before connect() completes) so
    // disconnect() can shutdown()+close() it from another thread to
    // unblock whatever blocking call threadFunc() is currently in --
    // std::atomic since it's written on the background thread and read/
    // acted on from the GUI thread.
    std::atomic<int> socketFd_{-1};
};

} // namespace camsyringe
