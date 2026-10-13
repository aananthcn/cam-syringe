# 0017. Live target-side statistics are printed on the PC terminal

Status: Accepted. The parser is tested on canned lines from real logs. The live
ssh stream was built but **not yet run against a board** (the board was
unreachable when it was written), see "Implementation status".

Related: ADR 0004 (never block the GUI thread), ADR 0007 (inject only, no
display on the target), ADR 0010 (the target's shell quirks).

## Context

A board with no display can't show whether injection is working, and
CamSyringe's own stderr only reported what the PC *sent* (`camsyringe[camN]:
30.0 fps`). Everything that says what the target *received and did* sat in logs
on the target (`/tmp/qcarcam_dispatcher_cam<id>_injector.log` and
`..._receiver.log`), readable only by ssh-ing in by hand.

## Decision

- **While a session runs, CamSyringe prints a report of the target's own
  numbers on the terminal it was started from, every 5 seconds, in the style of
  qcarcam_test's "FPS Report" table.** One long-lived `ssh ... tail -f` per READY
  camera follows that camera's injector and receiver logs as they are written,
  and the numbers are kept up to date as lines arrive. **Nothing changes on the
  target**: it works with an already-installed bundle.
- **One row per camera** (`TargetStatsParser`, `formatTargetStatsReport`):
  - `rx fps`, `size`: rate decoded on the target and published into the ring
    (from the receiver's average publish interval), and the stream size.
  - `read fps`, `deliv fps`: the rate the injector takes frames from the ring, and
    writes them into an application's buffers. Rates come from the counters'
    change over the time between two lines, measured on the PC.
  - **Application columns** `app pid`, `mode`, `app fps`, `missed`, `last call`:
    what the attached application really did, **taken from the SHIM server's
    per-application lines** (injector ADR 0023), not from the application's own
    output. `app fps` is the rate it hands frames back (`ReleaseFrame`), which is
    the evidence that it consumed them; `missed` counts frames it didn't take;
    `last call` is the time since it last called into the SHIM. An older injector
    doesn't print these lines and the columns show `-`.
  - `corrupt`: corrupt packets seen since the session started.
  - `state`: `injecting` (application attached, calling, consuming), `APP STALLED`
    (no call for 3 s while started), `APP NOT CONSUMING` (started, but no frames
    handed back), `app not started`, `app paused`, `app attached`, `idle (no app)`,
    `receiving` (decoded, no injector numbers yet) or `no data` (nothing for 15 s).
- **Why the SHIM server and not the application's log:** different teams use
  different applications and their logs can't be relied on or found. Every
  application takes camera data through the SHIM library, which runs inside the
  application's process (so its own prints go to the application's stderr), but
  whose calls all arrive at the injector's SHIM server, the one place they can be
  counted and printed to a log CamSyringe already follows.
- **Problems are printed at once, not at the next report**: any line on those
  logs that says fail, error, cannot, refused or timeout, prefixed `target[camN]:`.
- **`clients=0` is normal, not a fault:** with no application reading the camera,
  nothing is delivered, and the line says so.
- **Informational only, and never prompts.** It runs only when ssh auth is
  already resolved for the target (`TargetSsh::sshInvocation`, same key and
  options as the other ssh calls). If the stream can't start or ends (the target
  went away), one line says so and the session carries on.
- **It never blocks the GUI thread** (ADR 0004): the ssh runs as a `QProcess`
  with signals, and stopping it only kills the local ssh, never waiting.
- **No remote process is left behind.** The remote command is
  `sh -c 'echo PID $$; exec tail -n 0 -f ...'`, a single simple command with no
  backgrounding (ADR 0010). The announced pid lets Stop send a short, bounded
  `kill <pid>`. Without it the tail would linger on the target until its next
  write (the same kind of leak the QCX probe had, fixed in `5e61a1b`).
- **Started** for each camera the target answers READY for; **stopped** whenever
  the session stops, including Pause (a resume declares again and restarts it).

## Implementation status

- **Done:** `TargetStatsParser`, `TargetStatsStream`, `TargetSsh::sshInvocation`,
  and the start and stop hooks in `MainWindow`. `tests/target_stats_parser_test.cpp`
  covers the line formats above, rate arithmetic, counters restarting and the
  folded corrupt count.
- **Not verified on a board:**
  - that the board's `tail` accepts `-n 0 -f` with two files. If `-n 0` is not
    supported, old log lines would print once at the start;
  - that the injector's counter line, with an application attached, reads
    `clients=N frames_read=R frames_delivered=D` (only `clients=0` has been seen
    in logs here);
  - that `tail` flushes each line promptly over ssh;
  - that the injector's per-application line (injector ADR 0023) prints on the board
    and the application columns fill in with a real application attached;
  - that Stop leaves no `tail` running on the target.

## Consequences

- The terminal shows both ends of the pipe: what the PC sent, what the receiver
  decoded and what the SHIM handed to an application. A mismatch between them is
  now visible without a display.
- Each camera adds one ssh connection to the target for the session.
- The line formats depend on the target's log text. A change to those log lines
  in `qcarcam-injector` needs the parser updated, and its test.
- Not done: a switch to turn the statistics off.
