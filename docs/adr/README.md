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
