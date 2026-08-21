@echo off
REM Stop the INSTALLED service, then run the DEV service in the foreground with
REM the output-device split on and the weave-latency CSV armed.
REM Usage: dev-service.bat [off]      ("off" = split off, for the A arm)
call "%~dp0_env.bat"
taskkill /IM displayxr-service.exe /F >nul 2>&1
timeout /t 2 /nobreak >nul
if /I "%~1"=="off" (set "DXR_WEAVE_ON_SCANOUT=0") else (set "DXR_WEAVE_ON_SCANOUT=1")
set "DXR_WEAVE_LATENCY_CSV=%TEMP%\dxr918_R_%1.csv"
echo [t918svc] DXR_WEAVE_ON_SCANOUT=%DXR_WEAVE_ON_SCANOUT%
echo [t918svc] DXR_WEAVE_LATENCY_CSV=%DXR_WEAVE_LATENCY_CSV%
"%DXR_PKG%\bin\displayxr-service.exe"
