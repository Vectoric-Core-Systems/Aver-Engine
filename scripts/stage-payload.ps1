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

if (-not (Test-Path -LiteralPath $bin)) {
    throw "[stage] no build tree at $bin -- run ./scripts/build.ps1 $(if ($Config -ne 'Debug') {'-Release'}) first"
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
if (-not (Test-Path -LiteralPath $cachePath)) { throw "[stage] no CMakeCache.txt in $tree" }
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
if (-not (Test-Path -LiteralPath $allowPath)) { throw "[stage] missing $allowPath" }

$sections = @{ bin = @(); samples = @(); engine = @() }
$current = $null
foreach ($raw in Get-Content -LiteralPath $allowPath) {
    $line = $raw.Trim()
    if ($line -eq '' -or $line.StartsWith('#')) { continue }
    if ($line -match '^\[(\w+)\]$') {
        $current = $Matches[1]
        if (-not $sections.ContainsKey($current)) { throw "[stage] unknown allowlist section [$current]" }
        continue
    }
    if (-not $current) { throw "[stage] allowlist entry '$line' appears before any [section]" }
    $sections[$current] += $line
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

function Add-Section {
    param([string] $Name, [string] $Base, [string] $Prefix)
    foreach ($pattern in $sections[$Name]) {
        $hits = Resolve-Pattern -Base $Base -Pattern $pattern
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
foreach ($pattern in $sections['engine']) {
    $hits = Resolve-Pattern -Base $root -Pattern $pattern
    # Generated build output under the C# projects is not source and must not ship.
    $hits = @($hits | Where-Object { $_.FullName -notmatch '\\(bin|obj)\\' })
    if ($hits.Count -eq 0) { Fail "allowlist pattern [engine] '$pattern' matched no files under $root"; continue }
    foreach ($f in $hits) { $plan[$f.FullName.Substring($root.Length).TrimStart('\')] = $f }
}

if ($failures.Count -gt 0) {
    Write-Host "[stage] FAILED with $($failures.Count) error(s) before copying anything" -ForegroundColor Red
    exit $failures.Count
}

# ---------------------------------------------------------------------------------------------
# 4. Copy.
# ---------------------------------------------------------------------------------------------
if (Test-Path -LiteralPath $Out) {
    $existing = @(Get-ChildItem -LiteralPath $Out -Force)
    if ($existing.Count -gt 0) {
        if (-not $Force) { throw "[stage] $Out is not empty -- pass -Force to replace it" }
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
# 5. THIRD-PARTY-NOTICES.txt.
#
#    Required, and not discharged by whatever licence the binaries themselves ship under: Roboto is
#    Apache-2.0 (notice mandatory), Jolt and Dear ImGui are MIT (copyright notice mandatory). This
#    concatenates the vendored licence files rather than restating them, so it cannot drift.
# ---------------------------------------------------------------------------------------------
$tp = Join-Path $root 'third_party'
$notices = [System.Text.StringBuilder]::new()
[void]$notices.AppendLine("Aver Engine $version - third-party notices")
[void]$notices.AppendLine('')
[void]$notices.AppendLine('This build includes the following third-party components. Their licences are')
[void]$notices.AppendLine('reproduced in full below.')
[void]$notices.AppendLine('')

$components = @(
    @{ Name = 'Roboto (fonts)';        Licence = 'Apache-2.0';      File = 'fonts\LICENSE' }
    @{ Name = 'Dear ImGui';            Licence = 'MIT';             File = 'imgui\LICENSE.txt' }
    @{ Name = 'Jolt Physics';          Licence = 'MIT';             File = 'JoltPhysics\LICENSE' }
    @{ Name = 'stb (stb_image, stb_image_write)'; Licence = 'MIT / public domain'; File = 'stb\LICENSE.txt' }
)
foreach ($c in $components) {
    $p = Join-Path $tp $c.File
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
Set-Content -LiteralPath (Join-Path $outFull 'THIRD-PARTY-NOTICES.txt') -Value $notices.ToString() -Encoding utf8
Note 'wrote THIRD-PARTY-NOTICES.txt'

# ---------------------------------------------------------------------------------------------
# 6. Verify no staged binary imports the debug CRT.
#
#    A real import-directory walk, not a string search: the CRT names appear as plain ASCII all over
#    a PE for unrelated reasons (debug paths, embedded manifests), so a grep both false-positives
#    and gives no way to tell an import from a mention.
# ---------------------------------------------------------------------------------------------
function Get-PeImports {
    param([string] $Path)
    $b = [System.IO.File]::ReadAllBytes($Path)
    if ($b.Length -lt 0x40 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { return @() }   # 'MZ'
    $peOff = [BitConverter]::ToInt32($b, 0x3C)
    if ($peOff -le 0 -or $peOff + 0x18 -ge $b.Length) { return @() }
    if ([BitConverter]::ToUInt32($b, $peOff) -ne 0x00004550) { return @() }        # 'PE\0\0'

    $nSections  = [BitConverter]::ToUInt16($b, $peOff + 6)
    $optSize    = [BitConverter]::ToUInt16($b, $peOff + 20)
    $optOff     = $peOff + 24
    $magic      = [BitConverter]::ToUInt16($b, $optOff)
    # The data directory sits after the optional header's fixed part: 96 bytes for PE32, 112 for PE32+.
    $dirOff     = $optOff + $(if ($magic -eq 0x20B) { 112 } else { 96 })
    $importRva  = [BitConverter]::ToUInt32($b, $dirOff + 8)      # directory entry 1 = import
    if ($importRva -eq 0) { return @() }

    $sections = @()
    $secOff = $optOff + $optSize
    for ($i = 0; $i -lt $nSections; $i++) {
        $s = $secOff + ($i * 40)
        if ($s + 40 -gt $b.Length) { break }
        $sections += [pscustomobject]@{
            VirtualSize = [BitConverter]::ToUInt32($b, $s + 8)
            VirtualAddr = [BitConverter]::ToUInt32($b, $s + 12)
            RawSize     = [BitConverter]::ToUInt32($b, $s + 16)
            RawPtr      = [BitConverter]::ToUInt32($b, $s + 20)
        }
    }
    function ToOffset([uint32] $rva) {
        foreach ($s in $sections) {
            $span = [Math]::Max($s.VirtualSize, $s.RawSize)
            if ($rva -ge $s.VirtualAddr -and $rva -lt $s.VirtualAddr + $span) {
                return [int]($s.RawPtr + ($rva - $s.VirtualAddr))
            }
        }
        return -1
    }

    $names = @()
    $desc = ToOffset $importRva
    if ($desc -lt 0) { return @() }
    while ($desc + 20 -le $b.Length) {
        $nameRva = [BitConverter]::ToUInt32($b, $desc + 12)
        if ($nameRva -eq 0) { break }                            # null terminator descriptor
        $n = ToOffset $nameRva
        if ($n -lt 0) { break }
        $end = $n; while ($end -lt $b.Length -and $b[$end] -ne 0) { $end++ }
        $names += [System.Text.Encoding]::ASCII.GetString($b, $n, $end - $n)
        $desc += 20
    }
    ,$names
}

$debugCrt = '(?i)^(msvcp\d+d|vcruntime\d+(_\d+)?d|ucrtbased|msvcr\d+d)\.dll$'
$pes = Get-ChildItem -LiteralPath $outFull -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll' }
$checked = 0
foreach ($pe in $pes) {
    $imports = Get-PeImports -Path $pe.FullName
    if ($imports.Count -eq 0) { continue }   # managed assemblies have no classic import table
    $checked++
    foreach ($imp in $imports) {
        if ($imp -match $debugCrt) {
            Fail "$($pe.FullName.Substring($outFull.Length+1)) imports the debug CRT '$imp' -- this payload cannot be redistributed"
        }
    }
}
Note "import table checked on $checked of $($pes.Count) binaries (managed assemblies have none)"

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
    exit $failures.Count
}
Note "OK -> $outFull"
exit 0
