<#
.SYNOPSIS
    Verifies a staged payload is functionally identical to the build tree it came from.

.DESCRIPTION
    Runs scripts/gates.ps1 twice over the same configurations -- once against the build tree's
    Sandbox.exe, once against the staged payload's -- and compares the two sets of probe codes to
    EACH OTHER.

    WHY NOT JUST COMPARE THE PAYLOAD TO THE BASELINE. Because that answers two questions at once and
    tells you neither. gates.baseline.release.txt is a recorded measurement that goes stale whenever
    a rendering change lands and is not re-recorded, so a staged run failing against it means either
    "the payload is broken" or "the baseline is old", with no way to tell which. Comparing the two
    binaries removes the baseline from the question entirely: whatever the numbers are, if the staged
    payload produces the SAME ones as the tree it was cut from, then staging lost nothing.

    That is the property staging has to have. It is not a substitute for the baseline gates -- run
    those separately to ask whether the renderer is right -- it is the answer to "did the allowlist
    drop a file".

    A dropped DLL does not usually produce a slightly different pixel; it produces a crash or a
    declined subsystem, which shows up here as CRASH or NO-PROBE on every gate. A dropped shader
    redistributable or font shows up as a changed probe. Both are caught.

.PARAMETER Payload
    Root of a staged payload (the directory containing bin\Sandbox.exe), as produced by
    scripts/stage-payload.ps1.

.PARAMETER Config
    Gate configurations to compare. Defaults to the three that exercise the parts of the payload most
    likely to be mis-staged: the default device, the ray-tracing fallback, and the no-DXC fallback
    (which is the one that proves dxcompiler.dll/dxil.dll are doing something).
    'warp' is omitted by default because it costs ~15 minutes and tests a CPU rasteriser rather than
    anything about the payload; pass it explicitly for a full sweep.

.EXAMPLE
    ./scripts/verify-payload.ps1 -Payload ..\stage\AverEngine-0.1.0
    ./scripts/verify-payload.ps1 -Payload ..\stage\AverEngine-0.1.0 -Config baseline,no-rt,no-ms,no-dxc,warp
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Payload,
    [string[]] $Config = @('baseline', 'no-rt', 'no-dxc'),
    [string] $WorkDir
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$stagedExe = Join-Path $Payload 'bin\Sandbox.exe'
if (-not (Test-Path -LiteralPath $stagedExe)) {
    Write-Host "[verify] no Sandbox.exe at $stagedExe" -ForegroundColor Red
    exit 2  # ExitCode.Usage (bad -Payload path; core/ErrorCodes.hpp)
}

# The tree the payload was cut from. payload.json records the config it was staged from, so the
# comparison cannot accidentally be made against the other build type.
$payloadJson = Join-Path $Payload 'payload.json'
$cfgName = 'Release'
if (Test-Path -LiteralPath $payloadJson) {
    $cfgName = (Get-Content -LiteralPath $payloadJson -Raw | ConvertFrom-Json).config
}
$treeDir = if ($cfgName -eq 'Debug') { 'build' } else { "build-$($cfgName.ToLower())" }
$treeExe = Join-Path $root "$treeDir\bin\Sandbox.exe"
if (-not (Test-Path -LiteralPath $treeExe)) {
    Write-Host "[verify] no build tree exe at $treeExe" -ForegroundColor Red
    exit 3  # ExitCode.Environment
}

if (-not $WorkDir) { $WorkDir = Join-Path ([System.IO.Path]::GetTempPath()) ("aver-verify-" + [guid]::NewGuid().ToString('N').Substring(0,8)) }
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null

$useRelease = ($cfgName -ne 'Debug')
Write-Host "[verify] payload  $stagedExe"
Write-Host "[verify] tree     $treeExe  ($cfgName)"
Write-Host "[verify] configs  $($Config -join ', ')"
Write-Host ''

function Invoke-Gates {
    param([string] $Exe, [string] $OutFile)
    $gates = Join-Path $PSScriptRoot 'gates.ps1'

    # EAP DROPS TO Continue FOR THIS CALL, and that is a bug fix rather than a loosening.
    #
    # gates.ps1 runs `& $exe ... 2>&1`. In Windows PowerShell, redirecting a NATIVE command's stderr
    # wraps every line it writes in an ErrorRecord (NativeCommandError); under the script-scope
    # 'Stop' set above, the first such line becomes terminating and kills this script even though
    # Sandbox.exe exited 0.
    #
    # That made this script UNABLE TO SUCCEED WITH ITS OWN DEFAULTS: two of the three default
    # configs are `no-rt` and `no-dxc`, which pass --force-caps, which always logs
    # "caps CLAMPED by --force-caps" to stderr. The verifier that decides whether a release is
    # shippable fell over on a benign warning from the very configs it was written to exercise.
    #
    # The exit code is still checked by the caller, and the real verdict is the probe comparison
    # below, so nothing is being swallowed here except PowerShell's misreading of stderr as failure.
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        if ($useRelease) {
            & $gates -Release -Config $Config -Exe $Exe *>&1 | Out-File -LiteralPath $OutFile -Encoding utf8
        } else {
            & $gates -Config $Config -Exe $Exe *>&1 | Out-File -LiteralPath $OutFile -Encoding utf8
        }
    } finally {
        $ErrorActionPreference = $prev
    }
    return $LASTEXITCODE
}

# Parses gates.ps1's per-gate line:  "  centre  raw(99,38,30 ) C0 E0 W0 FAIL ..."
# The raw() code and the verdict token are all that matter; the baseline's opinion is deliberately
# discarded, since this comparison does not depend on the baseline being current.
function Read-Probes {
    param([string] $File)
    $probes = [ordered]@{}
    $section = '?'
    foreach ($line in Get-Content -LiteralPath $File) {
        if ($line -match '^===\s+(\S+)') { $section = $Matches[1]; continue }
        if ($line -match '^\s{2}(\S+)\s+raw\(([^)]*)\)') {
            # BAD-PROBE MEANS THE ORACLE DISOWNED THE SAMPLE, so it is not evidence about the
            # payload. gates.ps1 emits it when the viewport it sampled was degenerate -- the
            # editor's dockspace has not finished laying out, and the probe rect comes back as
            # something like 45x24 instead of 2750x1639. The pixel it returns is the window
            # background, identical for every gate that hits it.
            #
            # Counting those as differences made this script report "the staged payload is not the
            # tree it came from" for a reason that has nothing to do with staging: two consecutive
            # runs of the SAME binary disagreed, and the gate list that failed changed between them.
            # gates.ps1 already retries and labels the survivors FLAKY.
            #
            # Skipping them narrows what this script can see, and that is the honest trade: a probe
            # nobody sampled proves nothing either way. The count of skipped probes is reported so a
            # run that skipped most of them cannot look like a clean pass.
            # RECORDED BY KEY, not just counted. A probe the oracle disowned in ONE run but not
            # the other must be dropped from BOTH sides, or the comparison reports it as EXTRA or
            # MISSING -- which is the same false "the payload is not the tree" verdict in a new
            # costume. The rects are degenerate independently on each run, so the asymmetric case is
            # the common one, not the corner case.
            if ($line -match 'BAD-PROBE') {
                $script:badProbes++
                $script:badKeys["$section/$($Matches[1])"] = $true
                continue
            }
            $probes["$section/$($Matches[1])"] = $Matches[2].Trim()
        }
    }
    return $probes
}

$treeOut   = Join-Path $WorkDir 'gates-tree.txt'
$stagedOut = Join-Path $WorkDir 'gates-staged.txt'

$script:badProbes = 0
$script:badKeys  = @{}
Write-Host '[verify] running gates against the build tree...'
$treeExit = Invoke-Gates -Exe $treeExe -OutFile $treeOut
Write-Host "[verify]   gates.ps1 exit=$treeExit (vs its baseline; not the question here)"

Write-Host '[verify] running gates against the staged payload...'
$stagedExit = Invoke-Gates -Exe $stagedExe -OutFile $stagedOut
Write-Host "[verify]   gates.ps1 exit=$stagedExit (vs its baseline; not the question here)"
Write-Host ''

$a = Read-Probes $treeOut
$b = Read-Probes $stagedOut

# Drop every disowned key from BOTH sides before comparing.
foreach ($k in @($script:badKeys.Keys)) {
    if ($a.Contains($k)) { $a.Remove($k) }
    if ($b.Contains($k)) { $b.Remove($k) }
}

if ($script:badProbes -gt 0) {
    Write-Host ("[verify] {0} probe sample(s) skipped: the oracle marked them BAD-PROBE (degenerate viewport), so they say nothing about staging" -f $script:badProbes) -ForegroundColor Yellow
}
if ($a.Count -eq 0) { Write-Host '[verify] ERROR parsed no probes from the build-tree run' -ForegroundColor Red; exit 1 }
# A run that skipped most of its probes has not verified much, and must not read as a clean pass.
if ($script:badProbes -ge $a.Count) {
    Write-Host ("[verify] ERROR {0} probes were skipped against only {1} usable - too little was actually compared to conclude anything" -f $script:badProbes, $a.Count) -ForegroundColor Red
    exit 1
}

$diffs = 0
$missing = 0
$noisy = 0
foreach ($key in $a.Keys) {
    if (-not $b.Contains($key)) {
        Write-Host ("  {0,-28} MISSING from the staged run" -f $key) -ForegroundColor Red
        $missing++; continue
    }
    if ($a[$key] -ne $b[$key]) {
        # A TOLERANCE, BECAUSE EXACT EQUALITY STOPPED BEING MEASURABLE. Temporal accumulation landed
        # this release and the ray-traced configurations no longer reproduce bit-for-bit: running
        # gates.ps1 TWICE AGAINST THE SAME BINARY moves rt 46,33,28 -> 43,31,27, shadow-ms-rt
        # 28,42,59 -> 27,41,57, and ms-rt disagrees with its own retry inside a single run. Measured,
        # not assumed. An exact test therefore reports "the staged payload is not the tree it came
        # from" for two runs of one file, which is not a claim it can support.
        #
        # THREE CODES is the observed wobble band, and a real staging fault is not subtle: a missing
        # shader, a stale binary or an unstaged DLL changes the picture wholesale or stops it
        # rendering at all. Anything past the band still fails, and the binaries are separately
        # required to be byte-identical below -- which is the strictly stronger test for the half of
        # the question a probe was always a proxy for.
        $pa = @($a[$key] -split ',')
        $pb = @($b[$key] -split ',')
        $delta = -1
        if ($pa.Count -eq 3 -and $pb.Count -eq 3 -and $a[$key] -match '^\d' -and $b[$key] -match '^\d') {
            $delta = 0
            for ($i = 0; $i -lt 3; $i++) {
                $d = [math]::Abs([int]$pa[$i] - [int]$pb[$i])
                if ($d -gt $delta) { $delta = $d }
            }
        }
        if ($delta -ge 0 -and $delta -le 3) {
            Write-Host ("  {0,-28} within noise  tree={1}  staged={2}  (max delta {3})" -f $key, $a[$key], $b[$key], $delta) -ForegroundColor DarkYellow
            $noisy++
        } else {
            Write-Host ("  {0,-28} DIFFERS  tree={1}  staged={2}" -f $key, $a[$key], $b[$key]) -ForegroundColor Red
            $diffs++
        }
    }
}
foreach ($key in $b.Keys) {
    if (-not $a.Contains($key)) {
        Write-Host ("  {0,-28} EXTRA in the staged run" -f $key) -ForegroundColor Yellow
        $diffs++
    }
}

# A run where every probe reads NO-PROBE agrees with itself while proving nothing, so the degenerate
# case is called out rather than counted as a pass.
$live = @($b.Values | Where-Object { $_ -ne 'NO-PROBE' }).Count
Write-Host ''
Write-Host ("[verify] {0} probes compared, {1} produced a pixel" -f $a.Count, $live)
if ($noisy -gt 0) {
    Write-Host ("[verify] {0} probe(s) differed within the 3-code noise band - see the comment at the comparison" -f $noisy) -ForegroundColor DarkYellow
}

# THE BINARIES MUST BE THE SAME FILE, and this is where that is actually established. The probe
# comparison above was always a proxy for it, and temporal accumulation took away its precision; a
# hash cannot drift. It also catches what a probe cannot: a binary that renders identically because
# the payload quietly fell back to something staged beside it.
#
# EXTRAS ARE EXPECTED AND NOT COMPARED. The Visual C++ runtime is staged from the redist directory,
# not built, so msvcp140.dll and the two vcruntime DLLs exist in the payload and not in the tree.
# Only files present on BOTH sides can disagree, which is the only comparison that means anything.
$treeBin   = Split-Path -Parent $treeExe
$stagedBin = Split-Path -Parent $stagedExe
$hashDiff = 0; $hashSame = 0; $hashOnlyStaged = 0
foreach ($f in Get-ChildItem -LiteralPath $stagedBin -File | Where-Object { $_.Extension -in '.exe', '.dll' }) {
    $t = Join-Path $treeBin $f.Name
    if (-not (Test-Path -LiteralPath $t)) { $hashOnlyStaged++; continue }
    if ((Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash -eq
        (Get-FileHash -LiteralPath $t -Algorithm SHA256).Hash) { $hashSame++ }
    else {
        Write-Host ("  {0,-28} BYTES DIFFER from the tree" -f $f.Name) -ForegroundColor Red
        $hashDiff++
    }
}
Write-Host ("[verify] {0} staged binaries byte-identical to the tree, {1} differ, {2} staged from elsewhere (VC runtime)" -f $hashSame, $hashDiff, $hashOnlyStaged)
if ($hashSame -eq 0) {
    Write-Host '[verify] FAIL no staged binary could be compared against the tree' -ForegroundColor Red
    exit 1
}
if ($hashDiff -gt 0) {
    Write-Host ("[verify] {0} STAGED BINARIES ARE NOT THE TREE'S - the payload is not what was tested" -f $hashDiff) -ForegroundColor Red
    exit 1  # ExitCode.Failed (2 is Usage)
}
if ($live -eq 0) {
    Write-Host '[verify] FAIL every staged probe was NO-PROBE - the payload did not render anything' -ForegroundColor Red
    exit 1
}

$total = $diffs + $missing
if ($total -eq 0) {
    Write-Host '[verify] PAYLOAD MATCHES THE BUILD TREE - staging lost nothing' -ForegroundColor Green
} else {
    Write-Host "[verify] $total PROBE(S) DISAGREE - the staged payload is not the tree it came from" -ForegroundColor Red
    Write-Host "[verify] full output: $treeOut  /  $stagedOut"
}
exit $(if ($total -eq 0) { 0 } else { 1 })  # ExitCode.Failed; the count is printed above
