@echo off
REM Aver Engine build — configures a VS Developer env (vcvars64) then CMake+Ninja.
REM
REM Configuration and build tree come from the environment, not from the command line, so that
REM everything after the script name is still forwarded verbatim to CMake configure (callers rely
REM on `./scripts/build.ps1 -DAVER_MODULE_VOXI=OFF`). scripts/build.ps1 sets these for you.
REM
REM   AVER_BUILD_CONFIG   Debug (default) | Release | RelWithDebInfo | MinSizeRel
REM   AVER_BUILD_DIR      build (default) | build-release | ...
REM
REM The two trees are SEPARATE by default because the gates baseline is a Debug measurement and a
REM Release one has its own; a single tree reconfigured back and forth would silently invalidate
REM whichever was measured last.
setlocal
if not defined AVER_BUILD_CONFIG set "AVER_BUILD_CONFIG=Debug"
if not defined AVER_BUILD_DIR    set "AVER_BUILD_DIR=build"
REM Visual Studio: AVER_VS if set, else the newest install with the C++ tools that vswhere finds (any version or edition).
set "VS=%AVER_VS%"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined VS if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo [build] no Visual Studio with the C++ tools found; set AVER_VS to its folder & exit /b 3)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || (echo [build] vcvars64 failed & exit /b 1)
REM VS's own CMake and Ninja when installed, else whatever is on PATH.
set "CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if not exist "%CMAKE%" set "CMAKE=cmake"
if not exist "%NINJA%" set "NINJA=ninja"
set "ROOT=%~dp0.."
pushd "%ROOT%"
echo [build] %AVER_BUILD_CONFIG% -^> %AVER_BUILD_DIR%
"%CMAKE%" -S . -B "%AVER_BUILD_DIR%" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=%AVER_BUILD_CONFIG% %* || (echo [build] configure failed & popd & exit /b 1)
"%CMAKE%" --build "%AVER_BUILD_DIR%" || (echo [build] compile failed & popd & exit /b 1)
popd
echo [build] OK -^> %AVER_BUILD_DIR%\bin
