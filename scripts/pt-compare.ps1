# Compares the rasteriser against the path tracer on the same scene and camera, and REFUSES to
# report a number it cannot stand behind.
#
# WHY THIS IS A SCRIPT AND NOT A PAIR OF COMMANDS. Every one of the three checks below has already
# produced a confidently-wrong published result in this repo:
#
#   1. WHICH RENDERER ACTUALLY PAINTED. `--rt-render-mode 0` does NOT give you the rasteriser when
#      the project manifest enables path tracing -- it hands the frame to the PATH TRACER, and the
#      two then look nothing alike for reasons that have nothing to do with shading. True raster
#      needs `--pt 0` as well. The tell is a log line, and for raster it is the ABSENCE of one,
#      which is exactly the kind of evidence a human skims past. docs/BUGS.md:96-110 and :174-186
#      record this happening twice; a third instance produced a "3.8x brighter shadows" table that
#      was comparing the path tracer against ray-driven.
#
#   2. WHETHER THE PATH TRACER CONVERGED. It accumulates to 1600 samples per pixel at 8 per step,
#      and RE-ARMS (discarding everything) whenever the scene's draw list changes -- which it does
#      several times while a level streams in. A 200-frame capture of Sponza re-armed three times
#      and never converged; the resulting MAD was largely Monte Carlo noise being read as shading
#      disagreement.
#
#   3. WHAT RESOLUTION EACH IMAGE IS. The tracer works at 1280x720 at best and is UPSCALED into a
#      ~2750px viewport. Comparing at viewport resolution measures the upscaler as much as the
#      renderers, so both images are brought down to the tracer's native size instead -- downsample
#      the sharp one rather than upsample the soft one.
#
# Exit codes: 0 comparison valid, 1 a check failed (the number is not reported), 3 nothing to run.
[CmdletBinding()]
param(
    # Both are PATHS, and both are passed positionally -- see Invoke-Capture for why, and for the
    # bug that taught it. $Project is the .ocproject FILE (not its directory); $Level is the .ocmap
    # or .ocworld file.
    [Parameter(Mandatory = $true)] [string] $Project,
    [Parameter(Mandatory = $true)] [string] $Level,
    # 800 frames is ~200 accumulation steps clear of the re-arms a level's streaming causes. Raise it
    # if the convergence check fails; do NOT lower it to save time, because an unconverged reference
    # fails silently -- it just reports a slightly wrong number.
    [int]    $Frames = 800,
    [string] $Exe = "$PSScriptRoot\..\build-release\bin\Sandbox.exe",
    [string] $OutDir = "$env:TEMP\aver-pt-compare",
    # The 3D viewport inside the editor window, in pixels of the captured screenshot. The default is
    # the docked layout at the size --frames opens; pass explicit values if the window differs.
    [int]    $CropX = 100, [int] $CropY = 420, [int] $CropW = 2600, [int] $CropH = 1430
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Exe)) { Write-Error "Sandbox.exe not found at $Exe - build Release first"; exit 3 }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Add-Type -AssemblyName System.Drawing

$failures = 0
function Fail([string] $msg) { Write-Host "  FAIL  $msg"; $script:failures++ }
function Ok([string] $msg)   { Write-Host "  ok    $msg" }

# Runs one capture and returns the log as a single string.
#
# ErrorActionPreference IS LOWERED AROUND THE CALL, and that is not sloppiness. In Windows
# PowerShell 5.1, `2>&1` on a NATIVE executable wraps every stderr line in an ErrorRecord
# (NativeCommandError) rather than passing it through as text -- so under `Stop` the first
# AVER_WARN the engine emits aborts this script, even though Sandbox.exe went on to exit 0. The
# engine warns routinely (the GI volume being too large to cache, for one), so this is the common
# path, not an edge case. The stderr still has to be MERGED, because the checks below look for lines
# the engine writes to both streams.
function Invoke-Capture([string] $name, [string[]] $extra) {
    $shot = Join-Path $OutDir "$name.png"
    $log  = Join-Path $OutDir "$name.log"
    # THE PROJECT AND LEVEL ARE POSITIONAL, and this line used to pass them as `--project` and
    # `--open-level`. NEITHER FLAG EXISTS. SandboxApp classifies a bare argument by its EXTENSION
    # (see the `argv[i][0]!='-'` branch: isOcproject -> project, isLevelFile -> level), so both
    # switches were parsed as unknown flags and their values silently ignored -- and this script then
    # compared two captures of the EMPTY EDITOR SCENE and reported a MAD for them. That is precisely
    # the class of failure the header above exists to prevent, committed inside the tool written to
    # prevent it. $Level must now be a real path (.ocmap or .ocworld), not a project-relative name.
    #
    # Placed after a COMPLETE flag pair and before $extra so no flag can swallow them as its value.
    $args = @('--frames', $Frames, $Project, $Level, '--screenshot', $shot) + $extra
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    # Start-Process, for the reason spelled out in gates.ps1's Invoke-Gate: Sandbox.exe is
    # /SUBSYSTEM:WINDOWS since 0.5.0, and Windows PowerShell neither waits for nor captures a
    # GUI-subsystem process -- `& $Exe ... 2>&1` writes an EMPTY log and this script then compares
    # nothing against nothing.
    $errLog = "$log.err"
    Start-Process -FilePath $Exe -ArgumentList $args -NoNewWindow -Wait `
                  -RedirectStandardOutput $log -RedirectStandardError $errLog | Out-Null
    if (Test-Path -LiteralPath $errLog) {
        Get-Content -LiteralPath $errLog | Add-Content -LiteralPath $log -Encoding utf8
        Remove-Item -LiteralPath $errLog -Force -ErrorAction SilentlyContinue
    }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prev
    if ($code -ne 0) { Write-Host "  note: $name exited $code" }
    return @{ shot = $shot; log = (Get-Content $log -Raw); exit = $code }
}

Write-Host "=== capturing raster (--rt-render-mode 0 --pt 0) ==="
$raster = Invoke-Capture 'raster' @('--rt-render-mode', '0', '--pt', '0')

# THE ABSENCE OF A LINE IS THE EVIDENCE. Any feature that suppresses the rasteriser announces it;
# nothing announces that the rasteriser ran, so "no feature is painting" is the only positive
# confirmation available and has to be asserted rather than assumed.
if ($raster.log -match "is painting the scene") {
    $who = [regex]::Match($raster.log, "'([^']+)' is painting the scene").Groups[1].Value
    Fail "the raster capture was painted by '$who', not the rasteriser. Both --rt-render-mode 0 and --pt 0 are required."
} else {
    Ok "raster capture: no feature suppressed the rasteriser"
}

Write-Host "=== capturing path tracer ==="
$pt = Invoke-Capture 'pt' @('--rt-render-mode', '0')

if ($pt.log -match "'Aver\.PathTracer\.SceneView' is painting the scene") {
    Ok "path-traced capture: the tracer painted the frame"
} else {
    Fail "the path-traced capture was NOT painted by the tracer -- check that the project enables path tracing"
}

# Convergence. The tracer logs this exactly once per accumulation, and re-arming clears the flag, so
# its presence AFTER the last re-arm is what the check needs -- hence comparing positions in the log
# rather than merely matching the line.
$lastRearm    = $pt.log.LastIndexOf('scene view: re-armed')
$convergedAt  = $pt.log.LastIndexOf('converged')
if ($convergedAt -lt 0) {
    Fail "the path tracer never reported convergence in $Frames frames -- raise -Frames"
} elseif ($convergedAt -lt $lastRearm) {
    Fail "the path tracer converged and was then RE-ARMED; the captured image is partial. Raise -Frames"
} else {
    Ok "path tracer converged, after its last re-arm"
}

# 4. DID THE LEVEL ACTUALLY LOAD. Added after the flag bug above was found: with the project and
# level silently dropped, BOTH captures rendered the empty editor scene, both passed every check
# above -- no feature was suppressing the rasteriser, the tracer did paint, it did converge -- and
# the script reported a perfectly ordinary MAD for two pictures of nothing. Every precondition here
# was satisfied by a run that measured nothing at all.
#
# The engine says how many entities it walked, so ask it rather than trusting the arguments.
function Assert-SceneLoaded([string] $name, [string] $log) {
    $m = [regex]::Matches($log, 'over (\d+) entities')
    $n = 0
    foreach ($x in $m) { $n = [Math]::Max($n, [int]$x.Groups[1].Value) }
    if ($n -le 0) {
        Fail "$name rendered an EMPTY SCENE (0 entities). The project or level path did not load -- both are POSITIONAL arguments and must be real file paths (.ocproject, and .ocmap/.ocworld)."
    } else {
        Ok "${name}: $n entities in the scene"
    }
}
Assert-SceneLoaded 'raster capture' $raster.log
Assert-SceneLoaded 'path-traced capture' $pt.log

if ($failures -gt 0) {
    Write-Host ""
    Write-Host "$failures check(s) failed -- NOT reporting a difference, because it would not mean anything."
    exit 1
}

# ---- the comparison itself ----------------------------------------------------------------------
# Both viewports are resampled to the SAME size before differencing. That size is the tracer's own
# accumulator resolution, so the raster image is downsampled (losing detail the tracer never had)
# rather than the tracer's being upsampled (inventing detail neither has).
$ptNative = [regex]::Match($pt.log, 'tracing at (\d+)x(\d+)')
$tw = if ($ptNative.Success) { [int]$ptNative.Groups[1].Value } else { 1280 }
$th = if ($ptNative.Success) { [int]$ptNative.Groups[2].Value } else { 720 }
# Preserve the crop's aspect rather than the accumulator's: the two images must be resampled
# identically, and matching WIDTH is enough to put them on a common footing.
$rh = [int][Math]::Round($tw * $CropH / $CropW)
Write-Host "=== differencing at ${tw}x${rh} (tracer native width) ==="

function Get-Resampled([string] $path) {
    $img = [System.Drawing.Bitmap]::FromFile($path)
    $dst = New-Object System.Drawing.Bitmap $tw, $rh
    $g = [System.Drawing.Graphics]::FromImage($dst)
    $g.InterpolationMode = 'HighQualityBicubic'
    $g.DrawImage($img, (New-Object System.Drawing.Rectangle 0, 0, $tw, $rh),
                       (New-Object System.Drawing.Rectangle $CropX, $CropY, $CropW, $CropH),
                       [System.Drawing.GraphicsUnit]::Pixel)
    $g.Dispose(); $img.Dispose()
    return $dst
}

$a = Get-Resampled $raster.shot
$b = Get-Resampled $pt.shot
# PERCENTILES ALONGSIDE THE MEAN, and the reason is a wrong conclusion this script's own output
# already caused. MAD is an average over every pixel, so a LOCALISED, HIGH-FREQUENCY difference is
# divided by the whole image and vanishes: gating the mirror-reflection ray measured 8.44 -> 8.56 MAD
# -- "no visible change" -- for a term costing 30% of the frame, which is not credible for something
# that decides what a polished surface reflects. The mean was structurally blind to it.
#
# p99/p99.9 see exactly what the mean hides: a change confined to 1% of the pixels moves them and
# leaves MAD flat. Read all three. A change that raises the percentiles while MAD stays put is
# localised, and MAD is the wrong tool for judging it.
$sum = 0.0; $n = 0; $worst = 0
$diffs = New-Object System.Collections.Generic.List[int]
$ar = 0.0; $ag = 0.0; $ab = 0.0; $br = 0.0; $bg = 0.0; $bb = 0.0
for ($y = 0; $y -lt $rh; $y++) {
    for ($x = 0; $x -lt $tw; $x++) {
        $p = $a.GetPixel($x, $y); $q = $b.GetPixel($x, $y)
        $d = [Math]::Abs($p.R - $q.R) + [Math]::Abs($p.G - $q.G) + [Math]::Abs($p.B - $q.B)
        if ($d -gt $worst) { $worst = $d }
        $sum += $d; $n++
        $diffs.Add($d)
        $ar += $p.R; $ag += $p.G; $ab += $p.B
        $br += $q.R; $bg += $q.G; $bb += $q.B
    }
}
$a.Dispose(); $b.Dispose()

Write-Host ""
Write-Host ("  raster mean   R={0,6:N2} G={1,6:N2} B={2,6:N2}" -f ($ar/$n), ($ag/$n), ($ab/$n))
Write-Host ("  tracer mean   R={0,6:N2} G={1,6:N2} B={2,6:N2}" -f ($br/$n), ($bg/$n), ($bb/$n))
$sorted = $diffs.ToArray()
[Array]::Sort($sorted)
$p50 = $sorted[[int][Math]::Floor($sorted.Length * 0.50)]
$p99 = $sorted[[int][Math]::Floor($sorted.Length * 0.99)]
$p999 = $sorted[[int][Math]::Min($sorted.Length - 1, [Math]::Floor($sorted.Length * 0.999))]
Write-Host ("  MAD           {0:N3}   (per channel, 0-255 -- a MEAN, blind to localised differences)" -f ($sum / ($n * 3)))
Write-Host ("  median diff   {0,4}      (sum of three channels)" -f $p50)
Write-Host ("  p99  diff     {0,4}      <- what MAD hides: the worst 1% of pixels" -f $p99)
Write-Host ("  p99.9 diff    {0,4}      <- reflections and other small, bright disagreements live here" -f $p999)
Write-Host ("  worst pixel   {0,4}" -f $worst)
Write-Host ""
Write-Host "  Images: $($raster.shot)  |  $($pt.shot)"
exit 0
