@echo off
REM #918 regression diagnosis — dev service with the back-buffer COVERAGE probe.
REM Usage: cover-service.bat <on|off> <diagmode>
REM   arg1 on|off  -> DXR_WEAVE_ON_SCANOUT
REM   arg2 1|2     -> DXR_SPLIT_COVER_DIAG (1 = observe, 2 = + sentinel clear)
REM Launch this NON-ELEVATED (explorer.exe) so the client's integrity matches.
call "%~dp0_env.bat"
if /I "%~1"=="off" (set "DXR_WEAVE_ON_SCANOUT=0") else (set "DXR_WEAVE_ON_SCANOUT=1")
if "%~2"=="" (set "DXR_SPLIT_COVER_DIAG=1") else (set "DXR_SPLIT_COVER_DIAG=%~2")
set "DXR_COVER_TAG=%~1%~2"
echo [t918svc] split=%DXR_WEAVE_ON_SCANOUT% cover=%DXR_SPLIT_COVER_DIAG%
"%DXR_PKG%\bin\displayxr-service.exe" > "%TEMP%\dxr918_cover_%DXR_COVER_TAG%.txt" 2>&1
