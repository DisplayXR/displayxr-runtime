@echo off
setlocal enabledelayedexpansion

:: ============================================================
:: DisplayXR Local Build Script
:: Downloads all dependencies on first run, then builds.
:: Usage: scripts\build_windows.bat [generate|build|installer|test-apps|vs2022|all]
::   generate   - CMake generate only (Ninja Multi-Config)
::   build      - Build runtime + install
::   installer  - Build runtime installer
::   test-apps  - Build all test apps
::   vs2022     - Generate Visual Studio 2022 solution (build_vs2022\XRT.sln)
::   all        - Everything (default)
::
:: The DisplayXR Shell ships from a separate repo. Installer download:
::   https://github.com/DisplayXR/displayxr-shell-releases
:: ============================================================

set REPO=%~dp0..\
set OPENXR_VERSION=1.1.43
set VULKAN_SDK=C:\VulkanSDK\1.4.341.1
set OPENXR_SDK=%REPO%openxr_sdk
set NINJA_DIR=%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe

:: Parse argument (default: all)
set TARGET=%~1
if "%TARGET%"=="" set TARGET=all

:: ============================================================
:: 1. Setup MSVC environment
:: ============================================================
echo === Setting up MSVC environment ===
:: Locate the VS install via vswhere (the official MS way) so any edition
:: works - Community / Professional / Enterprise / BuildTools - instead of
:: hardcoding one. vswhere always lives in 32-bit Program Files.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_INSTALL="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%i"
)

set "VCVARS="
if defined VS_INSTALL if exist "%VS_INSTALL%\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%VS_INSTALL%\VC\Auxiliary\Build\vcvars64.bat"

:: Fallback: probe the known editions if vswhere is unavailable (older installs).
if not defined VCVARS (
    for %%E in (Community Professional Enterprise BuildTools) do (
        if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
    )
)

if not defined VCVARS (
    echo ERROR: Could not find Visual Studio 2022 with the C++ workload.
    echo Install VS 2022 ^(any edition^) with "Desktop development with C++".
    exit /b 1
)

call "%VCVARS%" >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: vcvars64.bat failed: %VCVARS%
    exit /b 1
)

:: Add ninja and Vulkan SDK tools to PATH
if exist "%NINJA_DIR%\ninja.exe" (
    set "PATH=%NINJA_DIR%;%PATH%"
)
if exist "%VULKAN_SDK%\Bin" (
    set "PATH=%VULKAN_SDK%\Bin;%PATH%"
)

:: ============================================================
:: 2. Check / download dependencies (one-time)
:: ============================================================

:: --- Ninja ---
where ninja >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: ninja not found. Install via: winget install Ninja-build.Ninja
    exit /b 1
)

:: --- Vulkan SDK ---
if not exist "%VULKAN_SDK%\Include\vulkan\vulkan.h" (
    echo ERROR: Vulkan SDK not found at %VULKAN_SDK%
    echo Install via: winget install KhronosGroup.VulkanSDK
    exit /b 1
)

:: --- GitHub CLI (needed for some workflows; not for the build itself) ---
where gh >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    set "PATH=C:\Program Files\GitHub CLI;%PATH%"
)

:: NOTE: Leia SR support is no longer built in-tree (ADR-019 / issues
:: #256 & #263). To run on Leia hardware, install the runtime then the
:: Leia plug-in installer from DisplayXR/displayxr-leia-plugin releases.

:: --- vcpkg ---
if not exist "%REPO%vcpkg\vcpkg.exe" (
    echo === Setting up vcpkg ===
    if not exist "%REPO%vcpkg\.git" (
        git clone https://github.com/microsoft/vcpkg.git "%REPO%vcpkg"
    )
    cd /d "%REPO%vcpkg"
    git checkout 5d90b0d5d0317336e65662f2bf0d671b0902c632 >nul 2>&1
    call bootstrap-vcpkg.bat
    cd /d "%REPO%"
)

:: --- OpenXR loader (for test apps) ---
if not exist "%OPENXR_SDK%\x64\lib\openxr_loader.lib" (
    echo === Downloading OpenXR loader %OPENXR_VERSION% ===
    powershell -Command "Invoke-WebRequest -Uri 'https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-%OPENXR_VERSION%/openxr_loader_windows-%OPENXR_VERSION%.zip' -OutFile '%REPO%openxr_loader.zip'"
    powershell -Command "Expand-Archive -Path '%REPO%openxr_loader.zip' -DestinationPath '%OPENXR_SDK%' -Force"
    del "%REPO%openxr_loader.zip" 2>nul
    echo OpenXR loader ready.
)

:: --- OpenXR loader short-path copy (avoids spaces-in-path linker issues) ---
:: The in-tree webxr_bridge target and the standalone test apps both link
:: against the OpenXR loader via a short path with no spaces. Versioned so
:: bumping OPENXR_VERSION doesn't silently reuse an older cached loader.
set OPENXR_SDK_SHORT=C:\dev\openxr_sdk_%OPENXR_VERSION%
if not exist "%OPENXR_SDK_SHORT%\x64\lib\openxr_loader.lib" (
    xcopy /E /I /Y "%OPENXR_SDK%" "%OPENXR_SDK_SHORT%" >nul
)

echo.
echo === Dependencies ready ===
echo   VULKAN_SDK=%VULKAN_SDK%
echo   OPENXR_SDK=%OPENXR_SDK%
echo   OPENXR_SDK_SHORT=%OPENXR_SDK_SHORT%
echo   vcpkg=%REPO%vcpkg
echo.

:: ============================================================
:: 3. CMake Generate
:: ============================================================
if "%TARGET%"=="build" if exist "%REPO%build\build.ninja" goto :do_build
if "%TARGET%"=="installer" if exist "%REPO%build\build.ninja" goto :do_installer
if "%TARGET%"=="test-apps" goto :do_test_apps
if "%TARGET%"=="vs2022" goto :do_vs2022

echo === CMake Generate ===
cmake -S "%REPO%." -B "%REPO%build" -G "Ninja Multi-Config" ^
  -DCMAKE_TOOLCHAIN_FILE="%REPO%vcpkg\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_MANIFEST_FEATURES=gui ^
  -DX_VCPKG_APPLOCAL_DEPS_INSTALL=ON ^
  -DCMAKE_INSTALL_PREFIX="%REPO%_package" ^
  -DXRT_BUILD_INSTALLER=ON ^
  -DXRT_FEATURE_SERVICE=ON ^
  -DXRT_FEATURE_HYBRID_MODE=ON ^
  -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"

if %ERRORLEVEL% NEQ 0 (
    echo CMake generate FAILED
    exit /b 1
)

if "%TARGET%"=="generate" goto :done

:: ============================================================
:: 4. Build runtime
:: ============================================================
:do_build
echo.
echo === Building runtime (Release) ===
cmake --build "%REPO%build" --config Release --target install

if %ERRORLEVEL% NEQ 0 (
    echo Build FAILED
    exit /b 1
)

if "%TARGET%"=="build" goto :done

:: ============================================================
:: 5. Build installer
:: ============================================================
:do_installer
echo.
echo === Building installer ===
cmake --build "%REPO%build" --config Release --target installer

if %ERRORLEVEL% NEQ 0 (
    echo Installer build FAILED - continuing with test apps...
)

if "%TARGET%"=="installer" goto :done

:: ============================================================
:: 6. Build test apps
:: ============================================================
:do_test_apps
echo.
echo === Building test apps ===
set TESTAPP_FAILED=

:: Copy OpenXR SDK to a short path to avoid spaces-in-path linker issues
set OPENXR_SDK_SHORT=C:\dev\openxr_sdk
if not exist "%OPENXR_SDK_SHORT%\x64\lib\openxr_loader.lib" (
    xcopy /E /I /Y "%OPENXR_SDK%" "%OPENXR_SDK_SHORT%" >nul
)

:: Use the known x64 openxr_loader.dll path directly
set LOADER_DLL=%OPENXR_SDK%\x64\bin\openxr_loader.dll

:: cube_handle_d3d11_win
if exist "%REPO%test_apps\cube_handle_d3d11_win\CMakeLists.txt" (
    echo --- cube_handle_d3d11_win ---
    cmake -S "%REPO%\test_apps\cube_handle_d3d11_win" -B "%REPO%\test_apps\cube_handle_d3d11_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\cube_handle_d3d11_win\build" || set TESTAPP_FAILED=1
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\cube_handle_d3d11_win\build\" >nul
)

:: cube_zones_d3d11_win (XR_EXT_display_zones exerciser)
if exist "%REPO%test_apps\cube_zones_d3d11_win\CMakeLists.txt" (
    echo --- cube_zones_d3d11_win ---
    cmake -S "%REPO%\test_apps\cube_zones_d3d11_win" -B "%REPO%\test_apps\cube_zones_d3d11_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\cube_zones_d3d11_win\build" || set TESTAPP_FAILED=1
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\cube_zones_d3d11_win\build\" >nul
)

:: cube_zones_texture_d3d11_win (texture-class XR_EXT_display_zones parity test)
if exist "%REPO%test_apps\cube_zones_texture_d3d11_win\CMakeLists.txt" (
    echo --- cube_zones_texture_d3d11_win ---
    cmake -S "%REPO%\test_apps\cube_zones_texture_d3d11_win" -B "%REPO%\test_apps\cube_zones_texture_d3d11_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\cube_zones_texture_d3d11_win\build" || set TESTAPP_FAILED=1
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\cube_zones_texture_d3d11_win\build\" >nul
)

:: weave_rpc_probe_d3d11_win (XR_EXT_weave probe, #625)
if exist "%REPO%test_apps\weave_rpc_probe_d3d11_win\CMakeLists.txt" (
    echo --- weave_rpc_probe_d3d11_win ---
    cmake -S "%REPO%\test_apps\weave_rpc_probe_d3d11_win" -B "%REPO%\test_apps\weave_rpc_probe_d3d11_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\weave_rpc_probe_d3d11_win\build" || set TESTAPP_FAILED=1
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\weave_rpc_probe_d3d11_win\build\" >nul
)

:: cube_hosted_d3d11_win
if exist "%REPO%test_apps\cube_hosted_d3d11_win\CMakeLists.txt" (
    echo --- cube_hosted_d3d11_win ---
    cmake -S "%REPO%\test_apps\cube_hosted_d3d11_win" -B "%REPO%\test_apps\cube_hosted_d3d11_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\cube_hosted_d3d11_win\build" || set TESTAPP_FAILED=1
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\cube_hosted_d3d11_win\build\" >nul
)

:: cube_handle_d3d12_win
if exist "%REPO%test_apps\cube_handle_d3d12_win\CMakeLists.txt" (
    echo --- cube_handle_d3d12_win ---
    cmake -S "%REPO%\test_apps\cube_handle_d3d12_win" -B "%REPO%\test_apps\cube_handle_d3d12_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\cube_handle_d3d12_win\build" || set TESTAPP_FAILED=1
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\cube_handle_d3d12_win\build\" >nul
)

:: cube_handle_gl_win
if exist "%REPO%test_apps\cube_handle_gl_win\CMakeLists.txt" (
    echo --- cube_handle_gl_win ---
    cmake -S "%REPO%\test_apps\cube_handle_gl_win" -B "%REPO%\test_apps\cube_handle_gl_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\cube_handle_gl_win\build" || set TESTAPP_FAILED=1
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\cube_handle_gl_win\build\" >nul
)

:: cube_zones_gl_win (#613 — native-GL handle-class zones parity app)
if exist "%REPO%test_apps\cube_zones_gl_win\CMakeLists.txt" (
    echo --- cube_zones_gl_win ---
    cmake -S "%REPO%\test_apps\cube_zones_gl_win" -B "%REPO%\test_apps\cube_zones_gl_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\cube_zones_gl_win\build" || set TESTAPP_FAILED=1
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\cube_zones_gl_win\build\" >nul
)

:: cube_texture_d3d11_win
if exist "%REPO%test_apps\cube_texture_d3d11_win\CMakeLists.txt" (
    echo --- cube_texture_d3d11_win ---
    cmake -S "%REPO%\test_apps\cube_texture_d3d11_win" -B "%REPO%\test_apps\cube_texture_d3d11_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\cube_texture_d3d11_win\build" || set TESTAPP_FAILED=1
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\cube_texture_d3d11_win\build\" >nul
)

:: cube_texture_d3d12_win
if exist "%REPO%test_apps\cube_texture_d3d12_win\CMakeLists.txt" (
    echo --- cube_texture_d3d12_win ---
    cmake -S "%REPO%\test_apps\cube_texture_d3d12_win" -B "%REPO%\test_apps\cube_texture_d3d12_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\cube_texture_d3d12_win\build" || set TESTAPP_FAILED=1
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\cube_texture_d3d12_win\build\" >nul
)

:: cube_zones_texture_d3d12_win (#613 — native-D3D12 zones texture parity app)
if exist "%REPO%test_apps\cube_zones_texture_d3d12_win\CMakeLists.txt" (
    echo --- cube_zones_texture_d3d12_win ---
    cmake -S "%REPO%\test_apps\cube_zones_texture_d3d12_win" -B "%REPO%\test_apps\cube_zones_texture_d3d12_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\cube_zones_texture_d3d12_win\build" || set TESTAPP_FAILED=1
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\cube_zones_texture_d3d12_win\build\" >nul
)

:: cube_zones_texture_gl_win (#613 — GL/D3D11 hybrid zones texture parity app)
if exist "%REPO%test_apps\cube_zones_texture_gl_win\CMakeLists.txt" (
    echo --- cube_zones_texture_gl_win ---
    cmake -S "%REPO%\test_apps\cube_zones_texture_gl_win" -B "%REPO%\test_apps\cube_zones_texture_gl_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\cube_zones_texture_gl_win\build" || set TESTAPP_FAILED=1
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\cube_zones_texture_gl_win\build\" >nul
)

:: workspace_minimal_d3d11_win (XR_EXT_spatial_workspace smoke test)
if exist "%REPO%test_apps\workspace_minimal_d3d11_win\CMakeLists.txt" (
    echo --- workspace_minimal_d3d11_win ---
    cmake -S "%REPO%\test_apps\workspace_minimal_d3d11_win" -B "%REPO%\test_apps\workspace_minimal_d3d11_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\workspace_minimal_d3d11_win\build" || set TESTAPP_FAILED=1
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\workspace_minimal_d3d11_win\build\" >nul
)

:: windowspace_handle_d3d11_win (#389)
if exist "%REPO%test_apps\windowspace_handle_d3d11_win\CMakeLists.txt" (
    echo --- windowspace_handle_d3d11_win ---
    cmake -S "%REPO%\test_apps\windowspace_handle_d3d11_win" -B "%REPO%\test_apps\windowspace_handle_d3d11_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\windowspace_handle_d3d11_win\build" || set TESTAPP_FAILED=1
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\windowspace_handle_d3d11_win\build\" >nul
)

:: windowspace_handle_d3d12_win (#389)
if exist "%REPO%test_apps\windowspace_handle_d3d12_win\CMakeLists.txt" (
    echo --- windowspace_handle_d3d12_win ---
    cmake -S "%REPO%\test_apps\windowspace_handle_d3d12_win" -B "%REPO%\test_apps\windowspace_handle_d3d12_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\windowspace_handle_d3d12_win\build" || set TESTAPP_FAILED=1
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\windowspace_handle_d3d12_win\build\" >nul
)

:: windowspace_handle_gl_win (#389)
if exist "%REPO%test_apps\windowspace_handle_gl_win\CMakeLists.txt" (
    echo --- windowspace_handle_gl_win ---
    cmake -S "%REPO%\test_apps\windowspace_handle_gl_win" -B "%REPO%\test_apps\windowspace_handle_gl_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\windowspace_handle_gl_win\build" || set TESTAPP_FAILED=1
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\windowspace_handle_gl_win\build\" >nul
)

:: cube_handle_vk_win (needs Vulkan SDK)
if exist "%REPO%test_apps\cube_handle_vk_win\CMakeLists.txt" (
    echo --- cube_handle_vk_win ---
    cmake -S "%REPO%\test_apps\cube_handle_vk_win" -B "%REPO%\test_apps\cube_handle_vk_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\cube_handle_vk_win\build" || set TESTAPP_FAILED=1
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\cube_handle_vk_win\build\" >nul
)

:: cube_zones_texture_vk_win (#613 Phase 1 — VK/D3D11 hybrid zones texture parity app; needs Vulkan SDK)
if exist "%REPO%test_apps\cube_zones_texture_vk_win\CMakeLists.txt" (
    echo --- cube_zones_texture_vk_win ---
    cmake -S "%REPO%\test_apps\cube_zones_texture_vk_win" -B "%REPO%\test_apps\cube_zones_texture_vk_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\cube_zones_texture_vk_win\build" || set TESTAPP_FAILED=1
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\cube_zones_texture_vk_win\build\" >nul
)

:: windowspace_handle_vk_win (#389, needs Vulkan SDK)
if exist "%REPO%test_apps\windowspace_handle_vk_win\CMakeLists.txt" (
    echo --- windowspace_handle_vk_win ---
    cmake -S "%REPO%\test_apps\windowspace_handle_vk_win" -B "%REPO%\test_apps\windowspace_handle_vk_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\windowspace_handle_vk_win\build" || set TESTAPP_FAILED=1
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\windowspace_handle_vk_win\build\" >nul
)

:: Any per-app configure/build failure above set TESTAPP_FAILED — fail the
:: script instead of silently exiting 0 (the silent form hid real breakage).
if defined TESTAPP_FAILED (
    echo.
    echo *** One or more test apps FAILED to build — see the per-app sections above ***
    exit /b 1
)


:: ============================================================
:: 7. Generate run scripts (like macOS run_*.sh pattern)
:: ============================================================
echo.
echo === Generating run scripts ===

set "RT_JSON=%REPO%build\Release\openxr_displayxr-dev.json"
set "PKG=%REPO%_package"

:: Run scripts for standalone test apps (each sets XR_RUNTIME_JSON to dev build).
:: Non-Vulkan apps also disable Vulkan implicit layers to prevent crashes from
:: buggy third-party layers (e.g., FPS Monitor). See issue #105.
:: All scripts prepend _package\bin to PATH so the runtime DLL's delay-loaded
:: SR SDK deps (SimulatedRealityVulkanBeta.dll, etc.) resolve without requiring
:: the installer to have run — important for third-party devs cloning the repo
:: and iterating against the dev-build runtime via XR_RUNTIME_JSON.
for %%A in (cube_handle_d3d11_win cube_zones_d3d11_win cube_zones_texture_d3d11_win cube_hosted_d3d11_win cube_handle_d3d12_win cube_handle_gl_win cube_texture_d3d11_win cube_texture_d3d12_win workspace_minimal_d3d11_win windowspace_handle_d3d11_win windowspace_handle_d3d12_win windowspace_handle_gl_win) do (
    if exist "%REPO%test_apps\%%A\build\%%A.exe" (
        > "%PKG%\run_%%A.bat" (
            echo @echo off
            echo set "XR_RUNTIME_JSON=%RT_JSON%"
            echo set "VK_LOADER_LAYERS_DISABLE=*"
            echo set "PATH=%PKG%\bin;%%PATH%%"
            echo "%REPO%test_apps\%%A\build\%%A.exe" %%*
        )
    )
)
:: Vulkan app — don't disable implicit layers (app needs them for its own VkInstance).
for %%A in (cube_handle_vk_win windowspace_handle_vk_win) do (
    if exist "%REPO%test_apps\%%A\build\%%A.exe" (
        > "%PKG%\run_%%A.bat" (
            echo @echo off
            echo set "XR_RUNTIME_JSON=%RT_JSON%"
            echo set "PATH=%PKG%\bin;%%PATH%%"
            echo "%REPO%test_apps\%%A\build\%%A.exe" %%*
        )
    )
)
:: Vulkan zones app — implicit layers enabled (app owns its VkInstance) AND the
:: zones dev gate set so the runtime advertises XR_EXT_display_zones (#613).
for %%A in (cube_zones_texture_vk_win) do (
    if exist "%REPO%test_apps\%%A\build\%%A.exe" (
        > "%PKG%\run_%%A.bat" (
            echo @echo off
            echo set "XR_RUNTIME_JSON=%RT_JSON%"
            echo set "DISPLAYXR_ZONES=1"
            echo set "PATH=%PKG%\bin;%%PATH%%"
            echo "%REPO%test_apps\%%A\build\%%A.exe" %%*
        )
    )
)
:: Non-Vulkan zones app — disable VK implicit layers (no app VkInstance) AND set
:: the zones dev gate so the runtime advertises XR_EXT_display_zones (#613). NOT
:: in the generic loop above, which omits the zones gate.
for %%A in (cube_zones_texture_d3d12_win cube_zones_gl_win cube_zones_texture_gl_win) do (
    if exist "%REPO%test_apps\%%A\build\%%A.exe" (
        > "%PKG%\run_%%A.bat" (
            echo @echo off
            echo set "XR_RUNTIME_JSON=%RT_JSON%"
            echo set "DISPLAYXR_ZONES=1"
            echo set "VK_LOADER_LAYERS_DISABLE=*"
            echo set "PATH=%PKG%\bin;%%PATH%%"
            echo "%REPO%test_apps\%%A\build\%%A.exe" %%*
        )
    )
)


:: Run script for the WebXR Bridge v2 host (in-tree target, installed into _package)
if exist "%PKG%\bin\displayxr-webxr-bridge.exe" (
    > "%PKG%\run_webxr_bridge.bat" (
        echo @echo off
        echo set "XR_RUNTIME_JSON=%RT_JSON%"
        echo "%PKG%\bin\displayxr-webxr-bridge.exe" %%*
    )
)

:: Shell mode: service + app (two-terminal workflow)
> "%PKG%\run_shell_service.bat" (
    echo @echo off
    echo echo Starting displayxr-service in shell mode...
    echo "%PKG%\bin\displayxr-service.exe" --shell
)
> "%PKG%\run_shell_app.bat" (
    echo @echo off
    echo set "XR_RUNTIME_JSON=%RT_JSON%"
    echo set "DISPLAYXR_WORKSPACE_SESSION=1"
    echo echo XR_RUNTIME_JSON=%%XR_RUNTIME_JSON%%
    echo echo DISPLAYXR_WORKSPACE_SESSION=%%DISPLAYXR_WORKSPACE_SESSION%%
    echo if "%%~1"=="" (
    echo     "%REPO%test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe"
    echo ^) else (
    echo     "%%~1"
    echo ^)
)

:: Shell mode: two apps (three-terminal or auto workflow)
> "%PKG%\run_shell_2apps.bat" (
    echo @echo off
    echo set "XR_RUNTIME_JSON=%RT_JSON%"
    echo set "DISPLAYXR_WORKSPACE_SESSION=1"
    echo echo Starting two shell apps...
    echo start "" cmd /c "set XR_RUNTIME_JSON=%RT_JSON%^&^& set DISPLAYXR_WORKSPACE_SESSION=1^&^& "%REPO%test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe""
    echo timeout /t 3 /nobreak ^>nul
    echo start "" cmd /c "set XR_RUNTIME_JSON=%RT_JSON%^&^& set DISPLAYXR_WORKSPACE_SESSION=1^&^& "%REPO%test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe""
    echo echo Both apps launched. Press any key to exit.
    echo pause ^>nul
)

echo Run scripts generated in %PKG%\

echo.
echo === Test apps done ===
goto :done

:do_vs2022
echo.
echo === Generating Visual Studio 2022 solution ===
cmake -S "%REPO%." -B "%REPO%build_vs2022" -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE="%REPO%vcpkg\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_MANIFEST_FEATURES=gui ^
  -DCMAKE_INSTALL_PREFIX="%REPO%_package" ^
  -DXRT_BUILD_INSTALLER=ON ^
  -DXRT_FEATURE_SERVICE=ON ^
  -DXRT_FEATURE_HYBRID_MODE=ON ^
  -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"

if %ERRORLEVEL% NEQ 0 (
    echo VS2022 CMake generate FAILED
    exit /b 1
)

echo.
echo === Visual Studio 2022 solution ready ===
echo   Solution: %REPO%build_vs2022\XRT.sln
echo   Build the INSTALL target, then set displayxr-service (or displayxr-cli)
echo   as the startup project to debug the runtime + plug-in + SR SDK.
echo   To debug a test app: build it with `build_windows.bat test-apps` and
echo   launch _package\run_cube_handle_d3d11_win.bat (sets XR_RUNTIME_JSON +
echo   has openxr_loader.dll beside the exe); attach VS, or add a debug profile
echo   pointing at that exe with the same env.
echo   (Folding a test app in as a one-click F5 target: WIP, see issue/PR notes
echo   — set -DXRT_VS_LAUNCH_APP=cube_handle_d3d11_win once that lands.)
goto :done

:done
echo.
echo === ALL DONE ===
endlocal
