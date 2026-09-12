# 0002. real-ref sync derived from target, not bundle

Status: Accepted

## Context

`/var/opt/lib/real-ref/libqcxclient.so` is a reference copy two
independent consumers depend on: `qcarcam-injector`'s shim
(`probeRealInputs()`, its first candidate path) and this project's own
`CameraGeometryResolver` (per-camera resolution discovery, via
`LD_LIBRARY_PATH`). It used to be shipped inside the injector bundle,
sourced from the build machine's `ext/lib/`. Confirmed live this was
wrong — see `qcarcam-injector`'s own
[0001-real-ref-reference-copy-contract](../../../qnx/qnx_toolkit/work-dir/qcarcam-injector/docs/adr/0001-real-ref-reference-copy-contract.md)
and `qnx_toolkit`'s
[0001-injector-bundle-must-not-ship-board-specific-reference-copies](../../../qnx/qnx_toolkit/docs/adr/0001-injector-bundle-must-not-ship-board-specific-reference-copies.md)
for the full story.

## Decision

`src/net/RealRefSync.h/.cpp` (`realRefSyncCommand()`) is the single
shared shell command that (re)populates `real-ref` from whichever board
file is CURRENTLY the genuine real driver: `.real` backup if the
SHIM/REAL toggle has already swapped it away, else the live file itself
(still genuine in that case). Never anything build-machine-sourced.

Run from two places, since the two consumers above don't otherwise
coincide in time:

- `InjectorBundleInstaller`, right after every install (best-effort, not
  fatal) — so `CameraGeometryResolver`'s discovery works even if SHIM/REAL
  is never toggled this session.
- `MainWindow`'s REAL→SHIM toggle itself — so `real-ref` is refreshed at
  the exact moment the genuine file is about to be swapped away
  (matters for the very first toggle after a fresh install).

## Consequences

- Any future code path that also needs a board-genuine reference file
  must call `realRefSyncCommand()` rather than inventing its own copy
  logic — keeps this a single source of truth instead of two
  independently-maintained copies drifting apart (the exact failure mode
  that caused this ADR to exist).
- `real-ref` is now always correct with respect to `libc`
  compatibility. It does NOT, and cannot, guarantee QCX API version
  compatibility — that's a build-time header concern, tracked separately
  in `qcarcam-injector`'s
  [0002-qcx-client-api-version-must-match-board](../../../qnx/qnx_toolkit/work-dir/qcarcam-injector/docs/adr/0002-qcx-client-api-version-must-match-board.md).
