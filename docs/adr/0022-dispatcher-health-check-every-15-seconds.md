# 0022. CamSyringe checks every 15 seconds that the dispatcher runs on the target, and starts it if not

Status: Accepted. Not yet verified against a real target.

## Context

Testers report that injection does not work because `qcarcam_dispatcher` is not running on the
target, and they start `run_injector.sh` by hand over SSH. Nothing in CamSyringe looked for this
state before Play: ADR 0003 and ADR 0005 only ping the host, and ADR 0010 starts the dispatcher
only as a one-shot recovery after a Play whose control-channel connect failed. The dispatcher is
also stopped on purpose by every Stop (ADR 0010's kill list), and it does not survive a target
reboot.

## Decision

- **A health check runs once as soon as the app is up, then every 15 s.** The first check is
  queued with `QTimer::singleShot(0)` at the end of the `MainWindow` constructor, so it runs when
  the event loop starts and the window is already built and shown. It does not wait 15 s.
- **The check asks `pidin` over SSH** (`DispatcherRemoteControl::isRunning()`, matching
  `[q]carcam_dispatcher` so the grep cannot match itself; `pidin` for the reasons in ADR 0010).
  It does not connect to the control port: the dispatcher accepts one connection at a time, and a
  probe would block or steal the connection (ADR 0010).
- **If the dispatcher is not running, the check calls `DispatcherRemoteControl::ensureRunning()`,**
  the same start as the Play-time recovery: `. /var/opt/env.sh && exec run_injector.sh [--ipv4]`,
  detached, then a poll of the control port for up to 40 s. The script is found through
  `env.sh`'s PATH (the installed copy is `/var/opt/bin/run_injector.sh`).
- **A check is skipped, not queued, when** no target is set; the target is not `Reachable` per
  `TargetConnectivityMonitor`; a session is Playing or Paused; an install is running (it restarts
  the dispatcher itself); or the previous check has not finished. The next tick tries again.
- **If the target could not be asked** (SSH unusable, command failed), nothing is started. A failed
  SSH call is not evidence that the dispatcher is down, and a second start would be harmful.
- **It never prompts for credentials,** like the other background SSH checks: it declines the
  credentials callback, so without passwordless SSH it does nothing.
- **Outcomes go to stderr only.** There is no dialog or status-bar message for a restart.

## Consequences

- After Stop, the dispatcher is started again within 15 s. That is intended: Play then finds
  it running, and a fresh dispatcher is what the receiver needs for a new stream.
- The first check can be skipped if the connectivity monitor has not resolved yet when the app
  starts (state `Unknown`). The next tick, 15 s later, covers it.
- Changing the target in Configure does not trigger an immediate check; the 15 s tick picks it up.
- One SSH round trip every 15 s while idle and reachable.
- A dispatcher that is running but wedged is not detected. This only checks that the process exists.
- **Not done / not verified:** a run against the real target. The renamed `run_injector.sh` must
  exist in the installed bundle (`qcarcam-injector` change).
