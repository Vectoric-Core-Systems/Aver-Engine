# A/B/C of many-light direct lighting (docs/plans/wave4/MANY_LIGHTS.md section 5C), and it REFUSES to report a
# number it cannot stand behind. Modelled on pt-compare.ps1, and for the same three reasons, restated for this
# feature:
#
#   1. WHICH PATH ACTUALLY LIT THE FRAME. voxi.manyLights 2 does nothing on a FidelityFX or None frame, below 1
#      light, on a backend without staged passes, or when CSRdManyLights did not compile or its two targets could
#      not be made -- each of which only logs. A capture that fell back to the light grid would be reported as the
#      stochastic path. The engine says "many lights running" when the pass dispatched; its absence is the failure,
#      so this script asserts the line for the tree capture and the ABSENCE of it for the legacy one.
#   2. WHETHER THE REFERENCE CONVERGED. The reference is voxi.manyLights 3 (every light, one ray) through the
#      reference path tracer (--denoiser 1 --set voxi.ptMode 1), which accumulates; it needs the 1500 frames the
#      design names, and re-arms on any camera, sun or lamp change. Fewer frames are refused.
#   3. WHETHER THE LEVEL LOADED. Both project and level are POSITIONAL; a wrong path renders the empty editor
#      scene twice and the numbers look ordinary. The engine reports the entities it walked.
#
# Scenarios per light count (-Lights 48,256,1000,2000,4000; 0 = the scene's own lights):
#   ref     manyLights 3 (needs <= 512 lights), --denoiser 1 --set voxi.ptMode 1, -RefFrames frames   [-Reference]
#   legacy  manyLights 0                       (the light grid's capped tail)
#   tree    manyLights 2                       (the stochastic estimator; add -Auto to also run manyLights 1)
# Run on NRD2 (--denoiser 2, the default here) so the tree is live; -Wobble adds the moving-camera capture
# (--cam-wobble 4 60). Metrics come from many_lights_metrics.py. At 48 lights or fewer, auto must equal legacy:
# -Identity compares them byte for byte.
#
# Do not run it while another Sandbox.exe is open: runs share the GPU. GPU idle first (aver-perf-measuring-traps).
# Exit codes: 0 valid, 1 a check failed (nothing is reported), 3 nothing to run.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Project,
    [Parameter(Mandatory = $true)] [string] $Level,
    [int[]]  $Lights = @(0),
    [int]    $Frames = 300,
    [int]    $RefFrames = 1500,
    [switch] $Reference,
    [switch] $Auto,
    [switch] $Identity,
    [switch] $Wobble,
    [string] $Exe = "$PSScriptRoot\..\build-release\bin\Sandbox.exe",
    [string] $OutDir = "$env:TEMP\aver-many-lights",
    [int]    $MaskTop = 70,
    [string[]] $ExtraArgs = @()
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Exe)) { Write-Host "Sandbox.exe not found at $Exe - build Release first" -ForegroundColor Red; exit 3 }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$metrics = Join-Path $PSScriptRoot 'many_lights_metrics.py'

$failures = 0
function Fail([string] $msg) { Write-Host "  FAIL  $msg"; $script:failures++ }
function Ok([string] $msg)   { Write-Host "  ok    $msg" }

# One headed, bounded capture. Quoted because Start-Process does not quote an element with a space (see
# pt-compare.ps1); stderr merged into the log because the engine warns on both streams.
function Invoke-Capture([string] $name, [int] $frames, [string[]] $extra) {
    $shot = Join-Path $OutDir "$name.png"
    $log  = Join-Path $OutDir "$name.log"
    $q = { param($s) '"' + $s + '"' }
    $args = @('--frames', $frames, (& $q $Project), (& $q $Level), '--screenshot', (& $q $shot), '--aversr', 'off') + $extra
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $errLog = "$log.err"
    Start-Process -FilePath $Exe -ArgumentList $args -NoNewWindow -Wait -RedirectStandardOutput $log -RedirectStandardError $errLog | Out-Null
    if (Test-Path -LiteralPath $errLog) {
        Get-Content -LiteralPath $errLog | Add-Content -LiteralPath $log -Encoding utf8
        Remove-Item -LiteralPath $errLog -Force -ErrorAction SilentlyContinue
    }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prev
    if ($code -ne 0) { Write-Host "  note: $name exited $code" }
    return @{ shot = $shot; log = (Get-Content $log -Raw); exit = $code; name = $name }
}

function Assert-SceneLoaded($cap) {
    $n = 0
    foreach ($m in [regex]::Matches($cap.log, 'over (\d+) entities')) { $n = [Math]::Max($n, [int]$m.Groups[1].Value) }
    if ($n -le 0) { Fail "$($cap.name) rendered an EMPTY SCENE (0 entities): project and level are positional real paths" }
    else { Ok "$($cap.name): $n entities in the scene" }
}

# The light count the engine built its tree for ("light tree: N light(s)"), or -1.
function Get-TreeLights($cap) {
    $m = [regex]::Matches($cap.log, 'light tree: (\d+) light')
    if ($m.Count -eq 0) { return -1 }
    return [int]$m[$m.Count - 1].Groups[1].Value
}

function Assert-TreeLive($cap, [int] $wantLights) {
    if ($cap.log -match 'many lights running') { Ok "$($cap.name): CSRdManyLights ran" }
    else {
        $why = [regex]::Match($cap.log, 'many lights: [^\r\n]*').Value
        Fail "$($cap.name): the stochastic tail did NOT run (no 'many lights running' line). $why"
    }
    $built = Get-TreeLights $cap
    if ($wantLights -gt 0 -and $built -ne $wantLights) {
        Fail "$($cap.name): asked for $wantLights lights, the tree was built for $built"
    }
}

function Assert-TreeAbsent($cap) {
    if ($cap.log -match 'many lights running|light tree: \d+ light') { Fail "$($cap.name): the tree ran in a capture that should be legacy" }
    else { Ok "$($cap.name): legacy tail, no tree" }
}

function Run-Metrics([string[]] $cmdArgs) {
    & python $metrics @cmdArgs
    if ($LASTEXITCODE -ne 0) { Fail "many_lights_metrics.py failed: $($cmdArgs -join ' ')" }
}

foreach ($n in $Lights) {
    $tag = if ($n -gt 0) { "n$n" } else { "scene" }
    $stress = if ($n -gt 0) { @('--set', 'voxi.debugStressLights', "$n") } else { @() }
    $common = $stress + @('--denoiser', '2') + $ExtraArgs
    Write-Host ""
    Write-Host "=== $tag ==="

    $ref = $null
    if ($Reference) {
        if ($RefFrames -lt 1500) { Fail "the reference needs 1500 frames to converge; -RefFrames $RefFrames is refused" }
        elseif ($n -gt 512) { Fail "voxi.manyLights 3 walks at most 512 lights; $n is refused" }
        else {
            $ref = Invoke-Capture "$tag-ref" $RefFrames ($stress + @('--denoiser', '1', '--set', 'voxi.ptMode', '1', '--set', 'voxi.manyLights', '3') + $ExtraArgs)
            Assert-SceneLoaded $ref
            if ($ref.log -notmatch 'many lights, exhaustive') { Fail "$($ref.name): voxi.manyLights 3 did not engage (no 'exhaustive' line)" }
            else { Ok "$($ref.name): exhaustive ground truth" }
        }
    }

    $legacy = Invoke-Capture "$tag-legacy" $Frames ($common + @('--set', 'voxi.manyLights', '0'))
    Assert-SceneLoaded $legacy
    Assert-TreeAbsent $legacy

    $tree = Invoke-Capture "$tag-tree" $Frames ($common + @('--set', 'voxi.manyLights', '2'))
    Assert-SceneLoaded $tree
    Assert-TreeLive $tree $n

    $caps = @($legacy, $tree)
    if ($Auto) {
        $auto = Invoke-Capture "$tag-auto" $Frames ($common + @('--set', 'voxi.manyLights', '1'))
        Assert-SceneLoaded $auto
        $caps += $auto
        if ($Identity) {
            if ($n -gt 0 -and $n -le 48) {
                Write-Host "-- auto vs legacy at $n lights (must be identical) --"
                Run-Metrics @('identical', $legacy.shot, $auto.shot)
            } else { Write-Host "  note: -Identity applies at 48 lights or fewer; skipped for $tag" }
        }
    }
    if ($Wobble) {
        $wt = Invoke-Capture "$tag-tree-wobble" $Frames ($common + @('--set', 'voxi.manyLights', '2', '--cam-wobble', '4', '60'))
        Assert-SceneLoaded $wt
        Assert-TreeLive $wt $n
        $caps += $wt
    }

    if ($failures -gt 0) { continue }
    $against = if ($ref) { $ref.shot } else { $legacy.shot }
    Write-Host "-- metrics against $(if ($ref) { 'the exhaustive reference' } else { 'the legacy capture' }) --"
    $tests = @($caps | Where-Object { $_.shot -ne $against } | ForEach-Object { $_.shot })
    Run-Metrics (@('compare', '--ref', $against, '--mask-top', $MaskTop) + $tests)
}

if ($failures -gt 0) {
    Write-Host ""
    Write-Host "$failures check(s) failed -- NOT reporting a difference, because it would not mean anything."
    exit 1
}
Write-Host ""
Write-Host "Captures in $OutDir. GPU time: run with --gpu-timing and read 'Voxi RD many lights' (and 'Voxi RD tail lights' for the filter)."
exit 0
