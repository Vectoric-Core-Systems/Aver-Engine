# Builds every module configuration and reports which ones actually work.
#
# WHY THIS IS THE REAL CHECK, and not a nice-to-have on top of the cheap ones. Two other guards exist:
# aver_check_module_dag() in cmake/AvModule.cmake catches an optional module named without an
# if(TARGET ...) guard, and modules/core/include/aver/core/ModuleCheck.hpp catches a module compiled
# in without a dependency it stands on. Between them they cover exactly two of the fifteen defects
# found when this matrix was first run.
#
# The rest were all the same shape: a member or function declared inside `#if AVER_MODULE_X` but used
# from a site guarded differently, or not at all. That is a preprocessor data-flow property of ONE
# SPECIFIC CONFIGURATION, and nothing short of preprocessing that configuration can decide it. So the
# matrix is not belt-and-braces -- it is the only thing that can answer the question the root
# CMakeLists.txt asserts when it says "the engine builds and runs with these OFF".
#
#   ./scripts/module-matrix.ps1                 # every configuration
#   ./scripts/module-matrix.ps1 -Only pbr-off   # just one
#   ./scripts/module-matrix.ps1 -KeepDirs       # leave the build trees for inspection
[CmdletBinding()]
param(
    [string] $Only = "",
    [switch] $KeepDirs,
    [string] $BuildRoot = "build-matrix"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

# Each entry is one configuration worth asserting. `default` is first on purpose: if it fails, every
# other result is noise.
$configs = [ordered]@{
    "default"       = @()
    "pbr-off"       = @("-DAVER_MODULE_PBR=OFF")
    "voxi-off"      = @("-DAVER_MODULE_VOXI=OFF")
    "physics-off"   = @("-DAVER_MODULE_PHYSICS=OFF")
    "landscape-off" = @("-DAVER_MODULE_LANDSCAPE=OFF", "-DAVER_MODULE_DEFORM=OFF")
    "scripting-off" = @("-DAVER_MODULE_SCRIPTING=OFF")
    "scene-off"     = @("-DAVER_MODULE_SCENE=OFF", "-DAVER_MODULE_FRAMEWORK=OFF")
    "trifactor-off" = @("-DAVER_MODULE_TRIFACTOR=OFF")
    "no-ui"         = @("-DAVER_ENABLE_UI=OFF")
    "no-game"       = @("-DAVER_BUILD_GAME=OFF")
    "no-sandbox"    = @("-DAVER_BUILD_SANDBOX=OFF")
    "all-off"       = @("-DAVER_MODULE_SCENE=OFF", "-DAVER_MODULE_FRAMEWORK=OFF",
                        "-DAVER_MODULE_PHYSICS=OFF", "-DAVER_MODULE_PBR=OFF",
                        "-DAVER_MODULE_VOXI=OFF", "-DAVER_MODULE_SCRIPTING=OFF",
                        "-DAVER_MODULE_LANDSCAPE=OFF", "-DAVER_MODULE_DEFORM=OFF",
                        "-DAVER_MODULE_TRIFACTOR=OFF")
}

# A module absent from this list is a module nobody checks. TRIFACTOR was, for its whole life --
# added as an option, defaulted ON, and never given a row here, so "the matrix passes" said nothing
# about the one module the virtualized-geometry work lives in. If you add an option() to the root
# CMakeLists, add it here in the same commit.

# The toolchain the normal build already found, rather than whatever is on PATH -- this script is
# routinely run from a plain shell with no MSVC environment.
$cache = Join-Path $repo "build\CMakeCache.txt"
if (-not (Test-Path $cache)) {
    Write-Error "No build/CMakeCache.txt. Configure the normal build once first, so this script can reuse its toolchain."
}
function Get-Cached([string] $key) {
    # "^${key}:" and NOT "^$key:". PowerShell parses `$key:` as a DRIVE-QUALIFIED variable -- the
    # same syntax as $env:PATH -- so it reads "key" as a drive name and throws
    # InvalidVariableReferenceWithDrive at PARSE time. Parse time is the important part: the script
    # died before building a single configuration, which is why the one tool meant to catch
    # modularity breaks had never actually run on this machine, and two broken configurations
    # (voxi-off, no-ui) sat at head unnoticed.
    $line = Select-String -Path $cache -Pattern "^${key}:" | Select-Object -First 1
    if (-not $line) { return "" }
    return ($line.Line -split "=", 2)[1]
}
$cmake = Join-Path (Split-Path -Parent (Get-Cached "CMAKE_MAKE_PROGRAM")) "..\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source }
if (-not $cmake) { Write-Error "Could not locate cmake.exe." }
$ninja = Get-Cached "CMAKE_MAKE_PROGRAM"
$cxx   = Get-Cached "CMAKE_CXX_COMPILER"
$cc    = Get-Cached "CMAKE_C_COMPILER"
$rc    = Get-Cached "CMAKE_RC_COMPILER"

$results = @()
foreach ($name in $configs.Keys) {
    if ($Only -and $name -ne $Only) { continue }
    $dir = Join-Path $BuildRoot $name
    Write-Host "`n=== $name ===" -ForegroundColor Cyan

    $args = @("-S", ".", "-B", $dir, "-G", "Ninja",
              "-DCMAKE_BUILD_TYPE=Debug",
              "-DCMAKE_MAKE_PROGRAM=$ninja",
              "-DCMAKE_CXX_COMPILER=$cxx", "-DCMAKE_C_COMPILER=$cc", "-DCMAKE_RC_COMPILER=$rc") +
            $configs[$name]

    $cfgOut = & $cmake @args 2>&1
    if ($LASTEXITCODE -ne 0) {
        $results += [pscustomobject]@{ Config = $name; Stage = "configure"; Ok = $false
                                       Detail = ($cfgOut | Select-String -Pattern "CMake Error" | Select-Object -First 1) }
        Write-Host "  CONFIGURE FAILED" -ForegroundColor Red
        continue
    }

    # -k 0 IS LOAD-BEARING. Ninja stops at the first failure by default, and the modularity audit
    # that produced this script was misled by exactly that: a link failure in a small target aborted
    # the build before SandboxApp.cpp was ever compiled, and the run was reported as "0 compiler
    # errors". Keep going, or the result describes how far ninja got rather than what is broken.
    $buildOut = & $ninja -C $dir -k 0 2>&1
    $ok = $LASTEXITCODE -eq 0
    $errs = @($buildOut | Select-String -Pattern "error [A-Z]+[0-9]+" | Select-Object -First 3)
    $results += [pscustomobject]@{ Config = $name; Stage = "build"; Ok = $ok
                                   Detail = if ($ok) { "" } else { ($errs -join " | ") } }
    if ($ok) { Write-Host "  OK" -ForegroundColor Green }
    else     { Write-Host "  BUILD FAILED" -ForegroundColor Red; $errs | ForEach-Object { Write-Host "    $_" } }
}

Write-Host "`n=== SUMMARY ===" -ForegroundColor Cyan
$results | Format-Table Config, Ok, Stage, Detail -AutoSize
$failed = @($results | Where-Object { -not $_.Ok })

if (-not $KeepDirs) { Remove-Item -Recurse -Force $BuildRoot -ErrorAction SilentlyContinue }

if ($failed.Count -gt 0) {
    Write-Host "$($failed.Count) of $($results.Count) configurations FAILED" -ForegroundColor Red
    exit $failed.Count
}
Write-Host "all $($results.Count) configurations build" -ForegroundColor Green
exit 0
