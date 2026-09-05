# Shared PE import-table helpers, dot-sourced by stage-payload.ps1.
#
# ONE CALLER TODAY, NOT TWO. This was split out when stage-game.ps1 existed alongside it and both
# staging scripts needed the same PE parser; the packaged-game path was removed with AverGame.exe, so
# stage-payload.ps1 is the only caller now. Kept as its own file rather than folded back in: the
# helpers below are the fiddliest part of staging, they are worth testing and reading on their own,
# and a second consumer (an edition-specific stager, a verifier that wants the same answers) is a
# likelier future than never needing them again.
#
# WHY IT WAS ITS OWN FILE. Both staging scripts had to answer the same two questions -- what does this
# binary import, and is every one of those imports satisfied -- and the answer must not be allowed to
# differ between them. A second copy of a PE parser is a second copy that drifts, and the failure it
# produces (one script accepting a payload the other rejects) looks like a bug in the payload rather
# than in the scripts.

# Reads a PE's import directory and returns the imported DLL names.
#
# A real import-directory walk, not a string search: the CRT names appear as plain ASCII all over a
# PE for unrelated reasons (debug paths, embedded manifests), so a grep both false-positives and
# gives no way to tell an import from a mention.
function Get-PeImports {
    param([string] $Path)
    $b = [System.IO.File]::ReadAllBytes($Path)
    if ($b.Length -lt 0x40 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { return @() }   # 'MZ'
    $peOff = [BitConverter]::ToInt32($b, 0x3C)
    if ($peOff -le 0 -or $peOff + 0x18 -ge $b.Length) { return @() }
    if ([BitConverter]::ToUInt32($b, $peOff) -ne 0x00004550) { return @() }        # 'PE\0\0'

    $nSections  = [BitConverter]::ToUInt16($b, $peOff + 6)
    $optSize    = [BitConverter]::ToUInt16($b, $peOff + 20)
    $optOff     = $peOff + 24
    $magic      = [BitConverter]::ToUInt16($b, $optOff)
    # The data directory sits after the optional header's fixed part: 96 bytes for PE32, 112 for PE32+.
    $dirOff     = $optOff + $(if ($magic -eq 0x20B) { 112 } else { 96 })
    $importRva  = [BitConverter]::ToUInt32($b, $dirOff + 8)      # directory entry 1 = import
    if ($importRva -eq 0) { return @() }

    $sections = @()
    $secOff = $optOff + $optSize
    for ($i = 0; $i -lt $nSections; $i++) {
        $s = $secOff + ($i * 40)
        if ($s + 40 -gt $b.Length) { break }
        $sections += [pscustomobject]@{
            VirtualSize = [BitConverter]::ToUInt32($b, $s + 8)
            VirtualAddr = [BitConverter]::ToUInt32($b, $s + 12)
            RawSize     = [BitConverter]::ToUInt32($b, $s + 16)
            RawPtr      = [BitConverter]::ToUInt32($b, $s + 20)
        }
    }
    function ToOffset([uint32] $rva) {
        foreach ($s in $sections) {
            $span = [Math]::Max($s.VirtualSize, $s.RawSize)
            if ($rva -ge $s.VirtualAddr -and $rva -lt $s.VirtualAddr + $span) {
                return [int]($s.RawPtr + ($rva - $s.VirtualAddr))
            }
        }
        return -1
    }

    $names = @()
    $desc = ToOffset $importRva
    if ($desc -lt 0) { return @() }
    while ($desc + 20 -le $b.Length) {
        $nameRva = [BitConverter]::ToUInt32($b, $desc + 12)
        if ($nameRva -eq 0) { break }                            # null terminator descriptor
        $n = ToOffset $nameRva
        if ($n -lt 0) { break }
        $end = $n; while ($end -lt $b.Length -and $b[$end] -ne 0) { $end++ }
        $names += [System.Text.Encoding]::ASCII.GetString($b, $n, $end - $n)
        $desc += 20
    }
    ,$names
}

# DLLs Windows itself provides, so they are imports a payload may leave unresolved.
#
# Grounded in an actual scan of all 69 native binaries in build-release\bin, not assembled from
# memory. 'api-ms-win-*' are the UCRT and API sets, part of Windows 10 and later, and are matched by
# pattern rather than listed. NOTE mfplat and mfreadwrite (Media Foundation) are present on desktop
# SKUs but NOT on 'N' editions without the Media Feature Pack -- they are not ours to redistribute,
# so they stay on this list, but that is a support note rather than a staging obligation.
#
# THE SCAN THIS WAS GROUNDED IN HAS SINCE GONE STALE ONCE, which is worth recording because the
# failure is a hard one: staging ABORTS on an unlisted import rather than warning, so the payload
# could not be produced at all. comctl32 and dbghelp are both Windows' own and both arrived with the
# crash reporter -- comctl32 for its dialog, dbghelp for the minidump and stack walk that are the
# whole point of it. Neither is ours to redistribute and neither was in the original scan.
$AverOsProvidedDlls = @(
    'advapi32.dll', 'avrt.dll', 'comctl32.dll', 'd3d11.dll', 'd3d12.dll', 'd3dcompiler_47.dll',
    'dbghelp.dll', 'dxgi.dll',
    'gdi32.dll', 'imm32.dll', 'kernel32.dll', 'mfplat.dll', 'mfreadwrite.dll', 'mscoree.dll',
    'ole32.dll', 'oleaut32.dll', 'shell32.dll', 'shlwapi.dll', 'user32.dll', 'version.dll',
    'ws2_32.dll', 'ucrtbase.dll'
)

# The debug CRT, which ships with Visual Studio and may not be redistributed at all.
$AverDebugCrtPattern = '(?i)^(msvcp\d+d|vcruntime\d+(_\d+)?d|ucrtbased|msvcr\d+d)\.dll$'

# The release CRT and its friends: redistributable, required, and NOT part of Windows.
$AverVcRuntimePattern = '(?i)^(msvcp140.*|vcruntime140.*|concrt140|vccorlib140)\.dll$'

# Locates the newest x64 Visual C++ redistributable directory, or $null.
#
# vswhere is badly behaved: measured on this machine it returns EXIT CODE -1 while succeeding, with
# the correct path on stdout and $? still true. That alone does not throw, but under
# $ErrorActionPreference = 'Stop' any stderr output from a native command raises NativeCommandError
# and would abort the caller. Hence the guard, and deliberately NO $LASTEXITCODE check.
function Find-VcRedistDir {
    $roots = @()
    if ($env:VCToolsRedistDir) { $roots += $env:VCToolsRedistDir }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $prev = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            $vsPath = & $vswhere -latest -products * -property installationPath | Select-Object -First 1
        } catch {
            $vsPath = $null
        } finally {
            $ErrorActionPreference = $prev
        }
        if ($vsPath) { $roots += (Join-Path $vsPath 'VC\Redist\MSVC') }
    }

    $dirs = @()
    foreach ($r in $roots) {
        if (-not (Test-Path -LiteralPath $r)) { continue }
        $dirs += Get-ChildItem -LiteralPath $r -Recurse -Directory -ErrorAction SilentlyContinue |
                 Where-Object { $_.Name -match '^Microsoft\.VC\d+\.CRT$' -and $_.FullName -match '\\x64\\' -and
                                $_.FullName -notmatch 'debug_nonredist|onecore' }
    }
    # Sorted on a parsed [version], not on the string: '14.9' must lose to '14.44', and as text it
    # wins. A directory whose name does not parse sorts to the bottom rather than throwing.
    $dirs |
        Sort-Object -Property @{ Expression = {
            $v = [version]'0.0'
            if ($_.FullName -match '\\MSVC\\(\d+(\.\d+)+)\\') { [void][version]::TryParse($Matches[1], [ref]$v) }
            $v
        } } |
        Select-Object -Last 1
}
