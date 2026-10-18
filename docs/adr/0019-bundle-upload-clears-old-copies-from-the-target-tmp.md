# 0019. The install clears its own leftovers from the target's `/tmp`

Status: Accepted. Built; not yet run as a full install on a board (the cause was checked
on the board, see below).

## Context

Installing the injector from CamSyringe failed with `Transfer failed: scp: write remote
"/tmp/qcarcam_injector_bundle_v0.12.0.bin": Failure`. `/tmp` on the target is a 64 MB
RAM disk (`/dev/ram0`), and a bundle is about 32 MB (33,663,132 bytes for v0.12.0).
The installer uploads the bundle to `/tmp/` and ran it from there, and **never deleted
it**. The previous install's copy (same name, same size) was still in `/tmp`, using about
half of the disk, together with the dispatcher's logs. The new upload then needed room for
a second full copy and ran out partway (29 MB free), even though it has the same name as
the old file. A bundle of a different version would have failed in the same way after
one install, because two of them do not fit.

## Decision

- **Before uploading**, `InjectorBundleInstaller` runs `rm -f
  /tmp/qcarcam_injector_bundle_*.bin` on the target and stops with a clear message if
  that fails. This only matches this project's own bundle names.
- **After a successful install**, it removes the uploaded bundle (best effort), since
  the self-extracting installer has finished with it. On a failed install the file is
  left, so the failure can be investigated or the installer re-run by hand.
- **This project's own old logs are removed before the dispatcher is restarted**:
  `/tmp/qcarcam_dispatcher*.log` (the dispatcher's log and each camera's injector and
  receiver logs), `/tmp/qcarcam_testapp_*` and `/tmp/qcarcam_test_report.txt`. About 120 KB
  on a bench board; the point is that the new run's logs start empty and an old log is not
  mistaken for a new one. It runs before the restart because the restart creates
  `qcarcam_dispatcher.log` afresh. Best effort, only these names. The rest of `/tmp`
  (`cam.xml`, `camxoverridesettings.txt`, system and diagnostic logs) is not ours and is
  never touched; wiping all of `/tmp` was considered and rejected.

## Relevance check

`InjectorBundleInstaller.cpp` has the three `rm -f` calls (earlier bundles, the uploaded
bundle, the project's logs). On a board, `/tmp` should hold no
`qcarcam_injector_bundle_*.bin` after a successful install from CamSyringe.

## Consequences

- Re-installing, or installing another version, no longer depends on how much of
  `/tmp` an earlier install used.
- A board that is already in the bad state is cleared by the next install, without
  anyone logging in.
- The bundle must still fit in `/tmp` on its own (about 32 MB of 64 MB). If bundles grow
  past about 60 MB, upload to `/var` (a 13 GB disk) instead.
- Logs of a previous session are gone after an install. Copy any you want to keep before
  installing. Other files in `/tmp` are not touched.
