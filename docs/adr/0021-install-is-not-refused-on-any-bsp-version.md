# 0021. Install Injector is never refused because of the BSP version

Status: Accepted. The bundle was reported working on the bench board (CARIAD OneBSP v14.4.1).
An install on a v14.2.6 board has not yet been confirmed from its log.

## Context

The testing team works on several BSP releases in parallel and does not itself know which one a
board runs. The requirement is that CamSyringe and `qcarcam-injector` work on any BSP in use.

The 0.12.2 report on a v14.2.6 board was `unknown symbol: cache_init referenced from
qcarcam_injector`. It looked like a BSP difference, and CamSyringe 0.13.0 refused older BSPs
because of it. It was not one:

- `cache_init` is a QNX SDK function (`sys/cache.h`, static archive `libcache.a`), not a BSP
  API. The injector's Makefile linked `-lcache` after `-lqcxclient`, and `ext/lib/libqcxclient.so`
  (a copy of the shim) also defines `cache_init`, so the linker took it from that shared library
  and never copied the static one in. The injector imported `cache_init` at load time and loaded
  only while the shim was the live `libqcxclient.so`.
- With the BSP's own `libqcxclient.so` live (REAL mode) the same injector failed with the same
  message on the v14.4.1 bench board. The BSP's `libqcxclient.so.real` there has no
  `cache_init`, and neither has any other library in the injector's chain.
- Fixed in `qcarcam-injector` 0.13.0 by putting `-lcache` first; verified on v14.4.1 in both
  REAL and SHIM mode.

## Decision

- **No BSP classification, no minimum version, no refusal, no override.** CamSyringe does not
  decide from a version number whether the bundle will work.
- **The BSP is recorded**: Install Injector runs `cat /etc/os-release; uname -a` first, and the
  output goes into `~/.cache/camsyringe/install.log` and the ADR 0020 diagnostic log, so a
  report always says which BSP the board runs.
- **Whether the programs work is decided by starting them**: the bundle's own check after the
  install (`--help` of each program) and the ADR 0020 log, which names a missing library or
  symbol.
- **If a real BSP difference ever appears** (a symbol that really is missing from some BSP's
  libraries), the diagnostic log names it. Then: collect the symbol tables and `DT_NEEDED` of the
  libraries involved from that board, and either ship a per-variant injector or resolve that one
  symbol with `dlopen`/`dlsym`. Nothing is built for this now, because none has been seen.
- **QNX 8.0 only**, by agreement with the testing team (ADR 0018); QNX 7.1 is not built.

## Consequences

- No board is refused for its version. A program that cannot load fails after the install, with
  the loader's message in the log, instead of the board being refused before it.
- The previous install is removed before the new one is checked. The ADR 0020 log and the
  bundle's FAIL line say what failed; re-installing a working bundle restores the board.
- The injector must import nothing from the shim or from a BSP library except the public
  `QCarCam*()` API; check `nm -D --undefined-only` on each program when building the bundle.
- ADR 0018's statement that older Qualcomm BSPs are not supported concerns a different failure
  (`libqcxplatform`) and is unchanged.

## Lessons

- **A failure that appears on one board is not evidence of a platform difference until it is
  reproduced.** The v14.2.6 failure was assumed to be the BSP. Putting the real library first
  on the *working* board reproduced it in minutes. Try to break the known-good system the same
  way before blaming the other one.
- **A version number says nothing about whether a program loads.** A gate on `VERSION_ID` turned
  a wrong guess into a refusal with a confident, false message, and blocked a bundle that works.
  Test the actual property (start the program) rather than a proxy for it.
- **`ldd` on QNX lists libraries and does not resolve symbols.** It passed the failing injector.
  Start the program (`--help`) to check that it loads.
- **Link order decides where a symbol comes from.** A library earlier on the link line that
  also defines a symbol wins over a later `-l`, so a symbol can end up imported when it was meant
  to be linked in. `make` does not relink when only the Makefile changes; delete the output.
- **Check a program is complete in both modes** (REAL and SHIM `libqcxclient.so`), because the
  shim can hide a dependency that the BSP's own library does not provide.
- **Do not assume the dependency is a platform one.** `cache_init` came from the SDK, and a
  glance at the library, not the BSP, said so.
