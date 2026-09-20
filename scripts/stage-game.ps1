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

        <out>\AverEngineRuntime.exe, the engine DLLs, dxcompiler/dxil/nethost, the VC++ runtime
        <out>\Scripting\          bridge + contract assemblies
        <out>\Game.ocproject      rewritten manifest -- every source KEY carried through, CONTENT -> Content
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

.PARAMETER Compile
    Build the project's C#/F# before staging, instead of requiring it to have been built already.

    This does NOT reimplement the build. It runs the editor's own -compile-scripts, which shells out to
    `dotnet build` with flags that carry real reasoning (-c Release because every contract assembly is
    Release; -p:UseSharedCompilation=false and -nodeReuse:false because both MSBuild process pools
    outlive the build). A second copy of that command line in PowerShell would drift from the first one
    silently, and the symptom would be a package built with different flags than the editor uses.

    MEASURED, because the plan this came from claimed the opposite: -compile-scripts already needs no
    human, and works on a WARP software device, so a machine with no GPU can do it. What it does need is
    a device of SOME kind, since the trigger lives inside the editor's Tools menu machinery.

    Off by default: staging should not silently rebuild a developer's assemblies underneath them.

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
    [switch] $AllowDebugCrt,
    [switch] $Compile,
    # See the NVIDIA clearance gate immediately below.
    [switch] $IAcceptNvidiaRedistribution
)

$ErrorActionPreference = 'Stop'

# ---- PACKAGING IS BLOCKED PENDING NVIDIA CLEARANCE (2026-09-21) --------------------------------
#
# THIS IS DELIBERATE AND IT IS NOT A BUG. A packaged build redistributes NVIDIA SDK code and
# currently ships NO third-party notice of any kind with it, which the NVIDIA RTX SDKs Licence does
# not allow. Rather than produce packages that are not clear to distribute, the packaging path
# refuses until that is settled. The engine is in beta; nothing is shipping today, so the cheap and
# honest answer is to stop rather than to paper over it.
#
# WHAT IS ACTUALLY OUTSTANDING, all recorded in docs/NVIDIA-SDK-COMPLIANCE.md:
#   * NRD is compiled into the runtime, and RTXDI's HLSL ships as VERBATIM NVIDIA SOURCE TEXT under
#     shaders/Rtxdi. The licence requires the notice "This software contains source code provided
#     by NVIDIA Corporation." to accompany distributed source.
#   * Clause 6.1(c) of the RTX Supplement routes attribution, for a product with no credit screen,
#     to "end user documentation for the application". LICENSE.md is the source repository's
#     documentation; a packaged game carries none of it.
#   * The MIT components (NVIDIA MathLib, ShaderMake, Dear ImGui, stb, meshoptimizer, Jolt, DXC)
#     each require their copyright and permission notices to travel with a binary distribution.
#     The $components list further down this script, which builds THIRD-PARTY-NOTICES.txt, names
#     none of them.
#
# TO LIFT THIS: settle the notices, then delete this block. -IAcceptNvidiaRedistribution exists so
# that somebody who HAS obtained clearance can proceed without editing the script, and it prints a
# loud line into the log when used so a package built that way is identifiable afterwards. It is
# not a way to skip the work.
if (-not $IAcceptNvidiaRedistribution) {
    Write-Host ""
    Write-Host "PACKAGING BLOCKED -- pending NVIDIA redistribution clearance." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  A packaged build redistributes NVIDIA SDK code (NRD, compiled in; RTXDI, as"
    Write-Host "  verbatim HLSL source under shaders/Rtxdi) and currently ships no third-party"
    Write-Host "  notice with it. See docs/NVIDIA-SDK-COMPLIANCE.md section 3."
    Write-Host ""
    Write-Host "  This is a deliberate block while the engine is in beta, not a failure."
    Write-Host "  Re-run with -IAcceptNvidiaRedistribution once clearance is in hand."
    Write-Host ""
    exit 2
}
Write-Host "[stage] -IAcceptNvidiaRedistribution was passed: this package is being built WITHOUT" -ForegroundColor Yellow
Write-Host "[stage] the third-party notices described in docs/NVIDIA-SDK-COMPLIANCE.md section 3." -ForegroundColor Yellow
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
# 1. The build tree's identity.
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

# THE BLANKET REFUSAL THAT USED TO LIVE HERE IS GONE. It read "AVER_ENABLE_UI=ON means this tree's
# AverGame.exe has ImGui in it" straight out of CMakeCache.txt, which was true only because
# Aver.RHI.D3D12 used to link imgui PUBLIC and Aver.Runtime linked that backend PUBLIC in turn (the
# ImGui/RHI split closed that: see modules/rhi.d3d12/CMakeLists.txt and
# include/aver/rhi/d3d12/UiBackend.hpp). A tree configured AVER_ENABLE_UI=ON is no longer evidence of
# anything about AverEngineRuntime.exe by itself -- only Sandbox links the concrete ImGui backend now, in every
# configuration. Refusing to stage from such a tree today would be refusing a perfectly good package
# on a premise that stopped being true.
#
# What replaces it is refusal 8d, far below: a scan of the ACTUAL STAGED AverEngineRuntime.exe for ImGui
# evidence, which is the real thing this script ever needed to guarantee. Kept as a check on the
# binary rather than removed outright -- belt-and-braces against a future regression of the coupling
# this file used to work around, caught with real evidence instead of a policy on a config flag.
if (-not $options['AVER_BUILD_GAME']) {
    Fail "this tree is configured AVER_BUILD_GAME=OFF, so it contains no AverEngineRuntime.exe to stage."
}
if ($Config -eq 'Debug' -and -not $AllowDebugCrt) {
    Fail ('refusing to stage a Debug build: it imports the debug CRT, which ships with Visual ' +
          'Studio and may not be redistributed. Stage Release, or pass -AllowDebugCrt for local use.')
}

# ---------------------------------------------------------------------------------------------
# 2. The project manifest. OC dialect: `#` comments, `KEY value`, one per line.
# ---------------------------------------------------------------------------------------------
# Read once, as raw lines rather than only into the hashtable below: section 6 carries every KEY
# value line of THIS SAME read through into the staged manifest, in its original order, so it
# needs the lines verbatim, not just the three keys this section cares about.
$manifestRawLines = Get-Content -LiteralPath $Project
$manifest = @{}
foreach ($raw in $manifestRawLines) {
    $line = $raw.Trim()
    if ($line -eq '' -or $line.StartsWith('#')) { continue }
    if ($line -match '^(\w+)\s+(.*)$') { $manifest[$Matches[1].ToUpper()] = $Matches[2].Trim() }
}
$gameName    = if ($manifest.ContainsKey('NAME'))     { $manifest['NAME'] }     else { 'Aver Game' }
$contentRel  = if ($manifest.ContainsKey('CONTENT'))  { $manifest['CONTENT'] }  else { 'Content' }
$startMap    = if ($manifest.ContainsKey('STARTMAP')) { $manifest['STARTMAP'] } else { '' }
if (-not $startMap) { Fail "the manifest has no STARTMAP, so the packaged game has no level to open." }
# `\w+` NEVER MATCHES A DOTTED KEY, so this section's own regex stops at the first `.` -- every
# RENDER.*/PHYSICS.*/AUDIO.*/WINDOW.*/IMPORT.*/STREAM.* key and INPUT.SCHEME is invisible to
# $manifest above. Harmless HERE, because none of NAME/CONTENT/STARTMAP has a dot. Section 6's own
# carry-through cannot reuse this pattern for that exact reason -- see it for why.

$contentSrc  = Join-Path $projectDir $contentRel
$binariesSrc = Join-Path $projectDir 'Binaries'
if (-not (Test-Path -LiteralPath $contentSrc)) { Fail "the manifest's CONTENT '$contentRel' does not exist at $contentSrc" }

Note "project '$gameName'  content=$contentRel  startmap=$startMap  tree=$BuildDir"

# Compiled scripts must exist and must not be older than their sources: a package built from stale
# script DLLs runs the PREVIOUS version of the game, silently.
#
# ONLY WHEN THE PROJECT ACTUALLY HAS C# TO COMPILE. This check used to be unconditional, which made
# every GRAPH-DRIVEN game impossible to package -- and graph-driven is the headline authoring story
# here. Measured: test-content/AN_Playable has ZERO .cs files, runs correctly (three Aver Node
# classes declared from .ocgraph, "0 behaviour(s) live" reported as the normal state, not an error),
# and could not be packaged by any means, because it was being asked for an artefact it does not use
# and has no way to produce. Every project on this machine was in that state.
#
# .ocgraph classes are NOT compiled into Binaries\Scripts. They are read from Content at runtime by
# HostBridge.DeclareGraphClasses and compiled to IL in-process, so they ride along in the ordinary
# content copy below and need nothing from this gate.
#
# The staleness half still applies to any project that DOES have sources -- shipping yesterday's
# assemblies silently is the failure this whole block exists to prevent -- but it is NOT untouched, as
# an earlier draft of this comment claimed: it carried the same silently-ignored -Include bug fixed
# below, so it had been comparing every file under Content/Scripts against the DLLs, not just .cs/.fs.
$scriptsBin = Join-Path $binariesSrc 'Scripts'
$scriptSrcDir = Join-Path $contentSrc 'Scripts'
$authoredScripts = @()
if (Test-Path -LiteralPath $scriptSrcDir) {
    # obj/ and bin/ excluded for the reason the staleness check below spells out at length: they are
    # build output living inside the source tree, and counting them here would demand a compile for a
    # project whose only .cs files are MSBuild's own generated ones.
    # -Include IS SILENTLY IGNORED WITH -LiteralPath. Get-ChildItem only applies -Include when the
    # PATH itself carries a wildcard, so `-LiteralPath <dir> -Recurse -Include *.cs` returns EVERY
    # file under <dir> and filters nothing at all. Measured on AN_Playable, whose Content/Scripts
    # holds three .ocgraph files and no C# whatsoever: it reported "3 authored script source(s)".
    # Extension filtering is therefore done in Where-Object, which has no such rule.
    $authoredScripts = @(Get-ChildItem -LiteralPath $scriptSrcDir -Recurse -File -ErrorAction SilentlyContinue |
                         Where-Object { $_.Extension -in '.cs', '.fs' } |
                         Where-Object { $_.FullName -notmatch '[\\/](obj|bin)[\\/]' })
}

# -Compile: build the scripts here rather than demanding they were built already.
#
# THROUGH THE EDITOR'S OWN --compile-scripts, not a second `dotnet build` written in PowerShell. The
# real invocation carries flags with reasoning behind them (-c Release because every contract assembly
# is Release, so a project's own Scripts.dll would otherwise be the one Debug assembly among them;
# -p:UseSharedCompilation=false and -nodeReuse:false because the compiler server and the MSBuild worker
# nodes each hold assemblies open and both outlive the build). Copying that command line here would
# give it two homes and one of them would go stale, and the symptom -- a package built with different
# flags than the editor uses -- is exactly the kind that surfaces months later.
#
# MEASURED FIRST, because the plan that asked for this claimed --compile-scripts "only arms the GUI
# button": it does not. It builds Scripts.dll AND Scripts.FSharp.dll with no human, and it does so on a
# WARP software device, so a runner with no GPU can package a game. What it genuinely needs is a device
# of SOME kind, because the trigger lives in the editor's Tools menu machinery.
if ($Compile -and $authoredScripts.Count -gt 0) {
    $sandbox = Join-Path $bin 'Sandbox.exe'
    if (-not (Test-Path -LiteralPath $sandbox)) {
        Fail "-Compile needs $sandbox, which this tree does not have (a game-only tree cannot compile scripts)."
    } else {
        Note "compiling $($authoredScripts.Count) script source(s) via $sandbox --compile-scripts"
        # --frames 60 is generous on purpose. The build is armed a few frames in and runs on a
        # background thread, and ~ToolsMenu joins that thread at shutdown, so the process cannot exit
        # mid-build however few frames are asked for -- the margin is for the editor reaching the point
        # where it fires at all, not for the compile itself.
        # ErrorActionPreference IS RELAXED ACROSS THIS CALL, and it is not optional. This script runs
        # with 'Stop', and in Windows PowerShell 5.1 a NATIVE executable writing anything to stderr is
        # wrapped in an ErrorRecord (NativeCommandError) -- which under 'Stop' terminates the script
        # even when the exe exits 0. The editor emits ordinary warnings on stderr as a matter of course
        # ("[WARN] [ChunkWorld] cannot enable streaming..."), so staging died on a line that meant
        # nothing. The EXIT CODE is the contract here, not the presence of stderr output.
        # START-PROCESS, NOT `& $sandbox`, AND THIS ONE COULD SHIP A STALE PACKAGE. Sandbox.exe is
        # linked /SUBSYSTEM:WINDOWS as of 0.5.0 so the editor opens no console window, and Windows
        # PowerShell neither waits for nor captures a GUI-subsystem process: the call returns
        # immediately, $compileLog comes back EMPTY and $LASTEXITCODE is not set at all.
        #
        # NOT SETTING IT IS THE DANGEROUS HALF. $LASTEXITCODE keeps whatever a PREVIOUS native command
        # left there, so this does not reliably fail -- it can read 0 from something unrelated, fall
        # through to the Test-Path below, find compiled assemblies left over from an EARLIER build,
        # and package those. A staging step that silently ships stale scripts is worse than one that
        # breaks loudly. gates.ps1, pt-compare.ps1 and run.ps1 were converted when the subsystem
        # changed; this call site was missed.
        #
        # -Wait waits whatever the subsystem is. The redirect files replace what `2>&1` used to
        # capture, and both streams are read back so the failure tail below still has something to
        # print. The ErrorActionPreference dance is gone with the pipeline that needed it: no native
        # command writes to this shell's stderr any more, so there is no NativeCommandError to relax.
        $compileOut = [System.IO.Path]::GetTempFileName()
        $compileErr = [System.IO.Path]::GetTempFileName()
        try {
            $proc = Start-Process -FilePath $sandbox -Wait -NoNewWindow -PassThru `
                        -ArgumentList @('--project', $Project, '--frames', '60', '--compile-scripts') `
                        -RedirectStandardOutput $compileOut -RedirectStandardError $compileErr
            $compileCode = $proc.ExitCode
            $compileLog = @(Get-Content -LiteralPath $compileOut -ErrorAction SilentlyContinue) +
                          @(Get-Content -LiteralPath $compileErr -ErrorAction SilentlyContinue)
        } finally {
            Remove-Item -LiteralPath $compileOut, $compileErr -Force -ErrorAction SilentlyContinue
        }

        if ($compileCode -ne 0) {
            # The tail only, and only on failure: the editor logs hundreds of lines about shaders and
            # assets that have nothing to do with why a build broke.
            $compileLog | Select-Object -Last 25 | ForEach-Object { Write-Host "[game]   $_" }
            Fail "the editor exited $compileCode while compiling scripts"
        } elseif (-not (Test-Path -LiteralPath $scriptsBin)) {
            # A clean exit that produced nothing means the build failed, or the project has no
            # Scripts.csproj. Either way the package would be wrong, and silence here would ship it.
            Fail "the compile ran but produced no $scriptsBin -- the build failed, or there is no Scripts.csproj"
        } else {
            Note "compiled: $((Get-ChildItem -LiteralPath $scriptsBin -Filter *.dll).Count) assembly(ies) in $scriptsBin"
        }
    }
}

if ($authoredScripts.Count -eq 0) {
    Note "no authored C#/F# under $scriptSrcDir -- skipping the compiled-scripts check (a graph-only game compiles its .ocgraph classes at runtime)"
} elseif (-not (Test-Path -LiteralPath $scriptsBin)) {
    Fail "$($authoredScripts.Count) authored script source(s) under $scriptSrcDir but no compiled scripts at $scriptsBin -- run Compile .NET in the editor before packaging."
} else {
    $newestDll = (Get-ChildItem -LiteralPath $scriptsBin -Filter *.dll -ErrorAction SilentlyContinue |
                  Sort-Object LastWriteTimeUtc | Select-Object -Last 1)
    if (-not $newestDll) {
        Fail "$scriptsBin contains no .dll -- the scripts have never been compiled."
    } else {
        $srcDir = Join-Path $contentSrc 'Scripts'
        if (Test-Path -LiteralPath $srcDir) {
            # AUTHORED SOURCES ONLY -- obj/ and bin/ are excluded, and without that this check can
            # never pass. MSBuild regenerates .AssemblyInfo.cs / .GlobalUsings.g.cs under
            # obj/<Config>/<TFM>/ on EVERY build, so one of them is always newer than the assembly
            # it helped produce. The recursive scan picked those up and reported
            #     script sources are NEWER than the compiled assemblies (net10.0 > Scripts.dll)
            # -- "net10.0" being an obj intermediate directory, not anything a person wrote --
            # which made packaging impossible no matter how many times Compile .NET was run.
            #
            # The content copy below already drops obj/bin for exactly the same reason: they are
            # build output living inside the source tree, not content.
            # SAME -Include-IS-IGNORED BUG AS ABOVE, and it was here first. With -LiteralPath this
            # scanned EVERY file under Content/Scripts rather than just .cs/.fs -- so on a project
            # holding both C# and .ocgraph, touching a graph made the newest "source" a .ocgraph and
            # reported "script sources are NEWER than the compiled assemblies", demanding a C#
            # recompile that could not have changed anything. Filtered where it is honoured.
            $newestSrc = (Get-ChildItem -LiteralPath $srcDir -Recurse -File -ErrorAction SilentlyContinue |
                          Where-Object { $_.Extension -in '.cs', '.fs' } |
                          Where-Object { $_.FullName -notmatch '[\\/](obj|bin)[\\/]' } |
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
#
#    EVERY KEY VALUE LINE OF THE SOURCE MANIFEST CARRIES THROUGH, in its original order. This used
#    to write only OCPROJECT/NAME/CONTENT/STARTMAP, which silently dropped every RENDER.*,
#    PHYSICS.*, AUDIO.*, WINDOW.*, IMPORT.*, STREAM.*, DRONE.GRAPH and INPUT.SCHEME key the source
#    project stated -- a packaged game opened with none of the render/physics/audio tuning, and no
#    default input scheme, its own project asked for, silently falling back to engine defaults
#    instead. Only CONTENT is rewritten, to the staged folder name; NAME and STARTMAP are
#    re-emitted from the values section 2 already parsed and validated (STARTMAP is refused above
#    when empty) rather than copied as raw text, so a manifest with more than one NAME/STARTMAP
#    line -- last one wins, the same rule loadOcproject itself applies when it reads the file back
#    -- lands in the package exactly as this script already logged and checked it, not as
#    whichever line happened to come first.
#
#    THE KEY PATTERN HERE IS `\S+`, NOT section 2's `\w+`. Section 2 only ever needs NAME/CONTENT/
#    STARTMAP, none of which contains a dot, so its narrower regex was never wrong for its own
#    purpose. But every SECTION.NAME key in this dialect -- RENDER.GI, INPUT.SCHEME, WINDOW.SIZE,
#    PHYSICS.GRAVITY, AUDIO.BUS and the rest -- does contain one, and `\w+` stops at the first dot.
#    Reusing section 2's pattern here would carry through NONE of them: the exact bug this section
#    exists to fix, just moved one step later.
# ---------------------------------------------------------------------------------------------
$carried = New-Object System.Collections.Generic.List[string]
$sawName = $false; $sawContent = $false; $sawStartMap = $false
foreach ($raw in $manifestRawLines) {
    $line = $raw.Trim()
    if ($line -eq '' -or $line.StartsWith('#')) { continue }
    if ($line -match '^(\S+)\s+(.*)$') {
        $key = $Matches[1]
        $value = $Matches[2].Trim()
        $keyUpper = $key.ToUpper()
        if ($keyUpper -eq 'OCPROJECT') {
            continue   # replaced by this script's own header line below, never duplicated
        } elseif ($keyUpper -eq 'CONTENT') {
            $carried.Add("$key Content"); $sawContent = $true
        } elseif ($keyUpper -eq 'NAME') {
            $carried.Add("$key $gameName"); $sawName = $true
        } elseif ($keyUpper -eq 'STARTMAP') {
            $carried.Add("$key $startMap"); $sawStartMap = $true
        } else {
            $carried.Add("$key $value")
        }
    }
}
# Defensive only -- ProjectScaffold always writes all three, so a real project never reaches this,
# but a hand-edited manifest missing one of them still produces a loadable package rather than one
# silently missing NAME, CONTENT or STARTMAP.
if (-not $sawName)     { $carried.Add("NAME $gameName") }
if (-not $sawContent)  { $carried.Add("CONTENT Content") }
if (-not $sawStartMap) { $carried.Add("STARTMAP $startMap") }

$outManifest = "OCPROJECT 1`n" +
               "# Written by scripts/stage-game.ps1. This is a PACKAGED GAME, not an editable project.`n" +
               (($carried -join "`n") + "`n")
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
    entryPoint    = 'AverEngineRuntime.exe'
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

# 8c. AverEngineRuntime.exe must actually be there. It is the entry point game.json names.
$gameExe = Join-Path $outFull 'AverEngineRuntime.exe'
if (-not (Test-Path -LiteralPath $gameExe)) { Fail 'AverEngineRuntime.exe is not in the package' }

# 8d. AverEngineRuntime.exe must not contain Dear ImGui. THIS is what makes refusal 1's old blanket
#     AVER_ENABLE_UI=ON rejection unnecessary rather than merely relaxed: it checks the actual staged
#     artifact instead of a config flag that used to (but no longer does) imply the same thing.
#
#     Dear ImGui is statically linked into the executable -- there is no imgui.dll for
#     Get-PeImports' import-table walk to find absent, the way section 8b proves the CRT and other
#     DLL dependencies. The only real evidence for a STATICALLY linked dependency is inside the
#     binary's own bytes, so this is a plain string search, not an import-table check: every marker
#     below is a symbol or literal that can only exist in this .exe if ImGui code was compiled into
#     it. Grounded in an actual scan of a built AverGame.exe (the runtime's name then) from an AVER_ENABLE_UI=ON tree, not
#     assumed -- see the imgui-split work that added this check for the dumpbin/strings output that
#     picked these specific markers.
if (Test-Path -LiteralPath $gameExe) {
    $exeText = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($gameExe))
    $imguiMarkers = @('ImGui_ImplDX12_Init', 'ImGui_ImplWin32_Init', 'ImGui_ImplDX12_RenderDrawData', 'Dear ImGui')
    $found = @($imguiMarkers | Where-Object { $exeText.Contains($_) })
    if ($found.Count -gt 0) {
        Fail ("AverEngineRuntime.exe contains Dear ImGui evidence (" + ($found -join ', ') + ") -- the RHI/UI " +
              "split is not clean in this build tree. A shipped game must never link the editor's UI toolkit.")
    } else {
        Note "AverEngineRuntime.exe: no Dear ImGui evidence found (checked $($imguiMarkers.Count) markers) -- clean"
    }
}

# ---------------------------------------------------------------------------------------------
Write-Host ''
Note ("package: {0} files, {1:N2} MB" -f $staged.Count, (($staged | Measure-Object Length -Sum).Sum / 1MB))
if ($failures.Count -gt 0) {
    Write-Host "[game] FAILED with $($failures.Count) error(s)" -ForegroundColor Red
    # The count is on the line above. The code is from modules/core/include/aver/core/ErrorCodes.hpp:
    # 0 ok, 1 failed. Exiting the count made two failures indistinguishable from a usage error.
    exit 1
}
Note "OK -> $outFull"
exit 0
