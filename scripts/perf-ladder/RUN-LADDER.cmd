@echo off
REM DXR Perf Ladder - double-click entry point (issue #1113).
REM Runs non-elevated by design. Results land in results\ next to this script
REM as ladder-results-<host>-<date>.zip - send that file back.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_ladder.ps1" %*
echo.
echo Ladder finished. If a results zip was named above, please send it back.
pause
