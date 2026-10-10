<#
.SYNOPSIS
    Verifies a staged game package actually runs, and is not secretly reading the tree that built it.

.DESCRIPTION
    The claim worth disproving about any package is "it only works on the machine that made it".
    Copying the output somewhere else and running it there is the cheapest way to find out, and it
    is not something a developer reliably does by hand.

    THE MECHANISM, and why the working directory matters more than the copy. The package is copied
    to a fresh %TEMP% directory and run with the process working directory set to C:\ rather than to
    the package. Two properties fall out:

      - Nothing up that chain contains both cmake/AvModule.cmake and modules/, the two markers
        EngineScaffold::engineRoot() requires, so the entire walk-up class of bug is dead by
        construction rather than by inspection.
      - A working directory that is not the package root exposes any relative path the runtime
        resolves against cwd instead of against its own location.

    THE ISOLATION ASSERTION, implemented 2026-08-02. The run passes --trace-opens and this script
    asserts every [open] path is inside the scratch copy. A relative path counts as outside by
    definition, because the working directory is C:\.

    WHAT THE TRACE COVERS, measured rather than assumed. The first version of this hooked only
    platform::readFileBytes/readFileText and was described as "the whole engine file API". It is
    not: the TEXT loaders (OcProject, OcWorld, OcMap, OcMat, OcBeam) go through those, but the
    BINARY ones (Avr1, OcMesh, OcAnim) open their own ifstream. A packaged game reported exactly
    2 traced opens -- the manifest and the level -- and would have passed while loading every mesh
    from the dev tree. Those three loaders now call aver::traceFileOpen explicitly.

    That distinction is the entire point. A package that falls back to a dev-tree asset RUNS
    PERFECTLY on the machine that built it: exit 0, device created, frames drawn. Only the list of
    what it actually opened tells the two apart, and a run that logs no [open] lines at all is
    treated as a FAILURE rather than a pass, because it means the flag is not wired and this script
    proved nothing.

    Its limits, stated rather than implied: the trace does NOT cover LoadLibraryW (dxcompiler, dxil,
    nethost, hostfxr), the CLR's own assembly probing, or stbi_load's internal fopen.

    TWO LIMITS MEASURED RATHER THAN GUESSED, both found by deliberately breaking a good package and
    watching this script pass anyway:

      1. DELETING vcruntime140.dll DOES NOT FAIL THE RUN on a developer machine, because the VC++
         Redistributable is installed system-wide and the loader finds the copy in System32. The
         app-local copy exists for machines WITHOUT it, which by definition are not this one. So a
         missing CRT cannot be caught here at all -- the static import-closure check in
         stage-game.ps1 is the thing that catches it, and that one is proven to bite.

      2. DELETING AN ENGINE DLL used to pass too, and no longer does. RESOLVED 2026-08-02 by the
         C1-C11 lift. When this note was first written AverGame.exe's import table named no
         Aver.*.dll at all -- it linked only static libraries, and the engine DLLs were staged for a
         game that could not yet load a level or draw a world. The instruction left here was to
         re-run the sabotage once those lifts landed, and to treat a still-passing run as proof the
         package was shipping DLLs nothing used.

         Re-run after C11: deleting Aver.Scene.dll now fails with exit -1073741515
         (0xC0000135, STATUS_DLL_NOT_FOUND). The check has teeth because the game finally uses what
         it ships. Keep this paragraph: the next person to add a staged DLL should ask the same
         question about it.

.PARAMETER Package
    The directory produced by stage-game.ps1.

.PARAMETER Frames
    How many frames to run. Headless by default, so no window appears.

.EXAMPLE
    ./scripts/verify-game.ps1 -Package ..\stage\SkyForge
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Package,
    [int] $Frames = 30,
    [switch] $Windowed,
    [switch] $KeepScratch,
    # The editor to compare against for the divergence gate. Defaults to the build tree matching
    # the configuration the package was staged from, which game.json records.
    [string] $Editor = '',
    # Skip the divergence gate outright. For a machine that has a package and no engine tree.
    [switch] $SkipDivergence
)

$ErrorActionPreference = 'Stop'
$failures = New-Object System.Collections.Generic.List[string]
# The configuration game.json says this package was staged from. Empty when game.json is missing or
# unparseable, which is already a failure by the time the gate reads it.
$stagedConfig = ''
function Fail([string] $m) { $script:failures.Add($m); Write-Host "[verify] ERROR $m" -ForegroundColor Red }
function Note([string] $m) { Write-Host "[verify] $m" }

if (-not (Test-Path -LiteralPath $Package)) { Write-Host "[verify] no package at $Package" -ForegroundColor Red; exit 2 }  # ExitCode.Usage (core/ErrorCodes.hpp)
$pkg = (Resolve-Path -LiteralPath $Package).Path

# ---------------------------------------------------------------------------------------------
# 1. Static checks. These cost nothing and catch the things a run cannot.
# ---------------------------------------------------------------------------------------------
# AverCrashReporter.exe is on this list because its absence was invisible in exactly the way this
# script exists to prevent: the game installs the crash handler, writes a complete crash folder, and
# then spawns a reporter that is not in the package. Nothing fails, nothing logs, and the player sees
# the process disappear. A missing file that only matters after a crash will not be noticed by
# testing the happy path, so it has to be asserted here.
foreach ($required in @('AverEngineRuntime.exe', 'AverCrashReporter.exe', 'game.json', 'Game.ocproject', 'THIRD-PARTY-NOTICES.txt')) {
    if (-not (Test-Path -LiteralPath (Join-Path $pkg $required))) { Fail "the package has no $required" }
}

$gameJsonPath = Join-Path $pkg 'game.json'
if (Test-Path -LiteralPath $gameJsonPath) {
    $bytes = [System.IO.File]::ReadAllBytes($gameJsonPath)
    # A BOM makes game.json fail a strict UTF-8 JSON reader; System.Text.Json's JsonDocument.Parse
    # over bytes rejects one outright, so this would break the game rather than look untidy.
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        Fail 'game.json begins with a UTF-8 BOM, which a strict JSON reader rejects'
    }
    try {
        $game = Get-Content -LiteralPath $gameJsonPath -Raw | ConvertFrom-Json
        # enableUi RECORDS THE TREE, IT NO LONGER IMPLIES THE BINARY. This was a failure, on the
        # reasoning that a UI tree links ImGui into everything it builds. The ImGui/RHI split
        # (c71e3fc) ended that: Aver.RHI.D3D12 never links imgui, the concrete implementation lives
        # in the separate Aver.RHI.D3D12.ImGui module, and only Sandbox links it -- so an
        # AVER_ENABLE_UI=ON tree produces a clean AverEngineRuntime.exe. stage-game.ps1 already proves that
        # per package by scanning the actual staged binary for ImGui markers, which is a real check
        # where this one was an inference. Kept as a note, because "which tree was this staged from"
        # is still worth seeing in the log.
        if ($game.enableUi) { Note 'staged from a UI tree (enableUi=true) -- stage-game.ps1 byte-scans the binary to prove no ImGui reached it' }
        Note "game.json: '$($game.name)' entry=$($game.entryPoint) startMap=$($game.startMap) files=$($game.fileCount)"
        # Kept for the divergence gate below, which has to compare against an editor built the SAME
        # way. stage-game.ps1 writes this field from its own -Config.
        $script:stagedConfig = $game.config
    } catch {
        Fail "game.json is not valid JSON: $($_.Exception.Message)"
    }
}

# Nothing that is a source file, a build intermediate or a debug symbol.
foreach ($f in (Get-ChildItem -LiteralPath $pkg -Recurse -File)) {
    $rel = $f.FullName.Substring($pkg.Length + 1)
    if ($f.Extension -in '.cs', '.fs', '.csproj', '.fsproj', '.pdb', '.ilk', '.exp', '.lib') {
        Fail "'$rel' should not be in a shipped package"
    }
}

# No absolute path from the build machine baked into a TEXT file the game reads.
#
# Filtered with Where-Object and NOT with -Include, which PowerShell silently ignores when the path
# came from -LiteralPath without a wildcard. The first version of this check used -Include and
# therefore scanned every .dll as well, reporting eight "failures" for managed assemblies that
# legitimately carry their source paths in the debug directory. That is an information leak worth
# its own conversation, not a reason a package will not run, and it is not what this check is for.
$textExt = @('.json', '.ocproject', '.ocmap', '.ocworld', '.ocmat', '.txt', '.ini', '.cfg')
foreach ($f in (Get-ChildItem -LiteralPath $pkg -Recurse -File | Where-Object { $textExt -contains $_.Extension.ToLower() })) {
    $text = Get-Content -LiteralPath $f.FullName -Raw -ErrorAction SilentlyContinue
    if ($text -and $text -match '[A-Za-z]:\\\\?(Users|Program Files|Windows)') {
        Fail "'$($f.Name)' contains an absolute path from the build machine"
    }
}

# ---------------------------------------------------------------------------------------------
# 2. The isolation run.
# ---------------------------------------------------------------------------------------------
$scratch = Join-Path $env:TEMP ("aver-game-verify-" + [guid]::NewGuid().ToString('N').Substring(0, 12))
Note "copying package to $scratch"
Copy-Item -LiteralPath $pkg -Destination $scratch -Recurse -Force

$exe = Join-Path $scratch 'AverEngineRuntime.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    Fail 'AverEngineRuntime.exe is missing from the scratch copy'
} else {
    $args = @('--frames', "$Frames", '--trace-opens')
    if (-not $Windowed) { $args += '--headless' }

    # cwd = C:\, deliberately. See the description: it is the working directory rather than the copy
    # that makes this a real isolation test.
    Note "running: AverEngineRuntime.exe $($args -join ' ')   (cwd = C:\)"
    $proc = Start-Process -FilePath $exe -ArgumentList $args -WorkingDirectory 'C:\' `
                          -NoNewWindow -Wait -PassThru `
                          -RedirectStandardOutput (Join-Path $scratch 'run.out') `
                          -RedirectStandardError  (Join-Path $scratch 'run.err')
    $code = $proc.ExitCode
    # BOTH STREAMS. This read run.out alone, and the engine writes part of its log to stderr -- so
    # the "[RHI] device created" assertion below could never see that line and reported a healthy
    # package as one that never made a device. It found the [open] trace only because those happen to
    # land on stdout. Concatenating is right either way: this is one log split across two handles,
    # not two logs.
    $out = ''
    foreach ($stream in @('run.out', 'run.err')) {
        $p = Join-Path $scratch $stream
        if (Test-Path -LiteralPath $p) { $out += (Get-Content -LiteralPath $p -Raw -ErrorAction SilentlyContinue) }
    }

    if ($code -ne 0) {
        Fail "the packaged game exited $code"
        if ($out) { Write-Host $out }
    } else {
        Note "the packaged game ran $Frames frame(s) and exited 0, from outside the tree that built it"
    }

    # The run must have got far enough to make a device. An exit code of 0 from a process that
    # failed to initialise and gave up quietly would otherwise read as a pass.
    if ($out -notmatch '\[RHI\] device created') { Fail 'the run never reported creating an RHI device' }
    if ($out -match 'ImGui')                     { Fail 'the packaged game initialised ImGui -- it was staged from a UI tree' }

    # ------------------------------------------------------------------------------------------
    # THE ISOLATION ASSERTION. --trace-opens logs one [open] line per path the engine reads through
    # platform::readFileBytes/readFileText, which is the WHOLE engine file API. Every one must be
    # inside the scratch copy.
    #
    # This is the check the rest of this script only approximates. A package that falls back to a
    # dev-tree asset RUNS PERFECTLY on the machine that built it, exits 0, creates a device and
    # draws -- and fails on every other machine. Nothing but the list of what it actually opened
    # distinguishes the two.
    #
    # Its limits, stated rather than implied: it does NOT cover LoadLibraryW (dxcompiler, dxil,
    # nethost, hostfxr), the CLR's own assembly probing, or stbi_load's internal fopen.
    # ------------------------------------------------------------------------------------------
    $opened = [regex]::Matches($out, '(?m)^\[INFO\s*\]\s*\[open\]\s*(.+?)\s*$') |
              ForEach-Object { $_.Groups[1].Value }
    if ($opened.Count -eq 0) {
        Fail 'the run logged no [open] lines at all -- --trace-opens is not wired, so this script proved nothing about isolation'
    } else {
        $outside = @()
        foreach ($path in $opened) {
            # Relative paths are resolved against the process working directory, which this script
            # deliberately sets to C:\ -- so a relative path is BY DEFINITION outside the package.
            if (-not [System.IO.Path]::IsPathRooted($path)) { $outside += $path; continue }
            $full = [System.IO.Path]::GetFullPath($path)
            if (-not $full.StartsWith($scratch, [StringComparison]::OrdinalIgnoreCase)) { $outside += $full }
        }
        Note "traced $($opened.Count) file open(s), $($outside.Count) outside the package"
        foreach ($p in ($outside | Select-Object -Unique)) {
            Fail "the packaged game opened '$p', which is outside the package"
        }
    }
}

# ---------------------------------------------------------------------------------------------
# 4. THE DIVERGENCE GATE -- does the game load the same scene the editor does?
#
# This is the check whose absence deleted AverGame.exe. The removal commit's reason was that a
# second host "rendered a different subset of the scene than the editor", and that it "made a
# graph-only project look broken this afternoon when the real gap was possession". Nothing in the
# tree could notice: there is no CI, no packaging test, and verify-payload.ps1 compares Sandbox.exe
# against Sandbox.exe.
#
# A CENSUS, NOT A FRAME DIFF, and deliberately. The editor's 3D viewport is a sub-rect of its window
# with panels around it, so the two hosts have different aspect ratios and therefore different
# projections; and their cameras differ by design. A pixel comparison would be dominated by framing
# and would fail for reasons that have nothing to do with divergence. What the removal complained
# about was the SUBSET, so that is what is compared: entity counts, mesh and material counts, and an
# order-independent hash of every (mesh, material) pair. See modules/world/.../SceneCensus.hpp.
#
# SKIPPED RATHER THAN FAILED when the editor is not present. This script's subject is a PACKAGE, and
# a package legitimately ships without Sandbox.exe beside it; a missing editor means this particular
# question cannot be asked, not that the answer is bad.
if (-not $SkipDivergence) {
    # THE DEFAULT FOLLOWS THE PACKAGE, NOT THE REPO'S DEBUG TREE. It used to be a hardcoded
    # 'build/bin/Sandbox.exe' while stage-game.ps1 defaults to -Config Release and stages out of
    # build-release. The default pairing therefore compared a DEBUG editor against a RELEASE
    # runtime, and neither script said so. The census is configuration-independent in principle, so
    # this would usually still pass -- which is worse than failing, because it means the gate was
    # quietly answering a question nobody asked. Same derivation stage-game.ps1:67-70 uses.
    $treeFor = { param($cfg) if ($cfg -eq 'Debug') { 'build' } else { "build-$($cfg.ToLower())" } }
    $editorExe = if ($Editor) {
        $Editor
    } elseif ($stagedConfig) {
        Join-Path (Split-Path -Parent $PSScriptRoot) "$(& $treeFor $stagedConfig)/bin/Sandbox.exe"
    } else {
        Join-Path (Split-Path -Parent $PSScriptRoot) 'build/bin/Sandbox.exe'
    }
    if ($stagedConfig) { Note "divergence gate: package staged $stagedConfig, comparing against $editorExe" }
    $pkgProject = Join-Path $pkg 'Game.ocproject'
    if (-not (Test-Path -LiteralPath $editorExe)) {
        Note "divergence gate SKIPPED -- no editor at $editorExe (pass -Editor to point at one)"
    } elseif (-not (Test-Path -LiteralPath $pkgProject)) {
        Note 'divergence gate SKIPPED -- the package has no Game.ocproject to open in both hosts'
    } else {
        function Get-Census([string] $exe, [string[]] $extra) {
            # NOT $args: that is a PowerShell automatic variable, and assigning to it inside a
            # function shadows the caller's in a way that bites much later than it is written.
            $runArgs = @($pkgProject, '--frames', "$Frames", '--scene-census') + $extra
            # THROUGH A FILE, NOT `2>&1`. Redirecting a native executable's stderr inside PowerShell
            # wraps every line in an ErrorRecord, which under this script's $ErrorActionPreference
            # of 'Stop' turns the engine's ordinary [WARN ] lines into a terminating NativeCommandError
            # -- measured: the first version of this died on "cannot enable streaming".
            $log = [System.IO.Path]::GetTempFileName()
            try {
                Start-Process -FilePath $exe -ArgumentList $runArgs -NoNewWindow -Wait `
                              -RedirectStandardOutput $log -RedirectStandardError "$log.err" | Out-Null
                $text = (Get-Content -LiteralPath $log -Raw -ErrorAction SilentlyContinue) +
                        (Get-Content -LiteralPath "$log.err" -Raw -ErrorAction SilentlyContinue)
            } finally {
                Remove-Item -LiteralPath $log, "$log.err" -Force -ErrorAction SilentlyContinue
            }
            $m = [regex]::Match($text, '\[Census\]\s*(entities=.+?)\s*$', 'Multiline')
            if ($m.Success) { return $m.Groups[1].Value.Trim() }
            return ''
        }
        # --no-editor-chrome is not about pixels here -- the census does not look at any -- but the
        # editor must not be left drawing a gizmo over a selection it made on load, which spawns
        # nothing but keeps the two runs honestly comparable in every other respect too.
        #
        # --open-legacy IS LOAD-BEARING, and without it this gate could never pass on any package
        # made from a project older than the current series. The editor refuses to open such a
        # project when the run has a frame limit, because the upgrade prompt is modal and nothing
        # can answer it -- it says so and carries on with no project, so the census came back
        # entities=0 while the game reported the real scene, and the gate called that a divergence.
        # Opening as-is is also the only honest comparison: the packaged runtime has no upgrade
        # prompt and no way to rewrite the manifest it ships with, so it always opens the staged
        # manifest exactly as staged. The editor has to be asked the same question.
        $censusEditor = Get-Census $editorExe @('--no-vsync', '--no-editor-chrome', '--open-legacy')
        $censusGame   = Get-Census (Join-Path $scratch 'AverEngineRuntime.exe') @()

        if (-not $censusEditor) {
            Fail 'the editor printed no [Census] line -- --scene-census is not wired, so divergence was not checked'
        } elseif (-not $censusGame) {
            Fail 'the packaged game printed no [Census] line -- --scene-census is not wired, so divergence was not checked'
        } elseif ($censusEditor -ne $censusGame) {
            Fail 'the game loaded a DIFFERENT scene than the editor'
            Note "  editor: $censusEditor"
            Note "  game  : $censusGame"
        } else {
            Note "divergence gate OK -- both hosts loaded: $censusEditor"
        }
    }
}

if (-not $KeepScratch -and (Test-Path -LiteralPath $scratch)) {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
} elseif ($KeepScratch) {
    Note "scratch kept at $scratch"
}

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host "[verify] FAILED with $($failures.Count) error(s)" -ForegroundColor Red
    # The COUNT is on the line above, where a count belongs. The exit code is from the engine's own
    # table (modules/core/include/aver/core/ErrorCodes.hpp): 0 ok, 1 failed. This used to exit the
    # count, so two failures exited 2 -- which that table spells "you called it wrong" -- and 256 of
    # them would have exited 0 once the shell truncated it to a byte.
    exit 1
}
Note 'OK -- package verified (see the description for what this does NOT yet check)'
exit 0
