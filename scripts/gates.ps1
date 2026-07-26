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

#   ./scripts/gates.ps1 -Release              # the Release build, against its OWN baseline
#
# RELEASE IS A SEPARATE BASELINE, not a second opinion on the Debug one. Optimisation settings
# change floating-point codegen (contraction, vectorisation, reassociation), so a probe code may
# legitimately differ by an LSB between the two. `-Release` therefore switches BOTH the executable
# and the baseline file together, because comparing one build against the other's numbers is the
# mistake this pairing exists to make impossible.
[CmdletBinding()]
param(
    [string[]] $Config = @(),
    [switch]   $Record,
    [switch]   $Release,
    [int]      $Frames = 40,
    [string]   $Exe = $(if ($Release) { "$PSScriptRoot\..\build-release\bin\Sandbox.exe" } else { "$PSScriptRoot\..\build\bin\Sandbox.exe" }),
    [string]   $BaselineFile = $(if ($Release) { "$PSScriptRoot\gates.baseline.release.txt" } else { "$PSScriptRoot\gates.baseline.txt" })
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
#                           probe was sun-blind and (1413,1042) sits in shadow (visibility ~ 0), so a
#                           change to the whole masking-shadowing formulation once left all thirteen
#                           bit-identical. BRDF work had no automated cover at all until this gate
#                           existed.
#
# THE CENTRE PROBE IS NO LONGER SUN-BLIND, and that is not a value change -- it is the premise of the
# sentence above dissolving. It held only because the old default sun sat 8 degrees behind the
# camera: the centre pixel aims straight down the view axis at the placeholder cube's +X/+Y CORNER
# EDGE, and the two candidate faces both had ndl exactly 0, so which one the rasteriser handed the
# pixel could not matter. Under the sun this engine now defaults to, ndl is 0 on +X and 0.3838 on
# +Y, and a subpixel decision -- flipped by MSAA, by WARP versus hardware, by any viewport parity
# change -- decides whether the probe sees direct sun.
#
# So the centre probe must be RE-PICKED off the corner edge, not merely re-recorded. It was always
# sitting on a geometric discontinuity; identical shading on both faces was masking it, and better
# lighting took the mask off. Re-recording alone would freeze a coin flip into the baseline.
$Gates = @(
    # --no-gi ON THE TEN NON-GI GATES, and it is not decoration. Global illumination became the
    # ENGINE DEFAULT, so "GI off" is now a thing that has to be asked for. Without these flags the
    # ten gates that exist to measure the unlit path would quietly start measuring the same path the
    # seven `--gi` gates do, seventeen numbers would collapse to seven distinct ones, and the oracle
    # would still report a confident 153/153 while having lost more than half of what it covers.
    #
    # Adding the flags is what keeps the recorded values UNCHANGED across that default flip: each
    # gate still renders exactly what it rendered before, it just has to name it now.
    @{ name = 'centre';        args = @('--no-gi') },
    @{ name = 'ms';            args = @('--no-gi','--ms') },
    @{ name = 'rt';            args = @('--no-gi','--rt') },
    @{ name = 'ms-rt';         args = @('--no-gi','--ms','--rt') },
    @{ name = 'gi';            args = @('--gi') },
    @{ name = 'ms-gi';         args = @('--ms','--gi') },
    @{ name = 'ms-rt-gi';      args = @('--ms','--rt','--gi') },
    @{ name = 'gi-debug';      args = @('--gi-debug') },
    @{ name = 'ms-gi-debug';   args = @('--ms','--gi-debug') },
    # RELATIVE, as fractions of the viewport rect, and re-picked against the current scene.
    #
    # Absolute pixels broke this oracle twice: once when the Content Browser became a drawer and grew
    # the viewport, and once when the editor's placeholder scene was rescaled to centimetres. Neither
    # was a shading change, and both times every hard-coded probe silently started sampling a
    # different surface while the centre probes -- which the engine has always derived from the rect
    # -- kept passing. A fraction of the rect cannot go stale that way.
    #
    # HOW THESE WERE CHOSEN, so they can be chosen again the same way: capture the scene twice, once
    # with `--no-gi` and once with `--no-gi --rt`, then take
    #   shadow    the DARKEST floor pixel where the two frames agree (diff <= 2)
    #   sunlit    the BRIGHTEST floor pixel where they agree
    #   penumbra  the LARGEST disagreement between them
    # each screened for a flat 7x7 neighbourhood so no probe sits on a one-pixel feature. `penumbra`
    # unavoidably remains on a shadow edge -- that is the only place the two paths ever differ -- so
    # it is the one probe that a sub-pixel change can move, and the invariant check at the end of
    # this script exists to say so out loud when it does.
    #
    # The older note below is kept because the failure it describes is the same one, first time round:
    # The coordinates below were RE-PICKED in July 2026. The Content Browser and Output Log became
    # bottom drawers, which removed the dock's bottom split and made the 3D viewport taller
    # (2750x1266 -> 2750x1711, same width). These probes are backbuffer-ABSOLUTE, so every one of them
    # started sampling a different surface: the old shadow pixel landed on the cube, and eighty-six
    # gates failed at once while the centre probes -- which the engine derives from the viewport rect,
    # and which are therefore size-invariant -- all still passed. That split is the tell, and it is
    # worth remembering: a probe that is not expressed relative to the rect is only valid for the
    # window layout it was recorded under.
    #
    # Re-picked by what each gate is DEFINED to sample rather than by scaling the old numbers, and
    # chosen from a frame captured at the new size: `shadow` is the darkest floor pixel the PCF and
    # RayQuery paths AGREE on, `sunlit` the brightest they agree on, and `penumbra` the largest
    # disagreement between them. Both were also checked for a flat 7x7 neighbourhood so a probe does
    # not sit on a one-pixel feature -- `sunlit` varies by 1 code across 7x7 and `shadow` by 8.
    #
    # `penumbra` unavoidably remains on an edge: a search for a disagreement with a flat neighbourhood
    # in BOTH images found none, because the two paths only ever differ across a shadow boundary. That
    # was equally true of the pixel it replaces.
    @{ name = 'shadow';        args = @('--no-gi','--probe-rel','0.50364','0.57101') },
    @{ name = 'shadow-rt';     args = @('--no-gi','--rt','--probe-rel','0.50364','0.57101') },
    @{ name = 'shadow-ms-rt';  args = @('--no-gi','--ms','--rt','--probe-rel','0.50364','0.57101') },
    @{ name = 'shadow-gi';     args = @('--gi','--probe-rel','0.50364','0.57101') },
    @{ name = 'penumbra';      args = @('--no-gi','--probe-rel','0.49709','0.57101') },
    @{ name = 'penumbra-rt';   args = @('--no-gi','--rt','--probe-rel','0.49709','0.57101') },
    @{ name = 'sunlit';        args = @('--no-gi','--probe-rel','0.36945','0.45003') },
    @{ name = 'sunlit-gi';     args = @('--gi','--probe-rel','0.36945','0.45003') }
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
    # --debug-layer is passed by the RUNNER, not defaulted on in the engine. The layer validates
    # every API call and is a per-call tax no ordinary run should pay, but the per-gate C/E/W counts
    # below come out of it, and a gate that reported no corruption because nothing was watching
    # would be worse than no gate at all.
    $all = @('--frames', "$frames", '--debug-layer') + $gateArgs + $extra
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

$buildHint = if ($Release) { './scripts/build.ps1 -Release' } else { './scripts/build.ps1' }
if (-not (Test-Path $Exe)) { Write-Error "Sandbox.exe not found at $Exe - run $buildHint first"; exit 1 }
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
$seen = @{}
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
        $seen["$c|$($g.name)"] = $r.raw
        Start-Sleep -Milliseconds $spacing
    }
}

# ---- INVARIANTS: does each gate still MEASURE what its name says? -----------------------------
#
# A probe can stay valid, stable and reproducible while the thing it was chosen to discriminate has
# quietly stopped existing -- and then the gate passes forever while covering nothing. That is not
# hypothetical: cascaded shadow maps made the PCF path sharp enough to agree with RayQuery at the
# `penumbra` pixel, so the pair that exists to prove the no-RT fallback DIFFERS was reporting two
# identical numbers. A re-record would have frozen that in.
#
# Recorded values cannot catch it, because the values were right. Only a statement of INTENT can.
function Get-Raw([string] $cfg, [string] $gate) {
    $v = $seen["$cfg|$gate"]
    if ($null -eq $v) { return $null }
    $p = $v -split ','
    return @([int]$p[0], [int]$p[1], [int]$p[2])
}
function Lum($rgb) { if ($null -eq $rgb) { return $null }; return 0.2126*$rgb[0] + 0.7152*$rgb[1] + 0.0722*$rgb[2] }

Write-Host ""
Write-Host "=== gate invariants (what each probe is DEFINED to sample) ==="
foreach ($c in $selected) {
    $sh = Get-Raw $c 'shadow'; $su = Get-Raw $c 'sunlit'
    $pe = Get-Raw $c 'penumbra'; $pr = Get-Raw $c 'penumbra-rt'
    # `shadow` is the darkest floor pixel and `sunlit` the brightest, so one must clearly outrank
    # the other. If they converge, both are sampling the same lighting condition.
    if ($null -ne $sh -and $null -ne $su) {
        $d = (Lum $su) - (Lum $sh)
        if ($d -lt 20) {
            Write-Host ("  {0,-20} INVARIANT FAIL  shadow/sunlit differ by only {1:N1} -- they no longer bracket the lighting" -f $c, $d)
            $failures++
        }
    }
    # `penumbra` exists ONLY to be a pixel where the shadow map and RayQuery disagree. Where ray
    # tracing is unavailable the two gates run the same path and are expected to match, so the
    # invariant applies only where they are genuinely different code paths.
    if ($null -ne $pe -and $null -ne $pr -and $c -notin @('no-rt','sm60','all-off','no-dxc','warp')) {
        $diff = [Math]::Abs($pe[0]-$pr[0]) + [Math]::Abs($pe[1]-$pr[1]) + [Math]::Abs($pe[2]-$pr[2])
        if ($diff -lt 8) {
            Write-Host ("  {0,-20} INVARIANT FAIL  penumbra vs penumbra-rt differ by {1} -- the probe no longer discriminates the paths" -f $c, $diff)
            $failures++
        }
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
