@echo off
setlocal enabledelayedexpansion
REM ============================================================
REM Point the machine's active OpenXR runtime at the VISUAL STUDIO build so you
REM can F5-debug the runtime in XRT.sln. Run from an ELEVATED prompt. Per-config.
REM
REM It uses the dev manifest the VS build ALREADY generates
REM (build_vs2022\<cfg>\openxr_displayxr-dev.json, which points at the build-tree
REM DisplayXRClient.dll) and registers the VS-built sim-display. Nothing is
REM hand-written, so there's nothing to mis-paste.
REM
REM Usage:
REM   scripts\setup-vs-runtime.bat            :: Debug (default)
REM   scripts\setup-vs-runtime.bat Release
REM   scripts\setup-vs-runtime.bat --restore  :: put the previous active runtime back
REM
REM Prereq: scripts\build_windows.bat vs2022, then open build_vs2022\XRT.sln,
REM pick the matching config, and build ALL_BUILD.
REM ============================================================

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "REPO=%%~fI"
set "CFG=%~1"
if "%CFG%"=="" set "CFG=Debug"

net session >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: run from an ELEVATED prompt ^(right-click cmd -^> Run as administrator^).
    exit /b 1
)

if /I "%CFG%"=="--restore" goto :restore

set "MANIFEST=%REPO%\build_vs2022\%CFG%\openxr_displayxr-dev.json"
if not exist "%MANIFEST%" (
    echo ERROR: %MANIFEST% not found.
    echo   1^) scripts\build_windows.bat vs2022
    echo   2^) open build_vs2022\XRT.sln, %CFG% config, build ALL_BUILD
    exit /b 1
)

REM Back up the current active runtime once, so --restore can undo this.
for /f "tokens=2,*" %%A in ('reg query "HKLM\Software\Khronos\OpenXR\1" /v ActiveRuntime 2^>nul ^| find "ActiveRuntime"') do set "PREV=%%B"
reg query "HKLM\Software\DisplayXR\DevSetup" /v PrevActiveRuntime >nul 2>&1
if %ERRORLEVEL% NEQ 0 if defined PREV reg add "HKLM\Software\DisplayXR\DevSetup" /v PrevActiveRuntime /t REG_SZ /d "!PREV!" /f >nul

reg add "HKLM\Software\Khronos\OpenXR\1" /v ActiveRuntime /t REG_SZ /d "%MANIFEST%" /f >nul
echo ActiveRuntime -^> %MANIFEST%

REM Register the VS-built sim-display (co-located under build_vs2022\bin\<cfg>).
set "SIM="
for /r "%REPO%\build_vs2022\bin\%CFG%" %%F in (DisplayXR-SimDisplay.dll) do set "SIM=%%F"
if not defined SIM for /r "%REPO%\build_vs2022" %%F in (DisplayXR-SimDisplay.dll) do set "SIM=%%F"
if defined SIM (
    reg add "HKLM\Software\DisplayXR\DisplayProcessors\sim-display" /v Binary      /t REG_SZ    /d "!SIM!"                        /f >nul
    reg add "HKLM\Software\DisplayXR\DisplayProcessors\sim-display" /v ProbeOrder  /t REG_DWORD /d 200                            /f >nul
    reg add "HKLM\Software\DisplayXR\DisplayProcessors\sim-display" /v DisplayName /t REG_SZ    /d "Simulated 3D Display (VS %CFG%)" /f >nul
    echo sim-display -^> !SIM!
) else (
    echo WARN: DisplayXR-SimDisplay.dll not found under build_vs2022 — build ALL_BUILD first.
)

echo.
echo Active OpenXR runtime is now the VS %CFG% build.
echo   - F5 displayxr-service in XRT.sln to debug the runtime, or
echo   - launch any OpenXR app (NON-elevated) and it loads this runtime.
echo Undo: scripts\setup-vs-runtime.bat --restore
echo.
"%REPO%\build_vs2022\bin\%CFG%\displayxr-cli.exe" dp list 2>nul
exit /b 0

:restore
for /f "tokens=2,*" %%A in ('reg query "HKLM\Software\DisplayXR\DevSetup" /v PrevActiveRuntime 2^>nul ^| find "PrevActiveRuntime"') do set "PREV=%%B"
if defined PREV (
    reg add "HKLM\Software\Khronos\OpenXR\1" /v ActiveRuntime /t REG_SZ /d "!PREV!" /f >nul
    reg delete "HKLM\Software\DisplayXR\DevSetup" /v PrevActiveRuntime /f >nul 2>&1
    echo Restored ActiveRuntime -^> !PREV!
) else (
    echo No backed-up ActiveRuntime to restore.
)
exit /b 0
