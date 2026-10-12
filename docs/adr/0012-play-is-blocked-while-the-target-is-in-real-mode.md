# 0012. Play is blocked while the target is in REAL mode

Status: Accepted. Implemented, built, not run against a target.

Pairs with `qcarcam-injector` ADR 0021 (the target-side refusal).

## Context

The SHIM exists to bypass `qcxserver` and feed PC frames to client applications.
When the target's SHIM/REAL toggle is on REAL, the real camera feed through
`qcxserver` must reach the system untouched, and injecting would take the
physical input away from it. Observed on 2026-10-12: with SHIM active, the
injector logged repeated frame-ready timeouts, while in REAL it would have held
the camera input.

## Decision

- **Pressing Play while the toggle shows REAL does nothing to the target.**
  `startStreaming()` returns early with a status-bar error telling the user to
  double-click the SHIM/REAL box (bottom right) to switch to SHIM. No stream
  starts, and no declaration is sent.
- **Only a confirmed REAL blocks.** BLIND (unknown, for example SSH
  unavailable) is let through, because it is not known to be REAL. The target's
  dispatcher refuses by itself when it sees REAL, so the rule still holds.
- **A REAL refusal from the target is not a resolution problem.** It carries
  "REAL mode" in its text, and `onDeclareComplete()` skips the camera-config
  invalidation of ADR 0011 for it.

## Consequences

- No injection, preview or local streaming happens in REAL. That is the point:
  CamSyringe has nothing to do while the real feed is in use.
- The toggle is polled every second, so the check follows a manual switch on
  the target within about a second.
