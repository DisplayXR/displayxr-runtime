@echo off
REM Build the two #918 hybrid-GPU measurement tools (gpu_loadgen, xbridge_bench).
REM Standalone: locates MSVC via vswhere, sets up vcvars64, compiles both.
setlocal EnableDelayedExpansion

set "HERE=%~dp0"
cd /d "%HERE%"

set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [build] vswhere.exe not found at "%VSWHERE%"
  exit /b 1
)

"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\dxr918_vspath.txt"
set /p VSPATH=<"%TEMP%\dxr918_vspath.txt"
del "%TEMP%\dxr918_vspath.txt" >nul 2>&1
if "%VSPATH%"=="" (
  echo [build] no VS installation with the C++ toolset found
  exit /b 1
)
echo [build] VS: %VSPATH%

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo [build] vcvars64.bat failed
  exit /b 1
)

if not exist "%HERE%obj" mkdir "%HERE%obj"

set "CLFLAGS=/nologo /std:c++17 /EHsc /O2 /W3 /MT /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /Fo:obj\\"
set "LIBS=d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib"

echo [build] compiling gpu_loadgen.cpp
cl %CLFLAGS% gpu_loadgen.cpp /Fe:gpu_loadgen.exe /link %LIBS%
if errorlevel 1 ( echo [build] gpu_loadgen FAILED & exit /b 1 )

echo [build] compiling xbridge_bench.cpp
cl %CLFLAGS% xbridge_bench.cpp /Fe:xbridge_bench.exe /link %LIBS%
if errorlevel 1 ( echo [build] xbridge_bench FAILED & exit /b 1 )

echo [build] compiling xbridge12_bench.cpp
cl %CLFLAGS% xbridge12_bench.cpp /Fe:xbridge12_bench.exe /link %LIBS%
if errorlevel 1 ( echo [build] xbridge12_bench FAILED & exit /b 1 )

echo [build] ALL DONE
dir /b gpu_loadgen.exe xbridge_bench.exe xbridge12_bench.exe
exit /b 0
