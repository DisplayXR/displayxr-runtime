@echo off
REM ALWAYS run this when done: put the INSTALLED service back, non-elevated,
REM via explorer (the harness terminal is elevated and the service must not be).
call "%~dp0_env.bat"
taskkill /IM displayxr-service.exe /F >nul 2>&1
timeout /t 2 /nobreak >nul
explorer.exe "%DXR_INSTALLED%"
timeout /t 3 /nobreak >nul
tasklist /FI "IMAGENAME eq displayxr-service.exe"
