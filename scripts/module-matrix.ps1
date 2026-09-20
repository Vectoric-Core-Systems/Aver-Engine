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
    # Occlusion (modules/occlusion). Added late: this module has defaulted ON since 0a84819 and had
    # no row at all until then, so nothing could have caught a regression in it. Its guards are also
    # the ones most recently disturbed -- every `#if AVER_MODULE_OCCLUSION` in SandboxApp.cpp had to
    # be paired with AVER_MODULE_SCENE (ba30436's parent) because two of its members are keyed on
    # scene::Entity, which is exactly the kind of thing this matrix exists to notice.
    "occlusion-off" = @("-DAVER_MODULE_OCCLUSION=OFF")
    # Particles (modules/particles). Depends on the scene (scene::CParticleEmitter), so this row
    # alone does not exercise the AVER_MODULE_SCENE=OFF interaction -- that is already covered by
    # scene-off below, which forces this module off too via its own AVER_MODULE_SCENE guard in the
    # root CMakeLists.txt. This row is what proves the engine builds, links and renders with
    # particles off ON ITS OWN, same shape as occlusion-off just above.
    "particles-off" = @("-DAVER_MODULE_PARTICLES=OFF")
    # AverSR (docs/AVERSR.md). The IUpscaler seam lives in Aver.RHI itself, not this module, so no
    # RENDERER may reference Aver.Render.Sr -- only sandbox, as the composition root that constructs
    # the concrete SpatialUpscaler and hands the seam an IUpscaler* it can hold as null, links it
    # (sandbox/CMakeLists.txt's `if(TARGET Aver.Render.Sr)` block). This row is what turns "sandbox
    # tolerates the module's absence" from a comment into something a build can contradict: with the
    # module off, `#if AVER_MODULE_SR` compiles it all out and Sandbox must still build, link and
    # render at native resolution.
    "sr-off"        = @("-DAVER_MODULE_SR=OFF")
    "no-ui"         = @("-DAVER_ENABLE_UI=OFF")
    "no-game"       = @("-DAVER_BUILD_GAME=OFF")
    "no-sandbox"    = @("-DAVER_BUILD_SANDBOX=OFF")
    "no-tests"      = @("-DAVER_BUILD_TESTS=OFF")
    # MCP defaults OFF, so every other row here already builds it off and none of them says anything
    # about it. The UNCHECKED configuration of an off-by-default option is the ON one -- the mirror
    # image of the rest of this table, and the same gap TRIFACTOR had.
    "mcp-on"        = @("-DAVER_MODULE_MCP=ON")

    # ---- the RHI is a module too, and swapping its implementation is the whole point of it ----
    #
    # These three rows are the reason `d3d12.h` must not appear outside modules/rhi.d3d12: everything
    # above the RHI talks to IDevice/IRenderContext and to DeviceCaps, never to a backend type. That
    # is a claim about the DEFAULT tree's include graph, and grep can support it but only a build can
    # decide it -- an `#include <d3d12.h>` reached transitively through some other module's public
    # header is invisible to grep of the file that suffers from it.
    #
    # d3d12-off is the sharp one. AVER_HAS_D3D12 goes undefined, tryBackend() returns nullptr for
    # Backend::D3D12, and the engine falls back to the null device -- so this configuration BUILDS
    # AND LINKS but renders nothing, which is correct and is exactly what is being asserted. It fails
    # only if something outside the backend needs a D3D12 symbol to link, which is the coupling worth
    # catching. Do not "fix" a failure here by linking Aver.RHI.D3D12 back in.
    #
    # vulkan-on costs nothing today: modules/rhi.vulkan is a 13-line stub with no SDK dependency (its
    # CMakeLists defers find_package(Vulkan) to Phase 3). The row exists so that the day the real
    # backend lands, the configuration is already being built rather than discovered broken.
    "d3d12-off"     = @("-DAVER_RHI_D3D12=OFF")
    "d3d11-off"     = @("-DAVER_RHI_D3D11=OFF")
    "vulkan-on"     = @("-DAVER_RHI_VULKAN=ON")
    "all-off"       = @("-DAVER_MODULE_SCENE=OFF", "-DAVER_MODULE_FRAMEWORK=OFF",
                        "-DAVER_MODULE_PHYSICS=OFF", "-DAVER_MODULE_PBR=OFF",
                        "-DAVER_MODULE_VOXI=OFF", "-DAVER_MODULE_SCRIPTING=OFF",
                        "-DAVER_MODULE_LANDSCAPE=OFF", "-DAVER_MODULE_DEFORM=OFF",
                        "-DAVER_MODULE_TRIFACTOR=OFF", "-DAVER_MODULE_SR=OFF")
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
# RUNS A NATIVE TOOL WITHOUT LETTING ITS STDERR END THE SCRIPT.
#
# THIS IS WHY THE MATRIX COULD NOT RUN AT ALL. `& $cmake @args 2>&1` under this script's
# $ErrorActionPreference of 'Stop' wraps every stderr LINE in an ErrorRecord and throws on the
# first one -- and CMake's configure prints the NRD submodule's ordinary banner, "NRD: v4.18.0", to
# stderr. So the whole matrix died on a version string before it had compiled anything, reporting a
# NativeCommandError that named cmake.exe and looked like a toolchain fault.
#
# scripts/verify-game.ps1 already documents this exact trap and routes around it, in its own words:
# redirecting a native executable's stderr inside PowerShell "turns the engine's ordinary [WARN]
# lines into a terminating NativeCommandError -- measured: the first version of this died on
# 'cannot enable streaming'". Same defect, second script.
function Invoke-Native {
    param([string] $Exe, [string[]] $Arguments)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try   { $out = & $Exe @Arguments 2>&1; return @{ Output = $out; Exit = $LASTEXITCODE } }
    finally { $ErrorActionPreference = $prev }
}

$cmake = Join-Path (Split-Path -Parent (Get-Cached "CMAKE_MAKE_PROGRAM")) "..\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source }
if (-not $cmake) { Write-Error "Could not locate cmake.exe." }

# THE COMPILER NEEDS ITS ENVIRONMENT, not just its path. The cached CMAKE_CXX_COMPILER above is
# where cl.exe lives; INCLUDE and LIB are what let it find a standard header, and those come from
# vcvars64. Without them CMake's own compiler probe fails first, with
# "CMakeTestCCompiler.cmake:67 (message)" and nothing about what is actually missing -- which reads
# as a broken tree rather than a shell that was never set up.
#
# scripts/build.ps1 hands this problem to build.bat, which calls vcvars64 before it does anything.
# This script had no equivalent, so it ran only from a Developer prompt and failed everywhere else.
# Given the header above calls this matrix "the only thing that can answer" whether the engine still
# builds with a module off, a check that silently cannot start is a check nobody runs.
if (-not $env:INCLUDE) {
    $vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvars)) {
        Write-Error "No MSVC environment (INCLUDE is unset) and no vcvars64.bat at $vcvars. Run this from a Developer prompt, or correct the path here."
    }
    # Import what vcvars sets rather than re-launching this script under cmd: one process, and the
    # variables are then visible to ninja too, which needs them just as much as the configure does.
    #
    # THROUGH Invoke-Native, because vcvars64.bat ITSELF trips the same stderr trap: on this machine
    # it prints "'vswhere.exe' is not recognized" and then carries on to set the environment
    # correctly anyway. build.bat never noticed because it sends the whole thing to nul. Left
    # unguarded here, that one benign line ends the script before a single configuration is built --
    # the same failure, one layer down, as the one this function was written for.
    $vc = Invoke-Native "cmd" @("/c", "call `"$vcvars`" >nul && set")
    $vc.Output | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] -ErrorAction SilentlyContinue }
    }
    if (-not $env:INCLUDE) { Write-Error "vcvars64 ran but INCLUDE is still unset; the MSVC environment did not take." }
    Write-Host "[matrix] MSVC environment imported from vcvars64" -ForegroundColor DarkGray
}
$ninja = Get-Cached "CMAKE_MAKE_PROGRAM"
$cxx   = Get-Cached "CMAKE_CXX_COMPILER"
$cc    = Get-Cached "CMAKE_C_COMPILER"
$rc    = Get-Cached "CMAKE_RC_COMPILER"

$results = @()
foreach ($name in $configs.Keys) {
    if ($Only -and $name -ne $Only) { continue }
    $dir = Join-Path $BuildRoot $name
    Write-Host "`n=== $name ===" -ForegroundColor Cyan

    # NOT $args: that is a PowerShell automatic variable holding this script's own arguments, and
    # assigning to it works only by accident of it being consumed on the very next line.
    # verify-game.ps1 carries the same warning over its own Get-Census for the same reason.
    $cfgArgs = @("-S", ".", "-B", $dir, "-G", "Ninja",
                 "-DCMAKE_BUILD_TYPE=Debug",
                 "-DCMAKE_MAKE_PROGRAM=$ninja",
                 "-DCMAKE_CXX_COMPILER=$cxx", "-DCMAKE_C_COMPILER=$cc", "-DCMAKE_RC_COMPILER=$rc") +
               $configs[$name]

    $cfg    = Invoke-Native $cmake $cfgArgs
    $cfgOut = $cfg.Output
    if ($cfg.Exit -ne 0) {
        $results += [pscustomobject]@{ Config = $name; Stage = "configure"; Ok = $false
                                       Detail = ($cfgOut | Select-String -Pattern "CMake Error" | Select-Object -First 1) }
        Write-Host "  CONFIGURE FAILED" -ForegroundColor Red
        continue
    }

    # -k 0 IS LOAD-BEARING. Ninja stops at the first failure by default, and the modularity audit
    # that produced this script was misled by exactly that: a link failure in a small target aborted
    # the build before SandboxApp.cpp was ever compiled, and the run was reported as "0 compiler
    # errors". Keep going, or the result describes how far ninja got rather than what is broken.
    $build    = Invoke-Native $ninja @("-C", $dir, "-k", "0")
    $buildOut = $build.Output
    $ok = $build.Exit -eq 0
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
