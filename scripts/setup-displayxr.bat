@echo off
REM DisplayXR dev orchestrator (#283) - Windows.
REM
REM One command takes a fresh contributor from `git clone` to a working
REM DisplayXR dev box: downloads each component's release installer from
REM the canonical GitHub Releases page (tags pinned in versions.json),
REM runs each NSIS installer silently (/S), verifies registry markers.
REM
REM Mirror of scripts/setup-displayxr.sh (#283 macOS). Same flag surface,
REM same default-install semantics, same warn-and-skip pattern for
REM components whose pinned release has no Windows asset attached.
REM
REM Usage:
REM   scripts\setup-displayxr.bat                      :: runtime + shell + leia
REM   scripts\setup-displayxr.bat --with mcp           :: also DisplayXR MCP Tools
REM   scripts\setup-displayxr.bat --with-demos         :: also install each demo's prebuilt release
REM   scripts\setup-displayxr.bat --with-demo-sources  :: also clone each demo's source into demos\
REM   scripts\setup-displayxr.bat --dry-run            :: print plan, install nothing
REM   scripts\setup-displayxr.bat --uninstall          :: uninstall every DisplayXR-published component
REM   scripts\setup-displayxr.bat --help
REM
REM Must be run from an ELEVATED command prompt (Right-click cmd.exe ->
REM Run as administrator). The NSIS installers all write to HKLM and
REM Program Files; non-elevated runs fail with a clearer error than
REM individual installers report.

setlocal EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
set "REPO_ROOT=%SCRIPT_DIR%.."
set "VERSIONS_JSON=%REPO_ROOT%\versions.json"

REM --- Component table (mirrors scripts\lib\components.sh; keep in sync). ---
set "COMPONENT_REPO_runtime=DisplayXR/displayxr-runtime"
set "COMPONENT_EXE_runtime=DisplayXRSetup-*.exe"
set "COMPONENT_MARKER_runtime=HKLM\Software\DisplayXR\Runtime"

set "COMPONENT_REPO_shell=DisplayXR/displayxr-shell-releases"
set "COMPONENT_EXE_shell=DisplayXRShellSetup-*.exe"
set "COMPONENT_MARKER_shell=HKLM\Software\DisplayXR\WorkspaceControllers\shell"

set "COMPONENT_REPO_leia_plugin=DisplayXR/displayxr-leia-plugin"
set "COMPONENT_EXE_leia_plugin=DisplayXRLeiaSRSetup-*.exe"
set "COMPONENT_MARKER_leia_plugin=HKLM\Software\DisplayXR\DisplayProcessors\leia-sr"

set "COMPONENT_REPO_mcp_tools=DisplayXR/displayxr-mcp"
set "COMPONENT_EXE_mcp_tools=DisplayXRMCPSetup-*.exe"
set "COMPONENT_MARKER_mcp_tools=HKLM\Software\DisplayXR\Capabilities\MCP"

set "COMPONENT_REPO_gauss_demo=DisplayXR/displayxr-demo-gaussiansplat"
set "COMPONENT_EXE_gauss_demo=DisplayXRGaussianSplatSetup-*.exe"
set "COMPONENT_MARKER_gauss_demo=HKLM\Software\DisplayXR\Demos\GaussianSplat"
REM Pin key defaults to the component name (top-level "gauss_demo" in
REM versions.json) — no COMPONENT_PINKEY override needed for the flat schema.

REM Demo components installed by --with-demos (prebuilt release assets only).
REM Mirrors DEMO_COMPONENTS in scripts\lib\components.sh; keep in sync.
set "DEMO_COMPONENTS=gauss_demo modelviewer_demo mediaplayer_demo"

set "COMPONENT_REPO_modelviewer_demo=DisplayXR/displayxr-demo-modelviewer"
set "COMPONENT_EXE_modelviewer_demo=DisplayXRModelViewerSetup-*.exe"
set "COMPONENT_MARKER_modelviewer_demo=HKLM\Software\DisplayXR\Demos\ModelViewer"

set "COMPONENT_REPO_mediaplayer_demo=DisplayXR/displayxr-demo-mediaplayer"
set "COMPONENT_EXE_mediaplayer_demo=DisplayXRMediaPlayerSetup-*.exe"
set "COMPONENT_MARKER_mediaplayer_demo=HKLM\Software\DisplayXR\Demos\MediaPlayer"

REM --- Default flag state ---
set "WITH_MCP=0"
set "WITH_DEMOS=0"
set "WITH_DEMO_SOURCES=0"
set "DRY_RUN=0"
set "ACTION=install"
set "EXITCODE=0"

REM --- Arg parse ---
:argloop
if "%~1"=="" goto :argdone
if /i "%~1"=="-h"           goto :show_help
if /i "%~1"=="--help"       goto :show_help
if /i "%~1"=="--dry-run"    set "DRY_RUN=1" & shift & goto :argloop
if /i "%~1"=="--uninstall"  set "ACTION=uninstall" & shift & goto :argloop
if /i "%~1"=="--with-demos" set "WITH_DEMOS=1" & shift & goto :argloop
if /i "%~1"=="--with-demo-sources" set "WITH_DEMO_SOURCES=1" & shift & goto :argloop
if /i "%~1"=="--with" goto :arg_with
echo ERROR: Unknown flag: %~1 1>&2
call :usage 1>&2
exit /b 2

:arg_with
shift
if "%~1"=="" echo ERROR: --with requires an argument (currently: mcp) 1>&2 & exit /b 2
if /i "%~1"=="mcp" set "WITH_MCP=1" & shift & goto :argloop
echo ERROR: Unknown --with target: %~1 (supported: mcp) 1>&2
exit /b 2

:argdone

REM --- Preflight ---
where gh >nul 2>&1
if errorlevel 1 (
    set "PATH=C:\Program Files\GitHub CLI;%PATH%"
    where gh >nul 2>&1
    if !errorlevel! NEQ 0 (
        echo ERROR: GitHub CLI ^(gh^) not found. 1>&2
        echo Install with: winget install --id GitHub.cli 1>&2
        exit /b 1
    )
)

gh auth status >nul 2>&1
if errorlevel 1 (
    echo ERROR: GitHub CLI is not authenticated. 1>&2
    echo Run: gh auth login 1>&2
    exit /b 1
)

REM Elevation is required for both install (NSIS installers write HKLM +
REM Program Files) and uninstall (the QuietUninstallString cmds remove
REM the same). Skip the check only on --dry-run, which does no
REM privileged work.
if "%DRY_RUN%"=="0" (
    net session >nul 2>&1
    if !errorlevel! NEQ 0 (
        echo ERROR: This script must be run from an elevated command prompt. 1>&2
        echo        Right-click cmd.exe and choose "Run as administrator", 1>&2
        echo        then re-run from this directory. 1>&2
        exit /b 1
    )
)

if not exist "%VERSIONS_JSON%" (
    echo ERROR: versions.json not found at %VERSIONS_JSON% 1>&2
    exit /b 1
)

REM --- Uninstall path ---
if "%ACTION%"=="uninstall" goto :do_uninstall

REM --- Read pins from versions.json ---
call :read_pin runtime RUNTIME_TAG
call :read_pin shell SHELL_TAG
call :read_pin leia_plugin LEIA_TAG
call :read_pin mcp_tools MCP_TAG

REM --- Staging dir ---
set "STAGING=%TEMP%\dxr-setup-%RANDOM%%RANDOM%"
if "%DRY_RUN%"=="0" mkdir "%STAGING%" 2>nul

REM --- Per-component install loop (runtime, shell, leia always; mcp opt-in) ---
call :install_component runtime "%RUNTIME_TAG%"
call :install_component shell "%SHELL_TAG%"
call :install_component leia_plugin "%LEIA_TAG%"
if "%WITH_MCP%"=="1" call :install_component mcp_tools "%MCP_TAG%"

REM --- --with-demos: install each demo's prebuilt release asset ---
REM After the core components so demo installers that require the runtime
REM (a hard prereq for some) find it already registered.
if "%WITH_DEMOS%"=="1" call :install_demos

REM --- --with-demo-sources: clone demo source trees (for building) ---
if "%WITH_DEMO_SOURCES%"=="1" call :clone_demos

REM --- Link release skills into %USERPROFILE%\.claude\skills ---
REM The /dxr-release + /installer-release skills live git-tracked in this
REM repo's .claude\skills\; symlink them into the user-level skills dir so
REM they're invocable from any working directory. The orchestrator already
REM runs elevated (NSIS installers need admin), so mklink /D succeeds.
if "%DRY_RUN%"=="0" (call :link_skills) else (echo   ^(dry-run^) would link release skills via mklink /D)

REM --- Smoke verification ---
if "%DRY_RUN%"=="0" if "%EXITCODE%"=="0" (
    echo.
    echo ==^> Smoke verification
    reg query "HKLM\Software\DisplayXR\Runtime" /v InstallPath >nul 2>&1
    if !errorlevel! NEQ 0 (
        echo WARN: HKLM\Software\DisplayXR\Runtime\InstallPath missing. 1>&2
        echo       ^(Only meaningful if the runtime install actually ran.^) 1>&2
    ) else (
        echo  OK  Runtime registered.
        reg query "HKLM\Software\Khronos\OpenXR\1" /v ActiveRuntime >nul 2>&1
        if !errorlevel! NEQ 0 (
            echo WARN: HKLM\Software\Khronos\OpenXR\1\ActiveRuntime missing. 1>&2
        ) else (
            echo  OK  Active OpenXR runtime registered.
            echo.
            echo  OK  DisplayXR dev box is ready.
        )
    )
)

REM --- Cleanup ---
if "%DRY_RUN%"=="0" if exist "%STAGING%" rmdir /s /q "%STAGING%"

exit /b %EXITCODE%

REM ============================================================================
REM Subroutines
REM ============================================================================

:show_help
echo DisplayXR dev orchestrator ^(#283, Windows^)
echo.
echo Usage: scripts\setup-displayxr.bat [flags]
echo.
echo   --with mcp        Also install DisplayXR MCP Tools.
echo   --with-demos      Also install each demo's prebuilt release installer
echo                     ^(no build needed; demos with no Windows asset skip^).
echo   --with-demo-sources
echo                     Also clone each DisplayXR/displayxr-demo-* repo into
echo                     .\demos\^<name^>\ for building from source ^(does not
echo                     build them^). Independent of --with-demos.
echo   --dry-run         Print everything that would be downloaded and
echo                     installed; perform no privileged operations.
echo   --uninstall       Silent-uninstall every DisplayXR-published component
echo                     ^(scans HKLM\...\Uninstall\* for Publisher=DisplayXR^).
echo   -h, --help        Show this message.
echo.
echo Pins live in versions.json. Bump that file to upgrade the dev box.
echo.
echo Must run from an ELEVATED command prompt. The NSIS installers all
echo write to HKLM and Program Files.
exit /b 0

:usage
echo Usage: scripts\setup-displayxr.bat [--with mcp] [--with-demos] [--with-demo-sources] [--dry-run] [--uninstall] [-h^|--help]
exit /b 0

REM Read a pinned tag from versions.json into the named env var.
REM   call :read_pin <key> <var-name>
REM Nested keys use dot notation, e.g. "demos.gaussiansplat".
:read_pin
set "_RP_KEY=%~1"
set "_RP_VAR=%~2"
for /f "usebackq delims=" %%i in (`powershell -NoProfile -Command "$d = ConvertFrom-Json (Get-Content -Raw '%VERSIONS_JSON%'); $v=$d; foreach ($p in '%_RP_KEY%'.Split('.')) { $v = $v.$p }; Write-Output $v"`) do set "%_RP_VAR%=%%i"
if not defined %_RP_VAR% (
    echo ERROR: versions.json has no pin for '%_RP_KEY%' 1>&2
    set "EXITCODE=1"
)
exit /b 0

REM Delegate to the standalone linker (single source of truth for the
REM mklink logic; also runnable on its own to relink skills without a
REM full component reinstall).
:link_skills
echo.
echo ==^> Linking DisplayXR release skills into %%USERPROFILE%%\.claude\skills
call "%SCRIPT_DIR%link-dxr-skills.bat"
exit /b 0

REM Install one component.
REM   call :install_component <name> <tag>
:install_component
set "_IC_NAME=%~1"
set "_IC_TAG=%~2"
call set "_IC_REPO=%%COMPONENT_REPO_%_IC_NAME%%%"
call set "_IC_GLOB=%%COMPONENT_EXE_%_IC_NAME%%%"
call set "_IC_MARKER=%%COMPONENT_MARKER_%_IC_NAME%%%"

if "%_IC_TAG%"=="" (
    echo WARN: skipping %_IC_NAME% — no pin in versions.json 1>&2
    exit /b 0
)

echo.
echo ==^> %_IC_NAME% @ %_IC_TAG%  ^(repo: %_IC_REPO%^)

if "%_IC_GLOB%"=="" (
    echo WARN: %_IC_NAME%: no Windows asset is published for this component today. 1>&2
    echo       Skipping. 1>&2
    exit /b 0
)

REM Validate the pin exists in the source repo's releases.
gh release view "%_IC_TAG%" --repo "%_IC_REPO%" >nul 2>&1
if errorlevel 1 (
    echo ERROR: versions.json pins %_IC_NAME% to '%_IC_TAG%', but 1>&2
    echo   gh release view %_IC_TAG% --repo %_IC_REPO% 1>&2
    echo failed. Bump the pin in versions.json, or verify the repo is accessible. 1>&2
    set "EXITCODE=1"
    exit /b 1
)

REM Confirm the Windows asset is attached.
set "_IC_FOUND="
for /f "usebackq delims=" %%i in (`gh release view "%_IC_TAG%" --repo "%_IC_REPO%" --json assets --jq ".assets[].name"`) do (
    set "_IC_LINE=%%i"
    if /i "!_IC_LINE:~-4!"==".exe" set "_IC_FOUND=1"
)
if not defined _IC_FOUND (
    echo WARN: %_IC_NAME% @ %_IC_TAG%: no Windows .exe asset attached to this release. 1>&2
    echo       Pin a newer tag in versions.json once one is available. 1>&2
    exit /b 0
)

REM Download into staging.
set "_IC_SUBDIR=%STAGING%\%_IC_NAME%"
if "%DRY_RUN%"=="0" (
    mkdir "%_IC_SUBDIR%" 2>nul
    gh release download "%_IC_TAG%" --repo "%_IC_REPO%" --pattern "%_IC_GLOB%" --dir "%_IC_SUBDIR%"
    if !errorlevel! NEQ 0 (
        echo ERROR: %_IC_NAME%: gh release download failed 1>&2
        set "EXITCODE=1"
        exit /b 1
    )
)

REM Find the .exe (NSIS installer's filename includes the build number).
set "_IC_EXE="
if "%DRY_RUN%"=="0" (
    for %%f in ("%_IC_SUBDIR%\*.exe") do set "_IC_EXE=%%f"
    if not defined _IC_EXE (
        echo ERROR: %_IC_NAME%: download succeeded but no .exe landed in %_IC_SUBDIR% 1>&2
        set "EXITCODE=1"
        exit /b 1
    )
) else (
    set "_IC_EXE=%_IC_SUBDIR%\%_IC_GLOB%"
)

if "%DRY_RUN%"=="1" (
    echo   ^(dry-run^) would run: "%_IC_EXE%" /S
    exit /b 0
)

echo ==^> Installing %_IC_EXE%
start /wait "" "%_IC_EXE%" /S
if errorlevel 1 (
    echo ERROR: %_IC_NAME%: installer returned non-zero exit code 1>&2
    set "EXITCODE=1"
    exit /b 1
)

REM Post-install marker check.
if not "%_IC_MARKER%"=="" (
    reg query "%_IC_MARKER%" >nul 2>&1
    if !errorlevel! NEQ 0 (
        echo ERROR: %_IC_NAME%: post-install registry marker missing: %_IC_MARKER% 1>&2
        echo       Installer ran but did not register itself. 1>&2
        set "EXITCODE=1"
        exit /b 1
    )
)

echo  OK  %_IC_NAME% installed.
exit /b 0

REM Install each demo in %DEMO_COMPONENTS% from its prebuilt release asset.
REM Resolves each demo's versions.json pin key (COMPONENT_PINKEY_<name>,
REM default <name>), reads the pin, and reuses :install_component — same
REM download + silent-install + marker-check path as the core components.
:install_demos
for %%d in (%DEMO_COMPONENTS%) do (
    set "_ID_PINKEY=!COMPONENT_PINKEY_%%d!"
    if not defined _ID_PINKEY set "_ID_PINKEY=%%d"
    call :read_pin "!_ID_PINKEY!" _ID_TAG
    call :install_component %%d "!_ID_TAG!"
)
exit /b 0

REM Discover and clone all DisplayXR/displayxr-demo-* repos into demos\<name>\.
:clone_demos
echo.
echo ==^> Discovering DisplayXR demo repos...
set "DEMOS_DIR=%REPO_ROOT%\demos"
if "%DRY_RUN%"=="0" mkdir "%DEMOS_DIR%" 2>nul
for /f "usebackq delims=" %%i in (`gh repo list DisplayXR --limit 50 --json name --jq ".[].name | select(startswith(\"displayxr-demo-\"))"`) do (
    set "_CD_NAME=%%i"
    set "_CD_TARGET=%DEMOS_DIR%\%%i"
    if exist "!_CD_TARGET!\.git" (
        echo   %%i already cloned at !_CD_TARGET! — skipping.
    ) else (
        if "%DRY_RUN%"=="1" (
            echo   ^(dry-run^) would run: gh repo clone DisplayXR/%%i !_CD_TARGET!
        ) else (
            echo ==^> Cloning DisplayXR/%%i
            gh repo clone "DisplayXR/%%i" "!_CD_TARGET!"
            echo     See !_CD_TARGET!\README.md for build instructions.
        )
    )
)
exit /b 0

REM Uninstall every DisplayXR-published component via the Apps & Features
REM registry. Uses Publisher=DisplayXR as the discovery key (set by each
REM component's NSIS installer); QuietUninstallString runs silently with /S.
:do_uninstall
echo ==^> Discovering installed DisplayXR components...
REM Match by DisplayName prefix as well — the Leia SR plug-in's installer
REM sets Publisher='Leia Inc.', not 'DisplayXR', but every DisplayXR-shipped
REM installer uses 'DisplayXR <component>' as the DisplayName.
if "%DRY_RUN%"=="1" (
    powershell -NoProfile -Command "Get-ItemProperty 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*','HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*' -ErrorAction SilentlyContinue | Where-Object { $_.Publisher -eq 'DisplayXR' -or $_.DisplayName -like 'DisplayXR *' } | ForEach-Object { Write-Host ('  (dry-run) would uninstall: ' + $_.DisplayName + ' (' + $_.DisplayVersion + ')') }"
    exit /b 0
)
powershell -NoProfile -Command "$comps = Get-ItemProperty 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*','HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*' -ErrorAction SilentlyContinue | Where-Object { $_.Publisher -eq 'DisplayXR' -or $_.DisplayName -like 'DisplayXR *' }; if (-not $comps) { Write-Host 'WARN: No DisplayXR-published components are installed.'; exit 0 }; foreach ($c in $comps) { Write-Host ('==> Uninstalling ' + $c.DisplayName + ' (' + $c.DisplayVersion + ')'); $u = if ($c.QuietUninstallString) { $c.QuietUninstallString } else { $c.UninstallString + ' /S' }; cmd /c $u; if ($LASTEXITCODE -ne 0) { Write-Host ('WARN: uninstaller for ' + $c.DisplayName + ' exited ' + $LASTEXITCODE) } else { Write-Host (' OK  ' + $c.DisplayName + ' uninstalled.') } }"
if exist "%REPO_ROOT%\demos" (
    echo WARN: %REPO_ROOT%\demos\ is present. Leaving it in place — remove 1>&2
    echo       manually if intended ^(it may contain local changes^). 1>&2
)
exit /b 0
