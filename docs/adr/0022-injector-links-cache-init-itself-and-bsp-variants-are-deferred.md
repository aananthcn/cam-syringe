# 0022. The injector links `cache_init` itself; per-BSP variants are deferred until a real ABI difference is seen

Status: Accepted. The cause is reproduced, and the fix is verified, on the v14.4.1 bench
board in both REAL and SHIM mode. No v14.2.6 board has been seen, and the fix is not yet
released (bundle `qcarcam_injector_bundle_v0.12.0-cachefix.bin`, built but not versioned).

## Context

The testing team works on several releases in parallel and does not itself know which BSP a
board runs. The requirement is that **CamSyringe and `qcarcam-injector` work on any BSP in
use**, without BSP source. The 0.12.2 report (v14.2.6, `unknown symbol: cache_init
referenced from qcarcam_injector`) suggested the BSPs differ. ADR 0020 acted on that and
refuses an install on a BSP older than v14.4.1.

**QNX 7.1 is out of scope.** The testing team agreed to QNX 8.0 only (ADR 0018), so every
build uses the QNX 8.0 toolchain.

### Cause (v0.12.0 bundle, and the bench board 192.168.1.1, CARIAD OneBSP v14.4.1)

- `cache_init` is a **QNX SDK** function (`sys/cache.h`, static archive `libcache.a` under
  `qnx800/target/qnx/aarch64le/usr/lib/`), not a Qualcomm or BSP API. It cleans the CPU data
  cache after the CPU copies a frame into a pmem buffer, whose mapping is write-back;
  without it a non-CPU reader such as the display compositor can show stale pixels.
  Linked with `-lcache`, it is copied into the binary; no board library provides it, and
  the board has no `libcache`.
- **The shim** (`libqcxclient_shim.so`) is linked with `-lcache` and so contains
  `cache_init`. **The injector was also linked with `-lcache`, but after
  `-lqcxclient`**: `ext/lib/libqcxclient.so` (a copy of the shim) defines `cache_init`, so
  the linker resolved the symbol from that shared library first and never pulled the static
  `libcache.a` copy in. `bin/qcarcam_injector` therefore has `cache_init` undefined, and it
  is looked up at load time in whatever `libqcxclient.so` resolves.
- On the bench board the real library, `/mnt/lib64/camera/libqcxclient.so.real`, has no
  `cache_init`, and neither has any other library in the injector's chain (checked: libc,
  libm, libstdc++, libpmem_client, libapdf, libslog2, libaosal, libqcxosal, libqcxplatform,
  libcameradb, libmemorylogger, libc++). Only the shim does.
- **Reproduced**: with a temporary `libqcxclient.so` symlink to the `.real` file first in
  the library path (the bundle's own load-check path), `qcarcam_injector --help` on the
  v14.4.1 board fails with `unknown symbol: cache_init referenced from qcarcam_injector` /
  `ldd:FATAL: Could not resolve all symbols`: the 0.12.2 message, on the same BSP that
  "works". The test directory was removed; the board's symlink was not touched.

So the injector loads only **while the shim is the live `libqcxclient.so`**
(`/mnt/lib64/camera/libqcxclient.so -> /var/opt/lib/libqcxclient_shim.so`, ADR 0006), as it
was on the bench board. A board in REAL mode fails whatever its BSP version. That the
v14.2.6 board was in REAL mode is the likely explanation, not confirmed.

`ldd qcarcam_injector` on the board **passes** in the failing case: on QNX `ldd` only lists
libraries and does not resolve symbols. Only starting the program (the bundle uses
`--help`) shows the failure. Any "does it load" check must start the program.

## Decision

1. **Put `-lcache` first in `INJECTOR_LDLIBS`** (the `qcarcam-injector` Makefile, with a
   comment), before `-lqcxclient`, so the static `cache_init` is linked into the injector
   and nothing is looked up on the board. Only the injector references `cache_init` (the
   dispatcher, receiver, viewer and ffmpeg tools do not), so nothing else changes.
   **Done and verified**: `nm -D` shows `cache_init` as `T` in the injector; on the v14.4.1
   board, `--help` with the real `libqcxclient.so` first fails with the old injector and
   succeeds with the new one, and `--version` succeeds with the shim live. The only new
   undefined symbols are libc's (`dlopen`, `dlsym`, `dlclose`, `getenv`, `msync`,
   `_syspage_ptr`), pulled in by the `libcache` code. Note `make` does not relink when only
   the Makefile changes: delete the built `qcarcam_injector` first.
2. **Check the bundle build for accidental imports**: `nm -D --undefined-only` on each
   program must list only symbols from libraries the board is expected to have (the public
   `QCarCam*()` API, libc, libstdc++, the BSP libraries in `NEEDED`), never from the shim.
   The injector has 98 undefined dynamic symbols; they are not yet classified by supplier.
3. **Verify on both modes**: run `qcarcam_injector --help` on the board with the real
   `libqcxclient.so` first in the path and with the shim, before a release.
4. **Per-BSP variants are deferred.** The earlier idea (fingerprint the target's libraries,
   collect a report from an unknown board, ship one injector per variant, choose at
   install) is **not built** now, because nothing yet shows a BSP difference that breaks the
   injector. It is the plan if, after step 1, a board still fails to load and its ADR 0021
   log shows a symbol that really is missing from that BSP's libraries. Then:
   - record exported symbols, `DT_NEEDED` and md5 of the libraries involved (symbol tables,
     not the proprietary binaries);
   - collect them from the board into `/var/opt/tmp` and have the tester send the file;
   - ship per-variant injectors chosen at install, or resolve that one symbol with
     `dlopen`/`dlsym` behind an adapter.
5. **Ask the testing team for the list of BSP releases in use** and, where possible, a v14.2.6
   board or its log, to confirm the REAL-mode explanation.

## Consequences

- If step 1 fixes v14.2.6, the requirement is met with no per-BSP code, and the multi-variant
  work never starts.
- ADR 0020's gate rests on a false reason (see its amendment) and should be relaxed or
  removed once a bundle with step 1 has been run on a v14.2.6 board.
- Open: struct layouts and function signatures can change between BSPs without a symbol
  changing; only a board that fails after step 1 would show that.
