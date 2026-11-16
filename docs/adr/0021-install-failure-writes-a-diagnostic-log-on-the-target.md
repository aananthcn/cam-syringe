# 0021. A failed install writes a diagnostic log on the target and names it on the console

Status: Accepted. The script is tested on the host (`tests/install_diagnostics_test.cpp`,
under `/bin/sh` against a fake install tree); not yet run on a board's ksh.

## Context

The 0.12.0 to 0.12.2 install failures were reported as `Install failed:` with nothing after
it. Three things hid the cause: the dialog showed only stderr (the bundle writes to stdout),
the bundle's `set -e` ended it silently when a program failed to load (`qnx_toolkit`
`create-qcarcam-inj-bundle.py`, fixed there), and nothing recorded which file, library or
symbol was involved. Finding the cause took a trace re-run and three rounds with the tester.
The people at the failing boards are not the developers, and what they can reliably do is
copy a file and send it.

## Decision

- **When the bundle exits non-zero, CamSyringe runs a diagnostic script on the target** that
  writes `/var/opt/tmp/qcarcam_install_diag.log` (overwritten each time), then reads it back.
  It is done by CamSyringe, over ssh, not by the bundle, so it works with any bundle already
  built, including the v0.12.0 one in the field.
- **The log records**: the BSP (`/etc/os-release`, `uname -a`); the bundle that ran (header,
  `BUILD_ID`) and the installed `BUILD_ID`; free disk; each directory of the library search
  path and whether it exists; **every installed file against `MANIFEST.md5`** (present, md5
  match, missing, mismatch); and for each of `qcarcam_dispatcher`, `qcarcam_injector`,
  `qcarcam_receiver`, `qcarcam_viewer`: the exit code and the loader's exact message when
  started with `--help`, its `ldd`, then **for every `unknown symbol` where the name appears
  in the search path (or that it appears nowhere), and for every library the loader names
  whether it exists in the search path**, and the installed tree.
- **It ends with `SUMMARY:` lines**, one per finding, in words ("qcarcam_injector cannot
  load: symbol 'cache_init' ... is in no library of the search path ... this board's BSP
  lacks the library or the version that provides it"). Those lines go at the top of the
  failure dialog.
- **The console and dialog name the file**: `root@<target>:/var/opt/tmp/qcarcam_install_diag.log`
  with the `scp` command to copy it, and ask for `~/.cache/camsyringe/install.log` too. The
  whole log is also written into `install.log` on the PC.
- **`/var/opt/tmp` is also the home of this log.** ADR 0019 said nothing else may use it;
  that is amended. The log's name does not match the `qcarcam_injector_bundle_*.bin` cleanup,
  and the install's `rm -rf` never touches `/var/opt/tmp`.

## Consequences

- A symbol "found" is only the name appearing in a library's bytes (no `nm` on the target), so
  it can be a reference rather than the definition. The log says so; "found nowhere" is
  conclusive, "found in X" is a lead.
- The script is written for the target's ksh (no `printf`, no bash-isms; ADR 0010). It has been
  run under dash on the host only; a ksh difference shows up as a missing section in the log.
- It runs only on a failed bundle run. A failed scp, `mkdir` or `rm` reports their own
  stderr and does not write this log.
- The log may name host and board paths, not credentials.
