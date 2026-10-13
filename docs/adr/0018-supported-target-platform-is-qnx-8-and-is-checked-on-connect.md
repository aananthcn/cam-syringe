# 0018. The supported target is QNX 8.0 with the current Qualcomm BSP, and it is checked on connect

Status: Accepted. The checks are unit-tested on canned output; not yet run on a board.

## Context

The 0.11 bench test (`release/artifacts/issues/0.11.0`) failed because the injector
could not start on the tester's target: `unknown symbol: _ZN8CameraDB11GetInstanceEv
referenced from libqcxplatform.so`, `ldd:FATAL: Could not resolve all symbols`. The
bundle is built with the QNX 8.0 toolchain against QNX 8.0 and the current Qualcomm
BSP's libraries, and the injector uses the board's own `libqcxplatform.so`, which on
that board needs a library the build doesn't link. The target was a different QNX
(7.1) or an older BSP. The dispatcher then reported "injector exited before reporting
ready", the receiver logged unrelated-looking RTP errors, and the test application
printed `ERROR` in every report. Nothing on the PC said that the target was the
wrong platform.

## Decision

- **Supported target: QNX 8.0 with the current Qualcomm BSP. QNX 7.1 and older
  Qualcomm BSPs are not supported**, by agreement with the team. Failures that come
  only from running on those are not bugs to fix here, and no work is done to make
  them run. This machine is also only licensed to build for QNX 8.0.
- **CamSyringe checks this when it connects**, in the existing background
  `QcxVersionProbe` that already runs the injector briefly on the target. The same ssh
  call now also runs `uname -sr` first, and the result is judged in two ways:
  - the QNX major version is not 8 (`unsupportedOs`), or
  - the injector's own output contains the loader's complaint (`unknown symbol`,
    `ldd:FATAL`, a missing library) (`libraryError`).
- **On either, Play is blocked, a running session is stopped, and the status bar
  says why** ("it runs QNX 7.1.0 ..." or "the injector can't load there (unknown
  symbol ...)"). This does not depend on `--inject-only`: nothing can be injected on
  such a target, so there is no preview to fall back on. The older API-version
  mismatch warning is skipped for such a target, since its result means nothing there.
- **An unreadable or non-QNX `uname` is not blocked.** If the probe can't tell, it says
  nothing, like the other probes: an unknown is not a fault.
- **The bundle installer repeats the library check at install time**
  (`qnx_toolkit` ADR 0003): it runs `ldd` on the installed programs and exits 1 with
  the loader's message if one can't load.

## Consequences

- A tester with an unsupported target sees the reason on connect or on the first Play,
  not a dead injector in a log on the board.
- The check runs while CamSyringe connects, so a Play pressed before it finishes may
  start a session that is stopped a moment later, as with the version check.
- The OS check is on the major version only. A QNX 8.x build with a different BSP is
  caught only by the library check, and only if the mismatch shows up as a loader
  error.
- Anything the probe can't reach (no passwordless ssh, no injector binary) is
  inconclusive and doesn't block, as before.
