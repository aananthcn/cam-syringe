#include "net/TcpConnect.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace camsyringe {

int connectWithTimeout(const std::string& host, int port, int timeoutSec, std::string* outErr) {
    addrinfo hints{};
    // AF_UNSPEC (not AF_INET): resolves to whichever family `host`
    // actually is -- IPv4, IPv6, or (for a hostname) whatever DNS
    // returns. This was hardcoded to AF_INET before, which made an IPv6
    // target fail to resolve at all (getaddrinfo() simply refuses to
    // return an IPv6 result under an IPv4-only hint) -- socket() just
    // below already correctly uses result->ai_family (not a hardcoded
    // one), so this one-line change is the whole fix here.
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    std::string portStr = std::to_string(port);
    int rc = ::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result);
    if (rc != 0 || result == nullptr) {
        if (outErr) *outErr = "failed to resolve '" + host + "': " + gai_strerror(rc);
        return -1;
    }

    int fd = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd < 0) {
        if (outErr) *outErr = std::string("socket() failed: ") + strerror(errno);
        freeaddrinfo(result);
        return -1;
    }

    const int origFlags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, origFlags | O_NONBLOCK);

    rc = ::connect(fd, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);
    if (rc != 0 && errno != EINPROGRESS) {
        if (outErr) {
            *outErr = std::string("connect to ") + host + ":" + portStr + " failed: " + strerror(errno);
        }
        ::close(fd);
        return -1;
    }
    if (rc != 0) { // EINPROGRESS -- wait for it to complete or time out
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        timeval tv{timeoutSec, 0};
        int sel = select(fd + 1, nullptr, &wfds, nullptr, &tv);
        int soerr = 0;
        socklen_t soerrLen = sizeof(soerr);
        if (sel <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &soerrLen) != 0 || soerr != 0) {
            if (outErr) {
                *outErr = sel == 0 ? "connect to " + host + ":" + portStr + " timed out"
                                    : std::string("connect to ") + host + ":" + portStr +
                                          " failed: " + strerror(soerr != 0 ? soerr : errno);
            }
            ::close(fd);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, origFlags); // back to blocking for the rest of the protocol
    return fd;
}

bool sendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

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

std::string bracketHostIfIPv6(const std::string& host) {
    if (host.find(':') != std::string::npos) {
        return "[" + host + "]";
    }
    return host;
}

} // namespace camsyringe
