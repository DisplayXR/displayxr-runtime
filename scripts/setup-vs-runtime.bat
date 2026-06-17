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
REM   scripts\setup-vs-runtime.bat                       :: Debug (default), sim-display
REM   scripts\setup-vs-runtime.bat Release
REM   scripts\setup-vs-runtime.bat RelWithDebInfo --leia :: also build+register the
REM                                                         Leia plug-in for real weaving
REM   scripts\setup-vs-runtime.bat --restore             :: put the previous active runtime back
REM
REM For --leia use RelWithDebInfo (NOT Debug): the SR SDK ships Release-only, so a
REM Debug plug-in CRT-mismatches it. RelWithDebInfo gives PDBs + Release CRT.
REM Build XRT.sln in the SAME config you pass here.
REM
REM Prereq: scripts\build_windows.bat vs2022, then open build_vs2022\XRT.sln,
REM pick the matching config, and build ALL_BUILD.
REM ============================================================

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "REPO=%%~fI"

REM --- parse args: [config] [--leia] [--restore], any order ---
set "CFG="
set "WANT_LEIA="
set "DO_RESTORE="
:parse
if "%~1"=="" goto :parsed
if /I "%~1"=="--leia"    ( set "WANT_LEIA=1" & shift & goto :parse )
if /I "%~1"=="--restore" ( set "DO_RESTORE=1" & shift & goto :parse )
if not defined CFG       ( set "CFG=%~1" & shift & goto :parse )
shift
goto :parse
:parsed
if not defined CFG set "CFG=Debug"

net session >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: run from an ELEVATED prompt ^(right-click cmd -^> Run as administrator^).
    exit /b 1
)

if defined DO_RESTORE goto :restore

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
    reg add "HKLM\Software\DisplayXR\DisplayProcessors\sim-display" /v Binary      /t REG_SZ    /d "!SIM!"                          /f >nul
    reg add "HKLM\Software\DisplayXR\DisplayProcessors\sim-display" /v ProbeOrder  /t REG_DWORD /d 200                              /f >nul
    reg add "HKLM\Software\DisplayXR\DisplayProcessors\sim-display" /v DisplayName /t REG_SZ    /d "Simulated 3D Display (VS %CFG%)" /f >nul
    echo sim-display -^> !SIM!
) else (
    echo WARN: DisplayXR-SimDisplay.dll not found under build_vs2022 — build ALL_BUILD first.
)

if defined WANT_LEIA call :build_register_leia

echo.
echo Active OpenXR runtime is now the VS %CFG% build.
echo   - F5 displayxr-service in XRT.sln to debug the runtime, or
echo   - launch any OpenXR app (NON-elevated) and it loads this runtime.
echo Undo: scripts\setup-vs-runtime.bat --restore
echo.
"%REPO%\build_vs2022\bin\%CFG%\displayxr-cli.exe" dp list 2>nul
exit /b 0


:build_register_leia
echo.
echo === Building + registering the Leia plug-in (%CFG%) for real weaving ===
if /I "%CFG%"=="Debug" (
    echo WARN: the SR SDK is Release-only — a Debug plug-in will CRT-mismatch it.
    echo       Recommended: setup-vs-runtime.bat RelWithDebInfo --leia
    echo       ^(and build XRT.sln in RelWithDebInfo too^).
)
set "LEIA_REPO=%REPO%\..\displayxr-leia-plugin"
if not exist "%LEIA_REPO%\.git" (
    echo Cloning displayxr-leia-plugin next to the runtime...
    git clone https://github.com/DisplayXR/displayxr-leia-plugin.git "%LEIA_REPO%" || ( echo WARN: clone failed — staying on sim-display. & exit /b 0 )
)
REM Build the plug-in against THIS runtime checkout, in the same config.
set "DXR_RUNTIME_SOURCE_DIR=%REPO%"
call "%LEIA_REPO%\scripts\build-windows.bat" build %CFG%
if errorlevel 1 ( echo WARN: Leia plug-in build failed — staying on sim-display. & exit /b 0 )
set "LEIA_DLL="
for /r "%LEIA_REPO%\_package" %%F in (DisplayXR-LeiaSR.dll) do set "LEIA_DLL=%%F"
if not defined LEIA_DLL for /r "%LEIA_REPO%\build" %%F in (DisplayXR-LeiaSR.dll) do set "LEIA_DLL=%%F"
if not defined LEIA_DLL ( echo WARN: built DisplayXR-LeiaSR.dll not found — staying on sim-display. & exit /b 0 )
reg add "HKLM\Software\DisplayXR\DisplayProcessors\leia-sr" /v Binary      /t REG_SZ    /d "!LEIA_DLL!"          /f >nul
reg add "HKLM\Software\DisplayXR\DisplayProcessors\leia-sr" /v ProbeOrder  /t REG_DWORD /d 50                   /f >nul
reg add "HKLM\Software\DisplayXR\DisplayProcessors\leia-sr" /v DisplayName /t REG_SZ    /d "Leia SR (VS %CFG%)" /f >nul
echo leia-sr -^> !LEIA_DLL!  (ProbeOrder 50 — wins over sim-display)
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
