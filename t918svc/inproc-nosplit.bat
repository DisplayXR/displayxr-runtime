@echo off
REM #918 control: the IN-PROCESS arm with the split OFF (weave on the render adapter).
call "%~dp0_env.bat"
set "XR_RUNTIME_JSON=%DXR_JSON%"
set "XRT_FORCE_MODE="
set "DXR_WEAVE_ON_SCANOUT=0"
set "DXR_WEAVE_PROBE=1"
"%DXR_APPS%\cube_handle_d3d11_win.exe"
