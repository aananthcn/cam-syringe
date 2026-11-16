# 0020. Install Injector is refused on a BSP older than the one the bundle is built for

Status: Accepted. The version check is unit-tested (`tests/target_bsp_test.cpp`); not yet
run as a full install on a v14.2.6 board.

**Amended by [ADR 0022](0022-injector-links-cache-init-itself-and-bsp-variants-are-deferred.md):**
the working explanation below (v14.2.6's libraries lack `cache_init`) is probably wrong.
`cache_init` is a QNX SDK function the injector never linked in; the v14.4.1 bench board
fails with the same message when the real `libqcxclient.so` is live instead of the shim.
The gate may therefore refuse boards that would work, and should be relaxed or removed once
a bundle whose injector links `-lcache` has been tried on a v14.2.6 board. Until then it
stays, with the `CAMSYRINGE_ALLOW_UNSUPPORTED_BSP=1` override.

## Context

The 0.12.2 bug report (`release/artifacts/issues/0.12.2`): Install Injector failed on a
second tester's board with `exit code 1` and, in the diagnostics, `unknown symbol:
cache_init referenced from qcarcam_injector` / `ldd:FATAL: Could not resolve all symbols`.
That board runs **CARIAD OneBSP v14.2.6** (build 2026_09_17); the bench board, where the
same bundle installs and loads, runs **v14.4.1**. Both report the same QNX 8.0 build, so
the difference is in the BSP's libraries, which the injector links against at load time.
(That `cache_init` comes from a library v14.2.6 lacks is the working explanation, not
confirmed: the diagnostics of ADR 0021 will say where the name is and is not.)

[ADR 0018](0018-supported-target-platform-is-qnx-8-and-is-checked-on-connect.md) already
says older Qualcomm BSPs are not supported, and detects an unloadable injector on connect.
It does not stop an install. Install Injector first runs `rm -rf /var/opt/bin /lib
/include`, so on such a board it deleted whatever injector was there, whether or not that
one worked, and replaced it with one that cannot load.

## Decision

- **Before it touches the target, the installer reads `/etc/os-release`** and compares
  `VERSION_ID` with the oldest BSP the bundle is built for, **v14.4.1**
  (`TargetBsp::minimumVersion()`), numerically per component (so `v14.10.0` is newer than
  `v14.4.1`).
  - **Older: refused.** The dialog says which BSP the board runs, which is the minimum,
    that nothing was changed on the target, and what to do (update the BSP, or use a
    bundle built for it). The check runs right after authentication, before the confirm
    dialog.
  - **v14.4.1 exactly: verified** (the only release the bundle has been run on). **Newer:
    untested but allowed**, noted in the log; the bundle's own load check still runs after.
  - **Unreadable or no parsable `VERSION_ID`: not blocked**, noted in the log. An unknown
    is not a fault (as in ADR 0018).
- **Override: start CamSyringe with `CAMSYRINGE_ALLOW_UNSUPPORTED_BSP=1`**, for someone who
  wants to try a bundle on an older board knowing the injector will probably not load. It
  is logged.
- **The BSP version is always in `~/.cache/camsyringe/install.log`** (the output of
  `cat /etc/os-release; uname -a`, and the verdict).

## Relevance check

`InjectorBundleInstaller.cpp` runs the `target-os-release` step and `TargetBsp::classify()`
before `confirm()`; a `TooOld` verdict calls the result callback without running `rm`.

## Consequences

- The minimum, and the list of verified releases, are constants in `TargetBsp.cpp`. Raise
  the verified list when the bundle is run on another release, and the minimum if a newer
  bundle needs a newer BSP. Nothing else needs to change.
- A board whose BSP is between v14.2.6 and v14.4.1 (v14.3.x) is refused although it may
  work; no such board has been seen. The override is for that.
- This gate does not replace the load check: a newer or unreadable BSP can still fail to
  load, and the bundle's check and ADR 0021's log cover that.
