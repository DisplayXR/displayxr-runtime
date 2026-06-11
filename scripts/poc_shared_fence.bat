@echo off
setlocal

REM Phase 2 PoC build + run: cross-process ID3D11Fence test.
REM See scripts\poc_shared_fence.cpp for description. Throwaway code; not part of the runtime.

set "POC_DIR=%~dp0"
set "POC_SRC=%POC_DIR%poc_shared_fence.cpp"
set "POC_OUT_DIR=%POC_DIR%..\_package\poc"
set "POC_EXE=%POC_OUT_DIR%\poc_shared_fence.exe"

if not exist "%POC_OUT_DIR%" mkdir "%POC_OUT_DIR%"

REM Mirror build_windows.bat: locate any VS2022 edition via vswhere.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
    )
)
if not defined VCVARS (
    for %%E in (Community Professional Enterprise BuildTools) do (
        if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
    )
)
if not defined VCVARS (
    echo [poc] FAIL: could not find VS2022 vcvars64.bat ^(any edition^)
    exit /b 1
)
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo [poc] FAIL: vcvars64.bat failed: %VCVARS%
    exit /b 1
)

echo [poc] compiling %POC_SRC%
cl.exe /nologo /EHsc /std:c++17 /O2 /Fe:"%POC_EXE%" /Fo:"%POC_OUT_DIR%\\" "%POC_SRC%" /link /SUBSYSTEM:CONSOLE d3d11.lib
if errorlevel 1 (
    echo [poc] compile FAIL
    exit /b 1
)

echo [poc] running %POC_EXE%
"%POC_EXE%"
exit /b %errorlevel%
