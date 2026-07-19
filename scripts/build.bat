@echo off
REM Aver Engine build — configures a VS Developer env (vcvars64) then CMake+Ninja.
setlocal
set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || (echo [build] vcvars64 failed & exit /b 1)
set "CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set "ROOT=%~dp0.."
pushd "%ROOT%"
"%CMAKE%" -S . -B build -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=Debug %* || (echo [build] configure failed & popd & exit /b 1)
"%CMAKE%" --build build || (echo [build] compile failed & popd & exit /b 1)
popd
echo [build] OK -^> build\bin
