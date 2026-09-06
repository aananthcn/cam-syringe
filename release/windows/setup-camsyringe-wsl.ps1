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
      1. Checks WSL2 itself is installed/enabled. If not, self-elevates
         via UAC (Windows requires Administrator for this step, not this
         script) and installs it (Windows may require a REBOOT after
         this -- the script exits and asks you to re-run it after that).
         If this account has no path to Administrator at all -- no
         admin credentials to satisfy the UAC prompt, or the prompt/
         feature is blocked outright by Group Policy -- prints a banner
         telling you to raise an IT ticket instead of failing raw.
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
      4. Copies camsyringe_bundle_vX.Y.bin (built on Linux via this
         project's own release/create-cam-syringe-bundle.sh -- nothing
         Windows-specific about the bundle file itself) into the distro
         and runs its installer there, same as on native Linux.
      5. Creates a Start Menu shortcut (backed by a launcher script under
         %LOCALAPPDATA%\CamSyringe\) that runs camsyringe inside WSL2 via
         `wsl.exe`, relying on WSLg for the Qt window to actually appear
         on the Windows desktop.

.PARAMETER BundlePath
    Path to camsyringe_bundle_vX.Y.bin (built on Linux). Optional --
    if omitted, auto-detects a single camsyringe_bundle_v*.bin sitting
    next to this script (that's how release/create-windows-bundle.sh
    packages the two together into one .zip, so double-clicking
    install-camsyringe.cmd needs no arguments at all).

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
    .\setup-camsyringe-wsl.ps1
    (auto-detects camsyringe_bundle_v*.bin next to this script)

.EXAMPLE
    .\setup-camsyringe-wsl.ps1 -BundlePath .\camsyringe_bundle_v0.5.bin

.NOTES
    Does NOT need to be run elevated. Step 1 (WSL feature enablement)
    needs Administrator, but only if WSL2 isn't already installed -- when
    that's the case, this script self-elevates itself (a UAC prompt) via
    Start-Process -Verb RunAs rather than requiring you to open an
    elevated prompt yourself. If this account has no path to
    Administrator at all (typical on a locked-down corporate machine, and
    the reason this isn't just a hard requirement), it prints a banner
    telling you to raise an IT ticket instead of failing with a raw
    error. Once WSL2 itself is installed, nothing else this script does
    needs elevation.
#>

[CmdletBinding()]
param(
    [string]$BundlePath = "",

    [string]$DistroName = "Ubuntu-22.04",

    [string]$InstallDirInWsl = "~/camsyringe"
)

$ErrorActionPreference = "Stop"

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# On a locked-down corporate machine, the signed-in Windows account
# frequently has no path to Administrator at all -- no local admin
# credentials to satisfy a UAC prompt, and/or the WSL2/Hyper-V Windows
# features are disabled outright by Group Policy, even for an admin who
# does elevate. Both are IT's call to fix, not something to leave the
# tester debugging from a raw PowerShell exception, so both paths below
# print this instead and stop cleanly.
function Show-ItTicketBanner {
    param([string]$Reason)
    Write-Host ""
    Write-Host "=================================================================" -ForegroundColor Yellow
    Write-Host "  CamSyringe needs WSL2 on this PC, and this account can't" -ForegroundColor Yellow
    Write-Host "  install/enable it here." -ForegroundColor Yellow
    Write-Host "=================================================================" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  $Reason"
    Write-Host ""
    Write-Host "  Raise an IT ticket asking for:"
    Write-Host "    - WSL2 installed/enabled on this machine (the 'Windows"
    Write-Host "      Subsystem for Linux' and 'Virtual Machine Platform'"
    Write-Host "      Windows features -- equivalent to running 'wsl --install'"
    Write-Host "      once as Administrator)."
    Write-Host "    - Windows 11 22H2+, if BLF/Ethernet replay is needed"
    Write-Host "      (older Windows can't use the WSL2 'mirrored' networking"
    Write-Host "      mode replay depends on -- camera streaming works either"
    Write-Host "      way)."
    Write-Host ""
    Write-Host "  Once IT confirms WSL2 is installed, re-run this installer"
    Write-Host "  (install-camsyringe.cmd) -- no admin rights are needed after"
    Write-Host "  that one-time step."
    Write-Host "=================================================================" -ForegroundColor Yellow
    Write-Host ""
}

if ([string]::IsNullOrWhiteSpace($BundlePath)) {
    # No -BundlePath given -- look for the one this script ships
    # alongside in release/create-windows-bundle.sh's .zip layout. Also
    # check ../artifacts (sibling to a "windows" folder) as a fallback --
    # that's this PROJECT'S OWN raw source-tree layout (release/windows/
    # + release/artifacts/ as separate folders), which is a mistake to
    # hand a teammate (they should get create-windows-bundle.sh's single
    # flat .zip instead) but has actually shown up in practice, so it's
    # worth recovering from rather than just failing.
    $searchDirs = @($PSScriptRoot)
    $siblingArtifacts = Join-Path (Split-Path $PSScriptRoot -Parent) "artifacts"
    if (Test-Path $siblingArtifacts) { $searchDirs += $siblingArtifacts }

    $candidates = @(Get-ChildItem -Path $searchDirs -Filter "camsyringe_bundle_v*.bin" -File -ErrorAction SilentlyContinue)
    if ($candidates.Count -eq 1) {
        $BundlePath = $candidates[0].FullName
        Write-Host "Using bundle: $BundlePath"
        if ($candidates[0].DirectoryName -ne $PSScriptRoot) {
            Write-Warning "That .bin wasn't next to this script -- it looks like you have this project's raw release/ folder (windows/ + artifacts/ as separate folders) rather than the single .zip create-windows-bundle.sh builds. Both work, but for a real handoff, use the .zip -- see release/README.md."
        }
    } elseif ($candidates.Count -gt 1) {
        Write-Error "Multiple camsyringe_bundle_v*.bin files found ($($searchDirs -join ', ')) -- pass -BundlePath to pick one: $($candidates.Name -join ', ')"
        exit 1
    } else {
        Write-Error "No camsyringe_bundle_v*.bin found next to this script (or in ..\artifacts), and -BundlePath wasn't given. If you were handed this project's raw release/ folder rather than a single camsyringe_windows_bundle_vX.Y.zip, that's the mismatch -- ask for the .zip that release/create-windows-bundle.sh builds, or pass -BundlePath pointing at the .bin directly."
        exit 1
    }
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
        # Don't just error out -- try to self-elevate (re-launch this
        # exact script, same arguments, in a new elevated PowerShell). On
        # a normal dev machine this is a one-click UAC "Yes". On a
        # corporate machine with no admin path at all, Start-Process
        # throws (user has no credentials to satisfy the prompt, or the
        # prompt itself is policy-blocked) -- that's the actual "can't
        # install" signal the IT-ticket banner is for.
        Write-Host "WSL2 isn't installed/enabled yet -- that needs a one-time Administrator step. Requesting elevation..."
        $elevated = $false
        try {
            $selfArgs = "-NoProfile -ExecutionPolicy Bypass -NoExit -File `"$PSCommandPath`" -BundlePath `"$BundlePath`" -DistroName `"$DistroName`" -InstallDirInWsl `"$InstallDirInWsl`""
            Start-Process -FilePath "powershell.exe" -ArgumentList $selfArgs -Verb RunAs -Wait
            $elevated = $true
        } catch {
            $elevated = $false
        }
        if (-not $elevated) {
            Show-ItTicketBanner "This Windows account doesn't have -- and couldn't obtain -- the Administrator rights WSL2's one-time setup needs. That's expected on a locked-down corporate machine."
            exit 1
        }
        Write-Host ""
        Write-Host "The elevated window that just ran (and that you closed) handled the one-time WSL2 setup step."
        Write-Host "Re-run this installer (or double-click install-camsyringe.cmd again) to continue."
        exit 0
    }

    Write-Host "Enabling WSL2 (this may require a reboot)..."
    wsl --install --no-distribution
    if ($LASTEXITCODE -ne 0) {
        Show-ItTicketBanner "'wsl --install' failed (exit code $LASTEXITCODE) even with Administrator rights -- likely a corporate Group Policy blocking the underlying Hyper-V/Virtual Machine Platform Windows features."
        exit 1
    }
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
    if ($LASTEXITCODE -ne 0) {
        Show-ItTicketBanner "'wsl --install -d $DistroName' failed (exit code $LASTEXITCODE) -- likely blocked network/Microsoft Store access needed to fetch the Linux distro image, common on a locked-down corporate network."
        exit 1
    }
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

# Optional: a qcarcam_injector_bundle_vX.Y.bin sitting next to this script
# (create-windows-bundle.sh packs one in when available -- see its own
# comment) gets copied to the INSTALL ROOT inside WSL (sibling to bin/
# lib/plugins, e.g. ~/camsyringe/qcarcam_injector_bundle_v0.5.bin) so
# CamSyringe's own "Install Injector" menu action auto-detects it with no
# extra setup (its InjectorBundleFinder looks at its own install
# directory). Best-effort -- nothing breaks if it's missing; the teammate
# just gets the same file-picker fallback as everyone else the first time.
$injectorBundles = @(Get-ChildItem -Path $PSScriptRoot -Filter "qcarcam_injector_bundle_v*.bin" -File -ErrorAction SilentlyContinue)
if ($injectorBundles.Count -eq 1) {
    Write-Host "Copying $($injectorBundles[0].Name) into the WSL install directory..."
    $injectorUncTarget = "\\wsl.localhost\$DistroName\tmp\$($injectorBundles[0].Name)"
    Copy-Item -Path $injectorBundles[0].FullName -Destination $injectorUncTarget -Force
    wsl -d $DistroName -- bash -lc "cp '/tmp/$($injectorBundles[0].Name)' $InstallDirInWsl/"
} elseif ($injectorBundles.Count -gt 1) {
    Write-Warning "Multiple qcarcam_injector_bundle_v*.bin found next to this script -- skipping automatic copy (CamSyringe's Install Injector action will prompt for one instead)."
}

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
Write-Host ""
Write-Host "If menu/button icons (they're plain Unicode characters, not bundled images) show up as empty boxes, WSLg's minimal font set is missing coverage for them -- fix with one more one-time step inside WSL:"
Write-Host "  wsl -d $DistroName -- sudo bash -c `"apt-get update && apt-get install -y fonts-noto-color-emoji fonts-noto-symbols fonts-noto-symbols2`""
