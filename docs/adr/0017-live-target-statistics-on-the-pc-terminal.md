# 0017. Live target-side statistics are printed on the PC terminal

Status: Accepted. The parser is tested on canned lines from real logs. The
log-following command was changed from `tail -f` to a polling loop because the
v14.4.1 BSP's `tail -f` doesn't work, and was checked in the board's own shell
(see "Following the logs without `tail -f`"). Run on a v14.4.1 board with
`qcarcam_test` attached, the report showed the injector and application columns
filled in (see "Implementation status").

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
  qcarcam_test's "FPS Report" table.** One long-lived `ssh` per READY
  camera follows that camera's injector and receiver logs as they are written
  (a once-a-second polling loop on the target, not `tail -f`; see below), and the
  numbers are kept up to date as lines arrive. **Nothing changes on the
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
    output. `app fps` is the rate at which it takes frames: `ReleaseFrame` calls for a
    classic-mode application, buffer re-submissions (`requested`) for a request-mode one
    such as `qcarcam_test`, which never calls `ReleaseFrame`. Run on the bench with
    `qcarcam_test -noDisplay`, request mode showed `released=0` while `requested` tracked
    `delivered` at 28.7 fps; `missed` counts frames it didn't take;
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
- **No remote process is left behind.** The remote command is a single
  `sh -c '...'` with no backgrounding (ADR 0010) that first prints `PID $$`. The
  announced pid lets Stop send a short, bounded `kill <pid>`. Without it the loop
  would keep running on the target until its next ssh write failed (the same kind
  of leak the QCX probe had, fixed in `5e61a1b`). The loop's `sleep` child exits
  within a second of the kill.
- **Started** for each camera the target answers READY for; **stopped** whenever
  the session stops, including Pause (a resume declares again and restarts it).

## Implementation status

- **Done:** `TargetStatsParser`, `TargetStatsStream`, `TargetSsh::sshInvocation`,
  and the start and stop hooks in `MainWindow`. `tests/target_stats_parser_test.cpp`
  covers the line formats above, rate arithmetic, counters restarting and the
  folded corrupt count.
- **Seen on a v14.4.1 board** (bench run with `qcarcam_test`, request mode): the
  report every 5 seconds; the injector's counter line with an application attached
  gave `read fps` and `deliv fps` (26-30 fps against about 30 sent); the
  application columns filled from the SHIM server's per-application line
  (`app pid`, `mode` = request, `app fps` close to `deliv fps`, `missed`, `last
  call`); the state went `injecting`, then `idle (no app)` when the application
  stopped; the receiver's rate and corrupt-packet count came through; and, after the
  stream-size fix below, the `size` column read `1920x1080`. `tail -f`
  does not work on that BSP and was replaced (below).
- **Not verified:**
  - that Stop leaves no polling loop running on the target (`pidin` doesn't show the
    script text, so check by looking for a leftover `sleep` after Stop);

## Following the logs without `tail -f`

On the v14.4.1 BSP, `tail -f` fails at once: `tail: xnotify_add failed on
/tmp/...: Function not implemented`, even for a plain file in `/tmp`. The first
version of the stream used `tail -n 0 -f` on both logs, which worked on the
earlier BSP, so on v14.4.1 it would have ended immediately with "live target
statistics ended". (The statistics were also missing in the first bench run for a
different reason: the camera's injector failed to start, so the camera was
rejected and the stream, which starts only for READY cameras, never started; see
`qcarcam-injector` ADR 0025.)

The stream now runs a small polling loop under `sh -c` (`TargetStatsStream::start`):

- once a second, for each log, count the complete lines (`wc -l`) and print the
  ones past the last count (`sed -n`);
- the counts are primed first with the output discarded, so only what is written
  from now on is printed (what `-n 0` did);
- a log that doesn't exist yet counts as 0, tested with `[ -r file ]` so the shell
  prints no "cannot open" error (the stream merges stderr, and the problem filter
  would flag the word "cannot");
- a log that shrinks, because the dispatcher truncates it when it respawns an
  injector, is read again from the top;
- the receiver prints the stream size (`FrameReceiver: stream is 1920x1080, ...`)
  once, when it opens the stream, which is before the camera is READY and so
  before this loop starts. The first bench run showed `size` as `-` for that
  reason. So after priming, the loop prints the last such line in the receiver
  log once (`grep | tail -n 1`, only if the log is readable), and the parser fills
  `size` from it;
- only complete lines are printed, so output from the two files can't interleave
  mid-line, and the parser, which already ignores `tail`'s `==>` headers, needs no
  change.

Cost: a `wc` and a `sed` per file per second on the target, and up to one second
of added delay, which the 5-second report doesn't notice.

Checked: the same loop gave identical output under `dash`, `bash` and `busybox sh`
with a log that appears late, a partial line completed later and a truncation, and
on the board under its own shell (`sh`): no old lines, both files followed, no
stderr noise, clean kill. The exact command string `start()` generates was printed
by a small Qt program and run on the board, including the once-only stream-size
line with an older one above it (only the latest was printed) and with no receiver
log at all (no error).

## Consequences

- The terminal shows both ends of the pipe: what the PC sent, what the receiver
  decoded and what the SHIM handed to an application. A mismatch between them is
  now visible without a display.
- Each camera adds one ssh connection to the target for the session.
- The line formats depend on the target's log text. A change to those log lines
  in `qcarcam-injector` needs the parser updated, and its test.
- Not done: a switch to turn the statistics off.
