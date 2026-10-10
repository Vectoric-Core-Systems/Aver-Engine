<#
.SYNOPSIS
    Stages the shipping payload for Aver Engine out of a build tree.

.DESCRIPTION
    Copies exactly the paths named in scripts/payload.allowlist into a clean output directory,
    emits THIRD-PARTY-NOTICES.txt and payload.json, and refuses to produce a tree that cannot
    legally or functionally ship.

    Nothing else in this repo produces a redistributable: there is no install() rule, no CPack and
    no export(). This script IS the packaging step, and the allowlist beside it is the definition
    of "what ships".

    LAYOUT. The output is deliberately NOT a copy of bin\:

        <out>\bin\                  the editor, its DLLs, its artwork
        <out>\scripting\csharp\     engine C# source
        <out>\THIRD-PARTY-NOTICES.txt
        <out>\payload.json

    bin\ and scripting\ are SIBLINGS because ProjectScaffold.cpp resolves engine .csproj paths by
    walking up from executableDir() -- from <install>\bin it finds <install>\scripting\csharp one
    level up. Flatten this and every generated project silently gets Include="" references.

.PARAMETER Config
    Build configuration to stage from. Release by default, and Debug is REFUSED without
    -AllowDebugCrt: a Debug build imports MSVCP140D.dll / VCRUNTIME140D.dll / ucrtbased.dll, the
    debug CRT, which ships with Visual Studio and may not be redistributed. That is a licence
    constraint, not a quality preference.

.PARAMETER Out
    Output directory. Created if absent; must be empty or -Force'd.

.PARAMETER WithSamples
    Also stage the [samples] section (SampleScripts\, ActorScripts\).

.EXAMPLE
    ./scripts/stage-payload.ps1 -Out ..\stage\AverEngine-0.1.0
    ./scripts/gates.ps1 -Release -Exe ..\stage\AverEngine-0.1.0\bin\Sandbox.exe
#>
[CmdletBinding()]
param(
    [string] $Config = 'Release',
    [Parameter(Mandatory = $true)] [string] $Out,
    [string] $BuildDir,
    [switch] $WithSamples,
    [switch] $Force,
    [switch] $AllowDebugCrt
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) {
    $BuildDir = if ($Config -eq 'Debug') { 'build' } else { "build-$($Config.ToLower())" }
}
$tree = Join-Path $root $BuildDir
$bin  = Join-Path $tree 'bin'

$failures = New-Object System.Collections.Generic.List[string]
function Fail([string] $msg) { $script:failures.Add($msg); Write-Host "[stage] ERROR $msg" -ForegroundColor Red }
function Note([string] $msg) { Write-Host "[stage] $msg" }

# The PE import-table helpers and the OS-provided DLL list live in their own file. They were shared
# with stage-game.ps1 until the packaged-game path was removed; see PeImports.ps1's own header for
# why it stays separate now that this is its only caller.
. (Join-Path $PSScriptRoot 'PeImports.ps1')
$osProvided = $AverOsProvidedDlls

if (-not (Test-Path -LiteralPath $bin)) {
    Write-Host "[stage] no build tree at $bin -- run ./scripts/build.ps1 $(if ($Config -ne 'Debug') {'-Release'}) first" -ForegroundColor Red
    exit 3  # ExitCode.Environment (core/ErrorCodes.hpp)
}

# ---------------------------------------------------------------------------------------------
# 1. Build provenance. The module set is read from CMakeCache.txt because that is the ONLY
#    machine-readable record of which AVER_MODULE_* were on -- there is no generated config header.
#
#    An option that is ABSENT is not the same as an option that is OFF: absent means the cache
#    predates the option, so the edition is unknowable and we refuse rather than guess. This is not
#    hypothetical -- build-release\CMakeCache.txt lacked AVER_MODULE_PHYSICS, SCENE and FRAMEWORK
#    entirely for a week while looking like a normal Release tree.
# ---------------------------------------------------------------------------------------------
$cachePath = Join-Path $tree 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cachePath)) {
    Write-Host "[stage] no CMakeCache.txt in $tree" -ForegroundColor Red
    exit 3  # ExitCode.Environment
}
$cache = Get-Content -LiteralPath $cachePath

$expected = @(
    'AVER_MODULE_VOXI', 'AVER_MODULE_PBR', 'AVER_MODULE_SCRIPTING', 'AVER_MODULE_SCENE',
    'AVER_MODULE_FRAMEWORK', 'AVER_MODULE_PHYSICS',
    'AVER_RHI_D3D12', 'AVER_RHI_D3D11', 'AVER_RHI_VULKAN', 'AVER_ENABLE_UI'
)
$options = @{}
foreach ($line in $cache) {
    if ($line -match '^(AVER_[A-Z0-9_]+):BOOL=(ON|OFF)$') { $options[$Matches[1]] = ($Matches[2] -eq 'ON') }
}
$absent = @($expected | Where-Object { -not $options.ContainsKey($_) })
if ($absent.Count -gt 0) {
    Fail ("CMakeCache.txt does not mention {0} -- the module set of this tree is unknowable. " -f ($absent -join ', ') +
          "Absent is not OFF: reconfigure with ./scripts/build.ps1 $(if ($Config -ne 'Debug') {'-Release'}) and stage again.")
}

$cachedConfig = ($cache | Where-Object { $_ -match '^CMAKE_BUILD_TYPE:STRING=(.*)$' } |
                 ForEach-Object { $Matches[1] } | Select-Object -First 1)
if ($cachedConfig -and $cachedConfig -ne $Config) {
    Fail "asked to stage $Config but $BuildDir\CMakeCache.txt says CMAKE_BUILD_TYPE=$cachedConfig"
}

if ($Config -eq 'Debug' -and -not $AllowDebugCrt) {
    Fail ('refusing to stage a Debug build: it imports the debug CRT (MSVCP140D.dll, ' +
          'VCRUNTIME140D.dll, VCRUNTIME140_1D.dll, ucrtbased.dll), which ships with Visual Studio ' +
          'and may not be redistributed. Stage Release, or pass -AllowDebugCrt for a local-only tree.')
}

$version = ($cache | Where-Object { $_ -match '^CMAKE_PROJECT_VERSION:STATIC=(.*)$' } |
            ForEach-Object { $Matches[1] } | Select-Object -First 1)
if (-not $version) {
    # Fallback: the authoritative source is project(AverEngine VERSION x.y.z) in the root CMakeLists.
    $pl = Select-String -LiteralPath (Join-Path $root 'CMakeLists.txt') -Pattern 'project\(AverEngine VERSION ([0-9.]+)' |
          Select-Object -First 1
    if ($pl) { $version = $pl.Matches[0].Groups[1].Value }
}
if (-not $version) { Fail 'could not determine the engine version from CMakeCache.txt or CMakeLists.txt' }

$commit = ''; $dirty = $false
try {
    $commit = (& git -C $root rev-parse HEAD 2>$null).Trim()
    $dirty  = [bool](& git -C $root status --porcelain 2>$null)
} catch { }

Note "engine $version  config=$Config  tree=$BuildDir  commit=$(if ($commit) { $commit.Substring(0,7) } else { '?' })$(if ($dirty) { ' (DIRTY)' })"

# ---------------------------------------------------------------------------------------------
# 2. Parse the allowlist.
# ---------------------------------------------------------------------------------------------
$allowPath = Join-Path $PSScriptRoot 'payload.allowlist'
if (-not (Test-Path -LiteralPath $allowPath)) {
    Write-Host "[stage] missing $allowPath" -ForegroundColor Red
    exit 3  # ExitCode.Environment
}

$sections = @{ bin = @(); samples = @(); engine = @() }
$current = $null
foreach ($raw in Get-Content -LiteralPath $allowPath) {
    $line = $raw.Trim()
    if ($line -eq '' -or $line.StartsWith('#')) { continue }
    if ($line -match '^\[(\w+)\]$') {
        $current = $Matches[1]
        if (-not $sections.ContainsKey($current)) {
            Write-Host "[stage] unknown allowlist section [$current]" -ForegroundColor Red
            exit 1  # ExitCode.Failed (malformed allowlist)
        }
        continue
    }
    if (-not $current) {
        Write-Host "[stage] allowlist entry '$line' appears before any [section]" -ForegroundColor Red
        exit 1  # ExitCode.Failed (malformed allowlist)
    }

    # `<pattern>  ?AVER_SOMETHING` ties an entry to a CMake option, which is what lets one allowlist
    # describe every edition instead of one per edition.
    $cond = $null
    if ($line -match '^(.*?)\s+\?([A-Za-z0-9_]+)\s*$') {
        $line = $Matches[1].Trim()
        $cond = $Matches[2]
        if (-not $options.ContainsKey($cond)) {
            Fail "allowlist entry '$line' is conditional on $cond, which $BuildDir\CMakeCache.txt does not mention"
        }
    }
    $sections[$current] += [pscustomobject]@{ Pattern = $line; Condition = $cond }
}
Note ("allowlist: {0} bin, {1} samples, {2} engine patterns" -f
      $sections.bin.Count, $sections.samples.Count, $sections.engine.Count)

# ---------------------------------------------------------------------------------------------
# 3. Resolve patterns to files.
#
#    A pattern matching NOTHING is an error. Silent non-matches are how a payload loses a file
#    when a build layout moves, and the resulting install fails at run time on someone else's
#    machine rather than here.
# ---------------------------------------------------------------------------------------------
$skipExt = @('.pdb', '.ilk', '.exp')

function Resolve-Pattern {
    param([string] $Base, [string] $Pattern)
    # Windows PowerShell has no globstar: Get-ChildItem treats '**' exactly like '*' and matches
    # within one path segment, so 'Scripting/**' would quietly miss nested directories. A trailing
    # '**' is therefore handled here as "this directory, recursively" rather than passed through.
    $p = ($Pattern -replace '/', '\').TrimEnd('\')
    $recurse = $false
    if ($p -eq '**') { $p = ''; $recurse = $true }
    elseif ($p -like '*\**' -and $p.EndsWith('\**')) { $p = $p.Substring(0, $p.Length - 3); $recurse = $true }

    $full = if ($p) { Join-Path $Base $p } else { $Base }
    $hits = @()
    if ($recurse) {
        if (Test-Path -LiteralPath $full -PathType Container) {
            $hits = @(Get-ChildItem -LiteralPath $full -Recurse -File -ErrorAction SilentlyContinue)
        }
    } elseif ($full -match '[*?]') {
        $hits = @(Get-ChildItem -Path $full -File -ErrorAction SilentlyContinue)
    } elseif (Test-Path -LiteralPath $full -PathType Container) {
        $hits = @(Get-ChildItem -LiteralPath $full -Recurse -File -ErrorAction SilentlyContinue)
    } elseif (Test-Path -LiteralPath $full -PathType Leaf) {
        $hits = @(Get-Item -LiteralPath $full)
    }
    ,@($hits | Where-Object { $skipExt -notcontains $_.Extension.ToLower() })
}

# path-in-output -> source FileInfo
$plan = [ordered]@{}

$excludedByOption = 0

function Add-Section {
    param([string] $Name, [string] $Base, [string] $Prefix)
    foreach ($entry in $sections[$Name]) {
        $pattern = $entry.Pattern
        $hits = Resolve-Pattern -Base $Base -Pattern $pattern

        if ($entry.Condition -and -not $options[$entry.Condition]) {
            # The module is OFF, so this file must not exist. A reconfigured tree keeps the DLL from
            # the previous configure -- Ninja has no reason to delete an output it no longer builds --
            # so without this check a "Minimal" edition ships the binaries it claims not to have.
            if ($hits.Count -gt 0) {
                Fail ("$($entry.Condition)=OFF but [$Name] '$pattern' still matched $($hits.Count) file(s) in " +
                      "$Base. That tree holds stale output from an earlier configure; build this edition " +
                      "in its own directory (build.ps1 -BuildDir) or delete the tree and rebuild.")
            } else {
                # Counted only when the entry was genuinely absent. Reporting a failed entry as
                # "skipped" would describe a refusal as a clean omission.
                $script:excludedByOption++
            }
            continue
        }

        if ($hits.Count -eq 0) {
            Fail "allowlist pattern [$Name] '$pattern' matched no files under $Base"
            continue
        }
        foreach ($f in $hits) {
            $rel = $f.FullName.Substring($Base.Length).TrimStart('\')
            $dest = if ($Prefix) { Join-Path $Prefix $rel } else { $rel }
            $plan[$dest] = $f
        }
    }
}

Add-Section -Name 'bin'    -Base $bin  -Prefix 'bin'
if ($WithSamples) { Add-Section -Name 'samples' -Base $bin -Prefix 'bin' }
# [engine] patterns are repo-relative and keep their own path, which is what puts scripting\csharp\
# where ProjectScaffold.cpp's walk-up expects it.
foreach ($entry in $sections['engine']) {
    if ($entry.Condition -and -not $options[$entry.Condition]) { $excludedByOption++; continue }
    $hits = Resolve-Pattern -Base $root -Pattern $entry.Pattern
    # Generated build output under the C# projects is not source and must not ship.
    $hits = @($hits | Where-Object { $_.FullName -notmatch '\\(bin|obj)\\' })
    if ($hits.Count -eq 0) { Fail "allowlist pattern [engine] '$($entry.Pattern)' matched no files under $root"; continue }
    foreach ($f in $hits) { $plan[$f.FullName.Substring($root.Length).TrimStart('\')] = $f }
}
if ($excludedByOption -gt 0) {
    Note "$excludedByOption allowlist entr$(if ($excludedByOption -eq 1) {'y'} else {'ies'}) skipped: their module is OFF in this edition"
}

if ($failures.Count -gt 0) {
    Write-Host "[stage] FAILED with $($failures.Count) error(s) before copying anything" -ForegroundColor Red
    exit 1  # ExitCode.Failed (core/ErrorCodes.hpp); count printed above
}

# ---------------------------------------------------------------------------------------------
# 4. Copy.
# ---------------------------------------------------------------------------------------------
if (Test-Path -LiteralPath $Out) {
    $existing = @(Get-ChildItem -LiteralPath $Out -Force)
    if ($existing.Count -gt 0) {
        if (-not $Force) {
            Write-Host "[stage] $Out is not empty -- pass -Force to replace it" -ForegroundColor Red
            exit 2  # ExitCode.Usage
        }
        Remove-Item -LiteralPath $Out -Recurse -Force
    }
}
$outFull = (New-Item -ItemType Directory -Path $Out -Force).FullName

foreach ($dest in $plan.Keys) {
    $target = Join-Path $outFull $dest
    $dir = Split-Path -Parent $target
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    Copy-Item -LiteralPath $plan[$dest].FullName -Destination $target -Force
}
Note "copied $($plan.Count) files -> $outFull"

# ---------------------------------------------------------------------------------------------
# 4b. Stage the Visual C++ runtime, app-local.
#
#     MSVCP140.dll / VCRUNTIME140.dll / VCRUNTIME140_1.dll ship with Visual Studio, NOT with
#     Windows -- only the UCRT (api-ms-win-crt-*) is part of the OS. 36 of the 69 native binaries
#     in a Release tree import them, so a payload without them does not start on a clean machine;
#     it fails at load with a dialog naming a DLL the user has never heard of.
#
#     App-local deployment of these is the sanctioned alternative to making the user run
#     vc_redist.x64.exe first, and "run this installer before the game works" is not a shippable
#     product. Copied next to the binaries that import them.
#
#     Which files get copied is decided by the import tables, not by a hardcoded list, so a
#     toolset bump that adds (say) msvcp140_atomic_wait.dll is picked up rather than silently
#     dropped. If the redist cannot be located we FAIL: a payload that will not start is worse
#     than no payload, and the whole point of this script is to not find that out later.
# ---------------------------------------------------------------------------------------------
$vcRuntimePattern = $AverVcRuntimePattern
$wantCrt = @{}
foreach ($f in (Get-ChildItem -LiteralPath $outFull -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll' })) {
    foreach ($imp in (Get-PeImports -Path $f.FullName)) {
        if ($imp -match $vcRuntimePattern) { $wantCrt[$imp.ToLower()] = $true }
    }
}

$stagedCrt = @()
if ($wantCrt.Count -eq 0) {
    Note 'no Visual C++ runtime imports found -- nothing to stage'
} else {
    $crtDir = Find-VcRedistDir

    if (-not $crtDir) {
        Fail ('cannot locate the Visual C++ redistributable DLLs (' + (($wantCrt.Keys | Sort-Object) -join ', ') +
              '). Looked in $env:VCToolsRedistDir and vswhere''s VC\Redist\MSVC. Install the ' +
              '"MSVC v143 - VS 2022 C++ x64/x86 build tools" component, or stage from a VS developer prompt.')
    } else {
        $binOut = Join-Path $outFull 'bin'
        if (-not (Test-Path -LiteralPath $binOut)) { New-Item -ItemType Directory -Path $binOut -Force | Out-Null }
        foreach ($name in ($wantCrt.Keys | Sort-Object)) {
            $src = Get-ChildItem -LiteralPath $crtDir.FullName -File | Where-Object { $_.Name.ToLower() -eq $name } | Select-Object -First 1
            if (-not $src) {
                Fail "the Visual C++ redist at $($crtDir.FullName) does not contain '$name', which staged binaries import"
                continue
            }
            Copy-Item -LiteralPath $src.FullName -Destination (Join-Path $binOut $src.Name) -Force
            $stagedCrt += $src.Name
        }
        if ($stagedCrt.Count -gt 0) {
            Note ("staged Visual C++ runtime from $($crtDir.FullName): " + ($stagedCrt -join ', '))
        }
    }
}

# ---------------------------------------------------------------------------------------------
# 5. THIRD-PARTY-NOTICES.txt.
#
#    Required, and not discharged by whatever licence the binaries themselves ship under: Roboto is
#    Apache-2.0 (notice mandatory), Jolt and Dear ImGui are MIT (copyright notice mandatory). This
#    concatenates the vendored licence files rather than restating them, so it cannot drift.
#
#    Paths below are REPO-RELATIVE, not relative to third_party/. Jolt's vendored tree moved to
#    modules/physics.jolt/ -- it is the engine's rigid-body backend and is named like every other
#    backend in the tree -- but moving it changed nothing about the obligation, and the notice is not
#    something that may quietly stop being emitted because a directory was renamed. A missing file is
#    a hard Fail here for exactly that reason.
# ---------------------------------------------------------------------------------------------
$notices = [System.Text.StringBuilder]::new()
[void]$notices.AppendLine("Aver Engine $version - licence and third-party notices")
[void]$notices.AppendLine('')
[void]$notices.AppendLine('Aver Engine is licensed under the GNU Lesser General Public License, version 2.1')
[void]$notices.AppendLine('(LGPL-2.1-only); its source is at https://github.com/Vectoric-Core-Systems/Aver-Engine.')
[void]$notices.AppendLine('This build also includes the following third-party components. All licences are')
[void]$notices.AppendLine('reproduced in full below.')
[void]$notices.AppendLine('')

$components = @(
    # Aver Engine itself, first: LGPL-2.1-only (LICENSE). The LGPL obliges every copy of the
    # engine's binaries to travel with its text.
    @{ Name = 'Aver Engine';           Licence = 'LGPL-2.1-only';   File = 'LICENSE' }
    @{ Name = 'Roboto (fonts)';        Licence = 'Apache-2.0';      File = 'third_party\fonts\LICENSE' }
    # Material Icons is Apache-2.0 like Roboto and shares that same LICENSE file, but it is listed
    # as its own component deliberately: NOTICE has to name what is redistributed, and two fonts
    # from different upstreams happening to carry one licence text is not a reason to name only one.
    @{ Name = 'Material Icons (font)'; Licence = 'Apache-2.0';      File = 'third_party\fonts\LICENSE' }
    @{ Name = 'Dear ImGui';            Licence = 'MIT';             File = 'third_party\imgui\LICENSE.txt' }
    @{ Name = 'AMD FidelityFX Denoiser'; Licence = 'MIT';           File = 'third_party\fidelityfx-denoiser\LICENSE.txt' }
    @{ Name = 'Jolt Physics';          Licence = 'MIT';             File = 'modules\physics.jolt\LICENSE' }
    @{ Name = 'stb (stb_image, stb_image_write)'; Licence = 'MIT / public domain'; File = 'third_party\stb\LICENSE.txt' }
)
foreach ($c in $components) {
    $p = Join-Path $root $c.File
    if (-not (Test-Path -LiteralPath $p)) { Fail "third-party licence file missing: $p"; continue }
    [void]$notices.AppendLine('=' * 78)
    [void]$notices.AppendLine("$($c.Name) - $($c.Licence)")
    [void]$notices.AppendLine('=' * 78)
    [void]$notices.AppendLine('')
    [void]$notices.AppendLine((Get-Content -LiteralPath $p -Raw).TrimEnd())
    [void]$notices.AppendLine('')
}
[void]$notices.AppendLine('=' * 78)
[void]$notices.AppendLine('Microsoft DirectX Shader Compiler - dxcompiler.dll, dxil.dll')
[void]$notices.AppendLine('=' * 78)
[void]$notices.AppendLine('')
[void]$notices.AppendLine('Redistributed from the Windows SDK under the Microsoft Windows SDK licence terms.')
[void]$notices.AppendLine('These are required at run time: HLSL is compiled on the fly, and dxil.dll is the')
[void]$notices.AppendLine('signing library without which drivers reject the compiled shaders.')
[void]$notices.AppendLine('')
[void]$notices.AppendLine('Microsoft .NET host - nethost.dll')
[void]$notices.AppendLine('')
[void]$notices.AppendLine('Redistributed from the Microsoft.NETCore.App host pack under the .NET Library')
[void]$notices.AppendLine('licence terms.')
# Roslyn, and CONDITIONALLY, because it only reaches the payload through Tools/** which is gated on
# AVER_MODULE_PBR (payload.allowlist:105). A notice for a component that is not there would be as
# wrong as the missing one this fixes: docs/PACKAGING.md recorded for months that
# Microsoft.CodeAnalysis(.CSharp).dll ships in bin/Tools while THIRD-PARTY-NOTICES.txt never named
# it, which is an MIT obligation unmet. Detected from the STAGED tree rather than from a build flag,
# so the notice tracks what was actually copied.
$stagedRoslyn = @(Get-ChildItem -LiteralPath $outFull -Recurse -File -Filter 'Microsoft.CodeAnalysis*.dll' |
                  ForEach-Object { $_.Name } | Sort-Object -Unique)
if ($stagedRoslyn.Count -gt 0) {
    $roslynLic = Join-Path $root 'third_party\nuget\LICENSE.roslyn.txt'
    if (-not (Test-Path -LiteralPath $roslynLic)) { Fail "third-party licence file missing: $roslynLic" }
    else {
        [void]$notices.AppendLine('')
        [void]$notices.AppendLine('=' * 78)
        [void]$notices.AppendLine('.NET Compiler Platform ("Roslyn") - ' + ($stagedRoslyn -join ', '))
        [void]$notices.AppendLine('=' * 78)
        [void]$notices.AppendLine('')
        [void]$notices.AppendLine((Get-Content -LiteralPath $roslynLic -Raw).TrimEnd())
    }
}
if ($stagedCrt.Count -gt 0) {
    [void]$notices.AppendLine('')
    [void]$notices.AppendLine('Microsoft Visual C++ Runtime - ' + ($stagedCrt -join ', '))
    [void]$notices.AppendLine('')
    [void]$notices.AppendLine('Redistributed under the Distributable Code terms of the Microsoft Visual Studio')
    [void]$notices.AppendLine('licence. Deployed application-local, as those terms permit. These are required at')
    [void]$notices.AppendLine('run time and are not part of Windows.')
}
Set-Content -LiteralPath (Join-Path $outFull 'THIRD-PARTY-NOTICES.txt') -Value $notices.ToString() -Encoding utf8
Note 'wrote THIRD-PARTY-NOTICES.txt'

# ---------------------------------------------------------------------------------------------
# 6. Two import-table checks over the STAGED tree.
#
#    (a) No binary imports the debug CRT. That one ships with Visual Studio and may not be
#        redistributed at all, so it is a licensing failure, not a runtime one.
#
#    (b) IMPORT CLOSURE: every DLL any staged binary imports is either provided by Windows or
#        present in the payload. This is the check that (a) alone could never be: for a week this
#        script rejected the debug CRT with a real PE walk while saying nothing about 36 of 69
#        binaries importing the RELEASE CRT, which is equally absent from Windows. A payload can
#        be perfectly licence-clean and still not start.
#
#    (b) is deliberately a closure check rather than three more filenames. The failure mode here is
#        not "we forgot MSVCP140" -- it is "a dependency was added and nobody thought about
#        staging", and only a closure check catches the next one.
# ---------------------------------------------------------------------------------------------
$debugCrt = $AverDebugCrtPattern
$pes = Get-ChildItem -LiteralPath $outFull -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll' }

# Anything staged anywhere in the payload satisfies an import; the loader's search path is not
# this simple, but a name present in the tree is at least not a name that is missing from it.
$stagedNames = @{}
foreach ($f in $pes) { $stagedNames[$f.Name.ToLower()] = $true }

$checked = 0
$unresolved = @{}
foreach ($pe in $pes) {
    $imports = Get-PeImports -Path $pe.FullName
    if ($imports.Count -eq 0) { continue }   # managed assemblies have no classic import table
    $checked++
    foreach ($imp in $imports) {
        if ($imp -match $debugCrt) {
            # -AllowDebugCrt is documented as "for a local-only tree", but this check used to fire
            # unconditionally, so passing the switch still failed the run and the flag did nothing.
            # Honour it here, loudly: the tree is still undistributable, it is just not an error.
            if ($AllowDebugCrt) {
                Note "$($pe.FullName.Substring($outFull.Length+1)) imports the debug CRT '$imp' -- LOCAL USE ONLY, not redistributable (-AllowDebugCrt)"
            } else {
                Fail "$($pe.FullName.Substring($outFull.Length+1)) imports the debug CRT '$imp' -- this payload cannot be redistributed"
            }
            continue
        }
        $lower = $imp.ToLower()
        if ($lower -like 'api-ms-win-*') { continue }
        if ($osProvided -contains $lower) { continue }
        if ($stagedNames.ContainsKey($lower)) { continue }
        if (-not $unresolved.ContainsKey($lower)) { $unresolved[$lower] = New-Object System.Collections.Generic.List[string] }
        $unresolved[$lower].Add($pe.FullName.Substring($outFull.Length + 1))
    }
}
Note "import table checked on $checked of $($pes.Count) binaries (managed assemblies have none)"

foreach ($miss in ($unresolved.Keys | Sort-Object)) {
    $by = $unresolved[$miss]
    $eg = if ($by.Count -gt 3) { "$($by[0]), $($by[1]), $($by[2]) and $($by.Count - 3) more" } else { $by -join ', ' }
    Fail "'$miss' is imported by $($by.Count) staged binary(ies) but is neither an OS DLL nor staged -- the payload will not start without it. Imported by: $eg"
}
if ($unresolved.Count -eq 0) { Note 'import closure OK -- every imported DLL is OS-provided or staged' }

# ---------------------------------------------------------------------------------------------
# 7. payload.json -- what averdist reads to build the feed manifest.
# ---------------------------------------------------------------------------------------------
$staged = Get-ChildItem -LiteralPath $outFull -Recurse -File
$modules = [ordered]@{}
foreach ($k in ($options.Keys | Sort-Object)) { $modules[$k] = $options[$k] }

$payload = [ordered]@{
    schemaVersion = 1
    engineName    = 'Aver'
    version       = $version
    config        = $Config
    stagedUtc     = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    withSamples   = [bool]$WithSamples
    entryPoint    = 'bin\Sandbox.exe'
    sourceCommit  = $commit
    sourceDirty   = $dirty
    options       = $modules
    fileCount     = $staged.Count
    totalBytes    = ($staged | Measure-Object Length -Sum).Sum
}
# No BOM: Windows PowerShell's -Encoding utf8 emits one, and a BOM makes payload.json fail a strict
# UTF-8 JSON reader (System.Text.Json's JsonDocument.Parse over bytes rejects it).
[System.IO.File]::WriteAllText(
    (Join-Path $outFull 'payload.json'),
    ($payload | ConvertTo-Json -Depth 6),
    (New-Object System.Text.UTF8Encoding($false)))

# ---------------------------------------------------------------------------------------------
Write-Host ''
Note ("payload: {0} files, {1:N2} MB" -f $staged.Count, (($staged | Measure-Object Length -Sum).Sum / 1MB))
if ($failures.Count -gt 0) {
    Write-Host "[stage] FAILED with $($failures.Count) error(s)" -ForegroundColor Red
    exit 1  # ExitCode.Failed (core/ErrorCodes.hpp); count printed above
}
Note "OK -> $outFull"
exit 0
