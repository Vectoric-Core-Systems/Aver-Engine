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

    WHAT THIS DOES NOT YET CHECK, stated plainly rather than implied. The design calls for a
    --trace-opens flag on AverGame that logs every path through platform::readFileBytes /
    readFileText and lets this script assert that all of them are under the package root. That flag
    is not implemented, so today this proves the package RUNS in isolation, not that it opened
    nothing outside itself. The difference matters: a package that falls back to a dev-tree asset
    and carries on would pass this script and fail on a player's machine.

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
    [switch] $KeepScratch
)

$ErrorActionPreference = 'Stop'
$failures = New-Object System.Collections.Generic.List[string]
function Fail([string] $m) { $script:failures.Add($m); Write-Host "[verify] ERROR $m" -ForegroundColor Red }
function Note([string] $m) { Write-Host "[verify] $m" }

if (-not (Test-Path -LiteralPath $Package)) { throw "[verify] no package at $Package" }
$pkg = (Resolve-Path -LiteralPath $Package).Path

# ---------------------------------------------------------------------------------------------
# 1. Static checks. These cost nothing and catch the things a run cannot.
# ---------------------------------------------------------------------------------------------
foreach ($required in @('AverGame.exe', 'game.json', 'Game.ocproject', 'THIRD-PARTY-NOTICES.txt')) {
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
        if ($game.enableUi) { Fail 'game.json says enableUi=true -- this package was staged from a UI tree and links ImGui' }
        Note "game.json: '$($game.name)' entry=$($game.entryPoint) startMap=$($game.startMap) files=$($game.fileCount)"
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

$exe = Join-Path $scratch 'AverGame.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    Fail 'AverGame.exe is missing from the scratch copy'
} else {
    $args = @('--frames', "$Frames")
    if (-not $Windowed) { $args += '--headless' }

    # cwd = C:\, deliberately. See the description: it is the working directory rather than the copy
    # that makes this a real isolation test.
    Note "running: AverGame.exe $($args -join ' ')   (cwd = C:\)"
    $proc = Start-Process -FilePath $exe -ArgumentList $args -WorkingDirectory 'C:\' `
                          -NoNewWindow -Wait -PassThru `
                          -RedirectStandardOutput (Join-Path $scratch 'run.out') `
                          -RedirectStandardError  (Join-Path $scratch 'run.err')
    $code = $proc.ExitCode
    $out = ''
    if (Test-Path -LiteralPath (Join-Path $scratch 'run.out')) { $out = Get-Content -LiteralPath (Join-Path $scratch 'run.out') -Raw }

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
}

if (-not $KeepScratch -and (Test-Path -LiteralPath $scratch)) {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
} elseif ($KeepScratch) {
    Note "scratch kept at $scratch"
}

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host "[verify] FAILED with $($failures.Count) error(s)" -ForegroundColor Red
    exit $failures.Count
}
Note 'OK -- package verified (see the description for what this does NOT yet check)'
exit 0
