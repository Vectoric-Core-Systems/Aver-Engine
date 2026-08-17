<#
.SYNOPSIS
    Cuts an engine release: build, test, gate, stage, verify.

.DESCRIPTION
    The whole path from a source tree to a redistributable, in the order the steps have to happen,
    with every stage refusing to continue after a failure. It exists because that order is not
    obvious and getting it wrong produces a release that looks fine:

      1. BUILD DEBUG      the test binaries live here
      2. BUILD RELEASE    what actually ships; Debug must never be staged (see stage-payload.ps1)
      3. TEST SUITES      the headless assertions, from the Debug tree
      4. GATES DEBUG      the render oracle against the Debug baseline
      5. GATES RELEASE    and against the Release baseline -- they are SEPARATE recordings and
                          legitimately differ by an LSB, so neither substitutes for the other
      6. STAGE PAYLOAD    allowlist -> output tree, notices, import closure
      7. VERIFY PAYLOAD   the staged binary must produce the same probes as the tree it came from

    NOTHING HERE MAY OVERLAP. gates.ps1 does NOT build -- it runs whatever is already in bin -- so a
    rebuild during a gate run swaps the executable out from under it and the numbers describe
    neither version. A gate run also holds Sandbox.exe, and therefore bin\Aver.Framework.dll, long
    enough that a concurrent build fails with LNK1168. Sequential is not conservatism, it is the
    only correct order.

    A DIRTY TREE IS A WARNING, NOT A REFUSAL. stage-payload.ps1 records the commit and a dirty flag
    in payload.json, so a release cut from uncommitted work is identifiable afterwards -- but it
    should not be published. -RequireClean turns that into a refusal.

.PARAMETER Out
    Where to stage. Defaults to a versioned directory under the system temp.

.PARAMETER RequireClean
    Refuse to start when the working tree has uncommitted changes.

.PARAMETER SkipGates
    Skip steps 4 and 5. For a quick staging check only -- never for something you intend to publish.

.EXAMPLE
    ./scripts/release.ps1
    ./scripts/release.ps1 -Out ..\stage\AverEngine-0.1.0 -RequireClean
#>
[CmdletBinding()]
param(
    [string] $Out,
    [switch] $RequireClean,
    [switch] $SkipGates
)

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not $Out) {
    $ver = 'unknown'
    $cml = Get-Content (Join-Path $root 'CMakeLists.txt') -TotalCount 40
    foreach ($l in $cml) { if ($l -match 'project\(\s*AverEngine\s+VERSION\s+([0-9.]+)') { $ver = $Matches[1]; break } }
    $Out = Join-Path ([System.IO.Path]::GetTempPath()) "aver-release\AverEngine-$ver"
}

$dirty = (git status --porcelain) | Where-Object { $_ }
if ($dirty) {
    if ($RequireClean) {
        Write-Host "[release] REFUSED: the working tree has uncommitted changes and -RequireClean was given:" -ForegroundColor Red
        $dirty | ForEach-Object { Write-Host "    $_" }
        exit 1
    }
    Write-Host "[release] WARNING: working tree is DIRTY; payload.json will record it as such and this should not be published" -ForegroundColor Yellow
}

function Step($label, $block) {
    Write-Host ""
    Write-Host "########## $label ##########"
    & $block
    if ($LASTEXITCODE -ne 0) {
        Write-Host "########## $label FAILED (exit $LASTEXITCODE) ##########" -ForegroundColor Red
        exit 1
    }
    Write-Host "########## $label OK ##########"
}

Step 'BUILD DEBUG'   { & .\scripts\build.ps1 }
Step 'BUILD RELEASE' { & .\scripts\build.ps1 -Release }

# Every headless suite in bin. Discovered rather than listed, so a suite added later is not silently
# left out of the release check -- the failure mode of a hard-coded list is that it stays green while
# covering less and less.
Write-Host ""
Write-Host "########## TEST SUITES ##########"
$skip = @('MakeRig','MakeSamples','MakeFoliage','ActorSweep','AudioProbe','Sandbox')
$exes = Get-ChildItem (Join-Path $root 'build\bin\*.exe') |
        Where-Object { $_.BaseName -like '*Test' -and $skip -notcontains $_.BaseName } |
        Sort-Object BaseName
if (-not $exes) { Write-Host "[release] no test binaries found in build\bin" -ForegroundColor Red; exit 1 }
$bad = 0; $totalPass = 0
foreach ($e in $exes) {
    $o = & $e.FullName 2>&1 | Out-String
    $code = $LASTEXITCODE
    # TWO REPORTING STYLES, both counted. Most suites print a line per assertion; some print only a
    # trailing "=== N assertions, M failed ===". Counting only the first style reported the second
    # as EMPTY -- a false alarm on suites with 228, 49 and 538 assertions, and a check that cries
    # wolf is a check that gets ignored, which is the opposite of what EMPTY is for.
    $p = ([regex]::Matches($o, '  ok    |   PASS  |  PASS  ')).Count
    $f = ([regex]::Matches($o, '  FAIL  ')).Count
    $summary = [regex]::Match($o, '===\s*(\d+)\s+assertions?,\s*(\d+)\s+failed')
    if ($summary.Success) {
        $p += [int]$summary.Groups[1].Value
        $f += [int]$summary.Groups[2].Value
    }
    $totalPass += $p
    # A suite that exits 0 having asserted NOTHING is reported, not counted as a pass. That shape --
    # a test binary that cannot fail -- has been found in this tree twice.
    $mark = if ($f -gt 0 -or $code -ne 0) { $bad++; 'FAIL' } elseif ($p -eq 0) { 'EMPTY' } else { 'ok' }
    Write-Host ("  {0,-24} {1,-6} pass={2,-5} fail={3} exit={4}" -f $e.BaseName, $mark, $p, $f, $code)
}
if ($bad -gt 0) { Write-Host "########## TEST SUITES FAILED ($bad) ##########" -ForegroundColor Red; exit 1 }
Write-Host "########## TEST SUITES OK -- $($exes.Count) suites, $totalPass assertions ##########"

if (-not $SkipGates) {
    Step 'GATES DEBUG'   { & .\scripts\gates.ps1 -Config baseline }
    Step 'GATES RELEASE' { & .\scripts\gates.ps1 -Release -Config baseline }
} else {
    Write-Host ""
    Write-Host "########## GATES SKIPPED -- this build is NOT publishable ##########" -ForegroundColor Yellow
}

Step 'STAGE PAYLOAD'  { & .\scripts\stage-payload.ps1 -Out $Out -Force }
Step 'VERIFY PAYLOAD' { & .\scripts\verify-payload.ps1 -Payload $Out }

Write-Host ""
Write-Host "########## RELEASE READY ##########" -ForegroundColor Green
Write-Host "  $Out"
Write-Host "  manifest: $(Join-Path $Out 'payload.json')"
if ($dirty) { Write-Host "  WARNING: cut from a DIRTY tree - do not publish this one" -ForegroundColor Yellow }
