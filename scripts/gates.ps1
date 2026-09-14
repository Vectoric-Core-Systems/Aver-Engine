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
# EXIT CODES, from the engine's own table in modules/core/include/aver/core/ErrorCodes.hpp:
#
#     0  ok           every gate matched
#     1  failed       at least one gate did not -- the COUNT is on the last line, not in the code
#     2  usage        an unknown -Config name
#     3  environment  no Sandbox.exe to run: the tree was never built
#
# This used to `exit $failures`, and that was wrong in two ways that only show up from a script. Two
# moved gates exited 2, which the table above spells 'you invoked me wrong' -- so a caller could not
# tell a renderer regression from a typo in a -Config name. And a shell truncates an exit code to a
# byte, so 256 failing gates would have exited 0. A count is a thing to PRINT; it is not a code.
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
    # The smallest 3D viewport that can be a real editor layout. Anything under this is the window
    # still coming up, and the reading must be thrown away rather than believed.
    #
    # MEASURED, not guessed: a nine-configuration sweep produced four readings at
    # `viewport (0,270 45x24)` out of 304 -- about 1.3% -- each returning raw(14,14,16), the dock
    # clear colour.
    #
    # The reason this needs its own check is the nasty part. The probe is a FRACTION of the rect, so
    # a probe inside a 45x24 rect is genuinely inside it: the engine tags the sample `in-viewport`,
    # quite correctly, and every other guard passes. It is the "plausible wrong pixel with a
    # correct-looking rect" the retry comment further down was written about, and nothing caught it.
    #
    # Under -Record that was a live hazard, not a theoretical one. A recorded gate was never retried
    # -- there is no baseline for it to disagree with -- so at 1.3% a 153-gate record would have
    # frozen the dock clear colour into the oracle as an expected value about twice, and exited 0.
    #
    # A parameter rather than a constant so the branch can be TESTED: `-MinViewport 99999` makes
    # every reading count as degenerate, which is how the detection was checked without waiting for
    # a 1-in-76 race to happen again.
    [int]      $MinViewport = 256,
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
#
# DONE, 2026-07-29, and the note above is kept because it is the reason. Measured rather than
# reasoned this time: the 7x7 neighbourhood around the viewport centre varies by **83 codes**, and a
# horizontal slice through it steps from (182,68,41) to (99,38,30) between x=1369 and x=1375 with the
# probe landing on 1375. One pixel. That is the coin flip, photographed.
#
# The nine gates below therefore probe (0.51691, 0.46461) -- the deepest interior point of the SAME
# face the centre was already reporting, 46 px of clear space in every direction and a 7x7 that
# varies by 0. Same face on purpose: it is the sun-blind +X one, where indirect light is the whole
# signal, which is what makes the `gi` gates worth running. Direct-lighting cover is `sunlit`'s job
# and is not lost.
#
# The names stay `centre`, `ms`, `rt` ... even though the probe is no longer the literal centre.
# Renaming would break every baseline key and orphan the history in docs/STATUS.md, and the property
# that made "centre" meaningful was never the 0.5 -- it was being derived from the viewport rect,
# which `--probe-rel` is too.
$Gates = @(
    # --no-gi ON THE TEN NON-GI GATES, and it is not decoration. Global illumination became the
    # ENGINE DEFAULT, so "GI off" is now a thing that has to be asked for. Without these flags the
    # ten gates that exist to measure the unlit path would quietly start measuring the same path the
    # seven `--gi` gates do, seventeen numbers would collapse to seven distinct ones, and the oracle
    # would still report a confident 153/153 while having lost more than half of what it covers.
    #
    # Adding the flags is what keeps the recorded values UNCHANGED across that default flip: each
    # gate still renders exactly what it rendered before, it just has to name it now.
    # --no-rt ON THE ELEVEN RASTER GATES. THIS IS THE SAME MISTAKE AS THE PARAGRAPH ABOVE, ONE
    # DEFAULT LATER, and it is recorded here because it got all the way to a release cut before
    # anything noticed.
    #
    # `3302f89` (2026-08-18) made ray-traced sun shadows the engine default. This gate list was last
    # edited 2026-08-02 and the baselines recorded 2026-08-11 -- both BEFORE that flip -- so from
    # 2026-08-18 every gate here that does not name an RT flag silently stopped measuring the raster
    # path and started measuring the ray-traced one. `centre` became a duplicate of `rt`, `shadow` of
    # `shadow-rt`, and `penumbra` of `penumbra-rt`.
    #
    # THE INVARIANT AT THE END OF THIS SCRIPT IS WHAT CAUGHT IT, exactly as its own comment said it
    # would: penumbra vs penumbra-rt differed by 0, because they had become the same run. Nothing
    # else did -- the eighteen value mismatches on their own look like ordinary shading drift, and
    # re-recording would have frozen the collapse in and left the oracle blind to raster-vs-RayQuery
    # disagreement permanently. Under -Record a mismatch is not a failure but a BROKEN INVARIANT is,
    # which is the only reason that could not happen automatically.
    #
    # As above: naming the flag does not change what these gates render, it restores what they
    # rendered when the baselines were recorded. A value that still moves after this is real drift.
    @{ name = 'centre';        args = @('--no-gi','--no-rt','--probe-rel','0.51691','0.46461') },
    @{ name = 'ms';            args = @('--no-gi','--ms','--no-rt','--probe-rel','0.51691','0.46461') },
    @{ name = 'rt';            args = @('--no-gi','--rt','--probe-rel','0.51691','0.46461') },
    @{ name = 'ms-rt';         args = @('--no-gi','--ms','--rt','--probe-rel','0.51691','0.46461') },
    @{ name = 'gi';            args = @('--gi','--no-rt','--probe-rel','0.51691','0.46461') },
    @{ name = 'ms-gi';         args = @('--ms','--gi','--no-rt','--probe-rel','0.51691','0.46461') },
    @{ name = 'ms-rt-gi';      args = @('--ms','--rt','--gi','--probe-rel','0.51691','0.46461') },
    @{ name = 'gi-debug';      args = @('--gi-debug','--no-rt','--probe-rel','0.51691','0.46461') },
    @{ name = 'ms-gi-debug';   args = @('--ms','--gi-debug','--no-rt','--probe-rel','0.51691','0.46461') },
    # THE LAYERED BSDF, which needs a coat authored or it proves nothing. --layered-bsdf alone
    # selects a shader variant; every material in the editor's placeholder scene authors coatWeight
    # 0, so the variant renders identically to the standard BRDF and a gate on the flag by itself
    # would pass forever while measuring nothing. --coat gives the placeholder materials a coat so
    # the lobe has something to do.
    #
    # PAIRED ON PURPOSE. `layered-off` runs the SAME --coat with the option off, so the two rows
    # must differ from each other. If they ever agree, the option has stopped reaching the shading
    # and both rows still pass on their own baselines -- the exact way nine gates once turned into
    # duplicates of their -rt twins without a single one going red.
    @{ name = 'layered-off';   args = @('--no-gi','--no-rt','--coat','1.0','--probe-rel','0.51691','0.46461') },
    @{ name = 'layered-coat';  args = @('--no-gi','--no-rt','--coat','1.0','--layered-bsdf','3','--probe-rel','0.51691','0.46461') },
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
    # RE-PICKED AGAIN, 2026-07-29, by the recipe above and not by scaling. `0fe81e2` made the level a
    # docked tab and the scene composite into a texture, which changed the viewport from 2750x1711 to
    # 2750x1639 -- and a relative probe is invariant to the rect's SIZE, not to its ASPECT. The
    # projection changed with the aspect, so the same fraction looked at different geometry: all
    # three probes slid off onto plain floor and five gates defined to differ returned one number.
    #
    # The tell that this was geometry and not shading: re-picking by intent lands `shadow` on
    # (23,40,86) against a recorded 22,40,86, and `sunlit` on (95,103,129) against 102,108,132.
    # The surfaces were exactly where they had always been. Only the probes had moved.
    #
    # `sunlit` gets one extra constraint it did not have before. Taking the brightest agreeing floor
    # pixel outright put it at u=0.047 -- hard against the left border, and bright because the floor
    # is fogging into the sky there rather than because it receives sun. That satisfies the letter of
    # "brightest floor pixel" while gutting a gate whose stated job is direct-light cover. So it is
    # now constrained to the SAME DEPTH BAND as `shadow`, and in fact to the same scanline: same
    # surface, same distance, same fog, differing only in visibility. That is what the invariant
    # means when it asks whether the pair still brackets the LIGHTING.
    #
    # Measured margins at the picked pixels, so a future reader can see how much room there is:
    #   shadow    (23,40,86)   7x7 varies by 0 in both frames
    #   sunlit    (95,103,129) 7x7 varies by 0 in both frames; lum(sunlit)-lum(shadow) = 63.5
    #                          against an invariant that fails below 20
    #   penumbra  pcf (54,67,105) vs rt (23,40,86), L1 = 77 against an invariant that fails below 8
    #
    # `penumbra` is scored differently now, and this is the one methodological change. It used to be
    # simply the largest disagreement, which is a knife edge by construction. It is now the pixel
    # with the largest disagreement in the WEAKEST cell of its 3x3 -- so the L1 stays at 14 even if
    # the sample slips a pixel in any direction, and the gate cannot be flipped by a subpixel wobble.
    # Only 576 pixels in the whole frame disagree at all; the two paths are bit-identical on the
    # other 4.5 million, which is itself worth knowing.
    @{ name = 'shadow';        args = @('--no-gi','--no-rt','--probe-rel','0.53545','0.48536') },
    @{ name = 'shadow-rt';     args = @('--no-gi','--rt','--probe-rel','0.53545','0.48536') },
    @{ name = 'shadow-ms-rt';  args = @('--no-gi','--ms','--rt','--probe-rel','0.53545','0.48536') },
    @{ name = 'shadow-gi';     args = @('--gi','--no-rt','--probe-rel','0.53545','0.48536') },
    @{ name = 'penumbra';      args = @('--no-gi','--no-rt','--probe-rel','0.57545','0.50610') },
    @{ name = 'penumbra-rt';   args = @('--no-gi','--rt','--probe-rel','0.57545','0.50610') },
    @{ name = 'sunlit';        args = @('--no-gi','--no-rt','--probe-rel','0.42636','0.48536') },
    @{ name = 'sunlit-gi';     args = @('--gi','--no-rt','--probe-rel','0.42636','0.48536') },

    # rt-penumbra -- the only gate that samples a PARTIALLY OCCLUDED ray-traced pixel.
    #
    # Every other ray-traced probe here is fully lit or fully shadowed, `penumbra-rt` included: it
    # reads 22,26,31, bit-identical to `shadow`. That is not an accident of where it was placed. It
    # was chosen as the pixel of largest DISAGREEMENT between the PCF cascade and RayQuery, which by
    # construction lands where the cascade is soft and the ray is not. So the oracle has been blind
    # to ray-traced soft-shadow quality entirely -- a change to the disc sampling moved 532 pixels
    # along shadow silhouettes and not one gate probe noticed.
    #
    # The reason no such pixel existed to sample: the sun's angular RADIUS is about a quarter of a
    # degree, so at these distances the true penumbra is narrower than a pixel. --sun-angle widens
    # the source until the transition is several pixels across. That is not cheating the test, it is
    # the only way to put a partially-occluded pixel on screen at all, and the width of a penumbra
    # is a property of the light rather than of the renderer.
    #
    # MEASURED at this probe: 22,26,31 at the real 0.545 degrees (fully occluded) against 43,43,45
    # at 8 degrees -- strictly between umbra and the ~90 of lit ground, so it is genuinely partial.
    # Bit-identical over three runs, under --ms, and on WARP, which is a second and independent
    # D3D12 implementation running on the CPU.
    #
    # The probe is expressed at a PIXEL CENTRE (1510.5/2750, 856.5/1639) rather than at the pixel
    # index. vpW*u truncates, so 0.54909 lands on 1509 and reads the umbra -- one pixel away and the
    # gate silently measures the wrong thing, which cost a full diagnosis to notice.
    # RE-PICKED 2026-08-27, and re-picked rather than re-recorded, which is the whole point.
    #
    # This gate had gone GREEN AND BLIND. Its recorded value was 18,25,35 -- bit-identical to
    # `shadow`, i.e. full umbra -- so the one gate that exists to sample a PARTIALLY OCCLUDED
    # ray-traced pixel was sampling a fully occluded one and passing. Re-recording would have frozen
    # that in again: the probe was in the wrong place, and a recorder faithfully records whatever the
    # probe is looking at.
    #
    # WHY IT MOVED. The old fraction (0.5492727, 0.5225747) was chosen when the shadow edge sat
    # there. This release's lighting work -- the fog composite rewrite and the sky-SH ambient among
    # them -- moved the edge out from under it, which is exactly the drift the header's own
    # "RE-PICKED, not merely re-recorded" note warns about for the centre probes.
    #
    # HOW THE NEW ONE WAS CHOSEN, by the gate's own original criterion rather than by eye: capture
    # the same flags with --rt and with --no-rt, and take the pixel of largest DISAGREEMENT between
    # the PCF cascade and RayQuery, scored on the WEAKEST cell of its 3x3 so a sub-pixel slip cannot
    # collapse it. The winner disagrees by 76 across the entire 3x3 (the old probe disagreed by 0),
    # and its own 3x3 is UNIFORM at 58,58,61 -- so a slip in any direction reads the same value, even
    # though the wider 7x7 is dithered by the stochastic sampling, which at one ray per pixel it
    # unavoidably is.
    #
    # AND IT IS GENUINELY PARTIAL: 58,58,61 sits between this scene's umbra (18,25,35) and its
    # sunlit value (89,85,82), a little over halfway. That is the property the gate is named for and
    # had lost.
    @{ name = 'rt-penumbra';   args = @('--no-gi','--rt','--sun-angle','8.0','--probe-rel','0.5672727','0.5149481') }
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
    # AverSR (0.6 optimisation wave 2, U2) now defaults to Auto at every quality rung, and Auto
    # applies to --frames runs the same as an interactive session (docs/AVERSR.md, "Default: Auto").
    # A gate's whole method is reading a raw probe pixel out of the composited frame; scaling the
    # internal resolution before that composite would change what pixel raw() is even reading,
    # regardless of which rung the running scene resolves to. Pin native resolution explicitly rather
    # than rely on whatever Auto would otherwise pick.
    $all = @('--frames', "$frames", '--debug-layer', '--aversr', 'off') + $gateArgs + $extra

    # START-PROCESS, NOT `& $exe`, AND THAT IS NOT A STYLE CHOICE. Sandbox.exe is linked
    # /SUBSYSTEM:WINDOWS as of 0.5.0 so the editor never opens a console window, and Windows
    # PowerShell does not wait for a GUI-subsystem process nor capture its output: `& $exe ... 2>&1`
    # returns ZERO lines and a BLANK $LASTEXITCODE, measured. Every gate would then read NO-PROBE and
    # the oracle would be silently blind -- the exact shape of failure this repo has a documented
    # history of. Start-Process -Wait waits whatever the subsystem is, and the redirect files give
    # the same text the pipe used to.
    #
    # FILES RATHER THAN A PIPE because -RedirectStandardOutput takes a path, and separate files for
    # out and err because Start-Process refuses to point both at one. They are read back and joined,
    # so callers downstream see exactly what `2>&1` used to hand them.
    $tmpOut = [System.IO.Path]::GetTempFileName()
    $tmpErr = [System.IO.Path]::GetTempFileName()
    try {
        $p = Start-Process -FilePath $exe -ArgumentList $all -NoNewWindow -Wait -PassThru `
                           -RedirectStandardOutput $tmpOut -RedirectStandardError $tmpErr
        $exit = $p.ExitCode
        $out = ((Get-Content -LiteralPath $tmpOut -Raw -ErrorAction SilentlyContinue) + "`n" +
                (Get-Content -LiteralPath $tmpErr -Raw -ErrorAction SilentlyContinue))
    } finally {
        Remove-Item -LiteralPath $tmpOut, $tmpErr -Force -ErrorAction SilentlyContinue
    }
    $r = [pscustomobject]@{ raw = 'NO-PROBE'; place = '?'; debug = 'NO-TOTALS'; exit = $exit; rect = '?'
                            rectW = 0; rectH = 0 }
    $probe = $out -split "`r?`n" | Select-String 'probe \(' | Select-Object -First 1
    if ($probe -match 'raw \(([\d, ]+)\)') { $r.raw = ($Matches[1] -replace '\s', '') }
    if ($probe -match '(in-viewport|OUTSIDE-VIEWPORT|VIEWPORT-MOVED)') { $r.place = $Matches[1] }
    # The viewport RECT, not just the in/out tag. The default probe is the viewport centre expressed
    # off this rect, so a window that came up at a different size moves what the centre pixel looks
    # at while still reporting `in-viewport`. Without the rect in the failure line, that is
    # indistinguishable from a shading regression -- which is exactly the confusion the probe's own
    # self-validation was added to end, and the runner should not throw the information away.
    if ($probe -match 'viewport \((\d+),(\d+) (\d+)x(\d+)\)') {
        $r.rect  = "$($Matches[1]),$($Matches[2]) $($Matches[3])x$($Matches[4])"
        # Kept as numbers too, so the size can be JUDGED and not merely printed. Printing it was the
        # old plan and it is not enough: nobody reads the rect on a line that says PASS.
        $r.rectW = [int]$Matches[3]
        $r.rectH = [int]$Matches[4]
    }
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
# Environment, not Failed: nothing was measured and nothing is wrong with the renderer -- there is no
# build tree to point at. CI wants to report this differently from a gate that moved.
if (-not (Test-Path $Exe)) { Write-Error "Sandbox.exe not found at $Exe - run $buildHint first"; exit 3 }
$Exe = (Resolve-Path $Exe).Path

$selected = if ($Config.Count -gt 0) { $Config } else { @($Configs.Keys) }
foreach ($c in $selected) {
    if (-not $Configs.Contains($c)) {
        Write-Error "unknown configuration '$c'; known: $($Configs.Keys -join ', ')"
        exit 2   # usage: the caller named something that does not exist
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
        # A rect too small to be a layout. Checked BEFORE the debug-layer and value branches because
        # such a run is not a measurement of anything -- see $MinViewport.
        elseif ($r.rectW -lt $MinViewport -or $r.rectH -lt $MinViewport) {
                                               $verdict = "BAD-PROBE tiny-rect $($r.rect)" }
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
        # $use is the reading that will be RECORDED and fed to the invariants. It starts as the first
        # run and is replaced only by a retry that came back clean. Before this existed the first
        # reading was recorded unconditionally, so a gate that read the dock clear colour out of a
        # 45x24 window and then measured perfectly on retry still wrote the clear colour.
        $use = $r

        # A miss is run ONCE more before it is believed, and BOTH results are printed whatever
        # happens. This is not a retry that hides a failure -- a gate that misses twice still fails,
        # and a gate that misses once is reported as FLAKY with both values and both viewport rects,
        # which is strictly more information than a single number.
        #
        # BAD-PROBE now retries under -Record too, and that is the point of the restructure. A
        # recorded gate has no baseline to disagree with, so it never took this path -- which is
        # exactly how a 1.3%-likely bad reading would have been frozen in silently.
        if ($verdict -like 'FAIL*' -or $verdict -like 'BAD-PROBE*') {
            # WHY the first reading was rejected, kept for the message below. Without it a retry that
            # also fails prints an ordinary value mismatch, and "the window came up at 45x24" becomes
            # indistinguishable from "the shading moved" -- which is the single most expensive
            # confusion this oracle produces.
            # ...but only worth SAYING when the first reading was rejected for a reason other than
            # its value. "FAIL [FAIL expected 98,37,30] expected 98,37,30" is noise, and noise in a
            # 153-line report is how the interesting line gets skimmed past.
            if ($verdict -like 'BAD-PROBE*') { $why = "[$verdict] " } else { $why = '' }
            Start-Sleep -Milliseconds $spacing
            $r2 = Invoke-Gate $Exe $g.args $extra $Frames
            $r2Clean = $r2.exit -eq 0 -and $r2.place -eq 'in-viewport' -and
                       $r2.rectW -ge $MinViewport -and $r2.rectH -ge $MinViewport -and
                       $r2.debug -match '^C0 E0'
            if ($Record) {
                # Recording: the question is not "does it match" but "is this a real measurement".
                if ($r2Clean) {
                    $use = $r2
                    $verdict = "recorded on retry (first $($r.raw) rect=$($r.rect))"
                } else {
                    $verdict = "FAIL ${why}unusable twice: $($r.raw) rect=$($r.rect) then $($r2.raw) rect=$($r2.rect)"
                }
            }
            elseif ($r2.raw -eq $want -and $r2Clean) {
                $verdict = "FLAKY ${why}first=$($r.raw) rect=$($r.rect) / retry=$($r2.raw) rect=$($r2.rect)"
            } else {
                $verdict = "FAIL ${why}expected $want, got $($r.raw) rect=$($r.rect) then $($r2.raw) rect=$($r2.rect)"
            }
        }

        if ($verdict -notlike 'PASS*' -and $verdict -notlike 'recorded*' -and $verdict -notlike 'FLAKY*') { $failures++ }
        if ($verdict -like 'FLAKY*') { $flaky++ }
        Write-Host ("  {0,-14} raw({1,-12}) {2,-8} {3}" -f $g.name, $use.raw, $use.debug, $verdict)
        $recorded.Add("$c | $($g.name) | $($use.raw)")
        $seen["$c|$($g.name)"] = $use.raw
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
    # REFUSE TO WRITE A POISONED BASELINE. Every gate's value is added to $recorded whatever happened
    # to it, including one that CRASHED or came back BAD-PROBE -- so without this guard a run could
    # exit non-zero and still have written editor chrome into the oracle as an expected value. The
    # next run then passes, and the gate is dead without ever having reported anything.
    #
    # Under -Record a value mismatch is not a failure (it becomes 'recorded'), so $failures counts
    # only the things that must never be recorded: crashes, probes outside the viewport, missing or
    # non-zero debug-layer totals, new TDRs, and BROKEN INVARIANTS. That last one is the important
    # one -- a baseline whose probes no longer measure what they claim is exactly what re-recording
    # must not be allowed to freeze in, and it is the mistake this repo has already come closest to
    # making.
    #
    # $flaky is a BACKSTOP here and should always be 0 under -Record: the FLAKY verdict is only
    # reachable on the comparison path, because recording asks "is this a real measurement" rather
    # than "does it match". It is left in the condition so that a future edit which makes FLAKY
    # reachable under -Record cannot silently start freezing unsettled values.
    if ($failures -gt 0 -or $flaky -gt 0) {
        Write-Host ""
        Write-Host "REFUSING TO RECORD: $failures gate(s) failed, $flaky flaky." -ForegroundColor Red
        Write-Host "$BaselineFile is UNCHANGED. Fix the runs above, then record again."
        if ($flaky -gt 0) {
            Write-Host "A FLAKY gate has no value to freeze -- compare its two viewport rects first." -ForegroundColor Red
        }
        exit 1   # the counts are printed above; the CODE just says it did not work
    }
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
exit $(if ($failures -eq 0) { 0 } else { 1 })
