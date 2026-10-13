# Architecture Decision Records

Each file here is one ADR: a single architectural decision or hard
requirement, captured once, kept up to date as long as the decision
stands. Numbered sequentially (`0001-`, `0002-`, ...), never renumbered
or reused even if a later ADR supersedes an earlier one — supersede by
adding a new ADR and marking the old one's Status as `Superseded by
000N`, not by editing history away.

## Format

Each ADR is a short markdown file:

```
# NNNN. Title

Status: Accepted | Superseded by 000N | Deprecated

## Context
What situation forced this decision? What was actually observed/confirmed
(live, on real hardware, where applicable) that made the default/obvious
approach wrong?

## Decision
What we're doing instead, stated as a clear rule.

## Consequences
What this rules out, what it costs, what still needs to happen, cross-
references to the other repos/files this decision touches.
```

Keep it short — this is a decision record, not a design doc. Link to
`CONTEXT.md` for the full narrative/evidence where one already exists;
the ADR itself should be readable in under a minute.

## When to add one

Any time a decision here would otherwise only live in a code comment or
commit message, AND getting it wrong again would cost real
debugging/rebuild time (matches the kind of thing `CONTEXT.md`'s own
"Known gotchas" section already tracked informally) — write it down as
an ADR instead, so it survives independently of whichever file happens
to carry the comment today.

## Index

- [0001. Interim IPv4 default while board IPv6 is broken](0001-interim-ipv4-default-while-board-ipv6-broken.md)
- [0002. real-ref sync derived from target, not bundle](0002-real-ref-sync-derived-from-target-not-bundle.md)
- [0003. Play-time reachability check pings the host, not the control port](0003-reachability-check-pings-host-not-control-port.md)
- [0004. Never synchronously join a stream/network thread from the GUI thread](0004-never-join-stream-thread-from-gui-thread.md)
- [0005. One shared connectivity monitor, not two independent checks](0005-shared-target-connectivity-monitor.md)
- [0006. Replace `libqcxclient.so` with a shim via a target-side symlink swap, shipped only in the injector bundle](0006-shim-library-replacement-via-target-side-symlink-swap.md)
- [0007. "Inject only" means no display on the target](0007-inject-only-means-no-display-on-target.md)
- [0008. The PC sizes each stream to the camera's own resolution, read from the target](0008-stream-sized-to-camera-resolution-from-target.md)
- [0009. Re-encode the source to what the target decoder expects, never remux](0009-re-encode-to-what-the-target-decoder-expects.md)
- [0010. Respect the target shell's quirks, and bound every probe on the control channel](0010-target-shell-quirks-and-bounded-probes.md)
- [0011. Camera configs are cached locally and learned only when needed](0011-camera-configs-are-cached-and-learned-only-on-demand.md)
- [0012. Play is blocked while the target is in REAL mode](0012-play-is-blocked-while-the-target-is-in-real-mode.md)
- [0013. Settings > Camera Injection is the one place for session settings, and it is remembered](0013-camera-injection-dialog-configs-must-be-cached.md)
- [0014. The installed injector is verified against the bundle, and the control protocol is versioned](0014-installed-injector-is-verified-and-protocol-version-is-checked.md)
- [0015. The stream canvas is capped at 1920×1080, even for a larger camera](0015-stream-canvas-capped-at-1080p.md)
- [0016. Any QCarCam id from 0 to 16 can be injected, and no id is special-cased](0016-any-camera-id-0-to-16-and-no-hardcoded-id-list.md)
- [0017. Live target-side statistics are printed on the PC terminal](0017-live-target-statistics-on-the-pc-terminal.md)
