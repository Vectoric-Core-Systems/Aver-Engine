# Code notes: tests

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-03 comment strip so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## tests/formats/src/ProjectRenderApplyTest.cpp

### Top-level test file scope
The header-only ProjectRenderApply functions apply voxi::Settings from a ProjectDesc manifest, handling:
- **N6 fix**: an absent derived knob (one without an explicit manifest value) resets to its NEW tier's ladder value, not whatever was stale in the live Settings. Proven for all three tiers (GI Low/Medium/Epic with giCones, giRestirVisibility, giUpdateInterval patterns).
- **R2 fix**: two-phase apply (applyManifestTiers then commit via setSettings, then applyManifestKnobs with the NEW tier's derivation live) avoids regression where a merged single call would re-derive a knob's ladder rung over an explicit manifest ask. Proven through the live voxi::Renderer singleton.
- **N7 fix**: command-line flag precedence mirrors R2: two-phase CLI apply (applyCliTiers, then setSettings commit, then applyCliKnobs) avoids re-derivation of tier-dependent knobs. The test itself is a regression net that FAILS if the two phases are ever merged into a single setSettings call.
- **N5 fix**: on a device without RT/mesh-shader hardware, the device's own clamp (live Settings = Off/false due to hardware lack) must not be read as a user edit and overwrite a teammate's pin for hardware that machine lacks.
- **N9 fix**: hasRenderSettings() now sees GIMODE and DENOISER keys alone (was blind to both, so entire apply blocks were skipped).
- **N9 extended**: hasRenderSettings correctly handles RESTIRVISIBILITY and AVERSR at 0 (both legitimate values, not merely >= 0 truthy).
- **Sentinel values**: -1 = absent/use default, 0 = meaningful value (e.g., "No ray" for RESTIRVISIBILITY, "Off" for AVERSR, "zero samples" is invalid but kept as distinct). Four flags use 0-means-absent: msaa, rtShadowRays, rtPixelsPerRayTile, giUpdateInterval default to 0, and take() must distinguish negative (absent) from 0 (stated).
- **Tier-derived vs non-derived knobs**: giRestirVisibility and giCones follow their tier's ladder value when absent; giRestirMaxHistory does NOT follow any ladder and is unconditional (absent leaves it alone, any value applies verbatim).
- **isOwnedKey duplicate line issue**: keys appended to kOwned list but missing from isOwnedKey's bool array in the capture path will be copy-through as unowned text AND re-emitted by appendKey, growing a duplicate line on every save. The STALE (first, copied-through) line wins on the next parse, making changes appear to work then silently revert. Test ensures RESTIRVISIBILITY, AVERSR, FRAMEINTERP round-trip cleanly (written once, no duplicate on rewrite, no line when absent).
- **manifestContradictions**: RT Off makes GIMODE/RTRENDERMODE pins contradictory and reportable; unset asks (-1) are not checked.
- **Manifest vs CLI precedence**: applyManifestTwoPhase (manifest) and applyCliOverrides (CLI) both use two-phase commit to avoid losing explicit asks to tier-derivation. The Phase A/B split is the reason collapsing the CLI apply was safe to attempt.

### fullyCapableDevice() vs noRtDevice()
fullyCapableDevice covers every gate: compute, RT tier 11, mesh shaders, all MSAA counts, denoiser, DXC. Most tests use it and ignore device gates, exercising manifest-apply and capture logic.
noRtDevice (rayTracingTier=0, meshShaderTier=0, denoiserSupported=false) represents actual hardware without RT cards, used for N5 device-clamp test.

### Sentinel shape and range guards
- RenderCliOverrides uses three sentinel conventions: -1 (absent), 0 (absent for legacy flags), -1.0f (absent for floats).
- Four 0-means-absent flags (msaa, rtShadowRays, rtPixelsPerRayTile, giUpdateInterval) default to 0. take() ignores negative-only, so applyCliKnobs must guard each with `> 0` to avoid writing unsolicited 0 into the renderer on every project open.
- --restir-visibility and --rt-render-mode use -1 sentinel precisely so 0 stays expressible: 0 = "No ray" (valid), "ray-driven rasteriser comparison" (valid). A collapsed "0-means-absent" convention would make both unusable at their most interesting value.

### Two-phase mechanism detail
When a tier changes (e.g. GI Epic -> Low) and a knob has an explicit manifest value matching the OLD tier's default:
- Single merged call: takes() compares knob against live (which already has old tier's value), sees no change, logs nothing, rides into setSettings unchanged → setSettings' own change-gated derivation sees tier change and re-derives the knob to new tier's ladder value, silently discarding the explicit ask.
- Two-phase: Phase A (tier only) commits first. Phase B (knobs) now compares against the NEW tier's derivation, sees the explicit value as an edit, marks it, applies it, and survives setSettings' re-derivation gate because the knob is already different.

### Field wiring check (testCliEveryFlagReachesItsField)
The collapse that created RenderCliOverrides from twenty separately-read `*Override_` members has one wiring failure mode: mistyped assignment (e.g. `cli.rtShadowDenoise = ptBouncesOverride_`) both are ints with the same sentinel, result is silent wrong-knob drive. Test covers all 20 fields reaching their correct Settings members by wiring check.

### Notes round-trip (isOwnedKey pattern)
The round-trip tests (RESTIRVISIBILITY/AVERSR/FRAMEINTERP/RESTIRHISTORY) exercise isOwnedKey's failure mode: a key in kOwned list but missing from isOwnedKey's bool array is copy-through as unowned text, then re-emitted by appendKey. Second save = duplicate. STALE line wins on parse → change appears to work then silently reverts on reload. Test runs writeOcproject(desc, ""), parses back, then writeOcproject(reparsed, first), ensuring exactly 1 occurrence of the key, not 2.

## tests/render.neural/src/NeuralMlpTest.cpp

- `testValidationAndLayout` and test suite scope: Tests cover backward pass against finite differences for every activation and both losses (L2 and RelativeL2), learning a toy function (sin of 2D input) under both losses with EMA tracking, Adam's bias-corrected first step, deterministic init with He bound, weight file round-trip and rejections, and fixed-point gradient quantisation with order-independent accumulation.

- `gradientCheck`: Test setup shrinks He init and adds biases to keep weights away from saturation and kinks so every weight has a nonzero gradient to check. ReLU hidden layer biases are lifted so units sit on the active side of the kink. Measurement on a real rig: hidden relu + output sigmoid at seed 11 left fewer than a quarter of the gradients nonzero with zero outliers—this justifies the setup to ensure the check is about maths, not initialisation luck.

- ReLU kink numerics: A ReLU kink within eps of a pre-activation can make the numeric slope wrong for one weight, so the test uses a small outlier budget (3% of weight count) rather than zero.

- Adam's first step (bias-corrected): First update computes mhat / (sqrt(vhat) + eps), which simplifies to the sign of the gradient scaled by learning rate. Weights with tiny accumulators are skipped because eps becomes non-negligible against |g|.

- Fixed-point gradient accumulation: GPU structure uses grouped atomics—sum within groups of 64 records, then add group totals. Integer addition makes grouping exact, so the optimiser step is order-independent.

## tests/render.voxi/src/GiVisibilityTest.cpp

- **GiVisibilityTest purpose**: This test verifies U1's shared bit definitions and reconstruction arithmetic used in voxi_restir.hlsli. It does NOT test GPU/RHI behaviour; the header under test depends only on aver/core/Types.hpp by design, so the test links Aver.Core alone. A struct-typed ternary (HLSL has no conditional operator over non-numeric types) slipped past an earlier pass and was only caught by the DXC harness, which is why the shader compilation step is separate and mandatory, not a replacement for this test suite.

- **tracedPixel phase schedule (2.10 D/E)**: Every pixel of a 2x2 block traces on exactly one of frames 0..3 in a 2-periodic tiling pattern. Frame f-1's writer matches voxi_restir.hlsli's kGiVisPhase[(frameIdx - 1u) & 3u] at frameIdx 0, with kGiVisPhase[3] == uint2(0, 1).

- **packAmbientW bit layout**: Mode stays 2-bit on the wire (& 3u), so CPU-side RestirVisibility::Cached = 4 packs as mode 2 plus bit 128 (cache bit). Bits 12-15 (spatialSamples 0..15) never disturb bits 0-6 (mode, flags). Bits 18-22 (maxHistory 0..31) never disturb bits 0-17 and must stay under 2^24 for exact f32 representation in gAmbientParams.w (bug history: a debug dial parked at bit 24 silently corrupted data during camera-motion fade investigation).

- **Ratio estimator vs naive average (2.10 D.6)**: rho2 = sum(w*g)/sum(w*b) is a RATIO OF EXPECTATIONS, not an average of per-tap ratios. The two estimators are only interchangeable when sampling weight and per-tap brightness scale are uncorrelated. Test uses synthetic field (bright: b=10, V=0.1; dim: b=1, V=0.9, g:=V*b) where unweighted average is biased >5% against the true expectation ratio to confirm the ratio-of-sums formula is correct.

- **Half-res history EMA (2.10 E)**: history_n = fresh*(1 - h) + history_{n-1}*h with h = kHistWeight. Geometric convergence: error after n steps toward steady value is exactly kHistWeight^n times initial error.

- **reuse.numSamples = 0u ordering**: Must appear AFTER motion-discount lerp (reuse.samplingRadius = lerp(32.0, 8.0, motionT)), not before. If placed before, the discount's own numSamples write silently undoes Reconstructed's forced temporal-only mode.

- **gAverHistoryWrite gates in voxi_restir.hlsli**: Exactly 4 write gates (store, surface-history, denoiser-input, visibility-write) plus 2 denoised readback gates (reprojected read with denoisedReproject, decode with gw > 0u). Both readback gates keep blended fragments from reading opaque surface's denoised answer.

- **gAverHistoryWrite gates in voxi_rt.hlsli**: Exactly 4 write gates (AO history, AO hit-distance, RT-shadow tiled, RT-shadow untiled). No unconditional denoised-occlusion readback gate (df4122cc added denoisedAoUsable guard).

- **PSRayDriven ungated writes**: Sky-miss surface-history sentinel (gGiSurfNrmHistOut[uint2(i.pos.xy)] = float2(0.0, asfloat(0u))) and AO hit-distance write (gAoHitDistOut[uint2(i.pos.xy)] = amb.hitDist) are deliberately NOT gated on gAverHistoryWrite at all (checklist item 12). Positive control confirms both lines still exist; negative control confirms "if (gAverHistoryWrite" gate does not precede them.

- **VoxiRenderer.cpp binding counts**: Asserts kVoxiSrvCount == 23 and kVoxiUavCount == 22. Binding layout: u11-u15 are staged ray-driven buffers, t17/u16 are occlusion-aware fog design's air sky-visibility volume, u17/u18 are sub-stage splits' GI-trace candidate and shadow-probe tile buffers, t18/t19/u19 are local-light list and its visibility history, t22/u20/u21 are radiance cache's cascade info, accumulator and cells.

## tests/render.voxi/src/NeuRaCTest.cpp

- **Test scope**: Tests NeuRaCLayout.hpp's CPU-checkable half. Checks arithmetic (fixed-point headroom, tag/cell addressing, origin snapping, fp16 SH packing, normal packing), that HLSL constants match header values. Does NOT compile HLSL or run the resolve kernel.

- **Fixed-point headroom**: Worst sample is the maximum radiance (L_max) weighted by the SH max coefficient (0.488603), divided by the minimum cosine floor (kMinCos), multiplied by π. This worst case summed with 64 samples (kMaxCap) via fixed-point (kShScale = 2^15) must stay under half of int32 max to leave headroom for the GPU's InterlockedAdd without wrapping (which would be a wrong colour, not a crash).

- **Negative cell arithmetic**: Negative world cells use arithmetic shift to extract their block: floor(cell/64) fits in one byte via arithmetic shift, distinct from block 0. Example: cells -1 through -64 all map to block -1 (0xFF); cell -65 maps to block -2 (0xFE).

- **Window and toroidal addressing**: A 64³ window anchored at any origin (including negative) maps every world cell to exactly one distinct texel. The recovery formula `world = origin + ((texel - origin) & 63)` is exact: both the X and Y axes have only two tag values at most (the toroidal property the resolve depends on). This is the property the resolve's window wrapping relies on.

- **Meta word bit packing**: The meta word packs 24-bit tag, 4-bit n_eff, and 4-bit age. Fields that overflow their width (e.g., 99 with 4 bits) mask to their width without spilling into neighbours: only low 4 bits land in each 4-bit field, only low 24 bits in the tag.

- **Axis-aligned normals**: Common case where octahedral pack must land exactly on axis (e.g., +Y at (0,1,0), -Z at (0,0,-1)).

- **cos floor bias**: The fixed-point Monte Carlo uses weight π / max(cos, kMinCos). This floor under-weights the ~1% of cosine-sampled directions below 0.1 (since kMinCos ≈ 0.1), introducing a few-percent low bias in constant-radiance tests, never high.

- **fp16 SH packing layout**: 12 SH floats packed as 6 uint32 words; low half of word 0 holds k=0, high half holds k=1, etc. Worst relative error across 20,000 random tests: ~1/1024 (fp16's 2^-11 step).

## tests/render.voxi/src/RenderSettingsResolverTest.cpp

- **Header block (removed)**: File tests three independent quality-ladder decision systems (QualityLadder, RenderSettingsResolver, Scalability) plus regressions through the live Renderer singleton for F-d (giMode/denoiser stored as requested, resolved at read time). Calls Renderer::get(), setDeviceInfo(), and exported *ForQuality statics from Aver.Render.Voxi. Follows tests/editor/PtRenderConflictTest.cpp's idiom: plain std::printf with no AVER_ macros, testing pure functions with no ImGui or globals.

- **Using namespace comment (removed)**: u32/f32 live in aver::core, while Quality, Settings, Feature, Renderer, DisableReason and ladder:: live one level down in aver::voxi—same pattern CameraFactorTest.cpp already uses.

- **checkForwards comment (shortened)**: Removed explanation about "one-line forward to ladder::X" and "proves the forward actually forwards, at every tier".

- **fullDevice comment (shortened)**: Removed "DXR 1.1 + SM 6.5 + DXC (so Ray Tracing AND Path Tracing are both Ready)" detailed requirements.

- **noRtHardwareDevice comment (shortened)**: Removed explanation that Path Tracing's own gate shares the RT hardware set, and that GlobalIllumination/MeshShaders are untouched and stay Ready.

- **Lines 124-125 giRestirVisibility comment (removed)**: "U1: {Off 3, Low 1, Medium 2, High 3, Epic 3} -- see ladder::giRestirVisibility's own comment for why High/Epic are your decision (Full) and Low/Medium are mine, UNMEASURED." This refers to another comment and includes "UNMEASURED" (measurement note).

- **Line 127 averSrLevel comment (removed)**: "U2: {Off 0, Low 3, Medium 2, High 1, Epic 1} -- kAverSr* mirror sr::Quality's own numbering." Cross-reference to another system's design.

- **Lines 200-205 PT monotonicity comment (shortened)**: Removed long explanation about PT Low/Medium being deliberately equal on the ladder's only knob, references to "section 4's own Neighbouring tiers differ list", "the accumulator grows at each step", and cross-references to QualityLadder.hpp's top comment explaining why accumulator resolution is deliberately not included in ladder::ptBounces.

- **Lines 214-216 monotonicity printf (kept)**: This is code inside a string literal, left unchanged.

- **Lines 241-243 rtRenderMode exception comment (removed)**: "Exception 1 (section 4): rtRenderMode at Low changes the METHOD (raster), not the amount -- it is not cost-monotone across cameras (D3's own evidence is PARTIAL, not a speed claim), so this file deliberately does not assert monotonicity for it at all."

- **Line 290 giMode regression string (kept)**: "(the Voxi.cpp:60 clamp is gone)" is inside the check string, not a comment—preserved as code.

- **Lines 347-349 refractionMode pipeline comment (shortened)**: Removed "Through the full pipeline" explanation about setSettings' own range clamp catching the garbage value BEFORE resolve(), and that this is a regression test for the pipeline order, not for resolve() alone.

- **Line 429 rayTracing setup comment (kept)**: "rayTracing is left at Settings{}'s own Medium, following Medium's ladder by construction." Kept as it explains intentional design.

- **Line 641-642 comment (kept)**: Off and Low deliberately differ (3 vs 1) comment explaining why this check passes only if derivation ran, kept because it's a subtle precondition for correctness.

- **Line 689-691 Custom branch comment (shortened)**: Removed setup explanation about GI Epic and RT Low both not following their own ladder rungs, and that this is a precondition for autoAverSrLevel's Custom branch.

- **Lines 717-733 oracle comments (shortened)**: Removed "A correct oracle, written independently of resolveAverSrLevel's own body, against the stated precedence" and detailed explanation of how the test covers "both who wins (presence) and which value wins for whichever source does".

- **Lines 759-767 comment (kept)**: "3 presence bits (cli/user/manifest) x 2 value variants per present source = 16 cases" kept—this is a brief explanation of test coverage.

Result: Comment lines reduced from 66 to 16 (76% reduction).
