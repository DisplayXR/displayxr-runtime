@echo off
setlocal enabledelayedexpansion
REM ============================================================
REM DisplayXR ONE-LINE DEVELOPER ENVIRONMENT SETUP (from source).
REM
REM The dev mirror of the user bundle installer: takes a fresh clone to a
REM fully working, debuggable dev environment in one command. Builds the
REM runtime FROM SOURCE, registers ABI-matched display-processor plug-ins,
REM points the machine's active OpenXR runtime at the dev build, and
REM generates the Visual Studio solution. No patching, re-runnable.
REM
REM Difference vs scripts\setup-displayxr.bat: that one DOWNLOADS released
REM installers (versions.json pins) for contributors who don't build. THIS
REM one builds your local checkout for people hacking on the runtime/plugins.
REM
REM Usage:
REM   scripts\dev-setup.bat                  :: runtime + sim-display + VS solution + active runtime
REM   scripts\dev-setup.bat --leia           :: also build+register the Leia plug-in FROM SOURCE (ABI-matched)
REM   scripts\dev-setup.bat --leia=release   :: install the latest RELEASED Leia plug-in instead (ABI risk vs a dev runtime)
REM   scripts\dev-setup.bat --all            :: every known vendor plug-in (currently == --leia source)
REM   scripts\dev-setup.bat --no-vs          :: skip the Visual Studio solution
REM   scripts\dev-setup.bat --no-active-runtime
REM   scripts\dev-setup.bat --clean          :: unregister dev plug-ins + restore the previous active runtime
REM
REM Run from an ELEVATED prompt (registers plug-ins + sets ActiveRuntime in HKLM).
REM Prereqs (same as build_windows.bat): VS 2022 + C++ workload, Ninja, Vulkan SDK,
REM GitHub CLI. --leia also needs gh auth (fetches the SR SDK) + a Leia repo.
REM ============================================================

set "SCRIPT_DIR=%~dp0"
REM Resolve the repo root to a clean absolute path (no pushd/!CD! timing deps).
for %%I in ("%SCRIPT_DIR%..") do set "REPO=%%~fI"

REM --- defaults ---
set "WANT_LEIA="
set "LEIA_MODE=source"
set "WANT_VS=1"
set "SET_ACTIVE=1"
set "DO_CLEAN="

REM --- parse flags ---
:parse
if "%~1"=="" goto :parsed
if /I "%~1"=="--leia"             set "WANT_LEIA=1"
if /I "%~1"=="--leia=source"      ( set "WANT_LEIA=1" & set "LEIA_MODE=source" )
if /I "%~1"=="--leia=release"     ( set "WANT_LEIA=1" & set "LEIA_MODE=release" )
if /I "%~1"=="--all"              ( set "WANT_LEIA=1" & set "LEIA_MODE=source" )
if /I "%~1"=="--no-vs"            set "WANT_VS="
if /I "%~1"=="--no-active-runtime" set "SET_ACTIVE="
if /I "%~1"=="--clean"            set "DO_CLEAN=1"
shift
goto :parse
:parsed

REM --- elevation check (HKLM) ---
net session >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: run from an ELEVATED prompt ^(right-click cmd -^> Run as administrator^).
    echo        Registering plug-ins and setting the active runtime both write HKLM.
    exit /b 1
)

if defined DO_CLEAN goto :clean

echo ============================================================
echo  DisplayXR dev environment setup
echo    repo:          %REPO%
echo    leia:          %WANT_LEIA% (%LEIA_MODE%)
echo    visual studio: %WANT_VS%
echo    set active:    %SET_ACTIVE%
echo ============================================================

REM --- 1. Build the runtime from source (fetches vcpkg + OpenXR loader on first run) ---
echo.
echo [1/5] Building runtime from source...
call "%SCRIPT_DIR%build_windows.bat" build
if %ERRORLEVEL% NEQ 0 ( echo ERROR: runtime build failed. & exit /b 1 )

REM --- 2. Register the freshly-built, ABI-matched sim-display (universal fallback) ---
echo.
echo [2/5] Registering dev sim-display plug-in...
call "%SCRIPT_DIR%register_dev_plugin.bat" sim
if %ERRORLEVEL% NEQ 0 ( echo ERROR: sim-display registration failed. & exit /b 1 )

REM --- 3. Vendor plug-ins (optional) ---
echo.
if not defined WANT_LEIA (
    echo [3/5] No vendor plug-in requested ^(pass --leia for Leia SR^).
) else (
    if /I "%LEIA_MODE%"=="release" (
        call :leia_release
    ) else (
        call :leia_source
    )
    if !ERRORLEVEL! NEQ 0 ( echo ERROR: Leia plug-in setup failed. & exit /b 1 )
)

REM --- 4. Point the machine's active OpenXR runtime at the dev build ---
echo.
if defined SET_ACTIVE (
    echo [4/5] Setting active OpenXR runtime to the dev build...
    call :set_active_runtime
) else (
    echo [4/5] Skipping active-runtime change ^(--no-active-runtime^); use XR_RUNTIME_JSON per run.
)

REM --- 5. Visual Studio solution ---
echo.
if defined WANT_VS (
    echo [5/5] Generating Visual Studio solution...
    call "%SCRIPT_DIR%build_windows.bat" vs2022
    if !ERRORLEVEL! NEQ 0 ( echo WARN: VS solution generation failed ^(runtime + plug-ins are still set up^). )
) else (
    echo [5/5] Skipping Visual Studio solution ^(--no-vs^).
)

echo.
echo ============================================================
echo  DONE. Registered display processors:
echo ============================================================
"%REPO%\_package\bin\displayxr-cli.exe" dp list 2>nul
echo.
echo Next:
echo   - Verify:  _package\bin\displayxr-cli.exe selftest
if defined WANT_VS echo   - Debug:   open build_vs2022\XRT.sln, build INSTALL, set displayxr-service as startup, F5
echo   - Run cube: _package\run_cube_handle_d3d11_win.bat  ^(after: build_windows.bat test-apps^)
echo   - Undo:    scripts\dev-setup.bat --clean
exit /b 0


REM ===================== subroutines =====================

:leia_source
echo [3/5] Building Leia plug-in FROM SOURCE ^(ABI-matched to this runtime^)...
set "LEIA_REPO=%REPO%\..\displayxr-leia-plugin"
if not exist "%LEIA_REPO%\.git" (
    echo   cloning displayxr-leia-plugin next to the runtime...
    git clone https://github.com/DisplayXR/displayxr-leia-plugin.git "%LEIA_REPO%" || exit /b 1
) else (
    echo   updating existing displayxr-leia-plugin checkout...
    git -C "%LEIA_REPO%" pull --ff-only 2>nul
)
REM Build against THIS runtime checkout so the plug-in matches the dev ABI.
REM The Leia build script auto-fetches the SR SDK via gh if LEIASR_SDKROOT is unset.
set "DXR_RUNTIME_SOURCE_DIR=%REPO%"
call "%LEIA_REPO%\scripts\build-windows.bat" build
if %ERRORLEVEL% NEQ 0 ( echo   ERROR: Leia plug-in build failed. & exit /b 1 )
REM Locate the built DLL and register it (ProbeOrder 50, wins over sim).
set "LEIA_DLL="
for /r "%LEIA_REPO%\_package" %%F in (DisplayXR-LeiaSR.dll) do set "LEIA_DLL=%%F"
if not defined LEIA_DLL ( echo   ERROR: built DisplayXR-LeiaSR.dll not found under %LEIA_REPO%\_package. & exit /b 1 )
call "%SCRIPT_DIR%register_dev_plugin.bat" leia "!LEIA_DLL!"
exit /b %ERRORLEVEL%

:leia_release
echo [3/5] Installing the latest RELEASED Leia plug-in...
echo   WARNING: a from-source runtime ahead of the released ABI may reject this plug-in.
where gh >nul 2>&1 || ( echo   ERROR: gh not found ^(needed to download the release^). & exit /b 1 )
set "DLDIR=%TEMP%\dxr-leia-release"
if not exist "%DLDIR%" mkdir "%DLDIR%"
gh release download -R DisplayXR/displayxr-leia-plugin -p "DisplayXRLeiaSRSetup-*.exe" -D "%DLDIR%" --clobber || ( echo   ERROR: download failed. & exit /b 1 )
for %%F in ("%DLDIR%\DisplayXRLeiaSRSetup-*.exe") do set "LEIA_EXE=%%F"
if not defined LEIA_EXE ( echo   ERROR: installer not found after download. & exit /b 1 )
echo   running %LEIA_EXE% /S ...
"%LEIA_EXE%" /S
exit /b %ERRORLEVEL%

:set_active_runtime
set "MANIFEST=%REPO%\_package\DisplayXR_win64.json"
if not exist "%MANIFEST%" ( echo   WARN: %MANIFEST% missing; skipping active-runtime change. & exit /b 0 )
REM Back up the current ActiveRuntime once, so --clean can restore it.
for /f "tokens=2,*" %%A in ('reg query "HKLM\Software\Khronos\OpenXR\1" /v ActiveRuntime 2^>nul ^| find "ActiveRuntime"') do set "PREV=%%B"
reg query "HKLM\Software\DisplayXR\DevSetup" /v PrevActiveRuntime >nul 2>&1
if %ERRORLEVEL% NEQ 0 if defined PREV (
    reg add "HKLM\Software\DisplayXR\DevSetup" /v PrevActiveRuntime /t REG_SZ /d "!PREV!" /f >nul
)
reg add "HKLM\Software\Khronos\OpenXR\1" /v ActiveRuntime /t REG_SZ /d "%MANIFEST%" /f >nul
echo   ActiveRuntime -^> %MANIFEST%
exit /b 0

:clean
echo Cleaning up dev registrations...
call "%SCRIPT_DIR%register_dev_plugin.bat" unregister-sim
reg delete "HKLM\Software\DisplayXR\DisplayProcessors\leia-sr" /f >nul 2>&1
REM Restore the previous active runtime if we backed one up.
for /f "tokens=2,*" %%A in ('reg query "HKLM\Software\DisplayXR\DevSetup" /v PrevActiveRuntime 2^>nul ^| find "PrevActiveRuntime"') do set "PREV=%%B"
if defined PREV (
    reg add "HKLM\Software\Khronos\OpenXR\1" /v ActiveRuntime /t REG_SZ /d "!PREV!" /f >nul
    reg delete "HKLM\Software\DisplayXR\DevSetup" /v PrevActiveRuntime /f >nul 2>&1
    echo   restored ActiveRuntime -^> !PREV!
) else (
    echo   no backed-up ActiveRuntime to restore ^(left as-is^).
)
echo Done.
exit /b 0
