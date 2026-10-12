# 0013. Settings > Camera Injection is the one place for session settings, and it is remembered

Status: Accepted. Implemented, including saving, pre-loading and keeping the
configuration across reinstalls and releases. Tested through the real window and dialog
(`tests/gui_dialog_test.cpp`) as well as the storage rules. A new release bundle has to be
built and installed for an installed copy to get the fixes.

Formerly described only in `CONTEXT.md` (as "Configure"). Related: ADR 0007
(Inject only), ADR 0008 and 0011 (camera sizes), ADR 0012 (REAL blocks Play),
ADR 0001 (IPv4 default), ADR 0011 (`camera_configs.json`).

## Context

A run needs several settings that belong together: where the target is, how to
reach and authenticate to it, which camera ids get which video files, whether
the target should show a display, and whether to replay a BLF capture. They
used to be command-line arguments. A session also has to keep working when one
of them changes, such as pointing at a different board.

## Decision

- **`Settings > Camera Injection` opens one modal dialog for all session
  settings** (`CameraConfigDialog`, opened by `MainWindow::onConfigureTriggered`).
  A future setting gets a field in this dialog, not another menu action.
  (`Settings > Camera Configs` is a different action: the explicit read of the
  target's camera sizes, ADR 0011.)
- **It is only reachable while Idle.** The action is disabled while Playing or
  Paused, and the handler returns early if called in any other state.
- **Fields, with their defaults:**
  - target address, with a **Force IPv4** helper that only rewrites the target
    to one of the two known default addresses. IPv4 versus IPv6 is always
    derived from the target string itself, so the checkbox can't disagree with a
    typed target;
  - control port (5000);
  - SSH user (`root`) and an optional SSH key path with a Browse button
    (empty means ssh's own default identity or agent);
  - number of cameras, 1 to 4 (`kMaxCameras`);
  - per camera: a video file, and a QCarCam id (distinct defaults);
  - **Inject only (no display on target)** (ADR 0007), checked by default;
  - **Replay BLF Ethernet capture**: file and network interface (`enp6s0`),
    unchecked when no path is set.
- **Each camera row shows the id's learned size as information only.** It comes
  from the cached camera configs and updates as the id is edited. The dialog
  never starts a live query itself.
- **Cancel changes nothing. Accept applies everything at once:** the target
  becomes current; the SHIM/REAL indicator goes to BLIND and is re-queried
  against the new target; the reachability monitor restarts; the IPv6 and QCX
  API version checks run; the camera pool is rebuilt (ports from
  `PortScheme`, 5004 plus 2 per camera, and an RTP destination that brackets an
  IPv6 host); each camera is seeded from the cached size, and only ids with no
  usable entry are learned (ADR 0011); then the state is Idle.
- **Reopening the dialog shows the current session's values.** `MainWindow`
  passes its own current target, port, SSH settings, files, ids, inject-only
  and BLF settings back in, so reopening it doesn't reset them.

### Requirement: remember the last configuration across restarts, reinstalls and releases

- **The dialog's configuration is saved on every Accept and loaded at startup**, so the dialog opens
  pre-filled with the previous configuration and the user doesn't re-enter it.
- **It must survive a reinstall and a new release.** This was not met at first: the file was kept next to
  the binary, which for an installed copy is `<install-dir>/bin/`, and the bundle installer deletes and
  recreates `bin/`, `lib/` and `plugins/` on every install. A reinstall at 21:46 on 2026-10-12 removed the
  saved configuration, and the dialog opened empty a minute later. The location rules below exist for
  that reason.
- **Where the files live** (`ConfigFiles`, used for `camera_injection.json` and for `camera_configs.json`
  of ADR 0011), most preferred first:
  1. **the install directory itself** (the parent of `bin/`, recognised by `run-camsyringe.sh` being
     there), which the installer never deletes. A development build with no such layout uses the binary's
     own directory;
  2. **the per-user cache directory** (`~/.cache/camsyringe`), which needs no particular layout and
     survives deleting or reinstalling the application.
  A save is written to **every** location that accepts it, atomically, so losing one location loses
  nothing. A load takes the **newest** copy, and falls back to an older one if the newest doesn't parse.
  `CAMSYRINGE_CONFIG_DIRS` (a `:`-separated list) replaces the locations, for tests or a chosen place.
- **The files outlive any one build.** The location doesn't depend on the version number, and:
  - the file carries a `schemaVersion`, and a save never lowers a version a newer release wrote;
  - loading ignores keys it doesn't know and gives missing ones their defaults, so a file written by an
    older or newer release loads;
  - a save starts from what is already in the file and keeps keys this release doesn't know, so an older
    release saving doesn't erase a newer release's settings;
  - a file left in the binary's own directory by an earlier release is still read (it is never written to
    again), so upgrading doesn't lose a configuration that hadn't been reinstalled over yet.
- **The saved file must be read after the application object exists, and must not depend on that.** A
  second cause was found after the location fix: `main()` read the saved configuration *before*
  constructing the `QApplication`, and `QCoreApplication::applicationDirPath()` (and the application name
  that the cache directory is derived from) are empty until it exists, so every load silently found
  nothing, even though the file had been saved correctly. The dialog opened empty again with the
  configuration sitting on disk. Two changes: `main()` now creates the application object first, and
  `ConfigFiles` no longer relies on it (it asks the kernel for the binary's directory and spells the cache
  directory `~/.cache/camsyringe` out), so loading works in either order.
- **A failed save is visible.** If no location accepts the file, the status bar says so ("Couldn't save this
  configuration, so it won't be remembered next time: ..."). Before, the failure only went to stderr, which
  is why "it doesn't remember" was so hard to diagnose. A failure in only some locations is logged, since
  the configuration is still kept elsewhere.
- **The installer must never delete these files.** It only recreates `bin/`, `lib/` and `plugins/`.
- **Precedence, per option:** an option given on the command line wins for that
  launch; otherwise the saved value; otherwise the built-in default. Details:
  - the saved cameras are used only when the command line gave **no** video
    files and no `--cam-ids` (a partial mix would misalign files and ids);
  - the saved SSH user and key belong to the saved target, so they are reused
    only when this launch targets the same machine or names none. A different
    `--target` gets `root` and no key unless it says otherwise;
  - `--inject-only` can only turn the option on, so a saved "off" is kept
    unless the flag is given.
- **A bad saved value is dropped, not the file:** an out-of-range control port
  is treated as unset. The camera list is all-or-nothing, so one entry with an
  out-of-range id drops the whole list.
- **What is stored:** target, control port, SSH user and key path, per-camera
  video file and id, inject-only, BLF path and interface. **Never** a password
  or any credential typed into the credentials prompt, and never playback
  state.
- **Plain JSON, best effort**, in the same spirit as `camera_configs.json`:
  readable and editable by hand, a missing or unparsable file means defaults, and
  a failed save never blocks Play or Accept (it is reported, see above).
- **A saved video path that no longer exists is kept**, not silently dropped, so
  the user sees what was configured. Play reports the unreadable file in the
  normal way.

## Implementation status

- **Done:** the dialog and its apply path; `SessionConfigStore` and the startup merge in `main.cpp`; and the
  storage rules above (`src/util/ConfigFiles.{h,cpp}`, shared with `CameraConfigStore`), including the
  visible failure message in `MainWindow::onConfigureTriggered()`.
- **Tested.**
  - `tests/config_persistence_test.cpp` (host, all passing): a save lands in both locations and loads back;
    a reinstall that deletes the install location loses nothing and the next save restores it; a read-only
    install location still saves via the cache; nowhere writable reports failure; the newest copy wins; a
    corrupt newest copy falls back to the older one; files from an older and a newer release both load,
    and saving over a newer one keeps its keys and version; `camera_configs.json` follows the same rules;
    and a **regression test that loads before any application object exists** (the bug above).
  - `tests/gui_dialog_test.cpp` (headless, drives the real `MainWindow` and the real Settings > Camera
    Injection dialog; build with `cmake --build build --target camsyringe_gui_test`): the launch-time
    merge of command line, saved file and defaults (`mergeLaunchConfig`, extracted from `main()` so the
    test runs the real code); then, from a saved file, the dialog's first open shows the saved
    configuration; **Cancel loses nothing and writes nothing**; reopening after Cancel shows the same
    values; edits followed by Cancel are discarded and the file is untouched; edits followed by OK are
    saved and shown on the next open; a relaunch from the file shows what was accepted; and after a
    simulated reinstall (the install location deleted) the cache copy restores everything. All passing.
  - The new binary, started headlessly against the real saved file, reports `loaded
    ~/.cache/camsyringe/camera_injection.json` and leaves the file unchanged.
  - **What went wrong before:** the first version was only checked by starting the app and seeing it
    didn't crash, and by testing the store on its own, never by looking at what the dialog showed. That
    is why the load-order bug got through, and why the GUI test now exists.
- **Not yet done:** a new CamSyringe release bundle has to be built and installed. The copy installed at
  21:56 on 2026-10-12 has the location fix but still reads the file too early, so it saves the
  configuration (to `~/camsyringe/` and `~/.cache/camsyringe/`, both present) and then doesn't show it.
  The next bundle includes the load-order fix, and its first launch will pick the saved file up.

## Consequences

- A restart, a reinstall or an upgrade opens with the last accepted configuration. Passing
  arguments on the command line still wins, option by option.
- There are normally two copies of each file. Editing one by hand is fine, but the newer file wins,
  so edit the one that was saved last (or both).
- Anything that wipes both the install directory and `~/.cache` (a cache cleaner plus a full
  uninstall) loses the configuration. That is accepted: the alternative of a third, hidden location
  was not worth the surprise.
- A saved file can go stale (a target address that changed, a moved video). That
  is visible in the pre-filled dialog, and fixed by editing it.
- Credentials are deliberately not remembered, so an SSH password prompt still
  appears when passwordless authentication isn't set up.
