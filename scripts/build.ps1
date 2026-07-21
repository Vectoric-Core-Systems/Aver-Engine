# Aver Engine build wrapper (PowerShell). Delegates to build.bat so vcvars64 sets
# up the MSVC environment correctly. Extra args are forwarded to CMake configure.
#
#   ./scripts/build.ps1                       # Debug   -> build/bin
#   ./scripts/build.ps1 -Release              # Release -> build-release/bin
#   ./scripts/build.ps1 -Config RelWithDebInfo
#   ./scripts/build.ps1 -DAVER_MODULE_VOXI=OFF        # anything else goes to CMake configure
#
# The two trees are separate on purpose: scripts/gates.baseline.txt is a DEBUG measurement and
# scripts/gates.baseline.release.txt is the Release one, so both binaries have to exist at once
# for either baseline to mean anything. `-BuildDir` overrides the default pairing.
[CmdletBinding()]
param(
    [switch] $Release,
    [string] $Config = $(if ($Release) { 'Release' } else { 'Debug' }),
    [string] $BuildDir = $(if ($Config -eq 'Debug') { 'build' } else { "build-$($Config.ToLower())" }),
    [Parameter(ValueFromRemainingArguments = $true)] [string[]] $CMakeArgs
)

# Restored afterwards. build.bat reads these, but leaving them set would silently reconfigure the
# NEXT thing run in the same shell — scripts/run.ps1 calls build.bat directly and would build the
# Release tree while launching the Debug binary.
$prevConfig = $env:AVER_BUILD_CONFIG
$prevDir    = $env:AVER_BUILD_DIR
try {
    $env:AVER_BUILD_CONFIG = $Config
    $env:AVER_BUILD_DIR    = $BuildDir
    & "$PSScriptRoot\build.bat" @CMakeArgs
    $code = $LASTEXITCODE
} finally {
    $env:AVER_BUILD_CONFIG = $prevConfig
    $env:AVER_BUILD_DIR    = $prevDir
}
exit $code
