# 0006. Replace `libqcxclient.so` with a shim via a target-side symlink swap, shipped only in the injector bundle

Status: Accepted

## Context

Validation teams need consumers of camera frames (FrameGrabber and future
processes) to receive deterministic injected content with no code change
and no test-only backend switch on their side. `qcarcam-injector`'s
`former ARCHITECTURE.md (map: docs/adr/README.md)` item 36 chose to do this by reimplementing the public
`QCarCam*()` client API as `libqcxclient_shim.so` (soname
`libqcxclient.so`), not by mocking `qcxserver`, whose wire protocol is
private and undocumented. `qcxserver` never links the client library, so
swapping it only changes what client processes see.

Two deployment options were weighed: per-process `LD_PRELOAD` (confirmed
to work on QNX 8) or a system-wide file swap at
`/mnt/lib64/camera/libqcxclient.so`. The file swap was chosen because it
scales to "many other processes" without editing each one's launch
environment. `/mnt` is read-only but not verity-protected, so
`mount -uvw /mnt` works live.

CamSyringe needs a one-click way to see and flip which library is live.
An earlier revision found a loose shim build on the PC and `scp`ed it to
the target during the toggle. That was a second delivery path for the
same file and was removed.

## Decision

The toggle is implemented in `MainWindow::toggleShimStatus()` and
`refreshShimStatus()` (`src/ui/MainWindow.cpp`), as coded:

- **`/mnt/lib64/camera/libqcxclient.so` is a symlink, switched, never
  copied.** In REAL it points at `libqcxclient.so.real` (relative, same
  directory). In SHIM it points at `/var/opt/lib/libqcxclient_shim.so`.
  Copying onto `/mnt` was dropped because a copy can fail partway and
  leave a truncated library for every client. A symlink swap cannot.
- **State is read from the link.** Queried over SSH: if the live name is a
  symlink, SHIM iff `ls -l` shows it ending in `libqcxclient_shim.so`,
  else REAL. A regular file with a `.real` beside it is the old copied-shim
  layout and still reads SHIM. A regular file with no `.real` was never
  toggled and reads REAL. No target-side binary or protocol is added.
- **REAL→SHIM**, one target-side command: remount `/mnt` rw, then fail
  with the `CAMSYRINGE_SHIM_NOT_INSTALLED` marker if
  `/var/opt/lib/libqcxclient_shim.so` is missing, **before** anything is
  changed, so the link can never dangle. Then `chmod 555` the shim (the real
  library is `-r-xr-xr-x`, the bundle installs the shim `0644`), so a failure
  aborts before `/mnt` is touched. Then build the new symlink under
  the scratch name `libqcxclient.so.new`. Then
  `[ -L live ] || [ -e .real ] || mv live .real`, so an existing backup is
  **never overwritten** and the real file is moved only on the first
  toggle. Then refresh `real-ref` (ADR 0002). Last, `mv -f` renames the
  scratch link over the live name. This is atomic, and every step that can
  fail runs before the live name is touched.
- **SHIM→REAL**: remount `/mnt` rw, refuse if `.real` is missing (so the
  link can't dangle), then build a symlink to `libqcxclient.so.real` under
  the scratch name and `mv -f` it over the live name. This also replaces a
  copied shim from the old layout.
- **No PC↔target file transfer, ever.** The shim reaches a target only
  through the one injector bundle install
  (`create-qcarcam-inj-bundle.py` → `/var/opt/lib/libqcxclient_shim.so`).
  If it is missing, the toggle fails with a message pointing at
  **⇪ Install Injector**. It does not look for a substitute on the PC.
- **`mount -uvw /mnt` is prefixed to every toggle.** It is idempotent, so
  mount state is not tracked.
- **Failure and uncertainty show BLIND, never a guess.** A failed toggle
  (which may have partially applied) goes to BLIND. Double-click is
  ignored while BLIND, and BLIND also covers the in-flight window, so a
  second click cannot overlap.
- **Credentials:** the deliberate double-click toggle may prompt for SSH
  credentials. Automatic refreshes (window show, Configure apply, the 1s
  `shimStatusTimer_`, post-toggle) are passwordless-only and never
  prompt. `shimBusy_` guards against overlapping SSH attempts.
- **Connectivity gating (ADR 0005):** the refresh shows BLIND without
  attempting SSH unless `connectivityMonitor_` reports Reachable.
- **`qcxserver` is never touched or restarted.** A client that already
  has the library mapped keeps whichever copy it loaded; only clients
  started after the toggle see the new one.

## Consequences

- REAL→SHIM works only on a target that has had the current bundle
  installed at least once.
- **A dangling link is the new failure mode.** While SHIM is active, the
  live `libqcxclient.so` depends on `/var/opt/lib/libqcxclient_shim.so`.
  Reinstalling the bundle (which clears `/var/opt/lib`) or a `/var` that
  isn't mounted early at boot leaves clients without the library until the
  shim is back. The old copy design had no such dependency. The toggle
  checks for the shim before switching, but nothing guards later removal.
- The real library must stay at `libqcxclient.so.real` permanently, even in
  REAL. It is the link target, not just a backup.
- The `chmod 555` on the shim is untested on the target (it is idempotent;
  confirm with `ls -l /var/opt/lib/libqcxclient_shim.so` after a toggle).
- Not tested on the target. `ln -s`, `mv -f` and `ls -l | grep` on the
  board's QNX shell, and the `-L` test, are assumed to behave as on a
  POSIX `sh`.
- Building or shipping the shim is inert by itself. It takes effect only
  when this toggle (or a manual swap) puts it at
  `/mnt/lib64/camera/libqcxclient.so`.
- The toggle's own `real-ref` refresh is the contract from ADR 0002 and
  `qcarcam-injector` ADR 0001.
- A running client is not told it is using the "wrong" library after a
  toggle. Surfacing this in the UI is not solved.
- If more processes ever need per-process scoping instead of a system-wide
  swap, `LD_PRELOAD` remains the known alternative.
- See `qcarcam-injector/docs/adr/README.md (former ARCHITECTURE.md item map)` items 36 and 37 for the full
  evidence and the UI layout history, and `CONTEXT.md`'s "SHIM/REAL/BLIND
  status control" entry for the PC-side UI spec.
