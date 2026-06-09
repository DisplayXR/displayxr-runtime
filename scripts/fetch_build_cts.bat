@echo off
setlocal enabledelayedexpansion

:: ============================================================
:: DisplayXR — fetch + build the Khronos OpenXR-CTS (Windows/x64)
::
:: Builds conformance_cli + conformance_test out-of-tree under
:: build-cts\ (gitignored). Mirrors the dep/env setup in
:: build_windows.bat. NOT wired into build_windows.bat — the CTS is
:: a developer/CI harness, not a runtime artifact.
::
:: Usage: scripts\fetch_build_cts.bat
::   Pin via CTS_TAG below; matches the runtime's OpenXR loader (1.1.43).
:: Output: build-cts\build\...\conformance_cli.exe (path echoed at end).
:: ============================================================

set REPO=%~dp0..\
set CTS_TAG=openxr-cts-1.1.43.0
set CTS_ROOT=%REPO%build-cts
set CTS_SRC=%CTS_ROOT%\OpenXR-CTS
set CTS_BUILD=%CTS_ROOT%\build
set VULKAN_SDK=C:\VulkanSDK\1.4.341.1
set NINJA_DIR=%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe

:: ------------------------------------------------------------
:: 1. MSVC environment (same as build_windows.bat)
:: ------------------------------------------------------------
echo === Setting up MSVC environment ===
:: Skip vcvars if MSVC is already on PATH (e.g. CI ran msvc-dev-cmd).
where cl.exe >nul 2>&1
if %ERRORLEVEL% EQU 0 goto :msvc_ready
:: Find any VS 2022 edition (Community/Professional/Enterprise/BuildTools) via vswhere.
set "_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%_VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
where cl.exe >nul 2>&1
if %ERRORLEVEL% EQU 0 goto :msvc_ready
:: Last-resort hardcoded Community path.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
where cl.exe >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Could not find Visual Studio 2022 C++ tools ^(tried PATH, vswhere, Community^).
    exit /b 1
)
:msvc_ready
if exist "%NINJA_DIR%\ninja.exe" set "PATH=%NINJA_DIR%;%PATH%"
if exist "%VULKAN_SDK%\Bin" set "PATH=%VULKAN_SDK%\Bin;%PATH%"

where ninja >nul 2>&1 || ( echo ERROR: ninja not found. winget install Ninja-build.Ninja & exit /b 1 )
where cmake >nul 2>&1 || ( echo ERROR: cmake not found. & exit /b 1 )
where python >nul 2>&1 || ( echo ERROR: python not found ^(CTS generates sources via Python^). & exit /b 1 )

:: ------------------------------------------------------------
:: 2. Fetch OpenXR-CTS at the pinned tag (out-of-tree)
:: ------------------------------------------------------------
if not exist "%CTS_ROOT%" mkdir "%CTS_ROOT%"

if not exist "%CTS_SRC%\.git" (
    echo === Cloning OpenXR-CTS @ %CTS_TAG% ===
    git clone --depth 1 --branch %CTS_TAG% https://github.com/KhronosGroup/OpenXR-CTS.git "%CTS_SRC%"
    if %ERRORLEVEL% NEQ 0 ( echo CTS clone FAILED & exit /b 1 )
) else (
    echo === OpenXR-CTS present; ensuring tag %CTS_TAG% ===
    git -C "%CTS_SRC%" fetch --depth 1 origin tag %CTS_TAG% >nul 2>&1
    git -C "%CTS_SRC%" checkout -q %CTS_TAG%
    if %ERRORLEVEL% NEQ 0 ( echo CTS checkout FAILED & exit /b 1 )
)

:: ------------------------------------------------------------
:: 3. Configure (Ninja Multi-Config). The CTS vendors its own deps
::    (Catch2, jsoncpp, tinygltf, ...) in src/external — no vcpkg,
::    no submodules. The OpenXR loader links statically (default);
::    conformance_cli finds the runtime via the Khronos loader's
::    normal discovery (HKLM ActiveRuntime on Windows).
:: ------------------------------------------------------------
echo === CMake configure (CTS) ===
cmake -S "%CTS_SRC%" -B "%CTS_BUILD%" -G "Ninja Multi-Config"
if %ERRORLEVEL% NEQ 0 ( echo CTS CMake configure FAILED & exit /b 1 )

:: ------------------------------------------------------------
:: 4. Build only the conformance harness (skips hello_xr, spec, etc.)
:: ------------------------------------------------------------
echo === Building conformance_cli + conformance_test (RelWithDebInfo) ===
cmake --build "%CTS_BUILD%" --config RelWithDebInfo --target conformance_cli conformance_test
if %ERRORLEVEL% NEQ 0 ( echo CTS build FAILED & exit /b 1 )

:: ------------------------------------------------------------
:: 5. Report the conformance_cli location
:: ------------------------------------------------------------
echo.
echo === CTS build complete ===
for /r "%CTS_BUILD%" %%F in (conformance_cli.exe) do echo   conformance_cli: %%F
for /r "%CTS_BUILD%" %%F in (conformance_test.dll) do echo   conformance_test: %%F
echo.
echo === DONE ===
endlocal
