@echo off
setlocal EnableExtensions DisableDelayedExpansion
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
REM   scripts\register_dev_plugin.bat                  :: register dev sim-display (fallback, no hardware)
REM   scripts\register_dev_plugin.bat leia <dll>       :: register a dev-built Leia plug-in (ProbeOrder 50)
REM   scripts\register_dev_plugin.bat unregister-sim   :: remove the dev sim-display entry
REM   scripts\register_dev_plugin.bat input <dll>      :: register an INPUT provider (ADR-034), ProbeOrder 50
REM   scripts\register_dev_plugin.bat input sim        :: register the dev SIMULATED input provider
REM                                                       (ProbeOrder 200). Synthetic controllers, NOT a
REM                                                       product path -- it also needs DXR_SIM_INPUT=1 in
REM                                                       the environment at run time before it will probe.
REM   scripts\register_dev_plugin.bat input gamepad    :: register the dev GAMEPAD provider (XInput), ProbeOrder 150
REM   scripts\register_dev_plugin.bat unregister-input :: remove the dev sim-input + gamepad entries
REM   scripts\register_dev_plugin.bat conv <dll> [--probe-order N]
REM                                                    :: register a CONVERSION provider (ADR-053),
REM                                                       id = DLL basename, ProbeOrder 50 by default
REM   scripts\register_dev_plugin.bat conv sim [--probe-order N]
REM                                                    :: register the dev sim-conversion provider
REM                                                       (ProbeOrder 250, FALLBACK). The runtime
REM                                                       installer never registers it on user installs.
REM   scripts\register_dev_plugin.bat unregister-conv [id] :: remove a conversion provider row
REM                                                       (default: sim-conversion)
REM   scripts\register_dev_plugin.bat list             :: show registered DPs, input + conversion providers
REM
REM Quoting / path safety (DLL paths with spaces, parentheses, ampersands):
REM   * The arguments are captured ONCE, with delayed expansion OFF, into
REM     REPO / MODE / ARG2..ARG4; everything after that reads them as !VAR!.
REM     A delayed-expanded value is substituted AFTER cmd has parsed the line,
REM     so a space, ")" or "&" in a path can no longer split an if-operand or
REM     close a ( ... ) block early. (Before: "i7 was unexpected at this time"
REM     for a DLL under C:\Users\<name with a space>\..., and any ")" -- e.g.
REM     "Program Files (x86)" -- broke the unquoted `echo %DLL%` lines that sit
REM     inside if-blocks, even when the file existed.)
REM   * Stray quotes are stripped from the DLL argument. A caller that is not
REM     cmd (Git Bash / MSYS, PowerShell, a CreateProcess wrapper) escapes the
REM     quotes it adds as \" , so %~2 arrives as \"C:\...\x.dll\" -- an odd
REM     quote state that made every `if "%DLL%"==...` mis-tokenise at the first
REM     space. Both \" pairs and bare " are removed (a Windows path never
REM     contains one). Quote the path normally from cmd: "C:\a b\x.dll".
REM   * :allow takes its inputs in ALLOW_FILE / ALLOW_NOTE rather than CALL
REM     arguments, because CALL expands %...% in its arguments a second time.
REM   * Not supported: a "!" in a path (consumed by delayed expansion).
REM ============================================================

set "REPO=%~dp0.."
set "MODE=%~1"
set "ARG2=%~2"
set "ARG3=%~3"
set "ARG4=%~4"
setlocal EnableDelayedExpansion
if defined ARG2 set "ARG2=!ARG2:\"=!"
if defined ARG2 set "ARG2=!ARG2:"=!"
if "!MODE!"=="" set "MODE=sim"

REM --- elevation check (HKLM writes need admin) ---
net session >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    if /I not "!MODE!"=="list" (
        echo ERROR: run from an ELEVATED prompt ^(right-click cmd -^> Run as administrator^).
        exit /b 1
    )
)

if /I "!MODE!"=="list" goto :list
if /I "!MODE!"=="unregister-sim" goto :unregister_sim
if /I "!MODE!"=="leia" goto :leia
if /I "!MODE!"=="input" goto :input
if /I "!MODE!"=="unregister-input" goto :unregister_input
if /I "!MODE!"=="conv" goto :conv
if /I "!MODE!"=="unregister-conv" goto :unregister_conv
goto :sim

:sim
set "DLL=!REPO!\_package\bin\plugins\DisplayXR-SimDisplay.dll"
if not exist "!DLL!" (
    echo ERROR: !DLL! not found. Build first: scripts\build_windows.bat build
    exit /b 1
)
set "KEY=HKLM\Software\DisplayXR\DisplayProcessors\sim-display"
reg add "%KEY%" /v Binary      /t REG_SZ    /d "!DLL!"                 /f >nul || goto :regfail
reg add "%KEY%" /v ProbeOrder  /t REG_DWORD /d 200                    /f >nul || goto :regfail
reg add "%KEY%" /v DisplayName /t REG_SZ    /d "Simulated 3D Display" /f >nul || goto :regfail
echo Registered dev sim-display (ProbeOrder 200, fallback):
echo   !DLL!
set "ALLOW_FILE=!DLL!"
set "ALLOW_NOTE=sim-display"
call :allow
goto :list

:leia
set "DLL=!ARG2!"
if "!DLL!"=="" (
    echo ERROR: leia mode needs a DLL path:
    echo   scripts\register_dev_plugin.bat leia C:\path\to\DisplayXR-LeiaSR.dll
    exit /b 1
)
if not exist "!DLL!" (
    echo ERROR: not found: !DLL!
    exit /b 1
)
set "KEY=HKLM\Software\DisplayXR\DisplayProcessors\leia-sr"
reg add "%KEY%" /v Binary      /t REG_SZ    /d "!DLL!"        /f >nul || goto :regfail
reg add "%KEY%" /v ProbeOrder  /t REG_DWORD /d 50             /f >nul || goto :regfail
reg add "%KEY%" /v DisplayName /t REG_SZ    /d "Leia SR (dev)" /f >nul || goto :regfail
echo Registered dev Leia plug-in (ProbeOrder 50, wins over sim):
echo   !DLL!
set "ALLOW_FILE=!DLL!"
set "ALLOW_NOTE=leia-sr (dev)"
call :allow
echo Note: this OVERWRITES any installed leia-sr entry. Re-run the installer to restore it.
goto :list

:unregister_sim
reg delete "HKLM\Software\DisplayXR\DisplayProcessors\sim-display" /f >nul 2>&1
echo Removed sim-display dev registration.
goto :list

:input
REM Register an input-provider plug-in (ADR-034 / #823). Pass a DLL path to
REM register a vendor provider (uses the DLL basename minus extension as the
REM id, ProbeOrder 50), or the literal `sim` for the freshly-built sim-input.
REM
REM `sim` used to be the no-argument DEFAULT here. It is not any more: a
REM leftover sim-input registration is exactly what puts phantom controllers
REM (a fixed synthetic motion pattern) in front of a user on a dev box, so it
REM has to be asked for by name -- and the provider itself now also declines
REM unless DXR_SIM_INPUT=1 is set in the loading process's environment.
REM
REM `gamepad` is presence-gated: inert until a pad is attached, then it takes
REM the gamepad role, drives the rig and, by default, the hand roles.
set "DLL=!ARG2!"
if "!DLL!"=="" (
    echo ERROR: 'input' needs a target:
    echo   scripts\register_dev_plugin.bat input C:\path\to\DisplayXR-Vendor.dll   :: a real provider
    echo   scripts\register_dev_plugin.bat input sim                               :: the SIMULATED one
    echo   scripts\register_dev_plugin.bat input gamepad                           :: the XInput GAMEPAD one
    exit /b 1
)
if /I "!DLL!"=="sim" (
    set "DLL=!REPO!\_package\bin\plugins\DisplayXR-SimInput.dll"
    set "IPKEY=HKLM\Software\DisplayXR\InputProviders\sim-input"
    set "IPNAME=DisplayXR Sim Input"
    set "IPORDER=200"
    set "IPSIM=1"
) else if /I "!DLL!"=="gamepad" (
    set "DLL=!REPO!\_package\bin\plugins\DisplayXR-Gamepad.dll"
    set "IPKEY=HKLM\Software\DisplayXR\InputProviders\gamepad"
    set "IPNAME=DisplayXR Gamepad"
    set "IPORDER=150"
    set "IPSIM="
) else (
    for %%F in ("!DLL!") do set "IPID=%%~nF"
    set "IPKEY=HKLM\Software\DisplayXR\InputProviders\!IPID!"
    set "IPNAME=Input Provider (dev)"
    set "IPORDER=50"
    set "IPSIM="
)
if not exist "!DLL!" (
    echo ERROR: !DLL! not found. Build first: scripts\build_windows.bat build
    exit /b 1
)
reg add "!IPKEY!" /v Binary      /t REG_SZ    /d "!DLL!"      /f >nul || goto :regfail
reg add "!IPKEY!" /v ProbeOrder  /t REG_DWORD /d !IPORDER!    /f >nul || goto :regfail
reg add "!IPKEY!" /v DisplayName /t REG_SZ    /d "!IPNAME!"   /f >nul || goto :regfail
echo Registered input provider (ProbeOrder !IPORDER!):
echo   !DLL!
set "ALLOW_FILE=!DLL!"
set "ALLOW_NOTE=input provider"
call :allow
if defined IPSIM (
    echo.
    echo NOTE: sim-input additionally requires DXR_SIM_INPUT=1 in the environment of the
    echo       process that loads the runtime - registration alone no longer enables it.
)
goto :list

:unregister_input
reg delete "HKLM\Software\DisplayXR\InputProviders\sim-input" /f >nul 2>&1
reg delete "HKLM\Software\DisplayXR\InputProviders\gamepad" /f >nul 2>&1
echo Removed sim-input + gamepad dev registrations.
goto :list

:conv
REM Register a conversion-provider plug-in (ADR-053 / #1999) under
REM HKLM\Software\DisplayXR\ConversionProviders\<id>, with the value schema the
REM installers write (conversion-provider-discovery.md section 3.1). Pass a DLL
REM path for a vendor / dev provider (id = DLL basename minus extension,
REM ProbeOrder 50 = the vendor band), or the literal `sim` for the freshly-built
REM sim-conversion (ProbeOrder 250 = the FALLBACK band). `--probe-order N`
REM overrides either. Version is written LAST, as installers must (section 3.8):
REM it is the "row complete" marker the service re-describes on.
set "DLL=!ARG2!"
if "!DLL!"=="" (
    echo ERROR: 'conv' needs a target:
    echo   scripts\register_dev_plugin.bat conv C:\path\to\Vendor-Conversion.dll [--probe-order N]
    echo   scripts\register_dev_plugin.bat conv sim [--probe-order N]
    exit /b 1
)
if /I "!DLL!"=="sim" (
    set "DLL=!REPO!\_package\bin\plugins\DisplayXR-SimConversion.dll"
    set "CPID=sim-conversion"
    set "CPNAME=DisplayXR Sim Conversion"
    set "CPORDER=250"
) else (
    for %%F in ("!DLL!") do set "CPID=%%~nF"
    set "CPNAME=Conversion Provider (dev)"
    set "CPORDER=50"
)
if /I "!ARG3!"=="--probe-order" (
    if "!ARG4!"=="" (
        echo ERROR: --probe-order needs a number.
        exit /b 1
    )
    set "CPORDER=!ARG4!"
)
if not exist "!DLL!" (
    echo ERROR: !DLL! not found. Build first: scripts\build_windows.bat build
    exit /b 1
)
REM Absolute path with backslashes: the loader hands it to LoadLibraryExW.
for %%F in ("!DLL!") do set "DLL=%%~fF"
set "CPKEY=HKLM\Software\DisplayXR\ConversionProviders\!CPID!"
reg add "!CPKEY!" /v Binary      /t REG_SZ    /d "!DLL!"    /f >nul || goto :regfail
reg add "!CPKEY!" /v ProbeOrder  /t REG_DWORD /d !CPORDER!  /f >nul || goto :regfail
reg add "!CPKEY!" /v DisplayName /t REG_SZ    /d "!CPNAME!" /f >nul || goto :regfail
REM ADR-053 P4 (#2108): the sim's fake model pack (relative to the DLL), so its
REM DOWNLOAD_MS knob runs the runtime-owned model store's write protocol.
if /I "!CPID!"=="sim-conversion" (
    reg add "!CPKEY!" /v Packs /t REG_MULTI_SZ /d "sim-conversion.models.json" /f >nul || goto :regfail
)
reg add "!CPKEY!" /v Version     /t REG_SZ    /d "dev"      /f >nul || goto :regfail
echo Registered conversion provider '!CPID!' (ProbeOrder !CPORDER!):
echo   !DLL!
set "ALLOW_FILE=!DLL!"
set "ALLOW_NOTE=!CPID!"
call :allow
goto :list

:unregister_conv
set "CPID=!ARG2!"
if "!CPID!"=="" set "CPID=sim-conversion"
reg delete "HKLM\Software\DisplayXR\ConversionProviders\!CPID!" /f >nul 2>&1
echo Removed conversion provider registration '!CPID!'.
goto :list

:list
echo.
set "CLI=!REPO!\_package\bin\displayxr-cli.exe"
if exist "!CLI!" (
    "!CLI!" dp list
    "!CLI!" input list
) else (
    echo ^(displayxr-cli not built yet; skipping dp/input list^)
)
REM No `displayxr-cli conv list` yet (ADR-053 section 7.1 lands later): show the raw rows.
echo.
echo Conversion providers (HKLM\Software\DisplayXR\ConversionProviders):
reg query "HKLM\Software\DisplayXR\ConversionProviders" /s 2>nul || echo   (none registered)
exit /b 0

:regfail
echo ERROR: reg add failed (are you elevated?).
exit /b 1

REM --- ADR-054 N4b: the provenance gate. A dev-built plug-in is unsigned, so
REM     registering it also allowlists its SHA-256 under
REM     HKLM\Software\DisplayXR\Trust\Allowlist (value name = hex digest, data =
REM     a note). Without this, the enforcing gate (the Windows default since
REM     PR B) REFUSES the binary as "unverified". A rebuilt DLL has a new
REM     digest: re-run this script after every build you want to run under the
REM     live service (push-runtime-pf.sh does the same for what it copies).
REM     In: ALLOW_FILE (the DLL path), ALLOW_NOTE (the value's data). The path
REM     reaches the for /f command as a delayed-expanded, quoted token, so the
REM     spawned `cmd /c certutil ...` sees it as one argument (its first char is
REM     not a quote, so cmd /c keeps the quotes).
:allow
set "ALLOW_HASH="
for /f "skip=1 delims=" %%H in ('certutil -hashfile "!ALLOW_FILE!" SHA256 2^>nul') do if not defined ALLOW_HASH set "ALLOW_HASH=%%H"
if not defined ALLOW_HASH exit /b 0
set "ALLOW_HASH=!ALLOW_HASH: =!"
reg add "HKLM\Software\DisplayXR\Trust\Allowlist" /v "!ALLOW_HASH!" /t REG_SZ /d "!ALLOW_NOTE! (register_dev_plugin.bat)" /f >nul
if !ERRORLEVEL! EQU 0 echo   allowlisted sha256 !ALLOW_HASH! (ADR-054 N4b provenance gate)
exit /b 0
