# 0019. The bundle is staged in `/var/opt/tmp`, not in the target's RAM

Status: Accepted. Supersedes the first version of this ADR, which only cleared old copies
out of `/tmp`. Not yet run as a full install on a board.

## Context

Installing the injector from CamSyringe failed with `Transfer failed: scp: write remote
"/tmp/qcarcam_injector_bundle_v0.12.0.bin": Failure`. `/tmp` on the target is a 64 MB
RAM disk (`/dev/ram0`), and a bundle is about 32 MB (33,663,132 bytes for v0.12.0).
The installer uploaded the bundle to `/tmp/`, ran it from there, and never deleted it.
The previous install's copy was still there, using about half of the disk, so the new
upload ran out of room partway (29 MB free).

The first answer to this was a reaction to the defect: delete old copies from `/tmp`
before each upload. That repairs this failure but keeps the cause, which is that a
32 MB file, and growing with each release, is staged in RAM. It was never decided that
the bundle belongs in `/tmp`; it was only where the upload happened to go. RAM on the
target is scarce and is needed by the camera pipeline, so it should not hold install
files, even for a short time.

## Decision

- **The bundle is uploaded to `/var/opt/tmp/` and run from there.** `/var/opt` is already
  the install root for the injector (`bin`, `lib`, `include`) and is on the persistent
  disk (`/var`, about 13 GB), so the bundle size is no longer a concern. The installer
  creates the directory with `mkdir -p /var/opt/tmp` before the upload.
- **Earlier bundles are removed from `/var/opt/tmp` before uploading**, with
  `rm -f /var/opt/tmp/qcarcam_injector_bundle_*.bin`, so versions do not pile up on the
  disk. This only matches this project's own bundle names.
- **After a successful install the uploaded bundle is removed** (best effort). On a failed
  install it is left, so the failure can be investigated or the installer re-run by hand.
- **The installer's removal of the previous install stays limited to `/var/opt/bin`,
  `/var/opt/lib` and `/var/opt/include`.** It must never be widened to `/var/opt`, or it
  would delete the staged bundle it is about to run.
- **No install file is placed in `/tmp` any more.** Bundles left in `/tmp` by versions
  before this change are removed once, by the same pre-upload step
  (`/tmp/qcarcam_injector_bundle_*.bin`), so a board in the bad state recovers on its
  next install. That line can be dropped when no such board is left.
- **Logs are not changed by this ADR.** The dispatcher, injector, receiver and test-app
  logs still go to `/tmp` (they are small, about 120 KB), and the installer still removes
  this project's own old logs there before restarting the dispatcher, so the new run's
  logs start empty. Moving them out of RAM is a separate decision, because the dispatcher
  restart command, the stats stream and several other places read those paths.

## Relevance check

`InjectorBundleInstaller.cpp` uploads to `/var/opt/tmp/` and runs the bundle from there.
After a successful install from CamSyringe, `/var/opt/tmp` holds no
`qcarcam_injector_bundle_*.bin`, and `/tmp` holds none either.

## Consequences

- Installing, re-installing, or installing another version does not depend on how much of
  `/tmp` is in use, and no longer takes about half of the target's RAM disk.
- A failed install leaves the bundle on the persistent disk, so it survives a reboot.
  The next install removes it.
- `/var/opt/tmp` is a new directory under the install root. Nothing else may use it.
- The camera-pipeline logs remain in RAM (see the last Decision item).
