@echo off
REM T2: hosted cube (SERVICE_WINDOW presenter) on the IPC path.
call "%~dp0_env.bat"
set "XR_RUNTIME_JSON=%DXR_JSON%"
set "XRT_FORCE_MODE=ipc"
"%DXR_APPS%\cube_hosted_d3d11_win.exe"
