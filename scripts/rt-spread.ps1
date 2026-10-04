# How far does one gate's probe move between identical runs? Answers it by measurement.
#
# WHY THIS EXISTS. Since temporal accumulation landed, the ray-traced gate configurations no longer
# reproduce: running gates.ps1 twice against the SAME binary moved `rt` from 46,33,28 to 43,31,27 and
# `shadow-ms-rt` from 27,41,57 to 30,45,63. The non-ray gates -- centre, ms, gi, shadow, penumbra,
# sunlit and the rest -- were bit-identical across every run in that same session.
#
# That partition matters more than the magnitude, because it decides what a fix can even look like.
# An oracle that demands exact equality cannot be green on a measurement that is not exact, so either
# the renderer is made deterministic again or the oracle is given a tolerance. A tolerance invented
# from two samples is a guess; verify-payload.ps1 already carries one (3 codes) that this script
# exists to replace with a number somebody measured.
#
# WHAT IT DELIBERATELY DOES NOT DO. It does not compare against the baseline and has no opinion about
# whether a gate passes. The question here is only "how much does this move when nothing changes",
# which is a property of the renderer and the driver, not of any recorded expectation.
#
#   ./scripts/rt-spread.ps1                        # the `rt` gate, 20 runs, Release
#   ./scripts/rt-spread.ps1 -Gate shadow-ms-rt     # a different gate
#   ./scripts/rt-spread.ps1 -Gate centre -Runs 10  # a control: this one should not move at all
#
# RUN A CONTROL. `-Gate centre` is the falsification: if a gate with no ray tracing in it also moves,
# the cause is not the ray path and every conclusion drawn from this partition is wrong.
[CmdletBinding()]
param(
    [string] $Gate = 'rt',
    [int]    $Runs = 20,
    [switch] $Debug,
    [int]    $Frames = 40,
    [string] $Exe
)

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) {
    $tree = if ($Debug) { 'build' } else { 'build-release' }
    $Exe = Join-Path $root "$tree\bin\Sandbox.exe"
}
if (-not (Test-Path -LiteralPath $Exe)) { Write-Error "Sandbox.exe not found at $Exe"; exit 3 }
$Exe = (Resolve-Path $Exe).Path

# The gate argument lists, lifted verbatim from gates.ps1 so the two cannot describe different runs.
$centre = @('--probe-rel', '0.51691', '0.46461')
$shadow = @('--probe-rel', '0.53545', '0.48536')
$penum  = @('--probe-rel', '0.57545', '0.50610')
$Gates = @{
    # Copied verbatim from gates.ps1's own table, probe coordinates included. Inventing a probe
    # location here would measure a different surface and answer a different question.
    'centre'       = @('--no-gi', '--no-rt') + $centre
    'ms'           = @('--no-gi', '--ms', '--no-rt') + $centre
    'rt'           = @('--no-gi', '--rt') + $centre
    'ms-rt'        = @('--no-gi', '--ms', '--rt') + $centre
    'gi'           = @('--gi', '--no-rt') + $centre
    'ms-rt-gi'     = @('--ms', '--rt', '--gi') + $centre
    'shadow'       = @('--no-gi', '--no-rt') + $shadow
    'shadow-rt'    = @('--no-gi', '--rt') + $shadow
    'shadow-ms-rt' = @('--no-gi', '--ms', '--rt') + $shadow
    'shadow-gi'    = @('--gi', '--no-rt') + $shadow
    'penumbra'     = @('--no-gi', '--no-rt') + $penum
    'penumbra-rt'  = @('--no-gi', '--rt') + $penum
    'rt-penumbra'  = @('--no-gi', '--rt', '--sun-angle', '8.0', '--probe-rel', '0.5672727', '0.5149481')
}
if (-not $Gates.ContainsKey($Gate)) {
    Write-Error "unknown gate '$Gate'; known: $($Gates.Keys -join ', ')"; exit 2
}

Write-Host "[spread] $Gate  x$Runs  $(Split-Path -Leaf (Split-Path -Parent (Split-Path -Parent $Exe)))"
Write-Host ''

$samples = New-Object System.Collections.Generic.List[object]
for ($i = 1; $i -le $Runs; $i++) {
    # Start-Process, not `& $exe`: Sandbox.exe is /SUBSYSTEM:WINDOWS, and Windows PowerShell neither
    # waits for nor captures a GUI-subsystem process. See gates.ps1's Invoke-Gate.
    $out = [System.IO.Path]::GetTempFileName()
    $err = [System.IO.Path]::GetTempFileName()
    try {
        # AverSR (0.6 optimisation wave 2, U2) now defaults to Auto at every quality rung, and Auto
        # applies to --frames runs too. This script's whole question is how much a probe pixel moves
        # between IDENTICAL runs of the same binary; an upscaler resolving to a different internal
        # resolution would be a second, uncontrolled source of that movement, confounding the one the
        # header's "RUN A CONTROL" note is trying to isolate. Pin native resolution.
        Start-Process -FilePath $Exe -ArgumentList (@('--frames', "$Frames", '--no-vsync', '--aversr', 'off') + $Gates[$Gate]) `
                      -NoNewWindow -Wait -RedirectStandardOutput $out -RedirectStandardError $err | Out-Null
        $text = (Get-Content -LiteralPath $out -Raw -ErrorAction SilentlyContinue)
    } finally {
        Remove-Item -LiteralPath $out, $err -Force -ErrorAction SilentlyContinue
    }
    $line = $text -split "`r?`n" | Select-String 'probe \(' | Select-Object -First 1
    if ($line -match 'raw \(([\d, ]+)\)') {
        $rgb = ($Matches[1] -replace '\s', '') -split ',' | ForEach-Object { [int]$_ }
        $samples.Add([pscustomobject]@{ r = $rgb[0]; g = $rgb[1]; b = $rgb[2] })
        Write-Host ("  run {0,2}  {1},{2},{3}" -f $i, $rgb[0], $rgb[1], $rgb[2])
    } else {
        Write-Host ("  run {0,2}  NO-PROBE" -f $i) -ForegroundColor Yellow
    }
}

if ($samples.Count -lt 2) { Write-Error '[spread] too few usable samples to say anything'; exit 1 }

Write-Host ''
$worst = 0
foreach ($ch in 'r', 'g', 'b') {
    $v = $samples | ForEach-Object { $_.$ch }
    $lo = ($v | Measure-Object -Minimum).Minimum
    $hi = ($v | Measure-Object -Maximum).Maximum
    $distinct = ($v | Sort-Object -Unique) -join ','
    if (($hi - $lo) -gt $worst) { $worst = $hi - $lo }
    Write-Host ("[spread] {0}: min {1,3}  max {2,3}  range {3,2}   values {4}" -f $ch, $lo, $hi, ($hi - $lo), $distinct)
}
Write-Host ''
Write-Host ("[spread] {0}: {1} samples, WIDEST CHANNEL RANGE {2} code(s)" -f $Gate, $samples.Count, $worst)
if ($worst -eq 0) {
    Write-Host '[spread] bit-identical across every run -- this gate is deterministic' -ForegroundColor Green
} else {
    Write-Host "[spread] a tolerance derived from THIS gate would need to be at least $worst" -ForegroundColor DarkYellow
    Write-Host '[spread] run -Gate centre as a control before concluding the ray path is the cause' -ForegroundColor DarkYellow
}
