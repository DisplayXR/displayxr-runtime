@echo off
REM Copyright 2026, DisplayXR / Leia Inc.
REM SPDX-License-Identifier: BSL-1.0
REM
REM Slice 2 (Phase B) — list_windows against a running service.
REM Requires: DISPLAYXR_MCP=1 displayxr-shell.exe <app1> <app2> already running.

setlocal EnableDelayedExpansion
set "ROOT=%~dp0..\.."
pushd "%ROOT%" >nul

set "ADAPTER="
for %%P in (
    "%ROOT%\_package\DisplayXR\bin\displayxr-mcp.exe"
    "%ROOT%\_package\bin\displayxr-mcp.exe"
    "%ROOT%\build\src\xrt\targets\mcp_adapter\displayxr-mcp.exe"
) do if exist %%P set "ADAPTER=%%~P"

if "!ADAPTER!"=="" (
    echo FAIL: displayxr-mcp.exe not found 1>&2
    popd >nul & exit /b 1
)

if "%EXPECTED_WINDOWS%"=="" set EXPECTED_WINDOWS=2

echo === Slice 2 list_windows ===
echo     adapter:  !ADAPTER!
echo     expected: ^>= %EXPECTED_WINDOWS% windows

"!ADAPTER!" --target service <nul >nul 2>&1
if errorlevel 1 (
    echo FAIL: displayxr-service MCP endpoint not reachable 1>&2
    echo       start with DISPLAYXR_MCP=1 and ^>= %EXPECTED_WINDOWS% app^(s^) 1>&2
    popd >nul & exit /b 1
)

python tests\mcp\_list_windows_helper.py "!ADAPTER!" %EXPECTED_WINDOWS%
set RC=%errorlevel%
popd >nul
exit /b %RC%
