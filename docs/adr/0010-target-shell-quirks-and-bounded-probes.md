# 0010. Respect the target shell's quirks, and bound every probe on the control channel

Status: Accepted

Formerly `qcarcam-injector`'s `ARCHITECTURE.md` items 34 (bug 1) and 35 (bug 2).

## Context

Two defects produced one confusing symptom, a dialog that appeared "randomly"
and Plays that took a minute or more.

- The target's shell is QNX's minimal ksh. It does **not** detach a compound
  command backgrounded with `&`. `( true && sleep 5 ) &`, `( true; sleep 5 ) &`
  and `for` loops all blocked the ssh client for the full duration, and
  `( cmd & )` didn't detach either. `A || B &` also failed to detach `B`.
  Only a single simple command backgrounded from the outside, or
  `A ; B &` / `if ! A; then B & fi`, detached. The start command
  `. /var/opt/env.sh && nohup run_qcarcam.sh` is compound, so it blocked.
- The dispatcher accepts exactly one connection at a time
  (`qcarcam-injector` ADR 0015). The version probe opens its own short-lived
  connection. While a session held the main one, the probe's `connect()`
  succeeded (the kernel accepts the handshake) but its blocking `recv()` waited
  for the whole session and answered only when Stop freed the dispatcher. That
  produced the version-mismatch notice "after Stop".

## Decision

- **The start command is shaped for this shell**, `buildStartCommand()` in
  `DispatcherRemoteControl.cpp`:
  `if ! pidin | grep -qi qcarcam_dispatcher; then sh -c '. /var/opt/env.sh &&
  exec run_qcarcam.sh [--ipv4]' >/tmp/qcarcam_dispatcher.log 2>&1 </dev/null & fi`.
  An `if`/`then` replaces `||`, and a nested `sh -c` means the outer shell
  backgrounds one simple command. No pty is allocated (`-t` is never used), so
  no `disown` is needed. Verified to return in about 0.2 s. `--ipv4` is
  appended whenever the target isn't an IPv6 literal, so both ends use the
  same family (`qcarcam-injector` ADR 0003).
- **Check for a running dispatcher with `pidin`**, not `pgrep` (`qcarcam-injector`
  ADR 0018).
- **Every probe on a channel that may be busy has a receive timeout.**
  `DispatcherVersionProbe` sets `SO_RCVTIMEO` (3 s) and falls back to
  "unknown", with no notice, when the dispatcher is busy.
- **A timeout on the start command isn't fatal**, because a cold display start
  can outlast the SSH timeout. It stays as defence in depth.

## Relevance check

Both are in the code as described. The IPv4-by-default and `--ipv4`
interplay is the interim workaround for the board's IPv6 issue (ADR 0001) and
goes away when that is fixed.

## Consequences

- Any new remote start/stop command must be tried on the real target's shell,
  not assumed to behave like bash.
- A busy dispatcher is silent about version mismatches by design.
