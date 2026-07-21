# The oracle, as a runner. Drives Sandbox.exe over every gate in every device configuration and
# compares the raw 8-bit probe codes against scripts/gates.baseline.txt.
#
# Why this exists as a script and not as a paragraph in docs/STATUS.md: the degraded configurations
# (--force-caps, --warp) more than tripled the number of things that have to be re-measured before a
# renderer change can be called safe, and a table nobody can run is a table that goes stale. This is
# the one place the expected numbers live.
#
#   ./scripts/gates.ps1                       # every configuration, compare against the baseline
#   ./scripts/gates.ps1 -Config baseline      # one configuration
#   ./scripts/gates.ps1 -Config baseline,warp # several
#   ./scripts/gates.ps1 -Record               # re-record the baseline (ONLY with a reason, see below)
#
# Exit code 0 = every gate matched. Non-zero = the number of gates that did not.
#
# RE-RECORDING is a decision, not a chore. A moved number means either the change was announced as
# oracle-moving and is understood, or something broke. Say which in the commit message, and say it in
# docs/STATUS.md too.

[CmdletBinding()]
param(
    [string[]] $Config = @(),
    [switch]   $Record,
    [int]      $Frames = 40,
    [string]   $Exe = "$PSScriptRoot\..\build\bin\Sandbox.exe",
    [string]   $BaselineFile = "$PSScriptRoot\gates.baseline.txt"
)

# Launches are spaced by this much. Back-to-back, a process occasionally comes up while the previous
# one is still tearing its window down, the new window opens at a different size, and the probe lands
# on editor chrome — raw(14,14,16), the dock clear colour, which reads exactly like a shading
# regression and is not one. 800 ms makes it 0-in-13 on hardware; do not lower it to save a minute.
$SpacingMs = 800

# WARP gets much longer, because a software-rasterised GI frame takes ~1 s and its process is still
# shutting down long after a hardware one would have finished.
#
# This was raised from 800 ms after a WARP GI gate returned the SKY colour instead of the scene, on
# the theory that it was the launch race. **It was not** — it happened again at 4 s. The longer
# spacing is kept because it costs nothing on a configuration that already takes a quarter of an
# hour, but it is NOT the fix and must not be read as one. See docs/STATUS.md §4d item 22.
$WarpSpacingMs = 4000

# ---------------------------------------------------------------- what gets run
#
# The first thirteen are the oracle as docs/STATUS.md has recorded it since the Voxi/HAL refactor.
# The last three were added when the degraded configurations became part of standing verification:
#
#   penumbra / penumbra-rt  the only pixel where RayQuery and the 3x3 PCF shadow map disagree. The
#                           thirteen cannot tell them apart -- (1413,1042) is fully shadowed on both
#                           paths -- so without these, `--force-caps no-rt` produces thirteen
#                           identical numbers and proves nothing about the fallback it exists to test.
#   sunlit / sunlit-gi      a sunlit floor pixel that actually receives DIRECT SPECULAR. The centre
#                           probe sits on the cube's unlit left face (ndl ~ 0) and (1413,1042) sits
#                           in shadow (visibility ~ 0), so a change to the whole masking-shadowing
#                           formulation once left all thirteen bit-identical. BRDF work had no
#                           automated cover at all until this gate existed.
$Gates = @(
    @{ name = 'centre';        args = @() },
    @{ name = 'ms';            args = @('--ms') },
    @{ name = 'rt';            args = @('--rt') },
    @{ name = 'ms-rt';         args = @('--ms','--rt') },
    @{ name = 'gi';            args = @('--gi') },
    @{ name = 'ms-gi';         args = @('--ms','--gi') },
    @{ name = 'ms-rt-gi';      args = @('--ms','--rt','--gi') },
    @{ name = 'gi-debug';      args = @('--gi-debug') },
    @{ name = 'ms-gi-debug';   args = @('--ms','--gi-debug') },
    @{ name = 'shadow';        args = @('--probe','1413','1042') },
    @{ name = 'shadow-rt';     args = @('--rt','--probe','1413','1042') },
    @{ name = 'shadow-ms-rt';  args = @('--ms','--rt','--probe','1413','1042') },
    @{ name = 'shadow-gi';     args = @('--gi','--probe','1413','1042') },
    @{ name = 'penumbra';      args = @('--probe','1413','1150') },
    @{ name = 'penumbra-rt';   args = @('--rt','--probe','1413','1150') },
    @{ name = 'sunlit';        args = @('--probe','2200','1400') },
    @{ name = 'sunlit-gi';     args = @('--gi','--probe','2200','1400') }
)

# Each configuration is a device this machine can be made to look like. `--force-caps` is
# monotonically reducing and applied once inside queryCaps, so nothing anywhere branches on "was this
# overridden" -- every consumer just sees a smaller device. `warp` is not a clamp at all: it is a
# second, independent implementation of D3D12 running on the CPU.
$Configs = [ordered]@{
    'baseline'           = @()
    'no-rt'              = @('--force-caps','no-rt')
    'no-ms'              = @('--force-caps','no-ms')
    'sm60'               = @('--force-caps','sm=60')
    'tier1-no-typed-uav' = @('--force-caps','tier1,no-typed-uav')
    'no-cons-raster'     = @('--force-caps','no-cons-raster')
    'all-off'            = @('--force-caps','no-rt,no-ms,no-cons-raster,no-typed-uav,tier1,msaa=1')
    'no-dxc'             = @('--force-caps','no-dxc')
    'warp'               = @('--warp')      # ~19x slower than hardware; budget ~15 minutes
}

# ---------------------------------------------------------------- helpers

function Get-TdrCount {
    # 0x141 LiveKernelEvents. Counted before and after, because a render batch that leaves new ones
    # behind has not passed however good its pixels look.
    $ev = Get-WinEvent -FilterHashtable @{LogName = 'Application'; Id = 1001} -ErrorAction SilentlyContinue
    if (-not $ev) { return 0 }
    ($ev | Where-Object { $_.Message -match 'LiveKernelEvent' -and $_.Message -match '141' }).Count
}

function Read-Baseline([string] $path) {
    $table = @{}
    if (-not (Test-Path $path)) { return $table }
    foreach ($line in Get-Content $path) {
        if ($line -match '^\s*(#|$)') { continue }
        $f = $line.Split('|')
        if ($f.Count -ge 3) { $table["$($f[0].Trim())/$($f[1].Trim())"] = $f[2].Trim() }
    }
    return $table
}

# Runs one gate and returns what the process actually reported. Everything here is read out of the
# log rather than assumed, INCLUDING the in-viewport tag: a probe that sampled editor chrome still
# prints a plausible number, so a runner that only grepped the raw codes would happily record it.
function Invoke-Gate($exe, [string[]] $gateArgs, [string[]] $extra, [int] $frames) {
    $all = @('--frames', "$frames") + $gateArgs + $extra
    $out = & $exe @all 2>&1 | Out-String
    $exit = $LASTEXITCODE
    $r = [pscustomobject]@{ raw = 'NO-PROBE'; place = '?'; debug = 'NO-TOTALS'; exit = $exit; rect = '?' }
    $probe = $out -split "`r?`n" | Select-String 'probe \(' | Select-Object -First 1
    if ($probe -match 'raw \(([\d, ]+)\)') { $r.raw = ($Matches[1] -replace '\s', '') }
    if ($probe -match '(in-viewport|OUTSIDE-VIEWPORT|VIEWPORT-MOVED)') { $r.place = $Matches[1] }
    # The viewport RECT, not just the in/out tag. The default probe is the viewport centre expressed
    # off this rect, so a window that came up at a different size moves what the centre pixel looks
    # at while still reporting `in-viewport`. Without the rect in the failure line, that is
    # indistinguishable from a shading regression -- which is exactly the confusion the probe's own
    # self-validation was added to end, and the runner should not throw the information away.
    if ($probe -match 'viewport \((\d+),(\d+) (\d+)x(\d+)\)') { $r.rect = "$($Matches[1]),$($Matches[2]) $($Matches[3])x$($Matches[4])" }
    # A MISSING totals line is itself a result: the device logs them at destruction, so its absence
    # means the process died before shutdown. That is how the WARP fault was first seen.
    $tot = $out -split "`r?`n" | Select-String 'debug layer totals' | Select-Object -First 1
    if ($tot -match 'totals: (\d+) corruption, (\d+) error, (\d+) warning') {
        $r.debug = "C$($Matches[1]) E$($Matches[2]) W$($Matches[3])"
    }
    return $r
}

# ---------------------------------------------------------------- run

if (-not (Test-Path $Exe)) { Write-Error "Sandbox.exe not found at $Exe - run ./scripts/build.ps1 first"; exit 1 }
$Exe = (Resolve-Path $Exe).Path

$selected = if ($Config.Count -gt 0) { $Config } else { @($Configs.Keys) }
foreach ($c in $selected) {
    if (-not $Configs.Contains($c)) {
        Write-Error "unknown configuration '$c'; known: $($Configs.Keys -join ', ')"
        exit 1
    }
}

$baseline = Read-Baseline $BaselineFile
$recorded = [System.Collections.Generic.List[string]]::new()
$failures = 0
$flaky = 0
$tdrBefore = Get-TdrCount
Write-Host "0x141 LiveKernelEvent count before: $tdrBefore"

foreach ($c in $selected) {
    $extra = $Configs[$c]
    $spacing = if ($extra -contains '--warp') { $WarpSpacingMs } else { $SpacingMs }
    Write-Host ""
    Write-Host "=== $c  [$($extra -join ' ')]"
    foreach ($g in $Gates) {
        $r = Invoke-Gate $Exe $g.args $extra $Frames
        $key = "$c/$($g.name)"
        $want = $baseline[$key]

        $verdict = 'PASS'
        if ($r.exit -ne 0)                   { $verdict = "CRASH exit=0x{0:X8}" -f $r.exit }
        elseif ($r.place -ne 'in-viewport')  { $verdict = "BAD-PROBE $($r.place)" }
        elseif ($r.debug -eq 'NO-TOTALS')    { $verdict = 'NO-TOTALS (died before shutdown?)' }
        elseif ($r.debug -notmatch '^C0 E0') { $verdict = "DEBUG-LAYER $($r.debug)" }
        elseif ($Record)                     { $verdict = 'recorded' }
        elseif ($null -eq $want)             { $verdict = 'NO-BASELINE' }
        elseif ($r.raw -ne $want)            { $verdict = "FAIL expected $want" }

        # A miss is run ONCE more before it is believed, and BOTH results are printed whatever
        # happens. This is not a retry that hides a failure -- a gate that misses twice still fails,
        # and a gate that misses once is reported as FLAKY with both values and both viewport rects,
        # which is strictly more information than a single number.
        #
        # It exists because the editor window intermittently comes up at 45x45 and grows afterwards.
        # That is measured, not supposed: `viewport (0,198 45x45) OUTSIDE-VIEWPORT` was captured twice
        # in a row inside ten otherwise identical runs. When the probe lands outside the viewport the
        # engine says so and this reads BAD-PROBE; the worrying variant is a plausible WRONG pixel
        # with a correct-looking rect, which is why both rects are printed and why a repeat is
        # required before anything is believed either way.
        if ($verdict -like 'FAIL*' -or $verdict -like 'BAD-PROBE*') {
            Start-Sleep -Milliseconds $spacing
            $r2 = Invoke-Gate $Exe $g.args $extra $Frames
            if ($r2.raw -eq $want -and $r2.place -eq 'in-viewport' -and $r2.exit -eq 0) {
                $verdict = "FLAKY first=$($r.raw) rect=$($r.rect) / retry=$($r2.raw) rect=$($r2.rect)"
            } else {
                $verdict = "FAIL expected $want, got $($r.raw) rect=$($r.rect) then $($r2.raw) rect=$($r2.rect)"
            }
        }

        if ($verdict -notlike 'PASS*' -and $verdict -ne 'recorded' -and $verdict -notlike 'FLAKY*') { $failures++ }
        if ($verdict -like 'FLAKY*') { $flaky++ }
        Write-Host ("  {0,-14} raw({1,-12}) {2,-8} {3}" -f $g.name, $r.raw, $r.debug, $verdict)
        $recorded.Add("$c | $($g.name) | $($r.raw)")
        Start-Sleep -Milliseconds $spacing
    }
}

$tdrAfter = Get-TdrCount
Write-Host ""
Write-Host "0x141 LiveKernelEvent count after: $tdrAfter (was $tdrBefore)"
if ($tdrAfter -ne $tdrBefore) { Write-Host "NEW TDRs -- this run is a failure regardless of pixels"; $failures++ }

if ($Record) {
    # Only the configurations that were actually run are rewritten; the rest of the file survives, so
    # recording one configuration cannot quietly erase the baseline of another.
    $keep = Get-Content $BaselineFile -ErrorAction SilentlyContinue | Where-Object {
        if ($_ -match '^\s*#' -or $_ -match '^\s*$') { $true }
        else { $selected -notcontains $_.Split('|')[0].Trim() }
    }
    Set-Content -Path $BaselineFile -Encoding utf8 -Value (@($keep) + @($recorded))
    Write-Host "recorded $($recorded.Count) gate(s) into $BaselineFile"
}

Write-Host ""
if ($flaky -gt 0) {
    # Deliberately loud, and deliberately NOT a failure. A flake is a fact about the harness or about
    # a genuinely non-deterministic path, and either way it wants a human eye rather than a silent
    # green tick. Compare the two viewport rects on the line first: if they differ, it was the
    # launch race; if they are identical, it is the renderer and this is the important line in the run.
    Write-Host "$flaky GATE(S) FLAKY -- passed on retry. Compare the two viewport rects on each line."
}
Write-Host $(if ($failures -eq 0) { "ALL GATES PASS" } else { "$failures GATE(S) FAILED" })
exit $failures
