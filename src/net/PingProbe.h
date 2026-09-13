#pragma once

#include <QString>

namespace camsyringe {

// Fast, best-effort ICMP reachability check, bounded to a couple of
// seconds -- factored out of TargetSsh.cpp's own original passwordless-
// probe pre-check (see there for the full "why ping first" rationale)
// so any other caller needing the same "is this even a plausible address
// at all" answer (e.g. TargetReachabilityProbe) uses the identical,
// already-proven logic rather than a second, slightly-different
// reimplementation.
//
// Deliberately used ONLY to fail fast on a NEGATIVE result -- never
// treated as conclusive proof of reachability on a positive one (a
// target can legitimately block ICMP while everything else works fine),
// and never treated as conclusive if ping itself doesn't run cleanly
// (missing binary, unexpected error) -- an inconclusive ping always
// falls through to whatever real check the caller actually needs, same
// as a reachable one. So this can only ever make an already-going-to-
// fail case faster, never turn a working target into a false negative.
bool quickPingUnreachable(const QString& host);

} // namespace camsyringe
