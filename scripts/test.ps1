# The headless suites, as a runner. Configures nothing and builds nothing: it drives CTest over the
# ~100 `*Test` executables a build already produced.
#
# WHY THIS EXISTS. Until it did, exactly two things could run these suites: tools/mcp/aver_mcp.py's
# `aver_tests`, which is reachable from an AI agent session and from nowhere else, and running each
# .exe by hand -- which README.md documents by naming 14 of them. There was no single command a
# person at a terminal, or a build server, could use. That is the structural reason this repo has a
# documented history of describing tests that never ran.
#
#   ./scripts/test.ps1                  # every suite, Debug
#   ./scripts/test.ps1 -Release         # every suite, Release
#   ./scripts/test.ps1 -Filter Import   # suites whose name matches (CTest regex)
#   ./scripts/test.ps1 -Jobs 4          # cap parallelism
#   ./scripts/test.ps1 -Rerun           # only the ones that failed last time, verbosely
#
# Exit code 0 = every suite passed. Non-zero = at least one did not.
#
# PASSING IS NOT MERELY EXITING 0. Registration in CMakeLists.txt pairs every suite with a
# FAIL_REGULAR_EXPRESSION, because suites in this tree have historically returned 0 however they
# went -- there is a commit named "Two skin tests that exited 0 however they went". A suite that
# prints a failure and returns success still fails here.
#
# THIS IS NOT THE RENDER ORACLE. scripts/gates.ps1 owns that, compares 8-bit probe codes against a
# recorded baseline, and needs a GPU. These two answer different questions and are deliberately
# separate commands.
[CmdletBinding()]
param(
    [switch] $Release,
    [string] $Filter = "",
    # Defaults to CPU count minus two, floored at one -- the same shape PhysicsWorld uses for its
    # job pool, and for the same reason: leave the machine usable while it runs.
    [int]    $Jobs = 0,
    [switch] $Rerun,
    [string] $BuildDir = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

if (-not $BuildDir) { if ($Release) { $BuildDir = "build-release" } else { $BuildDir = "build" } }
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $repo $BuildDir }

if (-not (Test-Path $BuildDir)) {
    Write-Host "[test] no build tree at $BuildDir -- configure and build it first" -ForegroundColor Red
    exit 2
}
# CTestTestfile.cmake is what registration produces. Its absence means the tree was configured
# before CTest registration existed, or with -DAVER_BUILD_TESTS=OFF; either way, say which rather
# than letting ctest report "No tests were found" and exit 0.
if (-not (Test-Path (Join-Path $BuildDir "CTestTestfile.cmake"))) {
    Write-Host "[test] $BuildDir has no CTest registration -- re-run CMake configure on it" -ForegroundColor Red
    Write-Host "[test] (a tree configured with -DAVER_BUILD_TESTS=OFF has none by design)"
    exit 2
}

# ctest ships beside cmake; VS's bundled copy is the one this repo builds with.
$ctest = (Get-Command ctest -ErrorAction SilentlyContinue).Source
if (-not $ctest) {
    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe",
        "C:\Program Files\CMake\bin\ctest.exe"
    )
    $ctest = (Get-ChildItem $candidates -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
}
if (-not $ctest) {
    Write-Host "[test] ctest not found on PATH or beside a known CMake install" -ForegroundColor Red
    exit 2
}

if ($Jobs -le 0) {
    $Jobs = [Math]::Max(1, [Environment]::ProcessorCount - 2)
}

$ctestArgs = @("--test-dir", $BuildDir, "-j", $Jobs, "--output-on-failure")
if ($Filter) { $ctestArgs += @("-R", $Filter) }
if ($Rerun)  { $ctestArgs += "--rerun-failed" }

Write-Host "[test] $BuildDir  |  $(if ($Release) { 'Release' } else { 'Debug' })  |  -j $Jobs$(if ($Filter) { "  |  filter '$Filter'" })"
$sw = [Diagnostics.Stopwatch]::StartNew()
& $ctest @ctestArgs
$code = $LASTEXITCODE
$sw.Stop()

if ($code -eq 0) {
    Write-Host "[test] all suites passed in $([Math]::Round($sw.Elapsed.TotalSeconds, 1))s" -ForegroundColor Green
} else {
    Write-Host "[test] FAILED (ctest exit $code) after $([Math]::Round($sw.Elapsed.TotalSeconds, 1))s" -ForegroundColor Red
    Write-Host "[test] re-run just the failures with: ./scripts/test.ps1 -Rerun$(if ($Release) { ' -Release' })"
}
exit $code
