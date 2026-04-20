@echo off
REM Copyright 2026, DisplayXR / Leia Inc.
REM SPDX-License-Identifier: BSL-1.0
REM
REM Slice 1 (Phase B) — service-mode adapter handshake, cmd.exe edition.
REM Mirrors tests\mcp\test_service_handshake.sh for Windows users without
REM Git Bash / WSL. Exits 0 on pass, non-zero on failure.

setlocal EnableDelayedExpansion

set "ROOT=%~dp0..\.."
pushd "%ROOT%" >nul

set "ADAPTER="
for %%P in (
    "%ROOT%\_package\DisplayXR\bin\displayxr-mcp.exe"
    "%ROOT%\_package\bin\displayxr-mcp.exe"
    "%ROOT%\build\src\xrt\targets\mcp_adapter\displayxr-mcp.exe"
    "%ROOT%\build\src\xrt\targets\mcp_adapter\Debug\displayxr-mcp.exe"
) do (
    if exist %%P set "ADAPTER=%%~P"
)

if "!ADAPTER!"=="" (
    echo FAIL: displayxr-mcp.exe not found; build first 1>&2
    popd >nul & exit /b 1
)

echo === Slice 1 service-handshake ===
echo     adapter: !ADAPTER!

REM 1. --list works.
"!ADAPTER!" --list >nul
if errorlevel 1 (
    echo FAIL: --list returned non-zero 1>&2
    popd >nul & exit /b 1
)
echo   PASS  --list

REM 2. --target service must fail when no service is running.
"!ADAPTER!" --target service <nul >nul 2>&1
if not errorlevel 1 (
    echo FAIL: --target service succeeded with no service 1>&2
    popd >nul & exit /b 1
)
echo   PASS  --target service fails with no service

REM 3. --target auto fallback.
"!ADAPTER!" --target auto <nul >_mcp_out.txt 2>&1
type _mcp_out.txt | findstr /C:"no running MCP sessions" >nul
if errorlevel 1 (
    echo FAIL: --target auto fallback did not report expected error 1>&2
    type _mcp_out.txt
    del _mcp_out.txt
    popd >nul & exit /b 1
)
del _mcp_out.txt
echo   PASS  --target auto fallback

REM 4. Service handshake (requires displayxr-service + DISPLAYXR_MCP=1).
set "SERVICE="
for %%P in (
    "%ROOT%\_package\DisplayXR\bin\displayxr-service.exe"
    "%ROOT%\_package\bin\displayxr-service.exe"
) do (
    if exist %%P set "SERVICE=%%~P"
)

if "!SERVICE!"=="" (
    echo   SKIP  service handshake ^(displayxr-service.exe not found^)
    echo test_service_handshake: OK
    popd >nul & exit /b 0
)

echo     service: !SERVICE!
set DISPLAYXR_MCP=1
start "" /B "!SERVICE!" --shell
REM Let the service bind its MCP endpoint.
timeout /t 3 /nobreak >nul

python tests\mcp\_service_handshake_helper.py "!ADAPTER!"
set RC=%errorlevel%

REM Shut the service down. `taskkill` by image name is the cleanest
REM path since we don't track its PID here.
taskkill /IM displayxr-service.exe /F >nul 2>&1

if not "%RC%"=="0" (
    echo FAIL: initialize handshake 1>&2
    popd >nul & exit /b 1
)

echo   PASS  initialize against service
echo test_service_handshake: OK
popd >nul
exit /b 0
