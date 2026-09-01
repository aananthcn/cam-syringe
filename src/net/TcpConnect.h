#pragma once

#include <string>

namespace camsyringe {

// Resolves host:port and connects with a bounded timeout, via a
// non-blocking connect() + select() -- NOT a plain blocking connect(),
// since an IP-routable but unreachable target can otherwise hang for the
// OS's own much longer default TCP connect timeout (see this project's
// history for a documented "unreachable target looks like a hang" case).
// On success returns the connected fd, restored to blocking mode (ready
// for ordinary send()/recv()). On failure returns -1 and fills *outErr.
// Shared by DispatcherClient (the long-lived declare connection) and
// DispatcherVersionProbe (a short-lived one-shot query) -- this is the
// non-trivial half of both; extracted here instead of duplicated.
int connectWithTimeout(const std::string& host, int port, int timeoutSec, std::string* outErr);

bool sendAll(int fd, const std::string& data);

// Byte-at-a-time line reader -- this is the client-side counterpart of
// qcarcam_dispatcher's own readLine() (main_dispatcher.cpp), same
// protocol, same "control messages are a handful of short lines, never a
// performance path" reasoning for why simplicity wins over efficiency
// here. Returns false on EOF/error, including when a socket shutdown()
// from another thread unblocks a pending recv().
bool readLine(int fd, std::string& out);

} // namespace camsyringe
