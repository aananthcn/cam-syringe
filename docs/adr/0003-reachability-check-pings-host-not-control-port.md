# 0003. Play-time reachability check pings the host, not the control port

Status: Accepted

## Context

A Play press needs to answer "is this even the right target address" fast
(a correct target responds well under a second) rather than silently
proceeding and only discovering a wrong/dead IP many seconds later via
`declareToTarget()`'s own 8s connect timeout (see `DispatcherClient`).

The first version of this pre-flight gate (`TargetReachabilityProbe`)
answered that question with a raw TCP connect to the CONTROL PORT
itself. This was wrong, confirmed live: a genuinely correct, reachable,
SSH-able target (`192.168.1.4` — its SHIM/REAL status query, entirely
independent, succeeded over SSH at the same time) got reported as "not
responding — check the target IP address" and had its whole session
auto-stopped, because `qcarcam_dispatcher` simply hadn't been started on
it yet — an ordinary, previously-recoverable state
`tryStartDispatcherThenRetry()` already exists specifically to handle
(SSH in, start it, retry). A raw connect to a closed port cannot tell
"wrong host" apart from "right host, service not up yet" — both refuse
the connection identically.

## Decision

The Play-time pre-flight check answers ONLY "is this a plausible, live
host at all", via ICMP ping (`net/PingProbe.h`'s `quickPingUnreachable()`
— the same logic `TargetSsh`'s own passwordless-probe fast-fail already
used, factored out so both share one implementation instead of two
independently-tuned ones). It never touches the control port or any
other service-specific check. Whether `qcarcam_dispatcher` itself is up
and answering stays entirely `declareToTarget()`/`onDeclareComplete()`'s
job, including the existing SSH-start-and-retry recovery — this gate
runs strictly *before* that, and only a CONCLUSIVE ping failure
auto-stops the session (`MainWindow::autoStopOnUnreachable()`).

## Consequences

- Any future Play-time (or similar) pre-flight target check must ask
  the same narrow host-liveness question, never substitute a
  service/port-specific probe for it — a closed port is inherently
  ambiguous between "wrong address" and "right address, service not up
  yet"; only a host-level check (ping, or equivalent) can tell those
  apart.
- Closes the "not yet addressed" gap CONTEXT.md's "Known gotchas" entry
  on unreachable-destination ~1fps used to flag (no code-level detection
  existed for a dead destination at all before this).
- An inconclusive ping (binary missing, ICMP blocked, no answer within
  the bound) is treated as reachable=true, never as a false negative —
  same contract `quickPingUnreachable()` already documented for
  `TargetSsh`'s own use of it.
