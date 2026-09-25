# The NVIDIA section of THIRD-PARTY-NOTICES.txt, dot-sourced by stage-payload.ps1 and stage-game.ps1.
#
# ONE COPY FOR BOTH STAGERS, because this is the section the NVIDIA RTX SDKs Licence is strict about
# and two hand-kept copies of it are two chances for one to fall behind. docs/NVIDIA-SDK-COMPLIANCE.md
# is the account of WHY each line below is owed; this file is only the mechanism.
#
# WHAT IS OWED, per shipped component (quoted in full in the compliance doc):
#   * "This software contains source code provided by NVIDIA Corporation." -- the RTX SDKs Licence's
#     notice for distributed source. RTXDI and SHaRC ship as verbatim HLSL under shaders\.
#   * Clause 6.1(c) attribution. With no credit screen, it goes "prominently in end user
#     documentation" -- and for a packaged build, this file IS that documentation.
#   * MIT copyright and permission notices for MathLib, which NRD compiles in.
#
# DERIVED FROM WHAT SHIPS, NOT FROM WHAT IS VENDORED: a notice for a component that is absent is as
# wrong as a missing one. NRD is compiled into the renderer, so it follows the build flags that put
# it there. RTXDI and SHaRC are source text, so they follow the staged tree itself -- a shaders
# directory named for them either reached the package or it did not.
#
# ShaderMake is deliberately absent: it runs at build time to compile NRD's shaders and none of it
# is redistributed. RTXGI's NRC is absent too -- it is not vendored at all (Tensor Cores only).
#
# Calls the caller's own Fail on a missing licence file, like both stagers' $components loops.

# Returns the NVIDIA components a staged tree actually redistributes.
function Get-AverNvidiaComponents([string] $StagedDir, [hashtable] $Options) {
    $shipped = New-Object System.Collections.Generic.List[hashtable]
    # NRD is linked only through the Voxi renderer (Aver.Render.NRD -> Aver.Render.Voxi.Renderer), and
    # cmake/AverNRD.cmake can switch AVER_WITH_NRD off by itself on a machine that cannot build it.
    if ($Options['AVER_WITH_NRD'] -and $Options['AVER_MODULE_VOXI']) {
        $shipped.Add(@{ Name = 'NVIDIA Real-Time Denoisers (NRD)'; Licence = 'NVIDIA RTX SDKs Licence'
                        File = 'third_party\nrd\LICENSE.txt'; How = 'compiled into the renderer' })
        $shipped.Add(@{ Name = 'NVIDIA MathLib'; Licence = 'MIT'
                        File = 'third_party\mathlib\LICENSE.txt'; How = 'compiled into the renderer as part of NRD' })
    }
    $dirs = @(Get-ChildItem -LiteralPath $StagedDir -Recurse -Directory -ErrorAction SilentlyContinue |
              ForEach-Object { $_.Name.ToLowerInvariant() })
    if ($dirs -contains 'rtxdi') {
        $shipped.Add(@{ Name = 'NVIDIA RTXDI (ReSTIR GI)'; Licence = 'NVIDIA RTX SDKs Licence'
                        File = 'third_party\rtxdi\LICENSE.txt'; How = 'HLSL source, shaders\Rtxdi' })
    }
    if ($dirs -contains 'sharc') {
        $shipped.Add(@{ Name = 'NVIDIA RTXGI (SHaRC)'; Licence = 'NVIDIA RTX SDKs Licence'
                        File = 'third_party\rtxgi\License.md'; How = 'HLSL headers, shaders\Sharc' })
    }
    return ,$shipped
}

# Appends the NVIDIA section -- the notice, the attribution, then each licence in full -- to $Notices.
# Writes nothing at all when the staged tree carries no NVIDIA component.
function Add-AverNvidiaNotices([System.Text.StringBuilder] $Notices, [string] $Root, [string] $StagedDir,
                               [hashtable] $Options) {
    $shipped = Get-AverNvidiaComponents -StagedDir $StagedDir -Options $Options
    if ($shipped.Count -eq 0) {
        Write-Host '[notices] no NVIDIA component staged; NVIDIA section omitted'
        return
    }
    [void]$Notices.AppendLine('=' * 78)
    [void]$Notices.AppendLine('NVIDIA SOFTWARE DEVELOPMENT KITS')
    [void]$Notices.AppendLine('=' * 78)
    [void]$Notices.AppendLine('')
    [void]$Notices.AppendLine('This software contains source code provided by NVIDIA Corporation.')
    [void]$Notices.AppendLine('')
    [void]$Notices.AppendLine('This product uses the following NVIDIA software development kits:')
    [void]$Notices.AppendLine('')
    foreach ($c in $shipped) {
        [void]$Notices.AppendLine("  * $($c.Name) - $($c.Licence) ($($c.How))")
    }
    [void]$Notices.AppendLine('')
    foreach ($c in $shipped) {
        $p = Join-Path $Root $c.File
        if (-not (Test-Path -LiteralPath $p)) { Fail "third-party licence file missing: $p"; continue }
        [void]$Notices.AppendLine('=' * 78)
        [void]$Notices.AppendLine("$($c.Name) - $($c.Licence)")
        [void]$Notices.AppendLine('=' * 78)
        [void]$Notices.AppendLine('')
        [void]$Notices.AppendLine((Get-Content -LiteralPath $p -Raw).TrimEnd())
        [void]$Notices.AppendLine('')
    }
    Write-Host ('[notices] NVIDIA section: ' + (($shipped | ForEach-Object { $_.Name }) -join ', '))
}
