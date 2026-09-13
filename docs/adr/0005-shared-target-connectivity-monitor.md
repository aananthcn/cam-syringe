# 0005. One shared connectivity monitor, not two independent checks

Status: Accepted

## Context

Two separate features each independently answered "is `currentTarget_`
alive" against the same target, on different cadences, via different
mechanisms:

- The SHIM/REAL status feature (`refreshShimStatus()`, ticking every 1s
  via `shimStatusTimer_`) went straight to `TargetSsh::ensureAuth()`,
  which has its own internal ping pre-check
  (`quickPingUnreachable()`) before attempting SSH.
- The Play-time reachability gate (ADR 0003) ran its own dedicated
  one-shot `TargetReachabilityProbe`, pinging the same target
  independently, once per Play press.

Spotted as real, avoidable duplication: both exist purely to answer the
same narrow "is this host even alive" question, just wired to two
different call sites with no shared state between them. Beyond the
wasted duplicate pings, this meant `BLIND` (the SHIM/REAL status
feature's "not connected" state) and Play's own reachability verdict
could each reach a different answer for the same target if their
independent checks happened to land at different moments — no single
place a caller could ask "is the target up" and get one consistent
answer.

## Decision

`net/TargetConnectivityMonitor.h/.cpp`: one `QObject`-based monitor,
owned by `MainWindow`, holding a tri-state (`Unknown` / `Reachable` /
`Unreachable`) for `currentTarget_`. It pings on the same ~1s cadence
`shimStatusTimer_` used to drive `refreshShimStatus()` directly, restarts
(resetting to `Unknown` and probing immediately, not waiting out the
first interval) whenever `currentTarget_` changes, and exposes:

- `state()` — the current answer.
- `setStateChangedCallback()` — fires only on an actual transition.
- `probeNow(callback)` — answers synchronously from `state()` if already
  known; if `Unknown`, ensures a probe is in flight right now (never
  waits out the rest of the regular interval) and answers once it
  resolves.

Both features now depend on this ONE monitor instead of running their
own checks:

- `refreshShimStatus()`: `BLIND` is DERIVED from `state()` directly — if
  `state() != Reachable`, it shows `BLIND` and returns without ever
  attempting SSH at all (saving `ensureAuth()`'s own worst-case ~8s for a
  target already known unreachable). Only when `Reachable` does it run
  its own strictly-additional SSH + board-file check on top — the
  monitor answers a narrower question than SHIM/REAL needs, and never
  replaces that stronger check, only gates whether it's worth attempting.
- `MainWindow::beginReachabilityCheck()`: calls `probeNow()` instead of
  spawning `TargetReachabilityProbe` — the common case (monitor already
  ticking since launch/Configure) resolves SYNCHRONOUSLY, on the same
  call stack, so Play no longer shows an artificial "Searching…" pause
  when the answer was already known. Only the rare `Unknown` case (Play
  pressed within ~1-2s of launch/Configure, before the first probe
  resolved) actually waits.
- `TargetReachabilityProbe` is deleted outright — fully superseded, see
  ADR 0003's own implementation note.

## Consequences

- One ping cadence per target, not two — `TargetSsh::ensureAuth()`'s own
  internal pre-check is now largely redundant for calls that go through
  `refreshShimStatus()` (still exercised, harmlessly, since `ensureAuth()`
  is also called directly by several OTHER SSH call sites this ADR
  deliberately did NOT touch — `toggleShimStatus()`, `resolveCameraGeometry()`,
  `tryStartDispatcherThenRetry()`, `killTargetProcesses()`,
  `InjectorBundleInstaller` — those still rely on `ensureAuth()`'s own
  pre-check + failure cache unchanged; only the two features named above
  were in scope here).
- `BLIND` now means one specific thing at the connectivity layer
  (`connectivityMonitor_.state() != Reachable`) rather than being an
  independently-derived guess — any future feature needing the same
  "is the target here" fact should ask `connectivityMonitor_` rather
  than adding a third independent check.
- `TargetConnectivityMonitor` only ever answers the host-liveness
  question (ADR 0003's own scope) — it does NOT know whether SSH, the
  control port, or `qcarcam_dispatcher` itself is reachable/running.
  Those remain separate, stronger checks layered on top where needed.
