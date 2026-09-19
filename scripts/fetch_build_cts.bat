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
::   Pin via CTS_TAG below. CURRENT PIN: openxr-cts-1.1.63.0 (#1487).
::   Why 1.1.63.0:
::    - >= 1.1.60.0 is the FLOOR. CTS 1.1.58.0 added the
::      XR_EXT_view_configuration_views_change test and 1.1.60.0
::      corrected it to allow XrEventDataViewConfigurationViewsChangedEXT
::      at a 1 Hz rate; below 1.1.60.0 a correct implementation fails.
::      Adopting that extension is #1488.
::    - There is NO openxr-cts-1.1.62.0 (nor .55/.56/.59). The releases
::      in the window are 1.1.54.0 -> .57.0 -> .58.0 -> .60.0 -> .61.0
::      -> .63.0, so 1.1.63.0 is simply the newest, and it matches the
::      loader/header train this repo is moving to.
::    - 1.1.63.0's only new test is XR_KHR_extended_result_name_lengths,
::      which is extension-gated and SKIPs (we do not advertise it). It
::      also RELAXES test_xrResultToString, which now truncates expected
::      names to XR_MAX_RESULT_STRING_SIZE-1 for runtimes without that
::      extension. The rest of the 1.1.61 -> 1.1.63 delta is refactor
::      (GlobalData::invalid* -> InvalidValues::InvalidHandleValue<T>,
::      IPlatformPlugin::GetInstanceCreateInfoStruct, PLATFORM_EXPORT);
::      the Win32 platform plugin still returns nullptr, so instance
::      creation is byte-identical for us.
::   KNOWN-RED, EXCLUDED BY NAME: xrLocateSpace_xrLocateViews (added CTS
::   1.1.57.0) asserts views.size() == 2 for PRIMARY_STEREO; we advertise
::   the max-across-modes view count. run_cts.ps1 / cts.yml exclude it by
::   name (#1486, docs/reference/view-configuration-model.md) — never
::   silently. Do not "fix" that by pinning CTS backwards.
::   HISTORY (still true, do not delete): CTS 1.1.51-1.1.53 had a
::   test-side stack-buffer overflow — test_XR_KHR_extended_struct_name_-
::   lengths passed a 64-byte XR_MAX_RESULT_STRING_SIZE buffer to
::   xrStructureTypeToString2KHR (a 256-byte API), so any runtime
::   returning a >63-char struct name (we do) fail-fasted the CTS
::   process with 0xC0000409 mid-suite — truncated result XML, dead
::   nightly (#830). Fixed upstream in openxr-cts-1.1.54.0, which is
::   why the pin sat there. Separately, openxr-cts-1.1.44+ renamed the
::   CLI arg --apiVersion -> --minApiVersion (Khronos MR 3576);
::   run_cts.ps1 was updated to match (#726). The earlier "1.1.51
::   crashes the runtime mid-run" was a misdiagnosis — conformance_cli
::   rejected the stale arg and exited before running any test (no
::   result XML).
:: Output: build-cts\build\...\conformance_cli.exe (path echoed at end).
:: ============================================================

set REPO=%~dp0..\
set CTS_TAG=openxr-cts-1.1.63.0
set CTS_ROOT=%REPO%build-cts
set CTS_SRC=%CTS_ROOT%\OpenXR-CTS
set CTS_BUILD=%CTS_ROOT%\build
:: VULKAN_SDK decides whether this build gets a Vulkan CTS at all: the CTS's
:: find_package(Vulkan) is what gates XR_USE_GRAPHICS_API_VULKAN, and without it
:: `conformance_cli -G vulkan` / `-G vulkan2` cannot start -- two of the five
:: Windows arms #1523 owes. The old unconditional `set` clobbered any inherited
:: value, so a machine (or a CI runner) with a perfectly good loader+headers
:: elsewhere silently built a Vulkan-less CTS and logged only
:: "-- Could NOT find Vulkan" 40 lines deep in the configure output (#1525).
:: Honour a pre-set VULKAN_SDK; keep the LunarG default for a dev box that has
:: the SDK installed. CI points this at the vcpkg tree, so the CTS links the
:: SAME loader the runtime does -- see cts.yml and the "CI Vulkan = vendored
:: vcpkg" note. CMake's FindVulkan wants <VULKAN_SDK>\Include and \Lib, which a
:: vcpkg x64-windows tree satisfies case-insensitively (include\, lib\).
if not defined VULKAN_SDK set VULKAN_SDK=C:\VulkanSDK\1.4.341.1
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
echo     VULKAN_SDK=%VULKAN_SDK%
if exist "%VULKAN_SDK%\Include\vulkan\vulkan.h" (
    echo     Vulkan headers: found  ^-^> the vulkan / vulkan2 CTS plugins WILL be built
) else (
    echo     Vulkan headers: MISSING ^-^> conformance_cli will have NO vulkan/vulkan2 plugin
)
cmake -S "%CTS_SRC%" -B "%CTS_BUILD%" -G "Ninja Multi-Config"
if %ERRORLEVEL% NEQ 0 ( echo CTS CMake configure FAILED & exit /b 1 )

:: ------------------------------------------------------------
:: 4. Build only the conformance harness (skips hello_xr, spec, etc.)
:: ------------------------------------------------------------
echo === Building conformance_cli + conformance_test (RelWithDebInfo) ===
cmake --build "%CTS_BUILD%" --config RelWithDebInfo --target conformance_cli conformance_test
if %ERRORLEVEL% NEQ 0 ( echo CTS build FAILED & exit /b 1 )

:: ------------------------------------------------------------
:: 5. Embed + verify the per-monitor DPI manifest (#1506)
::
:: conformance_cli is built from upstream Khronos sources, which carry no
:: DPI manifest, so it runs DPI-UNAWARE on a scaled display: Windows hands it
:: virtualised window geometry and every geometric CTS measurement (window
:: size/position, Kooima projection, view poses) comes back silently wrong by
:: the scale factor (measured 2.47x at 250% scaling — issue #1502, #1506).
:: We do not own the CTS CMake tree and its pin moves, so this is a
:: post-build step rather than a CMake patch: embed the SAME manifest every
:: DisplayXR executable ships (src/xrt/targets/common/dpi_aware.manifest,
:: #1201) via mt.exe, then assert it actually landed — a future CTS pin bump
:: (new target, different output layout, ...) must not silently drop this
:: again. See docs/reference/dpi-awareness.md.
:: ------------------------------------------------------------
set DPI_MANIFEST=%REPO%src\xrt\targets\common\dpi_aware.manifest
if not exist "%DPI_MANIFEST%" ( echo ERROR: DPI manifest not found at %DPI_MANIFEST% & exit /b 1 )

:: `for /r` with a bare (non-wildcard) filename does NOT enumerate existing
:: files -- it visits every directory under the root and yields
:: <dir>\conformance_cli.exe whether or not it exists (e.g. it will also
:: yield build-cts\build\Testing\Temporary\conformance_cli.exe, a phantom
:: CTest scratch path that is never created). Guard with `if exist` inside
:: the loop, or CTS_EXE ends up pointing at the last (non-existent)
:: candidate visited and every step below fails.
set CTS_EXE=
for /r "%CTS_BUILD%" %%F in (conformance_cli.exe) do if exist "%%F" set "CTS_EXE=%%F"
if not defined CTS_EXE ( echo ERROR: conformance_cli.exe not found under %CTS_BUILD% after build. & exit /b 1 )
if not exist "%CTS_EXE%" ( echo ERROR: resolved conformance_cli.exe path does not exist: %CTS_EXE% & exit /b 1 )

:: ------------------------------------------------------------
:: 5a. Stage the Vulkan loader next to conformance_cli.exe
::
:: Once find_package(Vulkan) succeeds, conformance_cli.exe, conformance_test.dll
:: AND XrApiLayer_runtime_conformance.dll all carry a STATIC import on
:: vulkan-1.dll. A machine with no Vulkan runtime installed -- a GitHub-hosted
:: windows runner, for instance -- has no vulkan-1.dll in System32, so the
:: process dies at load with 0xC0000135 STATUS_DLL_NOT_FOUND before a single
:: test runs: no result XML, no console log, no message, and it takes down the
:: d3d11 and d3d12 arms too, which never asked for Vulkan (#1525). Put the same
:: loader the build linked against next to the exe -- the exe's own directory is
:: the first place Windows looks. This belongs to the BUILD, not to any one arm,
:: so it is cached with it and nothing per-run removes it.
:: ------------------------------------------------------------
for %%D in ("%CTS_EXE%") do set "CTS_EXE_DIR=%%~dpD"
if exist "%VULKAN_SDK%\bin\vulkan-1.dll" (
    echo === Staging vulkan-1.dll from %VULKAN_SDK%\bin next to conformance_cli.exe ===
    copy /Y "%VULKAN_SDK%\bin\vulkan-1.dll" "%CTS_EXE_DIR%" >nul
    :: !ERRORLEVEL!, not %ERRORLEVEL%: inside a parenthesised block cmd expands
    :: % at PARSE time, so the check would read the value from before the copy
    :: and never fire. Delayed expansion is enabled at the top of this script.
    if !ERRORLEVEL! NEQ 0 ( echo ERROR: failed to copy vulkan-1.dll next to %CTS_EXE% & exit /b 1 )
) else (
    echo NOTE: no %VULKAN_SDK%\bin\vulkan-1.dll to stage; relying on a system-installed Vulkan loader.
)

where mt.exe >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: mt.exe not found on PATH.
    echo        mt.exe ships with the Windows SDK and is normally placed on
    echo        PATH by vcvars64.bat ^(step 1 above^). A CTS build that cannot
    echo        embed the DPI manifest would silently run DPI-UNAWARE on a
    echo        scaled display and corrupt every geometric measurement it
    echo        makes -- refusing to continue rather than ship a quietly-wrong
    echo        conformance_cli.exe. See #1506.
    exit /b 1
)

echo === Embedding DPI-awareness manifest into conformance_cli.exe ^(#1506^) ===
mt.exe -nologo -manifest "%DPI_MANIFEST%" -outputresource:"%CTS_EXE%;#1"
if %ERRORLEVEL% NEQ 0 ( echo ERROR: mt.exe failed to embed the DPI manifest into %CTS_EXE% & exit /b 1 )

:: Assert it actually took: extract resource #1 back out and check for the
:: PerMonitorV2 marker. This is the regression guard -- a silent embed
:: failure or a future CTS output-layout change must fail the script loudly.
set "CTS_MANIFEST_CHECK=%TEMP%\dxr_cts_manifest_check_%RANDOM%.manifest"
mt.exe -nologo -inputresource:"%CTS_EXE%;#1" -out:"%CTS_MANIFEST_CHECK%" >nul
if %ERRORLEVEL% NEQ 0 ( echo ERROR: mt.exe failed to extract the embedded manifest back out of %CTS_EXE% for verification. & exit /b 1 )
findstr /c:"PerMonitorV2" "%CTS_MANIFEST_CHECK%" >nul
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: embedded manifest in %CTS_EXE% does not contain PerMonitorV2 -- the CTS DPI manifest embed did NOT take effect.
    del "%CTS_MANIFEST_CHECK%" >nul 2>&1
    exit /b 1
)
del "%CTS_MANIFEST_CHECK%" >nul 2>&1
echo CTS DPI manifest: EMBEDDED and verified

:: ------------------------------------------------------------
:: 6. Report the conformance_cli location
:: ------------------------------------------------------------
echo.
echo === CTS build complete ===
echo   conformance_cli: %CTS_EXE%
for /r "%CTS_BUILD%" %%F in (conformance_test.dll) do if exist "%%F" echo   conformance_test: %%F
echo.
echo === DONE ===
endlocal
