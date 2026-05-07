@echo off
setlocal enabledelayedexpansion

:: ============================================================
:: DisplayXR Local Build Script
:: Downloads all dependencies on first run, then builds.
:: Usage: scripts\build_windows.bat [generate|build|installer|test-apps|all]
::   generate   - CMake generate only
::   build      - Build runtime + install
::   installer  - Build runtime installer
::   test-apps  - Build all test apps
::   all        - Everything (default)
::
:: The DisplayXR Shell ships from a separate repo. Installer download:
::   https://github.com/DisplayXR/displayxr-shell-releases
:: ============================================================

set REPO=%~dp0..\
set SR_TAG=1.35.0.2011
set OPENXR_VERSION=1.1.38
set LEIASR_SDKROOT=%REPO%LeiaSR-SDK-%SR_TAG%-win64
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
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Could not find Visual Studio 2022. Install VS 2022 with C++ workload.
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

:: --- GitHub CLI (needed for SR SDK download) ---
where gh >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    set "PATH=C:\Program Files\GitHub CLI;%PATH%"
)
where gh >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: GitHub CLI not found. Install via: winget install GitHub.cli
    echo Then run: gh auth login
    exit /b 1
)

:: --- Leia SR SDK ---
:: Marker file (cmake config installed by the SDK zip) — checking the bare
:: lib/ dir wasn't enough: the Vulkan-extras download below creates lib/
:: even when the main zip extract failed, which then made this idempotency
:: check skip a re-extract on the next run, leaving CMake to die on missing
:: simulatedreality_DIR / srDirectX_DIR.
set SR_SDK_MARKER=%LEIASR_SDKROOT%\lib\cmake\srDirectX\srDirectXConfig.cmake
:: %REPO% ends in '\', so "%REPO%" passes "...\.." to gh/PS where the
:: trailing \" is parsed as an escaped quote. Strip the trailing slash for
:: the download destination args.
set REPO_NOSLASH=%REPO:~0,-1%
if not exist "%SR_SDK_MARKER%" (
    echo === Downloading Leia SR SDK %SR_TAG% ===
    gh release download sr-sdk-v%SR_TAG% -R DisplayXR/displayxr-runtime -p "LeiaSR-SDK-%SR_TAG%-win64.zip" -D "%REPO_NOSLASH%"
    if %ERRORLEVEL% NEQ 0 (
        echo ERROR: Failed to download SR SDK. Run: gh auth login
        exit /b 1
    )
    powershell -Command "Expand-Archive -Path '%REPO%LeiaSR-SDK-%SR_TAG%-win64.zip' -DestinationPath '%REPO_NOSLASH%' -Force"
    if not exist "%SR_SDK_MARKER%" (
        echo ERROR: SR SDK extract did not produce %SR_SDK_MARKER%
        echo Check the Expand-Archive output above.
        exit /b 1
    )
    del "%REPO%LeiaSR-SDK-%SR_TAG%-win64.zip" 2>nul

    echo === Downloading Vulkan weaver extras ===
    gh release download sr-sdk-v%SR_TAG% -R DisplayXR/displayxr-runtime -p "SimulatedRealityVulkanBeta.lib" -D "%LEIASR_SDKROOT%\lib"
    gh release download sr-sdk-v%SR_TAG% -R DisplayXR/displayxr-runtime -p "vkweaver.h" -D "%LEIASR_SDKROOT%\include\sr\weaver"
    echo SR SDK ready.
)

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
:: against the OpenXR loader via a short path with no spaces.
set OPENXR_SDK_SHORT=C:\dev\openxr_sdk
if not exist "%OPENXR_SDK_SHORT%\x64\lib\openxr_loader.lib" (
    xcopy /E /I /Y "%OPENXR_SDK%" "%OPENXR_SDK_SHORT%" >nul
)

echo.
echo === Dependencies ready ===
echo   LEIASR_SDKROOT=%LEIASR_SDKROOT%
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

echo === CMake Generate ===
cmake -S "%REPO%." -B "%REPO%build" -G "Ninja Multi-Config" ^
  -DXRT_HAVE_LEIA_SR=ON ^
  -DCMAKE_PREFIX_PATH="%LEIASR_SDKROOT%" ^
  -DCMAKE_TOOLCHAIN_FILE="%REPO%vcpkg\scripts\buildsystems\vcpkg.cmake" ^
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
    cmake --build "%REPO%\test_apps\cube_handle_d3d11_win\build"
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\cube_handle_d3d11_win\build\" >nul
)

:: cube_hosted_d3d11_win
if exist "%REPO%test_apps\cube_hosted_d3d11_win\CMakeLists.txt" (
    echo --- cube_hosted_d3d11_win ---
    cmake -S "%REPO%\test_apps\cube_hosted_d3d11_win" -B "%REPO%\test_apps\cube_hosted_d3d11_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\cube_hosted_d3d11_win\build"
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\cube_hosted_d3d11_win\build\" >nul
)

:: cube_handle_d3d12_win
if exist "%REPO%test_apps\cube_handle_d3d12_win\CMakeLists.txt" (
    echo --- cube_handle_d3d12_win ---
    cmake -S "%REPO%\test_apps\cube_handle_d3d12_win" -B "%REPO%\test_apps\cube_handle_d3d12_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\cube_handle_d3d12_win\build"
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\cube_handle_d3d12_win\build\" >nul
)

:: cube_handle_gl_win
if exist "%REPO%test_apps\cube_handle_gl_win\CMakeLists.txt" (
    echo --- cube_handle_gl_win ---
    cmake -S "%REPO%\test_apps\cube_handle_gl_win" -B "%REPO%\test_apps\cube_handle_gl_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\cube_handle_gl_win\build"
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\cube_handle_gl_win\build\" >nul
)

:: cube_texture_d3d11_win
if exist "%REPO%test_apps\cube_texture_d3d11_win\CMakeLists.txt" (
    echo --- cube_texture_d3d11_win ---
    cmake -S "%REPO%\test_apps\cube_texture_d3d11_win" -B "%REPO%\test_apps\cube_texture_d3d11_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\cube_texture_d3d11_win\build"
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\cube_texture_d3d11_win\build\" >nul
)

:: cube_texture_d3d12_win
if exist "%REPO%test_apps\cube_texture_d3d12_win\CMakeLists.txt" (
    echo --- cube_texture_d3d12_win ---
    cmake -S "%REPO%\test_apps\cube_texture_d3d12_win" -B "%REPO%\test_apps\cube_texture_d3d12_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\cube_texture_d3d12_win\build"
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\cube_texture_d3d12_win\build\" >nul
)

:: workspace_minimal_d3d11_win (XR_EXT_spatial_workspace smoke test)
if exist "%REPO%test_apps\workspace_minimal_d3d11_win\CMakeLists.txt" (
    echo --- workspace_minimal_d3d11_win ---
    cmake -S "%REPO%\test_apps\workspace_minimal_d3d11_win" -B "%REPO%\test_apps\workspace_minimal_d3d11_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\workspace_minimal_d3d11_win\build"
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\workspace_minimal_d3d11_win\build\" >nul
)

:: cube_handle_vk_win (needs Vulkan SDK)
if exist "%REPO%test_apps\cube_handle_vk_win\CMakeLists.txt" (
    echo --- cube_handle_vk_win ---
    cmake -S "%REPO%\test_apps\cube_handle_vk_win" -B "%REPO%\test_apps\cube_handle_vk_win\build" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    cmake --build "%REPO%\test_apps\cube_handle_vk_win\build"
    if defined LOADER_DLL copy /Y "%LOADER_DLL%" "%REPO%test_apps\cube_handle_vk_win\build\" >nul
)


:: ============================================================
:: 7. Generate run scripts (like macOS run_*.sh pattern)
:: ============================================================
echo.
echo === Generating run scripts ===

set "RT_JSON=%REPO%build\Release\openxr_displayxr-dev.json"
set "PKG=%REPO%_package"

:: Run scripts for standalone test apps (each sets XR_RUNTIME_JSON to dev build)
:: Non-Vulkan apps also disable Vulkan implicit layers to prevent crashes from
:: buggy third-party layers (e.g., FPS Monitor). See issue #105.
for %%A in (cube_handle_d3d11_win cube_hosted_d3d11_win cube_handle_d3d12_win cube_handle_gl_win cube_texture_d3d11_win cube_texture_d3d12_win workspace_minimal_d3d11_win) do (
    if exist "%REPO%test_apps\%%A\build\%%A.exe" (
        > "%PKG%\run_%%A.bat" (
            echo @echo off
            echo set "XR_RUNTIME_JSON=%RT_JSON%"
            echo set "VK_LOADER_LAYERS_DISABLE=*"
            echo "%REPO%test_apps\%%A\build\%%A.exe" %%*
        )
    )
)
:: Vulkan app — don't disable implicit layers (app needs them for its own VkInstance).
:: Prepend _package/bin to PATH so SimulatedRealityVulkanBeta.dll
:: (delay-loaded by the runtime DLL when the VK weaver path fires) is
:: discoverable. The runtime installer adds its install dir to system
:: PATH for installed users; this run-script mirrors that for dev.
for %%A in (cube_handle_vk_win) do (
    if exist "%REPO%test_apps\%%A\build\%%A.exe" (
        > "%PKG%\run_%%A.bat" (
            echo @echo off
            echo set "XR_RUNTIME_JSON=%RT_JSON%"
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

:done
echo.
echo === ALL DONE ===
endlocal
