# 0001. Interim IPv4 default while board IPv6 is broken

Status: Accepted (interim — revert condition below)

## Context

`qcarcam_dispatcher`'s `socket(AF_INET6, ...)` control-channel listener
fails with "Address family not supported by protocol family" on the
CARIAD/Qualcomm SA8650P bench board (OS image built 2023-11-09).
Root-caused hard, ruling out every layer this project's own code
touches: yesterday's `libsocket.so.4` link fix is correct and confirmed
working (the same binary succeeds instantly with `--ipv4`); kernel-level
IPC tracing (`tracelogger`/`traceprinter`) proves the rejection happens
entirely client-side inside `libsocket.so.4`, before any IPC reaches
`io-sock`; a from-scratch Python `ctypes` script calling
`socket(AF_INET6, SOCK_STREAM, 0)` directly against `libsocket.so.4`,
bypassing all of `qcarcam_dispatcher`/CamSyringe's own code, fails
identically. Only a small, fixed set of pre-existing base-image binaries
(`ping`, `telnet`, `sshd`) can create this socket; nothing new can,
regardless of path, language, or privilege.

This is a board OS-image/platform-level restriction. The real fix is a
board OS image upgrade, currently blocked on the board's own USB update
tool failing for reasons not yet found (status expected 2026-09-15 or
later).

## Decision

CamSyringe's default target (`kDefaultTarget`, `src/main.cpp`) is
`kDefaultTargetIPv4` (`192.168.1.1`, `src/net/TargetDefaults.h`) instead
of the project's real standard bench address, `kDefaultTargetIPv6`
(`fd53:7cb8:383:2::172`). `192.168.1.1` is the generic default IPv4
address across this board family (see `qcarcam-injector`'s own
`emac_iosock_network.sh`), deliberately NOT this specific bench's own
live address (`192.168.1.4`, set by `/persist/net/early-net-iosock.sh`)
— a code-level default should name the generic/documented address, not
one bench's local override.

No separate IPv4/IPv6 *mode* setting drives this anywhere. Both
`bracketHostIfIPv6()` (CamSyringe's own connect path) and
`DispatcherRemoteControl`'s remote `run_qcarcam.sh`/`run_qcarcam.sh --ipv4`
selection derive the family purely from whether the in-play target
string contains a `:` — typing either kind of address into `--target`/
Configure just works. A "Force IPv4" checkbox in `CameraConfigDialog`
(checked by default this release) is a pure UI convenience that rewrites
the Target field to one of the two `TargetDefaults.h` constants when
toggled; it's never read anywhere else, so it can't disagree with a
manually-typed target.

`qcarcam-injector`'s `run_qcarcam.sh` was extended to accept an optional
`--ipv4` argument, forwarded straight to `qcarcam_dispatcher`'s own
pre-existing `--ipv4` flag.

`MainWindow::maybeWarnAboutIpv6()` (`src/net/Ipv6SupportProbe.h/.cpp`)
proactively warns in the status bar if the user sets an IPv6-looking
target that would actually hit this issue — a live functional probe
(briefly starting `qcarcam_dispatcher` itself on a scratch port, not
`ping`/`telnet`, which are immune to this issue and would never detect
it), not a version check, since the underlying issue has no discovered
version signature.

## Consequences

- Reverting once the board is fixed is exactly one line:
  `kDefaultTarget = camsyringe::kDefaultTargetIPv6` in `src/main.cpp`.
  Nothing else — checkbox included — needs to change; everything derives
  from the target string already.
- Anyone hitting an unexpectedly-IPv4 CamSyringe default should check
  whether the board fix has actually landed before assuming this is
  stale/forgotten cleanup.
- Full root-cause narrative and diagnostic trail: see this session's own
  investigation (kernel IPC tracing, the `ctypes` repro) — not yet
  written up as a standalone doc; this ADR is currently the canonical
  record of the decision.
