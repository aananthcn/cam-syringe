<#
.SYNOPSIS
    Sets up CamSyringe to run on Windows via WSL2 + WSLg, using this
    project's existing Linux release bundle.

.DESCRIPTION
    Author: Aananth C N

    CamSyringe is not being ported to native Windows -- its BLF/Ethernet
    replay feature (BlfReplayer) sends raw Ethernet frames via Linux's
    AF_PACKET/SOCK_RAW socket family, which has no Windows equivalent at
    all (not even POSIX -- it's a Linux kernel-specific API; native
    Windows raw L2 send needs a third-party driver like Npcap, a
    different codebase entirely). Rather than dropping BLF replay to make
    a native port tractable, this runs the SAME Linux binary/bundle this
    project already builds and verifies on Linux, inside a WSL2
    distribution, with a Windows-side launcher so it still opens as a
    normal-looking window on the Windows desktop (via WSLg, Windows'
    built-in WSL GUI support -- no separate X server to install).

    UNTESTED ON A REAL WINDOWS MACHINE. This project's own dev/build
    environment is Linux-only -- unlike every other release script in
    this project (which were all verified end-to-end against real
    hardware/machines before being called done), this one could only be
    written against Microsoft's documented wsl.exe/.wslconfig/WSLg
    behavior, not confirmed by actually running it. Treat it as a
    reviewed starting point, not a verified tool -- read it before
    running it, and expect to debug real Windows-version-specific
    surprises the first time it's tried on an actual machine.

    What this script does, in order:
      1. Checks WSL2 itself is installed/enabled. If not, and running
         elevated, installs it (Windows may require a REBOOT after this
         -- the script exits and asks you to re-run it after that).
         Requires Administrator; Windows enforces this, not this script.
      2. Checks for the target WSL distro (default Ubuntu-22.04),
         installs it if missing (first run of a fresh distro needs you
         to set a Linux username/password interactively -- the script
         exits and asks you to re-run after that's done).
      3. Configures WSL2 "mirrored" networking mode in %USERPROFILE%\.wslconfig
         -- REQUIRED for BLF replay's raw AF_PACKET send to actually
         reach the real LAN and hit the QNX target. WSL2's DEFAULT
         networking mode (NAT) virtualizes the network adapter entirely;
         a raw L2 frame sent from inside a NAT'd WSL2 guest never reaches
         the host's physical NIC, so BLF replay would silently send
         nothing meaningful even if the app itself ran fine. Camera RTP
         streaming to the target does NOT need this -- that's ordinary
         outbound UDP/TCP, which a NAT'd guest can already send -- only
         BLF replay strictly requires mirrored mode. Needs Windows 11
         22H2 or later; on anything older this step is skipped with a
         warning (BLF replay won't reach the real LAN there, camera
         streaming still will).
      4. Copies the given camsyringe_bundle_vX.Y.bin (built on Linux via
         this project's own release/create-cam-syringe-bundle.sh --
         nothing Windows-specific about the bundle file itself) into the
         distro and runs its installer there, same as on native Linux.
      5. Creates a Start Menu shortcut (backed by a launcher script under
         %LOCALAPPDATA%\CamSyringe\) that runs camsyringe inside WSL2 via
         `wsl.exe`, relying on WSLg for the Qt window to actually appear
         on the Windows desktop.

.PARAMETER BundlePath
    Path to camsyringe_bundle_vX.Y.bin (built on Linux). Required.

.PARAMETER DistroName
    WSL distro to install into. Default: Ubuntu-22.04.

.PARAMETER InstallDirInWsl
    Where the bundle installs to INSIDE the WSL distro (passed straight
    through to the bundle's own installer via CAMSYRINGE_INSTALL_DIR, and
    reused as-is for the launcher .cmd). Default: ~/camsyringe (that
    installer's own default). Keep this a plain Linux path with NO
    spaces -- it's passed to bash unquoted so "~" still expands (bash
    quoting of any kind suppresses tilde expansion), which means a path
    containing a space would word-split and break.

.EXAMPLE
    .\setup-camsyringe-wsl.ps1 -BundlePath .\camsyringe_bundle_v0.5.bin

.NOTES
    Run from an elevated ("Run as Administrator") PowerShell prompt the
    first time -- WSL feature enablement (step 1) needs it. Re-running
    after WSL/the distro is already set up does not need elevation.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BundlePath,

    [string]$DistroName = "Ubuntu-22.04",

    [string]$InstallDirInWsl = "~/camsyringe"
)

$ErrorActionPreference = "Stop"

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Path $BundlePath)) {
    Write-Error "Bundle not found: $BundlePath (build one on Linux first -- see release/README.md)"
    exit 1
}
$BundlePath = (Resolve-Path $BundlePath).Path

Write-Host "== Checking WSL2 =="
$wslInstalled = $false
try {
    wsl --status *> $null
    if ($LASTEXITCODE -eq 0) { $wslInstalled = $true }
} catch {
    $wslInstalled = $false
}

if (-not $wslInstalled) {
    if (-not (Test-IsAdmin)) {
        Write-Error "WSL2 isn't installed/enabled yet, and enabling it needs Administrator. Re-run this script from an elevated ('Run as Administrator') PowerShell prompt."
        exit 1
    }
    Write-Host "Enabling WSL2 (this may require a reboot)..."
    wsl --install --no-distribution
    Write-Warning "WSL2 was just enabled. If Windows asked you to reboot, do that now, then re-run this script (same command) to continue."
    exit 0
}

Write-Host "== Checking for WSL distro '$DistroName' =="
$existingDistros = @()
try {
    $existingDistros = (wsl -l -q) | ForEach-Object { $_.Trim().TrimEnd([char]0) } | Where-Object { $_ -ne "" }
} catch {
    $existingDistros = @()
}

if ($existingDistros -notcontains $DistroName) {
    Write-Host "Installing $DistroName (this opens a console window for first-time Linux user/password setup)..."
    wsl --install -d $DistroName
    Write-Warning "Finish setting up your Linux username/password in the window that just opened, then re-run this script (same command) to continue."
    exit 0
}

Write-Host "== Configuring WSL2 mirrored networking =="
# See this script's own header comment for why this matters specifically
# for BLF replay, and why camera streaming works without it.
$wslConfigPath = "$env:USERPROFILE\.wslconfig"
$wslConfigContent = ""
if (Test-Path $wslConfigPath) {
    $wslConfigContent = Get-Content $wslConfigPath -Raw
}
if ($wslConfigContent -match "networkingMode\s*=\s*mirrored") {
    Write-Host "Mirrored networking already configured in $wslConfigPath."
} else {
    Write-Host "Adding networkingMode=mirrored to $wslConfigPath (needs Windows 11 22H2+; BLF replay won't reach the real LAN without this on older Windows -- camera streaming is unaffected either way)..."
    Add-Content -Path $wslConfigPath -Value "`n[wsl2]`nnetworkingMode=mirrored`n"
    Write-Warning "Run 'wsl --shutdown' and reopen your WSL terminal for this to take effect (this script will still finish the install below either way)."
}

Write-Host "== Installing the camsyringe bundle inside $DistroName =="
$bundleFileName = Split-Path $BundlePath -Leaf
$wslBundlePath = "/tmp/$bundleFileName"

# Ensure the distro is actually started before copying into its
# filesystem via the \\wsl.localhost UNC path -- that path is served by
# a running distro instance, not a cold one.
wsl -d $DistroName -- true

$uncTarget = "\\wsl.localhost\$DistroName\tmp\$bundleFileName"
Copy-Item -Path $BundlePath -Destination $uncTarget -Force
# $InstallDirInWsl deliberately UNQUOTED in the bash command below (only
# safe because it's documented as a simple path with no spaces) -- both
# single AND double bash quotes suppress "~" expansion, which would
# otherwise turn the default "~/camsyringe" into a literal directory
# named "~" instead of expanding to $HOME. Left unquoted, bash expands it
# itself exactly like an interactive user typing the same path would.
wsl -d $DistroName -- bash -lc "chmod +x '$wslBundlePath' && CAMSYRINGE_INSTALL_DIR=$InstallDirInWsl '$wslBundlePath'"

Write-Host "== Creating a Windows launcher + Start Menu shortcut =="
$launcherDir = "$env:LOCALAPPDATA\CamSyringe"
New-Item -ItemType Directory -Path $launcherDir -Force | Out-Null

$launcherPath = "$launcherDir\run-camsyringe.cmd"
$launcherBody = "@echo off`r`nwsl.exe -d $DistroName -- bash -lc `"$InstallDirInWsl/run-camsyringe.sh %*`"`r`n"
Set-Content -Path $launcherPath -Value $launcherBody -Encoding ASCII -NoNewline

$startMenuDir = "$env:APPDATA\Microsoft\Windows\Start Menu\Programs"
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut("$startMenuDir\CamSyringe.lnk")
$shortcut.TargetPath = $launcherPath
$shortcut.WorkingDirectory = $launcherDir
$shortcut.Description = "CamSyringe (runs inside WSL2 '$DistroName' via WSLg)"
$shortcut.Save()

Write-Host ""
Write-Host "Done."
Write-Host "Launch CamSyringe from the Start Menu (search 'CamSyringe'), or directly:"
Write-Host "  $launcherPath"
Write-Host ""
Write-Host "For BLF/Ethernet replay specifically, also run inside WSL once:"
Write-Host "  wsl -d $DistroName -- sudo setcap cap_net_raw+ep $InstallDirInWsl/bin/camsyringe"
Write-Host "(needed after every fresh bundle install, same as on native Linux -- see the main README.md's setcap note)"
