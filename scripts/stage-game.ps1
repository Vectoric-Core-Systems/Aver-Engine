<#
.SYNOPSIS
    Packages an Aver project into a standalone, runnable game directory.

.DESCRIPTION
    Sibling of stage-payload.ps1: that one stages the ENGINE for a developer, this one stages a
    GAME for a player. The output is a project-shaped directory that happens to contain the engine
    runtime -- loose files, no archive.

    NO ARCHIVE, and that is not a style choice. The engine cannot read one: kAvrSubtypePak is
    reserved with no implementation, parseAvr1 requires the header FileSize to equal the passed
    length exactly, refuses any compressed chunk outright, and no codec is vendored anywhere in
    third_party/. A pak is its own project.

    LAYOUT. Chosen so that no path-derivation code in the engine has to change:

        <out>\AverGame.exe, the engine DLLs, dxcompiler/dxil/nethost, the VC++ runtime
        <out>\Scripting\          bridge + contract assemblies
        <out>\Game.ocproject      rewritten manifest, CONTENT Content
        <out>\Content\            the project's content, filtered
        <out>\Binaries\           the project's compiled scripts and materials
        <out>\THIRD-PARTY-NOTICES.txt
        <out>\game.json

    ProjectDesc::binariesDir() is dir + "\Binaries" and scriptsDir() is contentDir() + "\Scripts",
    and rebuildContentIndex hashes forward-slash relative paths under Content/. Put the manifest at
    the package root with CONTENT Content and every ObjectId in every .ocworld and .ocmat is
    unchanged by packaging. That property is why loose-file packaging is nearly free here.

.PARAMETER Project
    Path to the project's .ocproject manifest.

.PARAMETER Out
    Output directory. Created if absent; must be empty or -Force'd.

.EXAMPLE
    ./scripts/stage-game.ps1 -Project "C:\Projects\SkyForge\SkyForge.ocproject" -Out ..\stage\SkyForge
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Project,
    [Parameter(Mandatory = $true)] [string] $Out,
    [string] $Config = 'Release',
    [string] $BuildDir,
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
function Fail([string] $msg) { $script:failures.Add($msg); Write-Host "[game] ERROR $msg" -ForegroundColor Red }
function Note([string] $msg) { Write-Host "[game] $msg" }

. (Join-Path $PSScriptRoot 'PeImports.ps1')

if (-not (Test-Path -LiteralPath $bin))     { throw "[game] no build tree at $bin -- run ./scripts/build.ps1 first" }
if (-not (Test-Path -LiteralPath $Project)) { throw "[game] no project manifest at $Project" }

$projectDir = Split-Path -Parent (Resolve-Path -LiteralPath $Project).Path

# ---------------------------------------------------------------------------------------------
# 1. The build tree's identity, and the one refusal that makes the ImGui workaround safe.
# ---------------------------------------------------------------------------------------------
$cachePath = Join-Path $tree 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cachePath)) { throw "[game] no CMakeCache.txt in $tree" }
$cache = Get-Content -LiteralPath $cachePath

$options = @{}
foreach ($line in $cache) {
    if ($line -match '^(AVER_[A-Z0-9_]+):BOOL=(ON|OFF)$') { $options[$Matches[1]] = ($Matches[2] -eq 'ON') }
}
foreach ($k in @('AVER_MODULE_SCENE','AVER_MODULE_FRAMEWORK','AVER_MODULE_PBR','AVER_MODULE_VOXI',
                 'AVER_MODULE_PHYSICS','AVER_MODULE_SCRIPTING','AVER_ENABLE_UI','AVER_BUILD_GAME')) {
    if (-not $options.ContainsKey($k)) {
        Fail "CMakeCache.txt does not mention $k -- this tree's edition is unknowable. Absent is not OFF; reconfigure and stage again."
    }
}

# THE REFUSAL THE SECOND BUILD TREE EXISTS FOR.
#
# Aver.RHI.D3D12 links imgui PUBLIC and propagates AVER_WITH_IMGUI=1 PUBLIC, and Aver.Runtime links
# that backend PUBLIC -- so an AverGame.exe built from an AVER_ENABLE_UI=ON tree HAS ImGui in it. It
# runs perfectly and must not ship. Two executables with the same name, one shippable and one not,
# differing by nothing a human can see; without this check the workaround is worse than the problem.
if ($options['AVER_ENABLE_UI']) {
    Fail ("this tree is configured AVER_ENABLE_UI=ON, so its AverGame.exe links Dear ImGui and is " +
          "not shippable. Configure a second tree for games:`n" +
          "    cmake -S . -B build-game -G Ninja -DCMAKE_BUILD_TYPE=$Config -DAVER_ENABLE_UI=OFF -DAVER_BUILD_SANDBOX=OFF -DAVER_BUILD_TESTS=OFF`n" +
          "then pass -BuildDir build-game.")
}
if (-not $options['AVER_BUILD_GAME']) {
    Fail "this tree is configured AVER_BUILD_GAME=OFF, so it contains no AverGame.exe to stage."
}
if ($Config -eq 'Debug' -and -not $AllowDebugCrt) {
    Fail ('refusing to stage a Debug build: it imports the debug CRT, which ships with Visual ' +
          'Studio and may not be redistributed. Stage Release, or pass -AllowDebugCrt for local use.')
}

# ---------------------------------------------------------------------------------------------
# 2. The project manifest. OC dialect: `#` comments, `KEY value`, one per line.
# ---------------------------------------------------------------------------------------------
$manifest = @{}
foreach ($raw in Get-Content -LiteralPath $Project) {
    $line = $raw.Trim()
    if ($line -eq '' -or $line.StartsWith('#')) { continue }
    if ($line -match '^(\w+)\s+(.*)$') { $manifest[$Matches[1].ToUpper()] = $Matches[2].Trim() }
}
$gameName    = if ($manifest.ContainsKey('NAME'))     { $manifest['NAME'] }     else { 'Aver Game' }
$contentRel  = if ($manifest.ContainsKey('CONTENT'))  { $manifest['CONTENT'] }  else { 'Content' }
$startMap    = if ($manifest.ContainsKey('STARTMAP')) { $manifest['STARTMAP'] } else { '' }
if (-not $startMap) { Fail "the manifest has no STARTMAP, so the packaged game has no level to open." }

$contentSrc  = Join-Path $projectDir $contentRel
$binariesSrc = Join-Path $projectDir 'Binaries'
if (-not (Test-Path -LiteralPath $contentSrc)) { Fail "the manifest's CONTENT '$contentRel' does not exist at $contentSrc" }

Note "project '$gameName'  content=$contentRel  startmap=$startMap  tree=$BuildDir"

# Compiled scripts must exist and must not be older than their sources: a package built from stale
# script DLLs runs the PREVIOUS version of the game, silently.
$scriptsBin = Join-Path $binariesSrc 'Scripts'
if (-not (Test-Path -LiteralPath $scriptsBin)) {
    Fail "no compiled scripts at $scriptsBin -- run Compile .NET in the editor before packaging."
} else {
    $newestDll = (Get-ChildItem -LiteralPath $scriptsBin -Filter *.dll -ErrorAction SilentlyContinue |
                  Sort-Object LastWriteTimeUtc | Select-Object -Last 1)
    if (-not $newestDll) {
        Fail "$scriptsBin contains no .dll -- the scripts have never been compiled."
    } else {
        $srcDir = Join-Path $contentSrc 'Scripts'
        if (Test-Path -LiteralPath $srcDir) {
            $newestSrc = (Get-ChildItem -LiteralPath $srcDir -Recurse -Include *.cs, *.fs -ErrorAction SilentlyContinue |
                          Sort-Object LastWriteTimeUtc | Select-Object -Last 1)
            if ($newestSrc -and $newestSrc.LastWriteTimeUtc -gt $newestDll.LastWriteTimeUtc) {
                Fail ("script sources are NEWER than the compiled assemblies " +
                      "($($newestSrc.Name) > $($newestDll.Name)) -- this package would ship the previous build. " +
                      "Run Compile .NET and stage again.")
            }
        }
    }
}

# ---------------------------------------------------------------------------------------------
# 3. Allowlist.
# ---------------------------------------------------------------------------------------------
$allowPath = Join-Path $PSScriptRoot 'game.allowlist'
if (-not (Test-Path -LiteralPath $allowPath)) { throw "[game] missing $allowPath" }

$sections = @{ bin = @(); scripting = @(); crt = @() }
$current = $null
foreach ($raw in Get-Content -LiteralPath $allowPath) {
    $line = $raw.Trim()
    if ($line -eq '' -or $line.StartsWith('#')) { continue }
    if ($line -match '^\[(\w+)\]$') {
        $current = $Matches[1]
        if (-not $sections.ContainsKey($current)) { throw "[game] unknown allowlist section [$current]" }
        continue
    }
    if (-not $current) { throw "[game] allowlist entry '$line' appears before any [section]" }
    $cond = $null
    if ($line -match '^(.*?)\s+\?([A-Za-z0-9_]+)\s*$') {
        $line = $Matches[1].Trim(); $cond = $Matches[2]
        if (-not $options.ContainsKey($cond)) { Fail "allowlist entry '$line' is conditional on $cond, which the cache does not mention" }
    }
    $sections[$current] += [pscustomobject]@{ Pattern = $line; Condition = $cond }
}

# ---------------------------------------------------------------------------------------------
# 4. Copy.
# ---------------------------------------------------------------------------------------------
if (Test-Path -LiteralPath $Out) {
    $existing = @(Get-ChildItem -LiteralPath $Out -Force)
    if ($existing.Count -gt 0) {
        if (-not $Force) { throw "[game] $Out is not empty -- pass -Force to replace it" }
        Remove-Item -LiteralPath $Out -Recurse -Force
    }
}
$outFull = (New-Item -ItemType Directory -Path $Out -Force).FullName

$copied = 0
foreach ($entry in ($sections.bin + $sections.scripting)) {
    # Conditions are checked BOTH WAYS: ON must match at least one file, OFF must match none. The
    # second half is what catches a reconfigured Ninja tree still holding the previous DLL.
    $wanted = ($null -eq $entry.Condition) -or $options[$entry.Condition]
    $matches = @(Get-ChildItem -LiteralPath $bin -Recurse -File -ErrorAction SilentlyContinue |
                 Where-Object { $_.FullName.Substring($bin.Length + 1).Replace('\', '/') -like $entry.Pattern.Replace('\', '/') })
    if ($matches.Count -eq 0 -and $entry.Pattern -notlike '*/**') {
        $direct = Join-Path $bin $entry.Pattern
        if (Test-Path -LiteralPath $direct) { $matches = @(Get-Item -LiteralPath $direct) }
    }
    if ($wanted -and $matches.Count -eq 0) { Fail "allowlist entry '$($entry.Pattern)' matched no file in $bin"; continue }
    if (-not $wanted) {
        if ($matches.Count -gt 0) { Fail "'$($entry.Pattern)' is gated on $($entry.Condition)=OFF but $($matches.Count) file(s) exist in $bin -- the tree is stale" }
        continue
    }
    foreach ($m in $matches) {
        # Debug symbols are never wanted in a package and a `**` entry sweeps them up: Scripting/**
        # matched five .pdb files the first time this ran. Refusal 8a below is the backstop that
        # caught it; this is the fix, so the backstop stays a backstop rather than a workflow.
        if ($m.Extension -ieq '.pdb') { continue }
        $rel = $m.FullName.Substring($bin.Length + 1)
        $dst = Join-Path $outFull $rel
        $dir = Split-Path -Parent $dst
        if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
        Copy-Item -LiteralPath $m.FullName -Destination $dst -Force
        ++$copied
    }
}
Note "copied $copied engine file(s)"

# Content, FILTERED. Dropping these changes the id of nothing, because ObjectIds hash paths and no
# shipped asset references a .cs: sources, build intermediates and debug symbols only.
$dropDirs  = @('\bin\', '\obj\', '\Source\')
$dropExt   = @('.cs', '.fs', '.csproj', '.fsproj', '.pdb', '.user', '.sln')
$contentOut = Join-Path $outFull 'Content'
$contentCopied = 0; $contentSkipped = 0
foreach ($f in (Get-ChildItem -LiteralPath $contentSrc -Recurse -File)) {
    $rel = $f.FullName.Substring($contentSrc.Length + 1)
    $relPadded = '\' + $rel + '\'
    $drop = ($dropExt -contains $f.Extension.ToLower())
    if (-not $drop) { foreach ($d in $dropDirs) { if ($relPadded -like "*$d*") { $drop = $true; break } } }
    if ($drop) { ++$contentSkipped; continue }
    $dst = Join-Path $contentOut $rel
    $dir = Split-Path -Parent $dst
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    Copy-Item -LiteralPath $f.FullName -Destination $dst -Force
    ++$contentCopied
}
Note "content: $contentCopied file(s) copied, $contentSkipped dropped (sources, bin/obj, pdb)"

# Binaries: the project's compiled scripts and materials, as-is.
$binariesCopied = 0
if (Test-Path -LiteralPath $binariesSrc) {
    foreach ($f in (Get-ChildItem -LiteralPath $binariesSrc -Recurse -File)) {
        if ($f.Extension -ieq '.pdb') { continue }
        $rel = $f.FullName.Substring($binariesSrc.Length + 1)
        $dst = Join-Path (Join-Path $outFull 'Binaries') $rel
        $dir = Split-Path -Parent $dst
        if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
        Copy-Item -LiteralPath $f.FullName -Destination $dst -Force
        ++$binariesCopied
    }
}
Note "binaries: $binariesCopied file(s)"

# ---------------------------------------------------------------------------------------------
# 5. The Visual C++ runtime, app-local. Decided by the import tables, not by a hardcoded list.
# ---------------------------------------------------------------------------------------------
$wantCrt = @{}
foreach ($f in (Get-ChildItem -LiteralPath $outFull -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll' })) {
    foreach ($imp in (Get-PeImports -Path $f.FullName)) {
        if ($imp -match $AverVcRuntimePattern) { $wantCrt[$imp.ToLower()] = $true }
    }
}
$stagedCrt = @()
if ($wantCrt.Count -gt 0) {
    $crtDir = Find-VcRedistDir
    if (-not $crtDir) {
        Fail ('cannot locate the Visual C++ redistributable (' + (($wantCrt.Keys | Sort-Object) -join ', ') +
              '). A package without it does not start on a clean machine. Install the MSVC v143 x64 build tools.')
    } else {
        foreach ($name in ($wantCrt.Keys | Sort-Object)) {
            $src = Get-ChildItem -LiteralPath $crtDir.FullName -File | Where-Object { $_.Name.ToLower() -eq $name } | Select-Object -First 1
            if (-not $src) { Fail "the redist at $($crtDir.FullName) has no '$name', which staged binaries import"; continue }
            Copy-Item -LiteralPath $src.FullName -Destination (Join-Path $outFull $src.Name) -Force
            $stagedCrt += $src.Name
        }
        if ($stagedCrt.Count -gt 0) { Note ("staged VC++ runtime: " + ($stagedCrt -join ', ')) }
    }
}

# ---------------------------------------------------------------------------------------------
# 6. The rewritten manifest and game.json.
#
#    .ocproject CANNOT describe a shipped game -- it has no entry point, no window defaults, no
#    build id and no icon, and the editor never writes one back. game.json carries what it cannot.
# ---------------------------------------------------------------------------------------------
$outManifest = "OCPROJECT 1`n" +
               "# Written by scripts/stage-game.ps1. This is a PACKAGED GAME, not an editable project.`n" +
               "NAME $gameName`n" +
               "CONTENT Content`n" +
               "STARTMAP $startMap`n"
Set-Content -LiteralPath (Join-Path $outFull 'Game.ocproject') -Value $outManifest -Encoding utf8

$commit = ''; $dirty = $false
try {
    $commit = (& git -C $root rev-parse HEAD 2>$null)
    if ($commit) { $commit = $commit.Trim() }
    $dirty  = [bool](& git -C $root status --porcelain 2>$null)
} catch { }

$staged = Get-ChildItem -LiteralPath $outFull -Recurse -File
$game = [ordered]@{
    schemaVersion = 1
    name          = $gameName
    entryPoint    = 'AverGame.exe'
    project       = 'Game.ocproject'
    startMap      = $startMap
    window        = [ordered]@{ width = 1280; height = 720 }
    config        = $Config
    dotnetRequired = [bool]$options['AVER_MODULE_SCRIPTING']
    enableUi      = [bool]$options['AVER_ENABLE_UI']
    sourceCommit  = $commit
    sourceDirty   = $dirty
    stagedUtc     = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    fileCount     = $staged.Count
    totalBytes    = ($staged | Measure-Object Length -Sum).Sum
}
# No BOM: System.Text.Json's JsonDocument.Parse over bytes rejects one.
[System.IO.File]::WriteAllText((Join-Path $outFull 'game.json'),
    ($game | ConvertTo-Json -Depth 6), (New-Object System.Text.UTF8Encoding($false)))

# ---------------------------------------------------------------------------------------------
# 7. Notices, DERIVED from what was actually staged.
# ---------------------------------------------------------------------------------------------
$notices = [System.Text.StringBuilder]::new()
[void]$notices.AppendLine("$gameName - third-party notices")
[void]$notices.AppendLine('')
[void]$notices.AppendLine('This game includes the following third-party components.')
[void]$notices.AppendLine('')
$components = @(
    @{ Name = 'Jolt Physics'; Licence = 'MIT'; File = 'modules\physics.jolt\LICENSE'; When = { $options['AVER_MODULE_PHYSICS'] } }
    @{ Name = 'stb (stb_image, stb_image_write)'; Licence = 'MIT / public domain'; File = 'third_party\stb\LICENSE.txt'; When = { $true } }
)
foreach ($c in $components) {
    if (-not (& $c.When)) { continue }
    $p = Join-Path $root $c.File
    if (-not (Test-Path -LiteralPath $p)) { Fail "third-party licence file missing: $p"; continue }
    [void]$notices.AppendLine('=' * 78)
    [void]$notices.AppendLine("$($c.Name) - $($c.Licence)")
    [void]$notices.AppendLine('=' * 78)
    [void]$notices.AppendLine('')
    [void]$notices.AppendLine((Get-Content -LiteralPath $p -Raw).TrimEnd())
    [void]$notices.AppendLine('')
}
[void]$notices.AppendLine('Microsoft DirectX Shader Compiler - dxcompiler.dll, dxil.dll')
[void]$notices.AppendLine('Redistributed from the Windows SDK under the Microsoft Windows SDK licence terms.')
[void]$notices.AppendLine('')
if ($options['AVER_MODULE_SCRIPTING']) {
    [void]$notices.AppendLine('Microsoft .NET host - nethost.dll')
    [void]$notices.AppendLine('Redistributed from the Microsoft.NETCore.App host pack under the .NET Library licence terms.')
    [void]$notices.AppendLine('')
}
if (Test-Path -LiteralPath (Join-Path $outFull 'Scripting\FSharp.Core.dll')) {
    [void]$notices.AppendLine('FSharp.Core - MIT')
    [void]$notices.AppendLine('Redistributed from the .NET SDK. Present because this game uses F#.')
    [void]$notices.AppendLine('')
}
if ($stagedCrt.Count -gt 0) {
    [void]$notices.AppendLine('Microsoft Visual C++ Runtime - ' + ($stagedCrt -join ', '))
    [void]$notices.AppendLine('Redistributed under the Distributable Code terms of the Microsoft Visual Studio')
    [void]$notices.AppendLine('licence, deployed application-local as those terms permit.')
}
Set-Content -LiteralPath (Join-Path $outFull 'THIRD-PARTY-NOTICES.txt') -Value $notices.ToString() -Encoding utf8

# ---------------------------------------------------------------------------------------------
# 8. Refusals over the STAGED tree.
# ---------------------------------------------------------------------------------------------
# 8a. No source or build intermediate reached Content.
foreach ($f in (Get-ChildItem -LiteralPath $outFull -Recurse -File)) {
    $rel = $f.FullName.Substring($outFull.Length + 1)
    if ($f.Extension -in '.cs', '.fs', '.csproj', '.fsproj', '.pdb') { Fail "'$rel' is a source or symbol file and must not ship" }
    if ($rel -like '*\obj\*' -or $rel -like '*\bin\*') { Fail "'$rel' is a build intermediate and must not ship" }
}

# 8b. Import closure: every DLL any staged binary imports is OS-provided or present. This is the
#     check that catches a dependency nobody thought about, which three more filenames would not.
$pes = Get-ChildItem -LiteralPath $outFull -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll' }
$stagedNames = @{}; foreach ($f in $pes) { $stagedNames[$f.Name.ToLower()] = $true }
$checked = 0; $unresolved = @{}
foreach ($pe in $pes) {
    $imports = Get-PeImports -Path $pe.FullName
    if ($imports.Count -eq 0) { continue }
    ++$checked
    foreach ($imp in $imports) {
        if ($imp -match $AverDebugCrtPattern) {
            if ($AllowDebugCrt) { Note "$($pe.Name) imports the debug CRT '$imp' -- LOCAL USE ONLY" }
            else { Fail "$($pe.Name) imports the debug CRT '$imp' -- this package cannot be redistributed" }
            continue
        }
        $l = $imp.ToLower()
        if ($l -like 'api-ms-win-*') { continue }
        if ($AverOsProvidedDlls -contains $l) { continue }
        if ($stagedNames.ContainsKey($l)) { continue }
        if (-not $unresolved.ContainsKey($l)) { $unresolved[$l] = New-Object System.Collections.Generic.List[string] }
        $unresolved[$l].Add($pe.Name)
    }
}
Note "import table checked on $checked of $($pes.Count) binaries (managed assemblies have none)"
foreach ($miss in ($unresolved.Keys | Sort-Object)) {
    Fail "'$miss' is imported by $($unresolved[$miss].Count) staged binary(ies) but is neither an OS DLL nor staged -- the game will not start. First: $($unresolved[$miss][0])"
}
if ($unresolved.Count -eq 0) { Note 'import closure OK' }

# 8c. AverGame.exe must actually be there. It is the entry point game.json names.
if (-not (Test-Path -LiteralPath (Join-Path $outFull 'AverGame.exe'))) { Fail 'AverGame.exe is not in the package' }

# ---------------------------------------------------------------------------------------------
Write-Host ''
Note ("package: {0} files, {1:N2} MB" -f $staged.Count, (($staged | Measure-Object Length -Sum).Sum / 1MB))
if ($failures.Count -gt 0) {
    Write-Host "[game] FAILED with $($failures.Count) error(s)" -ForegroundColor Red
    exit $failures.Count
}
Note "OK -> $outFull"
exit 0
