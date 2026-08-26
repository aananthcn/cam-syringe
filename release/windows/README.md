# CamSyringe on Windows (via WSL2)

CamSyringe is not a native Windows app, and isn't becoming one. Its BLF/
Ethernet replay feature sends raw Ethernet frames via Linux's
`AF_PACKET`/`SOCK_RAW` socket family — a Linux kernel-specific API, not
even POSIX, with no Windows equivalent at all (native raw L2 send on
Windows needs a third-party driver like Npcap, a different codebase
entirely). Porting away from it to make a native Windows build possible
would mean dropping BLF replay, so instead: run the exact same Linux
binary this project already builds, tests, and bundles for Linux, inside
WSL2, with a thin Windows-side launcher so it still opens as a normal
window on the Windows desktop (via WSLg — Windows' built-in WSL GUI
support, no separate X server needed).

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

## What you need

- **Windows 10 version 2004+ or Windows 11**, with WSL2 support.
  Mirrored networking (see below) specifically needs **Windows 11
  22H2+** — check Microsoft's current WSL docs, this requirement has
  moved before and may move again.
- **Administrator access**, for the one-time WSL2 feature enablement
  (Windows enforces this, not anything in this project).
- **A `camsyringe_bundle_vX.Y.bin`** — build it on a Linux machine via
  `release/create-cam-syringe-bundle.sh` (see that script's own
  `release/README.md`), then copy the resulting file to the Windows
  machine (any way you like — USB, network share, etc.).

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
BLF replay actually needs. The setup script configures this
automatically; if you're on a Windows version that doesn't support it,
camera streaming still works, BLF replay won't.

## Automated setup

From an **elevated** ("Run as Administrator") PowerShell prompt, the
first time:

```powershell
cd release\windows
.\setup-camsyringe-wsl.ps1 -BundlePath C:\path\to\camsyringe_bundle_v0.5.bin
```

It may ask you to reboot (fresh WSL2 enablement) or finish an interactive
Linux user/password setup (fresh distro install) and re-run the same
command afterward — both are one-time steps. See the script's own
`.SYNOPSIS`/`.DESCRIPTION` comment block (`Get-Help .\setup-camsyringe-wsl.ps1 -Full`)
for exactly what each step does.

**Where things actually land**, answering the "does this go under
`AppData`?" question directly: the real `camsyringe` binary and its
libraries install *inside the WSL2 distro's own Linux filesystem*
(`~/camsyringe` there by default, reachable from Windows Explorer at
`\\wsl.localhost\<distro>\home\<user>\camsyringe\` if you need to browse
it) — **not** under Windows `AppData` in any meaningful sense, since it's
not a Windows binary. What the script puts under
`%LOCALAPPDATA%\CamSyringe\` is just the small Windows-side launcher
script (`run-camsyringe.cmd`) that the Start Menu shortcut points at —
that's the Windows-native part of this setup, and it's the conventional
place for a per-user, machine-local (non-roaming) launcher to live.

After setup, BLF replay additionally needs `CAP_NET_RAW` set on the
installed binary *inside WSL*, same as on native Linux (this needs
`sudo`, so it's a separate step from the rest of the script, which
avoids needing your Linux `sudo` password):

```powershell
wsl -d Ubuntu-22.04 -- sudo setcap cap_net_raw+ep ~/camsyringe/bin/camsyringe
```

Re-run this after every fresh bundle install (a new binary means the
capability needs setting again, same as on Linux).

## Manual setup (if you'd rather not run the script, or it doesn't work as written)

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
release/windows/
  setup-camsyringe-wsl.ps1   the automated setup script (untested -- see above)
  README.md                  this file
```
