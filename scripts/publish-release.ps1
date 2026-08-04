<#
.SYNOPSIS
    Publishes a staged engine payload to the public download repository as a GitHub release.

.DESCRIPTION
    The last mile: staged payload in, a release the Aver Launcher can install from out. It is the
    step release.ps1 deliberately stops short of, because staging is reversible and publishing is not.

      1. VERSION      read from CMakeLists.txt project(), never passed in
      2. VALIDATE     the payload really is that version, Release, and cut from a clean tree
      3. PACK         averdist pack -> <edition>-<version>.averpack + manifest-<edition>-<version>.json
      4. INDEX        averdist index -> index.json, the feed the launcher actually reads
      5. VERIFY       averdist verify -- reconstructs every pack and hash-checks it, offline
      6. ZIP          the same payload as an ordinary archive, for installing by hand
      7. UPLOAD       gh release create (or --clobber onto an existing tag)
      8. RESOLVE      fetch releases/latest/download/index.json and prove the feed is live

    WHY THIS IS NOT THE LAUNCHER REPO'S publish-release.ps1. That one publishes the launcher AND the
    engine together, and requires a built launcher asset set to do either. Cutting an engine release
    should not require having a launcher build to hand, and should never quietly ship a launcher.
    This publishes exactly one thing.

    THE TAG IS THE VERSION. Launcher discovery reads the version off the tag name, the editions off
    the manifest asset names, and nothing else -- so the tag must be v<version> and must match what
    the manifests say. A mismatch produces a release that looks fine on GitHub and is invisible or
    broken to every launcher.

    NEVER --prerelease. Discovery resolves through releases/latest/download/, which only ever points
    at a non-prerelease. Marking a beta as a GitHub pre-release is indistinguishable, from the
    launcher's side, from never having published at all. The word "beta" belongs in the title and the
    notes, which is where it goes.

.PARAMETER Payload
    The staged payload directory from release.ps1. Defaults to the versioned directory under temp.

.PARAMETER Out
    Where to assemble the feed (the exact set of files the release carries). Defaults to a versioned
    directory under temp, beside the payload.

.PARAMETER Edition
    Which edition to pack. Defaults to standard.

.PARAMETER Channel
    Appears in the zip name and the release title. Defaults to beta.

.PARAMETER NotesFile
    Markdown file for the release body. Without one, a minimal body is generated; a real release
    should pass one.

.PARAMETER AverDist
    Path to averdist.exe. Defaults to the launcher checkout beside this one.

.PARAMETER Repo
    The download repository. Defaults to Vectoric-Core-Systems/Aver-Engine-download.

.PARAMETER AllowDirty
    Publish a payload whose payload.json records sourceDirty. Refused by default: a release nobody
    can point at a commit is a release nobody can reproduce.

.PARAMETER WhatIf
    Do everything local -- pack, index, verify, zip -- and print the upload instead of running it.

.EXAMPLE
    ./scripts/publish-release.ps1 -WhatIf
    ./scripts/publish-release.ps1 -NotesFile .\notes-0.1.1.md
#>
[CmdletBinding()]
param(
    [string] $Payload,
    [string] $Out,
    [string] $Edition = 'standard',
    [string] $Channel = 'beta',
    [string] $NotesFile,
    [string] $AverDist,
    [string] $Repo = 'Vectoric-Core-Systems/Aver-Engine-download',
    [switch] $AllowDirty,
    [switch] $WhatIf
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Fail($msg) { Write-Host "[publish] $msg" -ForegroundColor Red; exit 1 }
function Say($msg)  { Write-Host "[publish] $msg" -ForegroundColor Cyan }

# ---- 1. the version, from the one place that defines it ------------------------------------------
# Not a parameter. Every other artefact in the chain -- Version.hpp, the scaffold's ENGINE line, the
# payload manifest -- derives from project(), so letting a caller pass a different one here would only
# ever create a release whose tag disagrees with the binaries inside it.
$version = $null
foreach ($line in (Get-Content (Join-Path $root 'CMakeLists.txt') -TotalCount 40)) {
    if ($line -match 'project\(\s*AverEngine\s+VERSION\s+([0-9.]+)') { $version = $Matches[1]; break }
}
if (-not $version) { Fail 'no project(AverEngine VERSION ...) in CMakeLists.txt' }

$tag     = "v$version"
$baseUrl = "https://github.com/$Repo/releases/download/$tag"
$temp    = [System.IO.Path]::GetTempPath()
if (-not $Payload) { $Payload = Join-Path $temp "aver-release\AverEngine-$version" }
if (-not $Out)     { $Out     = Join-Path $temp "aver-feed\$tag" }

Say "engine $version  ->  $Repo $tag"

# ---- 2. the payload has to be the thing we think it is -------------------------------------------
if (-not (Test-Path -LiteralPath $Payload)) {
    Fail "no staged payload at $Payload`n          run: ./scripts/release.ps1 -RequireClean"
}
$payloadJson = Join-Path $Payload 'payload.json'
if (-not (Test-Path -LiteralPath $payloadJson)) { Fail "no payload.json in $Payload - not a staged payload" }
$meta = Get-Content -LiteralPath $payloadJson -Raw | ConvertFrom-Json

# Each of these has a failure mode that is silent on GitHub and loud on a user's machine.
if ($meta.version -ne $version) {
    Fail "payload.json says $($meta.version), CMakeLists says $version`n          the staged tree is stale - re-run ./scripts/release.ps1"
}
if ($meta.config -ne 'Release') {
    Fail "payload is a $($meta.config) build. Debug binaries must never be published."
}
if ($meta.sourceDirty -and -not $AllowDirty) {
    Fail "payload was cut from a DIRTY tree (commit $($meta.sourceCommit))`n          commit first, or pass -AllowDirty if you know why"
}
Say ("payload ok: {0} files, {1:N0} bytes, commit {2}" -f $meta.fileCount, $meta.totalBytes, $meta.sourceCommit.Substring(0, 12))

# ---- 3. averdist ---------------------------------------------------------------------------------
# The pack format is the launcher's, so its tool owns it. Looked for beside this checkout rather than
# vendored, because a copy here would be the copy that goes stale.
if (-not $AverDist) {
    $guesses = @(
        (Join-Path (Split-Path -Parent $root) 'Aver launcher\src\Aver.Dist.Cli\bin\Release\net10.0\averdist.exe'),
        (Join-Path (Split-Path -Parent $root) 'Aver launcher\src\Aver.Dist.Cli\bin\Debug\net10.0\averdist.exe')
    )
    $AverDist = $guesses | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $AverDist -or -not (Test-Path -LiteralPath $AverDist)) {
    Fail "averdist.exe not found. Build it:`n          dotnet build `"..\Aver launcher\src\Aver.Dist.Cli`"`n          or pass -AverDist <path>"
}

# ---- gh, checked before anything is built ---------------------------------------------------------
# Up front rather than at the upload: finding out about a missing login after packing is merely
# annoying, but finding out halfway through an upload leaves a half-populated release that discovery
# will happily read.
$signedIn = $false
$gh = (Get-Command gh -ErrorAction SilentlyContinue).Source
if ($gh) {
    # Through cmd so the streams merge there. PowerShell 5.1 wraps a native command's stderr in
    # ErrorRecords and clears $?, turning a plain "not logged in" into what looks like a crash.
    $null = cmd /c "gh auth status 2>&1"
    $signedIn = $LASTEXITCODE -eq 0
}
if (-not $WhatIf) {
    if (-not $gh)       { Fail "GitHub CLI not found. winget install --id GitHub.cli" }
    if (-not $signedIn) { Fail "not signed in to GitHub. Run: gh auth login" }
}

# ---- 4-5. pack, index, verify ---------------------------------------------------------------------
# Rebuilt from scratch every time. An incrementally-updated feed directory is how you publish an
# index that advertises a pack the release does not carry.
if (Test-Path -LiteralPath $Out) { Remove-Item -LiteralPath $Out -Recurse -Force }
$null = New-Item -ItemType Directory -Path $Out -Force

Say "packing $Edition ..."
& $AverDist pack --payload $Payload --out $Out --edition $Edition --base-url $baseUrl
if ($LASTEXITCODE -ne 0) { Fail 'averdist pack failed' }

Say 'indexing ...'
& $AverDist index --out $Out --base-url $baseUrl
if ($LASTEXITCODE -ne 0) { Fail 'averdist index failed' }

# The one check that proves the download works without a launcher: every pack is reconstructed into a
# temp tree and hash-checked against its own manifest. A pack that fails here uploads perfectly and
# installs as corruption.
Say 'verifying the feed reconstructs ...'
& $AverDist verify --out $Out
if ($LASTEXITCODE -ne 0) { Fail 'averdist verify FAILED - the feed would install corrupt. Nothing uploaded.' }

# A feed whose URLs point at another tag is the most confusing way for this to fail: every asset
# uploads cleanly and every download 404s.
$index = Get-Content -LiteralPath (Join-Path $Out 'index.json') -Raw | ConvertFrom-Json
if ($index.manifestUrl -notlike "$baseUrl/*") {
    Fail "index.json points at $($index.manifestUrl)`n          expected a URL under $baseUrl"
}

# ---- 6. the hand-install zip ----------------------------------------------------------------------
# Same bytes as the pack, ordinary container. Kept because "download a zip and run the exe" should
# not require installing a launcher first.
$zipName = "AverEngine-$version-$Channel-win64.zip"
$zipPath = Join-Path $Out $zipName
Say "zipping $zipName ..."
Compress-Archive -Path (Join-Path $Payload '*') -DestinationPath $zipPath -CompressionLevel Optimal -Force
$zipHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash

# ---- assets ---------------------------------------------------------------------------------------
$assets = @(Get-ChildItem -LiteralPath $Out -File |
            Where-Object { $_.Name -eq 'index.json' -or $_.Name -like 'manifest-*.json' -or
                           $_.Extension -eq '.averpack' -or $_.Extension -eq '.zip' } |
            ForEach-Object { $_.FullName })
if ($assets.Count -lt 4) { Fail "expected index, manifest, pack and zip in $Out - found $($assets.Count)" }

Write-Host ''
Say "$Repo  $tag"
foreach ($a in $assets) { Write-Host ("            {0,-40} {1,14:N0} bytes" -f (Split-Path $a -Leaf), (Get-Item $a).Length) }
Write-Host ("            SHA256 {0}  {1}" -f $zipName, $zipHash)
Write-Host ''

# ---- notes ----------------------------------------------------------------------------------------
$title = "Aver Engine $tag $Channel"
if ($NotesFile) {
    if (-not (Test-Path -LiteralPath $NotesFile)) { Fail "no notes file at $NotesFile" }
    $notesPath = (Resolve-Path -LiteralPath $NotesFile).Path
} else {
    $notesPath = Join-Path $Out 'RELEASE-NOTES.md'
    @"
Engine $version. Binaries only -- this repository does not carry the engine source.

Install with the Aver Launcher, which reads ``index.json`` from this release. To install by hand,
download ``$zipName``, unpack it anywhere and run ``bin\Sandbox.exe``, keeping ``bin\`` and
``scripting\`` as siblings.

``````
SHA256  $zipHash
``````
"@ | Set-Content -LiteralPath $notesPath -Encoding utf8
    Say "no -NotesFile given; generated a minimal body at $notesPath"
}

# ---- 7. upload ------------------------------------------------------------------------------------
if ($WhatIf) {
    Write-Host '[publish] -WhatIf: nothing uploaded. Would run:' -ForegroundColor Yellow
    Write-Host "            gh release create $tag --repo $Repo --title `"$title`" --latest <$($assets.Count) assets>"
    if (-not $signedIn) { Write-Host '            (and you are not signed in yet: gh auth login)' -ForegroundColor Yellow }
    exit 0
}

$null = cmd /c "gh release view $tag --repo $Repo 2>&1"
if ($LASTEXITCODE -eq 0) {
    Say "$tag already exists; replacing its assets with --clobber"
    & gh release upload $tag --repo $Repo --clobber @assets
    if ($LASTEXITCODE -ne 0) { Fail 'asset upload failed' }
    & gh release edit $tag --repo $Repo --title $title --notes-file $notesPath --latest --prerelease=false
    if ($LASTEXITCODE -ne 0) { Fail 'updating the release failed' }
} else {
    # --latest, never --prerelease. See the header: prereleases are invisible to discovery.
    & gh release create $tag --repo $Repo --title $title --notes-file $notesPath --latest @assets
    if ($LASTEXITCODE -ne 0) { Fail 'creating the release failed' }
}

# ---- 8. prove the feed is actually live ------------------------------------------------------------
# The upload succeeding says the assets exist. This says the launcher can find them, which is a
# different claim and the one that matters.
Write-Host ''
Say 'resolving the published feed ...'
$feedUrl = "https://github.com/$Repo/releases/latest/download/index.json"
try {
    $r = Invoke-WebRequest -Uri $feedUrl -MaximumRedirection 10 -TimeoutSec 30 -UseBasicParsing
    $live = ($r.Content | ConvertFrom-Json)
    $latest = ($live.editions.PSObject.Properties | ForEach-Object { $_.Value.latest } | Sort-Object -Unique | Select-Object -Last 1)
    if ($latest -ne $version) {
        Write-Host "            served index advertises $latest, expected $version" -ForegroundColor Yellow
    } else {
        Write-Host "            $($r.StatusCode)  feed advertises $latest" -ForegroundColor Green
    }
} catch {
    Write-Host "            FAILED to fetch $feedUrl" -ForegroundColor Red
    Write-Host '            If this 404s the release was probably marked prerelease.' -ForegroundColor Yellow
    exit 1
}

Write-Host ''
Write-Host "########## PUBLISHED -- $Repo $tag ##########" -ForegroundColor Green
Write-Host "  https://github.com/$Repo/releases/tag/$tag"
exit 0
