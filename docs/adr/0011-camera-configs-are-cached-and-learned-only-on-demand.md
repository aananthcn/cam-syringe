# 0011. Camera configs are cached locally and learned only when needed

Status: Accepted. Implemented, but not yet exercised on a target. See
"Implementation status".

Refines ADR 0008 (the stream is sized to the camera's resolution).

## Context

Learning a camera's resolution costs real time and has a side effect on the
target. The XML lookup is cheap, but the live-query fallback runs
`qcarcam_test`, can take tens of seconds per id and, on a cold target, starts
the display service. A camera's configuration rarely changes, so doing that on
every launch or every Configure apply is wasteful and risks disturbing the
target for no benefit.

## Decision

- **The learned configs are kept in a local file**, `camera_configs.json`,
  in the install directory and in `~/.cache/camsyringe` (`ConfigFiles`, ADR 0013), so a reinstall
  or a new release doesn't lose them. It is plain JSON, per target and per
  camera id, with a read timestamp. It is written after every completed
  resolve and read back at startup. It is a convenience cache: a write
  failure is logged, never shown as an error, and neither Play nor Configure
  depends on it.
- **Don't learn every time.** A camera config is learned only on one of
  these triggers:
  1. **No usable local entry.** The config file doesn't exist (first launch,
     or deleted), or it has no entry for that target and camera id, or the
     entry is a failed result (`ok = false`). One exception: a failure that
     carries a specific known reason (currently the QCX API version mismatch)
     is not retried, because nothing short of a new injector bundle or driver
     changes the answer, so retrying would repeat a slow query for nothing.
  2. **A problem, conflict or error message about the camera's resolution.**
     The cached entry for the affected camera on that target is treated as
     untrustworthy and is learned again. Examples are a control-channel
     `ERROR`/`PREVIEW_ERROR` for that camera (which is how a receiver that
     exits because the decoder couldn't open shows up on the PC).
  3. **A user action.** Selecting **Settings > Camera Configs** starts an
     explicit read of every camera id (1-16) on the current target and
     refreshes the cache and its timestamp.
- **Otherwise the cached values are used as is.** A normal Configure apply
  seeds each stream's target size from the cache and resolves only the ids
  that trigger 1 says are missing.
- **An error-triggered relearn is deferred and rate-limited.** The cached entry
  is dropped at once (and the stream's size is reset to the safe capped size),
  but the relearn itself waits until the session is **Idle**, because its live
  query runs `qcarcam_test` on the target and must not overlap a running
  injection session (a Paused session still holds its control connection, so
  it doesn't count). The same camera isn't invalidated again within 10
  minutes (`kGeometryRelearnCooldownSecs`), so a camera the board always
  rejects, such as id 3 on the bench target, doesn't re-run the live query on
  every Play.
- **A relearn replaces the entry**, and runs passwordless-only with the same
  ascending-id fallback rule as ADR 0008, because it can fire without a
  deliberate "talk to the target" action.

## Implementation status

All in `MainWindow.cpp`/`.h`, and the cache in `CameraConfigStore`:

- **Cache and explicit read (existing):** the cache file and its location,
  loading at startup, saving after each resolve, and the full read from
  Settings > Camera Configs (`CameraSettingsDialog`).
- **Trigger 1, failed entries:** `resolveCameraGeometry()` now treats an id as
  needing learning when it has no entry, or a failed entry with no specific
  failure reason.
- **Trigger 2, errors:** `invalidateCameraGeometry(camId)` is called from
  `onDeclareComplete()` for each camera whose declaration came back `ERROR`,
  and for each `PREVIEW_ERROR`. It drops the cache entry, saves the file, clears
  the pool's copy of the size, and sets `geometryRelearnPending_`.
  `applyState(Idle)` then runs `resolveCameraGeometry()`. The per-camera error
  text tells the user the resolution will be re-learned after Stop.
- **Not exercised:** none of this has been run against a real target. It was
  built, not run.

**What counts as an error is limited to what reaches the PC.** The target's own
size errors (ring size mismatch, decoder open failures) are written to logs on
the target, not sent over the control channel. They reach CamSyringe only
indirectly, as the camera's `ERROR` when the receiver exits. Sending a
dedicated, machine-readable reason from the dispatcher would make trigger 2
precise, and is not done. As a result, any `ERROR` or `PREVIEW_ERROR` triggers
a relearn (subject to the cooldown), including ones unrelated to size.

## Consequences

- A cached size can be stale if the target's camera configuration changes. The
  explicit read is the way to refresh it today.
- A failure with a known reason (the QCX version mismatch) stays failed until
  the user re-reads from Settings > Camera Configs. Such a stream uses the
  capped size (ADR 0008).
- A relearn after an unrelated error costs one slow live query after Stop,
  limited to once per camera per 10 minutes.
- Deleting `camera_configs.json` is a supported way to force a relearn, and
  the file can be edited by hand for the same reason.
