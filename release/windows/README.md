# CamSyringe on Windows (via WSL2)

CamSyringe is not a native Windows app, and isn't becoming one. Its BLF/
Ethernet replay feature sends raw Ethernet frames via Linux's
`AF_PACKET`/`SOCK_RAW` socket family — a Linux kernel-specific API, not
even POSIX, with no Windows equivalent at all (native raw L2 send on
Windows needs a third-party driver like Npcap, a different codebase
entirely). Porting away from it to make a native Windows build possible
would mean dropping BLF replay, so instead: run the exact same Linux
binary this project already builds, tests, and bundles for Linux, inside
WSL2, with a thin Windows-side installer/launcher so it still opens as a
normal window on the Windows desktop (via WSLg — Windows' built-in WSL
GUI support, no separate X server needed).

> **This has not been run on a real Windows machine.** Every other
> release script in this project (`release/create-cam-syringe-bundle.sh`,
> `qcarcam-injector`'s own bundle script) was verified end-to-end against
> real hardware before being called done. This one couldn't be — there's
> no Windows machine in this project's own dev environment. It's written
> against Microsoft's documented `wsl.exe`/`.wslconfig`/WSLg behavior,
> reviewed carefully, but genuinely untested. Read `setup-camsyringe-wsl.ps1`
> before running it, and expect to debug real surprises the first time —
> particularly around exact WSL/Windows version requirements, which
> Microsoft changes over time.

## What a Windows teammate needs

- **Windows 10 version 2004+ or Windows 11**, with WSL2 support.
  Mirrored networking (see below) specifically needs **Windows 11
  22H2+** — check Microsoft's current WSL docs, this requirement has
  moved before and may move again.
- **Administrator access, if WSL2 isn't already installed.** Windows
  enforces this for the one-time feature-enablement step, not anything in
  this project. The installer asks for it itself via a UAC prompt, and
  only if it's actually needed — a machine where IT already installed
  WSL2 needs no elevation at all. **On a corporate machine where this
  account has no path to Administrator**, the installer prints a banner
  telling the teammate to raise an IT ticket instead of failing with a
  raw error — see "If there's no admin access" below.
- **The `camsyringe_windows_bundle_vX.Y.zip`** described below — that's
  the one file to hand them.

## Building the bundle (on your Linux dev machine)

```bash
cd release
./create-cam-syringe-bundle.sh 0.5   # builds & packages the Linux bundle
./create-windows-bundle.sh 0.5       # wraps it for Windows
```

This produces `release/artifacts/camsyringe_windows_bundle_v0.5.zip`,
containing:

```
camsyringe_bundle_v0.5.bin     the Linux bundle (create-cam-syringe-bundle.sh)
install-camsyringe.cmd         double-click entry point
setup-camsyringe-wsl.ps1       does the actual WSL2 setup + install
                                (self-elevates via UAC only if/when needed)
```

Send that one `.zip` to the Windows machine any way you like (USB,
network share, etc.) — it's gitignored, not committed to this repo, same
as the Linux `.bin` it wraps.

## What the teammate does

1. Extract the `.zip` anywhere.
2. Double-click `install-camsyringe.cmd`.
3. Approve the UAC prompt, if one appears — only shown the first time,
   and only if WSL2 isn't already installed on this machine.

That single double-click drives everything: WSL2 + a Linux distro get
installed if missing, WSL2 "mirrored" networking gets configured (see
below), the `camsyringe_bundle_v*.bin` sitting next to the `.cmd` gets
installed inside the distro, and a Start Menu shortcut appears. Windows
itself may require a reboot partway through (fresh WSL2 enablement) or a
one-time interactive Linux username/password prompt (fresh distro
install) — in either case, just double-click `install-camsyringe.cmd`
again afterward; it picks up where it left off.

After that, launch CamSyringe from the Start Menu (search "CamSyringe").

### If there's no admin access

Corporate laptops routinely give the signed-in account no path to
Administrator at all — no local admin credentials to satisfy the UAC
prompt, and/or WSL2's underlying Windows features blocked outright by
Group Policy. `setup-camsyringe-wsl.ps1` tries to self-elevate when WSL2
isn't already present, and if that fails, prints a clear banner instead
of a raw PowerShell error, telling the teammate to raise an IT ticket
asking for WSL2 (or, equivalently, the "Windows Subsystem for Linux" and
"Virtual Machine Platform" Windows features) to be installed on that
machine. The same banner appears if `wsl --install` itself fails even
under Administrator (Group Policy blocking Hyper-V/Virtual Machine
Platform), or if fetching the Linux distro image fails (blocked Store/
network access) — all IT-side fixes, not anything the teammate or this
script can work around. Once IT confirms WSL2 is installed, re-running
`install-camsyringe.cmd` needs no admin rights at all.

**Where things actually land**, answering the "does this go under
`AppData`?" question directly: the real `camsyringe` binary and its
libraries install *inside the WSL2 distro's own Linux filesystem*
(`~/camsyringe` there by default, reachable from Windows Explorer at
`\\wsl.localhost\<distro>\home\<user>\camsyringe\` if you need to browse
it) — **not** under Windows `AppData` in any meaningful sense, since it's
not a Windows binary. What the installer puts under
`%LOCALAPPDATA%\CamSyringe\` is just the small Windows-side launcher
script (`run-camsyringe.cmd`) that the Start Menu shortcut points at —
that's the Windows-native part of this setup, and it's the conventional
place for a per-user, machine-local (non-roaming) launcher to live.

**BLF/Ethernet replay** needs one extra one-time step *inside WSL*, same
as on native Linux (this needs `sudo`, which is why it's separate — the
installer avoids needing the Linux `sudo` password):

```powershell
wsl -d Ubuntu-22.04 -- sudo setcap cap_net_raw+ep ~/camsyringe/bin/camsyringe
```

Re-run this after every fresh bundle install (a new binary means the
capability needs setting again, same as on Linux). Camera streaming works
without it.

## Why mirrored networking matters

WSL2's *default* networking mode (NAT) puts the whole distro behind a
virtualized network adapter. Camera RTP streaming to a QNX target works
fine through that — it's ordinary outbound UDP/TCP, which a NAT'd guest
can already send. **BLF/Ethernet replay can't** — a raw Ethernet frame
sent via `AF_PACKET` from inside a NAT'd WSL2 guest never reaches the
host's real physical network adapter, so BLF replay would run without
any error but silently inject nothing onto the real wire. WSL2's
"mirrored" networking mode (Windows 11 22H2+) shares the host's real
network interfaces directly with the WSL2 guest instead, which is what
BLF replay actually needs. `setup-camsyringe-wsl.ps1` configures this
automatically; on a Windows version that doesn't support it, camera
streaming still works, BLF replay won't.

## Manual setup (if the installer doesn't work as written)

1. Install WSL2 + a distro (elevated PowerShell): `wsl --install -d Ubuntu-22.04`, reboot if asked, finish the Linux user/password prompt.
2. `notepad $env:USERPROFILE\.wslconfig`, add:
   ```
   [wsl2]
   networkingMode=mirrored
   ```
   then `wsl --shutdown` and reopen your WSL terminal.
3. Copy `camsyringe_bundle_vX.Y.bin` into the distro (e.g. via `\\wsl.localhost\Ubuntu-22.04\tmp\`) and, inside a WSL terminal:
   ```bash
   chmod +x /tmp/camsyringe_bundle_v0.5.bin
   /tmp/camsyringe_bundle_v0.5.bin
   sudo setcap cap_net_raw+ep ~/camsyringe/bin/camsyringe   # only if you need BLF replay
   ```
4. Run it: `~/camsyringe/run-camsyringe.sh` — the Qt window should appear on the Windows desktop via WSLg. If it doesn't, confirm WSLg itself is working first with a simpler GUI test (Microsoft's docs cover this) before assuming it's a CamSyringe-specific problem.
5. Optional: create your own Start Menu shortcut pointing at a `.cmd` file containing `wsl.exe -d Ubuntu-22.04 -- bash -lc "~/camsyringe/run-camsyringe.sh %*"`.

## Files here

```
release/
  windows/
    README.md                  this file
    install-camsyringe.cmd     double-click entry point (just runs
                                setup-camsyringe-wsl.ps1)
    setup-camsyringe-wsl.ps1   does the actual WSL2 setup + install,
                                self-elevating via UAC only if/when
                                needed, with an IT-ticket banner if that
                                fails (untested -- see above)
  create-windows-bundle.sh     zips the two files above together with a
                                built Linux .bin into one distributable
                                .zip (see release/README.md)
```
