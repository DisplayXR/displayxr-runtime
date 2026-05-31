@echo off
REM Symlink the DisplayXR release skills into %USERPROFILE%\.claude\skills
REM so /dxr-release + /installer-release are invocable from any working
REM directory (Claude Code reads skills from the launch repo + the
REM user-level skills dir). Windows sibling of scripts/link-dxr-skills.sh.
REM
REM The canonical, git-tracked copies live in this repo's .claude\skills\;
REM the symlinks make them globally reachable on this machine without
REM copying (no drift). Edit the canonical copy in this repo, never the
REM symlink target.
REM
REM mklink /D needs an elevated terminal OR Windows Developer Mode.
REM Idempotent — safe to re-run. setup-displayxr.bat calls this; you can
REM also run it standalone to (re)link skills without reinstalling
REM components.
REM
REM Usage:
REM   scripts\link-dxr-skills.bat            :: create/refresh the symlinks
REM   scripts\link-dxr-skills.bat --check    :: report state, change nothing
REM   scripts\link-dxr-skills.bat --unlink   :: remove the symlinks

setlocal enabledelayedexpansion

set "MODE=%~1"
if "%MODE%"=="" set "MODE=link"

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "REPO_ROOT=%%~fI"
set "SRC=%REPO_ROOT%\.claude\skills"
set "USKILLS=%USERPROFILE%\.claude\skills"

REM Skills to expose globally. The runtime's own /release is NOT listed —
REM it's only meaningful inside this repo, so it stays repo-scoped.
set "SKILLS=dxr-release installer-release"

if not exist "%USKILLS%" mkdir "%USKILLS%"

for %%S in (%SKILLS%) do (
    set "_SRC=%SRC%\%%S"
    set "_DST=%USKILLS%\%%S"

    if not exist "!_SRC!" (
        echo ERR  %%S: canonical source missing at !_SRC! - skipping. 1>&2
    ) else if /i "%MODE%"=="--check" (
        if exist "!_DST!" (
            REM dir /a:l lists reparse points; check if it's a link
            dir /a:l "%USKILLS%" 2>nul | findstr /i /c:"%%S" >nul && (echo  OK  %%S linked) || (echo WARN: %%S exists but is not a symlink ^(real copy?^))
        ) else (
            echo WARN: %%S not linked.
        )
    ) else if /i "%MODE%"=="--unlink" (
        if exist "!_DST!" (
            rmdir "!_DST!" >nul 2>&1 && (echo  OK  unlinked %%S) || (echo WARN: %%S is a real dir, not a symlink - NOT removing. 1>&2)
        ) else (
            echo  OK  %%S already absent
        )
    ) else (
        REM link mode
        if exist "!_DST!" rmdir "!_DST!" >nul 2>&1
        if exist "!_DST!" (
            echo ERR  %%S: !_DST! is a real file/dir ^(legacy manual copy^). Remove it, then re-run. 1>&2
        ) else (
            mklink /D "!_DST!" "!_SRC!" >nul 2>&1 && (echo  OK  linked %%S -^> !_SRC!) || (echo ERR  could not link %%S ^(needs admin or Developer Mode^) 1>&2)
        )
    )
)

if /i "%MODE%"=="link" (
    echo.
    echo Done. /dxr-release and /installer-release are now invocable from any directory.
    echo Canonical copies stay git-tracked in %SRC% - edit there, never the symlink target.
)

endlocal
exit /b 0
