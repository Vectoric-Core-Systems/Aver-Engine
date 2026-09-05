# Re-record the oracle baselines -- build, record, show what moved, and prove it green.
#
# WHY THIS EXISTS AS A SCRIPT. Re-recording is the one operation in this repo that can destroy
# information rather than produce it: it overwrites the only record of what the renderer used to do.
# Done by hand it is four commands in the right order, and getting the order wrong is silent -- record
# against a stale build and you freeze in numbers no source tree produces. Worse, a re-record that is
# never verified afterwards looks exactly like one that was.
#
# So this does the whole move as one reviewable operation:
#
#   1. builds, so what is recorded is what the tree currently produces
#   2. snapshots the old values
#   3. records
#   4. prints EVERY value that moved, old -> new, as a table
#   5. re-runs the gates read-only and requires ALL GATES PASS
#
# Step 5 is the one people skip. A baseline that does not immediately verify green against the binary
# it was recorded from means something is non-deterministic, and that is worth knowing at the moment
# it happens rather than three commits later.
#
#   ./scripts/record-gates.ps1                  # Debug then Release, build both, verify both
#   ./scripts/record-gates.ps1 -Only Debug      # just the Debug tree
#   ./scripts/record-gates.ps1 -SkipBuild       # trust the trees as they stand
#   ./scripts/record-gates.ps1 -Config baseline # one gate configuration (skips the slow ones)
#
# Budget ~25 minutes per tree for the full sweep. WARP is ~15 of that on its own: it is a software
# rasteriser, and a GI frame on it takes about a second.
#
# UNDO IS GIT. Both baselines are tracked, so nothing here is unrecoverable:
#   git checkout -- scripts/gates.baseline.txt scripts/gates.baseline.release.txt
[CmdletBinding()]
param(
    [ValidateSet('Both', 'Debug', 'Release')] [string] $Only = 'Both',
    [string[]] $Config = @(),
    [switch]   $SkipBuild,
    [switch]   $NoVerify
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Say([string] $m, [string] $colour = 'Gray') { Write-Host $m -ForegroundColor $colour }
function Rule([string] $title) {
    Write-Host ""
    Write-Host ("=" * 78) -ForegroundColor DarkCyan
    Write-Host "  $title" -ForegroundColor Cyan
    Write-Host ("=" * 78) -ForegroundColor DarkCyan
}

# Reads a baseline into @{ "config/gate" = "r,g,b" }. Deliberately the same parse gates.ps1 uses, so
# a file this script calls unchanged cannot be one gates.ps1 would read differently.
function Read-Baseline([string] $path) {
    $t = @{}
    if (-not (Test-Path -LiteralPath $path)) { return $t }
    foreach ($line in Get-Content -LiteralPath $path) {
        if ($line -match '^\s*(#|$)') { continue }
        $f = $line.Split('|')
        if ($f.Count -ge 3) { $t["$($f[0].Trim())/$($f[1].Trim())"] = $f[2].Trim() }
    }
    return $t
}

# The whole point of the exercise: say out loud which numbers changed. A re-record that reports only
# "recorded 153 gates" is indistinguishable from one that changed everything by accident.
function Show-Moves($before, $after, [string] $label) {
    $keys = @($before.Keys) + @($after.Keys) | Sort-Object -Unique
    $moved = @(); $added = @(); $dropped = @()
    foreach ($k in $keys) {
        $b = $before[$k]; $a = $after[$k]
        if ($null -eq $b)      { $added   += "$k = $a" }
        elseif ($null -eq $a)  { $dropped += "$k = $b" }
        elseif ($b -ne $a)     { $moved   += [pscustomobject]@{ Gate = $k; Was = $b; Now = $a } }
    }
    Write-Host ""
    Say "$label -- $($moved.Count) value(s) moved, $($added.Count) added, $($dropped.Count) dropped" 'White'
    if ($moved.Count) {
        Write-Host ("  {0,-34} {1,-16} {2}" -f 'gate', 'was', 'now') -ForegroundColor DarkGray
        foreach ($m in $moved) { Write-Host ("  {0,-34} {1,-16} {2}" -f $m.Gate, $m.Was, $m.Now) -ForegroundColor Yellow }
    }
    foreach ($x in $added)   { Say "  + $x" 'Green' }
    foreach ($x in $dropped) { Say "  - $x" 'Red' }
    if (-not $moved.Count -and -not $added.Count -and -not $dropped.Count) {
        Say "  nothing changed -- the baseline already described this build" 'Green'
    }
    return $moved.Count
}

# Runs one of this repo's own scripts and hands back its EXIT CODE.
#
# The $ErrorActionPreference dance is not defensive noise -- without it this wrapper does not work.
# Preference variables are inherited by a called script, so under 'Stop' a Write-Error inside
# gates.ps1 (an unknown configuration, a missing Sandbox.exe) becomes a TERMINATING error HERE. This
# script then died with a raw PowerShell stack before it could print "RECORD REFUSED ... UNCHANGED",
# and the Summary never ran at all -- which is the exact opposite of what a wrapper around a
# destructive operation is for. Measured by running it, not reasoned about.
#
# These scripts report failure through their exit code, so an error record from one of them is
# information to print, never a reason to unwind.
function Invoke-Child([string] $path, [hashtable] $named) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        # Out-Host, not a bare call. build.ps1 shells out to build.bat, and a native command's stdout
        # would otherwise become the CALLING function's pipeline output -- returned alongside its
        # $true/$false and read as a truthy array whatever actually happened.
        & $path @named | Out-Host
        return $LASTEXITCODE
    } finally { $ErrorActionPreference = $prev }
}

# One tree, end to end. Returns $true only if it recorded AND verified.
function Invoke-Tree([string] $flavour) {
    $isRelease   = ($flavour -eq 'Release')
    if ($isRelease) { $baseline = Join-Path $PSScriptRoot 'gates.baseline.release.txt' }
    else            { $baseline = Join-Path $PSScriptRoot 'gates.baseline.txt' }
    if ($isRelease) { $tree = 'build-release' } else { $tree = 'build' }

    Rule "$flavour"

    if (-not $SkipBuild) {
        Say "building $flavour ..." 'DarkGray'
        if ($isRelease) { $buildExit = Invoke-Child "$PSScriptRoot\build.ps1" @{ Release = $true } }
        else            { $buildExit = Invoke-Child "$PSScriptRoot\build.ps1" @{} }
        if ($buildExit -ne 0) {
            Say "BUILD FAILED -- nothing recorded. A baseline is only meaningful against a tree that builds." 'Red'
            return $false
        }
    } else {
        Say "-SkipBuild: recording against $tree as it stands" 'DarkYellow'
        # Worth stating rather than assuming: an exe older than the sources it came from is the
        # classic way to record numbers no source tree produces.
        $exe = Join-Path $root "$tree\bin\Sandbox.exe"
        if (Test-Path -LiteralPath $exe) {
            Say ("  Sandbox.exe last built {0}" -f (Get-Item -LiteralPath $exe).LastWriteTime) 'DarkYellow'
        } else {
            Say "  $exe does not exist -- build first, or drop -SkipBuild." 'Red'
            return $false
        }
    }

    $before = Read-Baseline $baseline

    # A HASHTABLE splat, not an argument array. gates.ps1 declares -Config as [string[]], and
    # `-Config ($Config -join ',')` would hand it the single string "baseline,warp" -- a one-element
    # array matching no configuration name, which fails the unknown-configuration check rather than
    # doing anything subtle. On the command line PowerShell splits that for you; programmatically it
    # does not.
    $recordArgs = @{ Record = $true }
    if ($isRelease)    { $recordArgs.Release = $true }
    if ($Config.Count) { $recordArgs.Config  = $Config }

    Say "recording -- this is the slow part" 'DarkGray'
    $started = Get-Date
    $recordExit = Invoke-Child "$PSScriptRoot\gates.ps1" $recordArgs
    Say ("record finished in {0:N1} min" -f ((Get-Date) - $started).TotalMinutes) 'DarkGray'

    if ($recordExit -ne 0) {
        # gates.ps1 refuses to write when anything failed for a reason other than a moved value, so
        # the baseline is untouched here rather than half-written. Say so explicitly: "it failed" and
        # "it failed and wrote nothing" are very different things to walk away from.
        Say ""
        # "exit code", not "N gate(s)". gates.ps1 also exits non-zero for reasons that are not gates
        # at all -- an unknown configuration, a missing Sandbox.exe -- and a wrapper that reports
        # those as failing gates sends the reader looking at the renderer.
        Say "RECORD REFUSED -- gates.ps1 exited $recordExit. $baseline is UNCHANGED." 'Red'
        Say "Read the run above: a CRASH, a BAD-PROBE or a broken INVARIANT must be fixed, not recorded." 'Red'
        return $false
    }

    $after = Read-Baseline $baseline
    [void](Show-Moves $before $after "$flavour baseline")

    if ($NoVerify) {
        Say "-NoVerify: not re-running. The baseline is unproven until something does." 'DarkYellow'
        return $true
    }

    Write-Host ""
    Say "verifying -- re-running the same gates against what was just recorded ..." 'DarkGray'
    $verifyArgs = @{}
    if ($isRelease)    { $verifyArgs.Release = $true }
    if ($Config.Count) { $verifyArgs.Config  = $Config }
    $verifyExit = Invoke-Child "$PSScriptRoot\gates.ps1" $verifyArgs

    if ($verifyExit -eq 0) {
        Say "$flavour VERIFIED -- recorded and green against the binary it was recorded from." 'Green'
        return $true
    }
    # This is the interesting failure. The numbers were just measured from this exact binary, so a
    # mismatch on the very next run is not a regression -- it is non-determinism, and it means the
    # gate that moved does not have a stable probe. Do not re-record it away.
    Say ""
    # NOT "$verifyExit gate(s)". gates.ps1 now exits 1 for "something failed" and prints the count on
    # its own last line, per the shared table in modules/core/include/aver/core/ErrorCodes.hpp.
    # Reading the code as a count was true while it WAS the count, and would have quietly become a
    # lie the moment that changed -- so this says only what it actually knows.
    Say "$flavour RECORDED BUT DID NOT VERIFY -- gates.ps1 exited $verifyExit; its last line has the count." 'Red'
    Say "These values came from this binary minutes ago, so this is NON-DETERMINISM, not a regression." 'Red'
    Say "Re-pick the offending probe onto a flat neighbourhood; do not record again to make it stop." 'Red'
    return $false
}

# ---------------------------------------------------------------- go

Rule "Re-recording the gate oracle"
Say "This overwrites the recorded expectations of the renderer. Undo, if you want it:" 'DarkYellow'
Say "  git checkout -- scripts/gates.baseline.txt scripts/gates.baseline.release.txt" 'DarkYellow'
if ($Config.Count) { Say "configurations: $($Config -join ', ')" 'DarkGray' }
else               { Say "configurations: all nine (WARP included -- budget ~25 min per tree)" 'DarkGray' }

# A running editor is not fatal -- the gates launch their own processes -- but it shares the GPU, and
# a build into a tree whose DLLs it holds open fails with LNK1168. Worth one line of warning.
if (Get-Process Sandbox -ErrorAction SilentlyContinue) {
    Say "NOTE: Sandbox.exe is already running. Close it -- it holds build outputs open (LNK1168)." 'DarkYellow'
}

if ($Only -eq 'Release') { $trees = @('Release') }
elseif ($Only -eq 'Debug') { $trees = @('Debug') }
else { $trees = @('Debug', 'Release') }

$began = Get-Date
$ok = @{}
foreach ($t in $trees) { $ok[$t] = Invoke-Tree $t }

Rule "Summary"
foreach ($t in $trees) {
    if ($ok[$t]) { Say ("  {0,-9} recorded and verified" -f $t) 'Green' }
    else         { Say ("  {0,-9} DID NOT COMPLETE -- see above" -f $t) 'Red' }
}
Say ("  elapsed {0:N1} min" -f ((Get-Date) - $began).TotalMinutes) 'DarkGray'

$bad = @($ok.Values | Where-Object { -not $_ }).Count
if ($bad -eq 0) {
    Write-Host ""
    Say "Both baselines are a measurement of THIS tree. Commit them on their own, with the reason:" 'White'
    # The cd is not padding. This script is meant to be run from anywhere -- it finds the repo from
    # $PSScriptRoot, not from the working directory -- so a bare `git add scripts/...` printed here
    # would be a relative path against whatever directory the user happened to be in.
    Say "  cd `"$root`"" 'DarkGray'
    Say "  git add scripts/gates.baseline*.txt" 'DarkGray'
    Say "  git commit    # say WHICH values moved and WHY -- see the table above" 'DarkGray'
}
exit $bad
