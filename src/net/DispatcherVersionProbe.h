#pragma once

#include <functional>
#include <string>

namespace camsyringe {

// One-shot query of the target's qcarcam_dispatcher version, over the
// SAME control port DispatcherClient talks to but as a completely
// separate, short-lived connection (bounded by an actual receive
// timeout, not just the connect -- see the .cpp's own comment on why
// that matters against a dispatcher that only ever handles one
// connection at a time) -- deliberately NOT woven into
// DispatcherClient's own CAM/FLAGS/END declaration protocol (see
// qcarcam-injector/src/main_dispatcher.cpp's parseDeclaration(): any
// unrecognized first line there rejects the WHOLE declaration, so
// prepending a version line to a real declaration would break an old
// dispatcher outright). Instead this sends a bare "VERSION" line as its
// own connection; a NEW dispatcher answers "VERSION <string>" and closes,
// an OLD dispatcher (or anything else that goes wrong -- unreachable,
// timeout, unexpected reply) is treated the same way: version unknown,
// never an error. Runs on its own background thread, callback marshaled
// to the GUI thread by the caller (see DispatcherClient's own class
// comment for why -- same convention).
class DispatcherVersionProbe {
public:
    // ok=false means "couldn't determine" (old dispatcher that doesn't
    // understand VERSION, unreachable target, or timeout) -- version is
    // then empty and this is NOT an error condition callers should
    // surface; it just means no comparison can be made.
    using Callback = std::function<void(bool ok, std::string version)>;

    static void queryAsync(std::string target, int controlPort, Callback callback);
};

} // namespace camsyringe
