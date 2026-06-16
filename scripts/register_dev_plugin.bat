@echo off
setlocal
REM ============================================================
REM Register a display-processor plug-in for a FROM-SOURCE dev runtime.
REM
REM Windows plug-in discovery is registry-only (HKLM\Software\DisplayXR\
REM DisplayProcessors) — there is no adjacent-dir / env fallback like POSIX.
REM `build_windows.bat` produces the sim-display plug-in DLL but does NOT
REM register it, so a from-source runtime finds no display processor and
REM xrt_instance_create_system fails (XRT_ERROR_DEVICE_CREATION_FAILED /
REM "Failed to initialize OpenXR"). This registers the freshly-built,
REM ABI-matched plug-in exactly as CI and the installer do.
REM
REM Run from an ELEVATED prompt (writes HKLM).
REM
REM Usage:
REM   scripts\register_dev_plugin.bat                 :: register dev sim-display (fallback, no hardware)
REM   scripts\register_dev_plugin.bat leia <dll>      :: register a dev-built Leia plug-in (ProbeOrder 50)
REM   scripts\register_dev_plugin.bat unregister-sim  :: remove the dev sim-display entry
REM   scripts\register_dev_plugin.bat list            :: show registered DPs (displayxr-cli dp list)
REM ============================================================

set "REPO=%~dp0.."
set "MODE=%~1"
if "%MODE%"=="" set "MODE=sim"

REM --- elevation check (HKLM writes need admin) ---
net session >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    if /I not "%MODE%"=="list" (
        echo ERROR: run from an ELEVATED prompt ^(right-click cmd -^> Run as administrator^).
        exit /b 1
    )
)

if /I "%MODE%"=="list" goto :list
if /I "%MODE%"=="unregister-sim" goto :unregister_sim
if /I "%MODE%"=="leia" goto :leia
goto :sim

:sim
set "DLL=%REPO%\_package\bin\plugins\DisplayXR-SimDisplay.dll"
if not exist "%DLL%" (
    echo ERROR: %DLL% not found. Build first: scripts\build_windows.bat build
    exit /b 1
)
set "KEY=HKLM\Software\DisplayXR\DisplayProcessors\sim-display"
reg add "%KEY%" /v Binary      /t REG_SZ    /d "%DLL%"                 /f >nul || goto :regfail
reg add "%KEY%" /v ProbeOrder  /t REG_DWORD /d 200                    /f >nul || goto :regfail
reg add "%KEY%" /v DisplayName /t REG_SZ    /d "Simulated 3D Display" /f >nul || goto :regfail
echo Registered dev sim-display (ProbeOrder 200, fallback):
echo   %DLL%
goto :list

:leia
set "DLL=%~2"
if "%DLL%"=="" (
    echo ERROR: leia mode needs a DLL path:
    echo   scripts\register_dev_plugin.bat leia C:\path\to\DisplayXR-LeiaSR.dll
    exit /b 1
)
if not exist "%DLL%" (
    echo ERROR: not found: %DLL%
    exit /b 1
)
set "KEY=HKLM\Software\DisplayXR\DisplayProcessors\leia-sr"
reg add "%KEY%" /v Binary      /t REG_SZ    /d "%DLL%"        /f >nul || goto :regfail
reg add "%KEY%" /v ProbeOrder  /t REG_DWORD /d 50             /f >nul || goto :regfail
reg add "%KEY%" /v DisplayName /t REG_SZ    /d "Leia SR (dev)" /f >nul || goto :regfail
echo Registered dev Leia plug-in (ProbeOrder 50, wins over sim):
echo   %DLL%
echo Note: this OVERWRITES any installed leia-sr entry. Re-run the installer to restore it.
goto :list

:unregister_sim
reg delete "HKLM\Software\DisplayXR\DisplayProcessors\sim-display" /f >nul 2>&1
echo Removed sim-display dev registration.
goto :list

:list
echo.
set "CLI=%REPO%\_package\bin\displayxr-cli.exe"
if exist "%CLI%" (
    "%CLI%" dp list
) else (
    echo (displayxr-cli not built yet; skipping dp list)
)
exit /b 0

:regfail
echo ERROR: reg add failed (are you elevated?).
exit /b 1
