@echo off
REM Copyright 2026, DisplayXR / Leia Inc.
REM SPDX-License-Identifier: BSL-1.0

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

"!ADAPTER!" --target service <nul >nul 2>&1
if errorlevel 1 (
    echo FAIL: service MCP endpoint not reachable 1>&2
    popd >nul & exit /b 1
)

python tests\mcp\_workspace_helper.py "!ADAPTER!"
set RC=%errorlevel%
popd >nul
exit /b %RC%
