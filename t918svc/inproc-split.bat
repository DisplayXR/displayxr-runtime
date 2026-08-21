@echo off
REM #918: the IN-PROCESS split (Phase 1/2a, already on main) — NO XRT_FORCE_MODE,
REM so the app drives comp_d3d11_compositor directly, not the service.
call "%~dp0_env.bat"
set "XR_RUNTIME_JSON=%DXR_JSON%"
set "XRT_FORCE_MODE="
set "DXR_WEAVE_ON_SCANOUT=1"
set "DXR_WEAVE_PROBE=1"
"%DXR_APPS%\cube_handle_d3d11_win.exe"
