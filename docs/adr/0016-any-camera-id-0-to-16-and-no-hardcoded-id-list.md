# 0016. Any QCarCam id from 0 to 16 can be injected, and no id is special-cased

Status: Accepted. Implemented on the PC side (this repo). The target side
(`qcarcam-injector`) still accepts 1 to 16 only, see "Implementation status".

Related: ADR 0008 and 0011 (per-id camera sizes), ADR 0013 (the dialog's id
fields); `qcarcam-injector` ADR 0015 (the id rule on the target), ADR 0010 item 7
and ADR 0020 (no hardcoded input list).

## Context

Which QCarCam id a camera is injected into depends on how the vehicle engineer
wired and configured the board, and it differs per board. Early code and the
bench only ever worked with ids 8, 9, 1 and 2, and other ids failed. The cause
was a hardcoded input list in the target-side shim (`1,2,8,9`), which describes
one bench's wiring. `qcarcam-injector` removed it and now probes the real
driver instead (its ADR 0010 item 7; the retired list is in its ADR 0020).

It is not known whether the board's developers number their ids from 0 or from
1. The range **0 to 16** covers both.

## Decision

- **CamSyringe supports any QCarCam id from 0 to 16 for injection output.** The
  user chooses the id for each camera, and every value in that range is accepted
  and treated alike.
- **No id is special-cased.** CamSyringe never keeps a list of "known good" ids
  (such as 1, 2, 8, 9), never prefers or defaults to particular hardware ids, and
  never rejects an id inside the range on its own judgement. Whether a given id
  exists on a board is decided by the board's driver, and a failure there is
  reported per camera (`ERROR <id> <reason>`), not guessed at on the PC.
- **0 is a real id,** not "unset". No code may use `camId == 0` or `!camId` to
  mean "no camera chosen".
- **Ids must be unique within a session** (two cameras can't share one id). This
  is a rule about the session, not about any particular id.
- **The range is a single shared constant pair** (`kMinCamId` and `kMaxCamId`
  in `camera/PortScheme.h`), used by the
  dialog's id field, the `--cam-ids` command line check, the saved-config loader
  and the Settings > Camera Configs sweep.

## Implementation status

- **Done (this repo):** `kMinCamId` is 0, defined once in `camera/PortScheme.h`
  (the duplicate in `main.cpp` is gone). The dialog's id field, the `--cam-ids`
  check, the saved-config loader and the Camera Configs sweep all follow it, so
  the sweep now reads 0 to 16.
  - `--cam-ids` parsing is stricter: digits only, in full. `std::atoi` couldn't
    tell `0` from `abc`, so it is no longer used for ids. `17`, `-1`, `abc` and
    `0-2x` are all refused; `0` and `0,16` are accepted.
  - A saved config or camera cache entry with **no** `camId` key is dropped,
    not read as id 0.
  - Tested in `tests/config_persistence_test.cpp` (id 0 and 16 round-trip, 17
    and a missing key are refused, a cache entry for id 0 loads and a keyless one
    is skipped) and by running the `--cam-ids` cases by hand.
- **Not done (`qcarcam-injector`):** its dispatcher says "ids 1-16" and rejects
  the declaration otherwise (the 16 bound is waived under `--qcx-bypass`). Until
  it accepts 0, a camera set to id 0 gets `ERROR 0 <reason>` from the target.
  That repo has its own build and board test cycle, and its ADR 0015 needs the
  same update.
- **Not verified:** that the board's driver accepts id 0 at all. If it doesn't,
  the per-camera error is the expected outcome, and the range still stands.

## Consequences

- A user can inject into whichever id their board uses, without a code change.
- Camera Configs reads 17 ids instead of 16, which makes the explicit sweep
  slightly slower.
- Code that built an id range (`kMinCamId` to `kMaxCamId`) or validated ids must
  keep deriving it from the constants, never from a literal.
- A camera id and a camera's position in the session are different things: the
  `cam0`, `cam1` labels and the port scheme (5004 plus 2 per camera) come from
  the position, so they are unaffected by id 0.
