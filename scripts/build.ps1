# Aver Engine build wrapper (PowerShell). Delegates to build.bat so vcvars64 sets
# up the MSVC environment correctly. Extra args are forwarded to CMake configure.
& "$PSScriptRoot\build.bat" @args
exit $LASTEXITCODE
