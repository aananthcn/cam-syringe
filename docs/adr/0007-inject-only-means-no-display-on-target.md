# 0007. "Inject only" means no display on the target

Status: Accepted. The target-side half is implemented in source but not yet
compiled or run on a board, see "Implementation status".

## Context

Newer target boards have **no display at all**. Previously CamSyringe's
"Inject only" option was worded as skipping the local preview render. That
understated the real need: on a display-less board, the target's display
services (`start-display-service.sh`, `openwfd_server`, Screen) have
nothing to drive either, so they shouldn't be started, not just the
preview.

CamSyringe doesn't start or render anything display-related itself. It only
tells the target's `qcarcam_dispatcher`, in its control-channel
declaration, whether to. The target-side half of this decision is
`qcarcam-injector`'s `docs/adr/0006-display-services-and-preview-only-when-not-inject-only.md`.

## Decision

- **"Inject only" is the single switch for "no display on target".**
  - GUI label (`CameraConfigDialog`): **"Inject only (no display on
    target)"**, replacing the earlier text about a local preview render.
  - CLI `--inject-only` help text uses the same wording.
  - Default is on (`main.cpp`: `injectOnly = true`).
- **Wire contract unchanged.** When checked, `DispatcherClient` sends
  `FLAGS --inject-only` before `END`. When unchecked, it sends no `FLAGS`
  line, and the target's default applies: display services plus an on-target
  preview. The flag keeps its name and protocol meaning, but its documented
  effect is widened from "no preview" to "no display services and no
  preview".
- **CamSyringe does not detect display-less targets.** The user states
  intent via the checkbox. Unchecking it on a board with no display is the
  user's choice, and leads to no preview and possibly `PREVIEW_ERROR`
  status-bar messages (ADR in `qcarcam-injector`).

## Implementation status

- **Done (this repo):** GUI label and CLI help wording.
- **Done in source (`qcarcam-injector`), unverified:** the dispatcher now
  starts the display services itself, only for a declaration without
  `--inject-only`, and `run_qcarcam.sh` no longer starts them at launch.
  That change has not been compiled or run on a board yet. See the
  target-side ADR's "Implementation status".

## Consequences

- A deployment guide for display-less boards should say: leave "Inject
  only" checked (it is by default).
- The older ADR 0001/0005 era `CONTEXT.md` text that describes `--inject-only`
  as only skipping the preview is superseded by this wording where it
  appears.
- Changing the checkbox text again requires the same two places (GUI label,
  `--inject-only` help text) to stay in sync.
