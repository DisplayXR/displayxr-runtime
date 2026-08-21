@echo off
REM Fallback-matrix arm. Usage: fallback.bat <tag> [ENV=VAL ...]
REM Starts the DEV service with whatever env the caller set, logs its Stage-A
REM verdict, and exits it after ~12 s. Zero clients => never touches the panel.
call "%~dp0_env.bat"
echo [t918svc] arm=%1 DXR_WEAVE_ON_SCANOUT=%DXR_WEAVE_ON_SCANOUT% DXR_LEGACY_STANDALONE=%DXR_LEGACY_STANDALONE% DXR_D3D_FORCE_GPU=%DXR_D3D_FORCE_GPU% DXR_TEST_SPLIT_FAIL_STAGEA=%DXR_TEST_SPLIT_FAIL_STAGEA%
start "" /B "%DXR_PKG%\bin\displayxr-service.exe"
timeout /t 12 /nobreak >nul
taskkill /IM displayxr-service.exe /F >nul 2>&1
