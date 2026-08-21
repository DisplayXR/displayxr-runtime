@echo off
REM T3: forced-IPC handle cube (APP_HWND presenter).
REM Optional arg: an explicit DXR_APP_HWND_LATENCY to A/B the split default.
call "%~dp0_env.bat"
set "XR_RUNTIME_JSON=%DXR_JSON%"
set "XRT_FORCE_MODE=ipc"
if not "%~1"=="" set "DXR_APP_HWND_LATENCY=%~1"
"%DXR_APPS%\cube_handle_d3d11_win.exe"
