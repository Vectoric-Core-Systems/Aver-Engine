<#
.SYNOPSIS
    Publishes a staged engine payload to the public download repository as a GitHub release.

.DESCRIPTION
    The last mile: staged payload in, a release the Aver Launcher can install from out. It is the
    step release.ps1 deliberately stops short of, because staging is reversible and publishing is not.

      1. VERSION      read from CMakeLists.txt project(), never passed in
      2. VALIDATE     the payload really is that version, Release, and cut from a clean tree
      3. BUILD TOOLS  dotnet build averdist + averlauncher-cli, fresh, from the launcher checkout
      4. PACK         averdist pack -> <edition>-<version>.averpack + manifest-<edition>-<version>.json
      5. INDEX        averdist index -> index.json, the feed the launcher actually reads
      6. VERIFY       averdist verify -- reconstructs every pack and hash-checks it, offline
      7. INSTALL      averlauncher-cli installs the feed for real, then boots what it installed
      8. ZIP          the same payload as an ordinary archive, for installing by hand
      9. UPLOAD       gh release create (or --clobber onto an existing tag)
     10. RESOLVE      fetch releases/latest/download/index.json and prove the feed is live

    STEP 7 IS THE ONE averdist ITSELF CANNOT DO. verify reconstructs a pack against its own manifest
    -- both written by the same tool -- so it only catches averdist disagreeing with itself. It never
    touches FeedSource, PackInstaller, or any code the shipping launcher actually runs. Step 7 does:
    a real install into a scratch directory through the launcher's own install path, then starting
    the binary it installed and confirming it reaches its startup line. A feed that fails here would
    never have failed averdist verify, and would have shipped.

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

.PARAMETER LauncherDir
    The Aver Launcher checkout that owns the pack format. Defaults to a sibling of this repo,
    "..\Aver launcher". Rebuilt fresh (Release) every run -- see -AverDist for why.

.PARAMETER AverDist
    Path to an already-built averdist.exe, skipping the rebuild in -LauncherDir. ONLY for a case
    where you deliberately want to pin an exact binary (a downloaded launcher release, a checkout at
    a specific commit). The default path always rebuilds rather than trusting a binary already on
    disk, because a stale one is a silent format bug: it exits 0, produces a feed that looks right,
    and only fails on a launcher build new enough to have moved past it. That exact failure was
    caught by hand once already -- an averdist.exe two days older than the Program.cs that migrated
    the pack layout from nested manifests\<edition>\<version>.json to flat manifest-<edition>-
    <version>.json, run without rebuilding first, silently produced the old layout. Nothing about
    that run looked wrong until the bytes were inspected.

.PARAMETER AverLauncherCli
    Path to an already-built averlauncher-cli.exe, for use alongside -AverDist. Without it, a pinned
    -AverDist SKIPS the real launcher-install self-check (step 7) with a loud warning -- averdist
    verify alone never proves the launcher itself can install what was packed.

.PARAMETER Repo
    The download repository. Defaults to Vectoric-Core-Systems/Aver-Engine-download.

.PARAMETER AllowDirty
    Publish a payload whose payload.json records sourceDirty. Refused by default: a release nobody
    can point at a commit is a release nobody can reproduce.

.PARAMETER WhatIf
    Do everything local -- build the tools, pack, index, verify, install and boot, zip -- and print
    the upload instead of running it. Nothing through step 8 touches the network either way; -WhatIf
    only changes whether step 9 runs.

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
    [string] $LauncherDir,
    [string] $AverDist,
    [string] $AverLauncherCli,
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
# The pack format is the launcher's, so its tools own it. Looked for beside this checkout rather
# than vendored, because a copy here would be the copy that goes stale.
if ($AverDist) {
    if (-not (Test-Path -LiteralPath $AverDist)) { Fail "-AverDist given but not found: $AverDist" }
    Say "using pinned averdist: $AverDist"
    if ($AverLauncherCli -and -not (Test-Path -LiteralPath $AverLauncherCli)) {
        Fail "-AverLauncherCli given but not found: $AverLauncherCli"
    }
    if (-not $AverLauncherCli) {
        Write-Host '[publish] -AverDist given with no -AverLauncherCli: the real launcher-install self-check is SKIPPED.' -ForegroundColor Yellow
        Write-Host '          averdist verify only checks the pack against its own manifest, not that the launcher can' -ForegroundColor Yellow
        Write-Host '          install from it. Pass -AverLauncherCli too to keep that check.' -ForegroundColor Yellow
    }
} else {
    if (-not $LauncherDir) { $LauncherDir = Join-Path (Split-Path -Parent $root) 'Aver launcher' }
    if (-not (Test-Path -LiteralPath $LauncherDir)) {
        Fail "no launcher checkout at $LauncherDir`n          pass -LauncherDir <path>, or -AverDist <exe> to skip the rebuild"
    }
    $dotnet = (Get-Command dotnet -ErrorAction SilentlyContinue).Source
    if (-not $dotnet) { Fail 'dotnet SDK not found' }

    # REBUILT, NEVER JUST LOCATED. A found-but-stale averdist.exe is a silent format bug: it exits 0
    # and produces a feed that looks right, and only fails on a launcher build new enough to have
    # moved past whatever it predates. See the parameter doc for the exact incident.
    Say 'building averdist (Release) ...'
    & $dotnet build (Join-Path $LauncherDir 'src\Aver.Dist.Cli\Aver.Dist.Cli.csproj') -c Release --nologo -v quiet
    if ($LASTEXITCODE -ne 0) { Fail 'dotnet build averdist failed' }
    $AverDist = Join-Path $LauncherDir 'src\Aver.Dist.Cli\bin\Release\net10.0\averdist.exe'
    if (-not (Test-Path -LiteralPath $AverDist)) { Fail "build succeeded but $AverDist is missing - check the csproj's output path" }

    # Same story for the CLI that actually drives an install: this is the code path the launcher's
    # own UI uses, and it is the one thing that proves the pack does not just look right but installs
    # and runs. Built alongside averdist so one -LauncherDir / dotnet failure covers both.
    Say 'building averlauncher-cli (Release) ...'
    & $dotnet build (Join-Path $LauncherDir 'src\Aver.Launcher.Cli\Aver.Launcher.Cli.csproj') -c Release --nologo -v quiet
    if ($LASTEXITCODE -ne 0) { Fail 'dotnet build averlauncher-cli failed' }
    $averLauncherCli = Join-Path $LauncherDir 'src\Aver.Launcher.Cli\bin\Release\net10.0-windows\averlauncher-cli.exe'
    if (-not (Test-Path -LiteralPath $averLauncherCli)) { Fail "build succeeded but $averLauncherCli is missing - check the csproj's output path" }
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

# ---- 4-6. pack, index, verify ---------------------------------------------------------------------
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

# ---- 7. the actual launcher install, not just averdist checking its own homework -----------------
# averdist verify reconstructs the pack against its own manifest -- both written by the same tool, so
# it can only catch averdist disagreeing with itself. It never calls FeedSource, PackInstaller, or
# any code the shipping launcher runs. This does: the same install path the app's UI drives, for
# real, into a scratch directory, followed by actually starting the binary it installed.
#
# A LOCAL DIRECTORY RESOLVES BY FILE NAME, not by the URLs embedded in the manifest -- see
# docs/FEED.md in the launcher checkout. So this is a faithful rehearsal of the published install
# even though $baseUrl points at GitHub and nothing has been uploaded yet: proven empirically, not
# just cited, by running it against a $baseUrl that does not resolve at all and watching it install
# anyway.
if ($AverLauncherCli) {
    $installRoot = Join-Path $temp "aver-install-check\$tag"
    if (Test-Path -LiteralPath $installRoot) { Remove-Item -LiteralPath $installRoot -Recurse -Force }

    Say 'installing through the launcher''s own code path (FeedSource + PackInstaller) ...'
    & $AverLauncherCli install --feed $Out --edition $Edition --root $installRoot --version $version
    if ($LASTEXITCODE -ne 0) { Fail 'averlauncher-cli install FAILED - this feed would not install through the real launcher. Nothing uploaded.' }

    $installedExe = Join-Path $installRoot "$Edition\$version\$($meta.entryPoint)"
    if (-not (Test-Path -LiteralPath $installedExe)) { $installedExe = Join-Path $installRoot "$Edition\$version\bin\Sandbox.exe" }
    if (-not (Test-Path -LiteralPath $installedExe)) { Fail "install reported success but $installedExe does not exist. Nothing uploaded." }

    # NO 2>&1 HERE. PowerShell 5.1 wraps a native command's stderr lines in NativeCommandErrorRecord
    # and clears $?, and with $ErrorActionPreference = 'Stop' (set at the top of this script) that is
    # a TERMINATING error -- an ordinary WARN line from the engine (e.g. "createTlas without
    # ray-tracing support" on a GPU without it) would abort this script and report a false failure on
    # a perfectly good install. The one line being checked for is INFO-level and lands on stdout
    # already; nothing here needs stderr merged in.
    Say 'booting the installed binary headlessly ...'
    $bootLog = & $installedExe --headless --frames 5
    $bootExit = $LASTEXITCODE
    if ($bootExit -ne 0) {
        Fail "installed Sandbox.exe exited $bootExit - the install is bad. Nothing uploaded. Output:`n$($bootLog -join "`n")"
    }
    $bootLine = $bootLog | Select-String 'Engine .* starting' | Select-Object -First 1
    if (-not $bootLine) {
        Fail "installed Sandbox.exe exited 0 but never logged its startup line - something is wrong post-install. Nothing uploaded. Output:`n$($bootLog -join "`n")"
    }
    Say "installed binary boots clean: $bootLine"
} else {
    Write-Host '[publish] SKIPPING the launcher-install self-check (-AverDist given without -AverLauncherCli).' -ForegroundColor Yellow
}

# ---- 8. the hand-install zip ----------------------------------------------------------------------
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

# ---- 9. upload ------------------------------------------------------------------------------------
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

# ---- 10. prove the feed is actually live ------------------------------------------------------------
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
