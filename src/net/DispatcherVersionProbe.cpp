#include "net/DispatcherVersionProbe.h"

#include "net/TcpConnect.h"

#include <thread>

#include <sys/socket.h>
#include <unistd.h>

namespace camsyringe {

void DispatcherVersionProbe::queryAsync(std::string target, int controlPort, Callback callback) {
    std::thread([target = std::move(target), controlPort, callback = std::move(callback)]() {
        // Short timeout -- this must never make Play feel slow. A real
        // dispatcher answers near-instantly; anything slower than this is
        // as good as unreachable for the purpose of a version notice.
        std::string err;
        int fd = connectWithTimeout(target, controlPort, 3, &err);
        if (fd < 0) {
            callback(false, std::string());
            return;
        }
        // Bound the REPLY wait too, not just the connect -- confirmed for
        // real this matters: qcarcam_dispatcher accepts exactly one
        // connection at a time (main_dispatcher.cpp's own accept() loop),
        // so while a real Play/Pause session already holds the main
        // connection open, THIS probe's connection completes the TCP
        // handshake fine (connectWithTimeout() above succeeds) but sits
        // fully unanswered until that session's connection closes --
        // readLine() below blocks on a plain recv() with no timeout of
        // its own, so without this it would hang for the ENTIRE session
        // instead of failing fast, and its (by-then-irrelevant) reply
        // would only ever arrive right as Stop closes the main
        // connection -- popping this notice at a confusing moment well
        // after Play, looking "random", instead of never appearing while
        // a session is actively using the control channel (the correct
        // behavior: there's genuinely nothing to report while busy).
        timeval tv{3, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (!sendAll(fd, "VERSION\n")) {
            ::close(fd);
            callback(false, std::string());
            return;
        }
        std::string line;
        bool got = readLine(fd, line);
        ::close(fd);
        if (got && line.rfind("VERSION ", 0) == 0) {
            callback(true, line.substr(8));
        } else {
            callback(false, std::string());
        }
    }).detach();
}

} // namespace camsyringe
