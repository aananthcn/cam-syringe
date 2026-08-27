@echo off
setlocal

rem CamSyringe Windows installer/launcher -- the double-click entry point
rem in this bundle's .zip (see release/create-windows-bundle.sh). Just
rem runs setup-camsyringe-wsl.ps1, which auto-detects the
rem camsyringe_bundle_v*.bin sitting next to it in this same folder and
rem handles WSL2 setup itself, INCLUDING requesting Administrator via UAC
rem only if/when it's actually needed -- deliberately NOT done here
rem up front, so a corporate machine where IT already installed WSL2
rem needs no elevation prompt at all. If this account can't get the
rem one-time Administrator access WSL2 enablement needs (typical on a
rem locked-down corporate machine), the script prints a banner telling
rem you to raise an IT ticket instead of failing with a raw error. See
rem release/windows/README.md, including its UNTESTED-on-real-hardware
rem caveat.

cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup-camsyringe-wsl.ps1"

echo.
echo Press any key to close this window...
pause >nul
