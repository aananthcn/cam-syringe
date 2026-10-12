# 0014. The installed injector is verified against the bundle, and the control protocol is versioned

Status: Accepted. Implemented. The manifest reader and comparison and the bundle
script's manifest were tested; the end-to-end check against a real target was
not run, and the target-side change (the dispatcher's `PROTOCOL` line) needs a
QNX build.

Related: `qnx_toolkit` ADR 0002 (the bundle script), `qcarcam-injector` ADR 0015
(the control protocol), ADR 0013 here.

## Context

On 2026-10-12 an injector bundle was built but not installed, and nothing said so:
the target kept running old binaries. The existing check compared `version.txt`
strings, which only works if the version is bumped on every rebuild. Several
rebuilds of v0.10 look identical. Two different questions were being mixed up:

- **Freshness:** is what is installed the bundle this PC has?
- **Compatibility:** can this CamSyringe talk to that dispatcher at all?

## Decision

### Freshness: a manifest of hashes, checked on the target

- **The bundle carries a manifest** (`create-qcarcam-inj-bundle.py`): an MD5 for
  each file the project itself builds or writes (the four `qcarcam_*` binaries,
  `run_qcarcam.sh`, `env.sh`, `libqcxclient_shim.so`) and a **build id**, the
  first 12 hex digits of the MD5 of the manifest text. The id changes whenever any
  listed file does, even at the same version number. ffmpeg and SDL2 are left out
  on purpose: they rarely change and would only add noise.
- **MD5, not SHA-256,** because the target has `md5sum`, `sha1sum` and `cksum` but
  no `sha256sum`. It is used for change detection, not security.
- **Two copies of it:** `MANIFEST.md5` and `BUILD_ID` go into the payload (so they
  land in `/var/opt`), and the same data goes into the installer's header as
  plain comment lines (`# BUILD_ID=`, `# INSTALL_DIR=`, `# MANIFEST <md5> <path>`).
  CamSyringe reads the header as text, with no unpacking.
- **On Play, CamSyringe hashes what is really installed** (`md5sum` over the
  manifest's paths, inside the install directory, passwordless SSH only) and
  compares. Hashing the actual files, not just reading `BUILD_ID`, also catches a
  hand-copied single binary, which a `BUILD_ID` file would miss.
- **A mismatch only warns:** the status bar names the files that differ or are
  missing and says to use **⇪ Install Injector**. It never blocks Play. It is
  silent when there is no local bundle, the bundle has no manifest (an older
  one), or the target can't be reached without a password.
- **A bundle's paths are validated** (plain relative names only), because they
  end up in a shell command on the target. Anything else rejects the whole
  manifest.

### Compatibility: a protocol version number

- **`qcarcam_dispatcher` has a `kProtocolVersion`** (currently 1), separate from
  the release version. It answers the existing `VERSION` probe with a second line,
  `PROTOCOL <n>`. A client that reads only the first line is unaffected.
- **CamSyringe has `kDispatcherProtocolVersion`** (`net/DispatcherProtocol.h`) and
  compares on the same probe.
- **A known mismatch is a hard stop:** Play is refused with a clear message, and a
  session already running is stopped, like the QCX API mismatch.
- **No `PROTOCOL` line means unknown, not a mismatch.** A dispatcher that predates
  versioning is still caught by the freshness check, not by this one.
- **Bump the number only when a change would break an older or newer peer** (a
  renamed or removed line, a changed reply). Adding an optional line that peers
  ignore needs no bump.

## Implementation status

- **Done:** manifest generation and the header block (tested against a fake stage:
  the manifest verifies with `md5sum -c`, the id changes with content, the archive
  marker stays at the start of its line); `InjectorManifestReader` and its
  comparison (tested for a good bundle, no manifest, an unsafe path, a missing
  file, identical, differing, missing and nothing installed); the probe's protocol
  field; the Play block; the install check and its warning.
- **Not run:** the install check against a real target, the dispatcher's
  `PROTOCOL` reply (host syntax check only, not built with the QNX toolchain), and
  a full bundle build.

## Consequences

- A stale or partial install is now visible on Play.
- The target-side half needs a new bundle: until it is installed, the target
  reports no protocol, so the protocol check can't fire.
- If a new file becomes important to the install, add it to `MANIFEST_FILES` in
  the bundle script.
