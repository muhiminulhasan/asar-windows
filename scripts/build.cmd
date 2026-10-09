@echo off
rem build.cmd - configure and build everything with the local VS toolchain.
rem
rem Usage:
rem   scripts\build.cmd              build ALL architectures (x64, Win32, ARM64)
rem   scripts\build.cmd x64          build one architecture
rem   scripts\build.cmd x64 Win32    build a subset
rem   scripts\build.cmd --clean x64  wipe build dirs first
rem
rem Architectures: x64 | Win32 | ARM64   (built into build\<arch>)
rem ARM64 requires the "MSVC v143 - C++ ARM64 build tools" VS component.
rem
rem Works even when vcvars64.bat is broken by locating MSVC and the Windows
rem SDK manually and setting INCLUDE/LIB/PATH itself. Uses delayed expansion
rem because default install paths contain "(x86)".
setlocal enabledelayedexpansion

set "ROOT=%~dp0.."
set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
    echo vswhere.exe not found; install Visual Studio 2022 Build Tools with the C++ workload.
    exit /b 1
)
set "VSROOT="
for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
    echo No Visual Studio installation with the C++ toolset was found.
    exit /b 1
)

rem Newest MSVC toolset with a Hostx64/x64 compiler.
set "MSVCROOT=!VSROOT!\VC\Tools\MSVC"
set "MSVC="
for /f "delims=" %%v in ('dir /b /ad /o-n "!MSVCROOT!" 2^>nul') do (
    if not defined MSVC if exist "!MSVCROOT!\%%v\bin\Hostx64\x64\cl.exe" set "MSVC=%%v"
)
if not defined MSVC (
    echo No MSVC toolset with Hostx64/x64 cl.exe was found under "!MSVCROOT!".
    exit /b 1
)

rem Newest Windows 10/11 SDK.
set "SDKROOT=C:\Program Files (x86)\Windows Kits\10"
set "SDKV="
for /f "delims=" %%v in ('dir /b /ad /o-n "!SDKROOT!\Include" 2^>nul') do (
    echo %%v| findstr /r "^[0-9][0-9]*\.[0-9][0-9]*" >nul && if not defined SDKV set "SDKV=%%v"
)
if not defined SDKV (
    echo No Windows SDK found under "!SDKROOT!".
    exit /b 1
)

set "MSVCDIR=!MSVCROOT!\!MSVC!"

rem CMake (bundled with VS).
set "CMAKEDIR=!VSROOT!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "CMAKE=!CMAKEDIR!\cmake.exe"
if not exist "!CMAKE!" (
    for /f "delims=" %%c in ('where cmake 2^>nul') do set "CMAKE=%%c"
)
if not exist "!CMAKE!" (
    echo cmake.exe was not found. Enable the "C++ CMake tools" component of
    echo Visual Studio Build Tools, or install CMake and put it on PATH.
    exit /b 1
)
set "CTEST=!CMAKEDIR!\ctest.exe"
if not exist "!CTEST!" set "CTEST="

rem Which architectures to build?
set "ARCHES="
set "CLEAN="
:ParseArgs
if "%~1"=="" goto ArgsDone
if /i "%~1"=="--clean" ( set "CLEAN=1" & shift & goto ParseArgs )
if /i "%~1"=="all"     ( set "ARCHES=x64 Win32 ARM64" & shift & goto ParseArgs )
set "ARCHES=!ARCHES! %~1"
shift
goto ParseArgs
:ArgsDone
if not defined ARCHES set "ARCHES=x64 Win32 ARM64"

rem ARM64 binaries can only be *tested* on an ARM64 host; Win32 binaries run
rem under WOW64 on x64 hosts. The host architecture decides what we test.
set "HOSTARCH=%PROCESSOR_ARCHITECTURE%"

set "FAILED="
for %%A in (!ARCHES!) do (
    call :BuildArch %%A
    if errorlevel 1 set "FAILED=1"
)
if defined FAILED (
    echo.
    echo One or more architectures FAILED to build.
    exit /b 1
)
echo.
echo Build finished. Outputs are under "!ROOT!\build\<arch>".
endlocal
exit /b 0

rem ---------------------------------------------------------------------------
:BuildArch
set "A=%~1"
set "TGT="
set "BIN="
set "LIBS="
if /i "!A!"=="x64"   ( set "TGT=x64"   & set "BIN=x64"   & set "LIBS=x64"   )
if /i "!A!"=="Win32" ( set "TGT=Win32" & set "BIN=x86"   & set "LIBS=x86"   )
if /i "!A!"=="ARM64" ( set "TGT=ARM64" & set "BIN=arm64" & set "LIBS=arm64" )
if not defined TGT (
    echo Unknown architecture "!A!" - use x64, Win32 or ARM64.
    exit /b 1
)

rem Host tool flavor: an ARM64 host uses its native cross compilers.
set "HOST=Hostx64"
if /i "%HOSTARCH%"=="ARM64" set "HOST=HostArm64"

if not exist "!MSVCDIR!\bin\!HOST!\!BIN!\cl.exe" (
    echo ----------------------------------------------------------------------
    echo The !A! compiler is not installed:
    echo   !MSVCDIR!\bin\!HOST!\!BIN!\cl.exe
    if /i "!A!"=="ARM64" (
        echo Add it with the Visual Studio Installer:
        echo   vs_installer.exe modify --add Microsoft.VisualStudio.Component.VC.Tools.ARM64
    ) else (
        echo Add the "MSVC v143 - C++ !A! build tools" component.
    )
    echo SKIPPED: !A!.
    echo ----------------------------------------------------------------------
    exit /b 0
)

set "PATH=!MSVCDIR!\bin\!HOST!\!TGT!;!PATH%"
set "INCLUDE=!MSVCDIR!\include;!SDKROOT!\Include\!SDKV!\ucrt;!SDKROOT!\Include\!SDKV!\um;!SDKROOT!\Include\!SDKV!\shared;!SDKROOT!\Include\!SDKV!\winrt;!SDKROOT!\Include\!SDKV!\cppwinrt"
set "LIB=!MSVCDIR!\lib\!LIBS!;!SDKROOT!\Lib\!SDKV!\ucrt\!LIBS!;!SDKROOT!\Lib\!SDKV!\um\!LIBS!"

echo.
echo ============ Building !A! ============
if defined CLEAN rmdir /s /q "!ROOT!\build\!A!" 2>nul
"!CMAKE!" -S "!ROOT!" -B "!ROOT!\build\!A!" -A !TGT! || exit /b 1
"!CMAKE!" --build "!ROOT!\build\!A!" --config Release || exit /b 1

rem Run tests only for architectures this host can execute.
set "DOTEST="
if /i "%HOSTARCH%"=="AMD64" if /i not "!A!"=="ARM64" set "DOTEST=1"
if /i "%HOSTARCH%"=="ARM64" if /i "!A!"=="ARM64"     set "DOTEST=1"
if /i "%HOSTARCH%"=="x86"   if /i "!A!"=="Win32"     set "DOTEST=1"
if defined DOTEST (
    if defined CTEST (
        "!CTEST!" --test-dir "!ROOT!\build\!A!" -C Release --output-on-failure || exit /b 1
    ) else (
        "!CMAKE!" --build "!ROOT!\build\!A!" --config Release --target RUN_TESTS || exit /b 1
    )
) else (
    echo Skipping tests for !A! - cannot execute on this host ^(host: %HOSTARCH%^).
)
exit /b 0
