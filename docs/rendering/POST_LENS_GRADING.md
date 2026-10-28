# Post chain: lens effects and colour grading

Lens effects and grading live in the existing camera post chain (`modules/rhi/shaders/post.hlsl`, recorded by
`runPostChain` in each backend). They are `PostSettings` fields (`modules/rhi/include/aver/rhi/RHI.hpp`); every
field at its default skips its effect, so a level that sets none of them renders exactly as before.

## Order of operations

```
scene (HDR, linear)
  -> [AverSR upscale]
  -> DOF prep + gather (half res, only when DOF is on)
  -> PSComposite:
       fetch scene (chromatic aberration)  -> motion blur along the velocity
       -> DOF mix -> night vision -> exposure / local exposure
       -> + bloom -> + lens flare
       -> grade: white balance, lift/gamma/gain, contrast, saturation   (linear, before the tonemap)
       -> vignette (linear)
       -> tonemap -> gamma
       -> 3D LUT (display space)
       -> film grain (display space)
```

## Fields

| Field | Range | Effect |
|---|---|---|
| `vignette` | 0..1 | Corner darkening, round at any aspect; the corners multiply by `1 - vignette`. |
| `grain` | 0..1 | Triangular noise re-rolled every frame, strongest in the midtones, added in display space. |
| `chromaticAberration` | 0..1 | Red and blue pushed apart along the radius; 1 is 1% of the width at the edge. |
| `lensFlare` | 0..1 | Five ghosts mirrored through the centre plus a halo ring, read from the bloom pyramid. Builds the pyramid even at `bloomIntensity` 0. Only pixels above the bloom threshold feed it, so it shows on the sun and lamps, not on lit walls. |
| `motionBlur` | 0..1 | Shutter fraction. A centred 12-tap blur along the G-buffer velocity, capped at 4% of the height, skipped under one texel of travel (so a TAA jitter in the vectors does not soften a still image). |
| `dofEnabled`, `dofFocusCm`, `dofFStop`, `dofFocalMm` | | Physical circle of confusion. See below. |
| `saturation`, `contrast` | 1 = identity | Saturation about luminance; contrast about 0.18, both in linear. |
| `lift`, `gamma`, `gain` | 0 / 1 / 1 | ASC-CDL-style: `pow(max(c * gain + lift, 0), 1 / gamma)`, per channel, in linear. |
| `whiteBalanceK`, `whiteBalanceTint` | 6500 / 0 | `whiteBalanceK` is the colour temperature of the scene's light; the image is corrected so that light reads neutral (so 3200 cools, 10000 warms), as Unreal's White Balance. Luminance-preserving; 2500..15000 K. Tint + is magenta, - is green. |
| `gradingLut`, `gradingLutIntensity` | | A 3D LUT texture (`TextureDim::Tex3D`, RGBA8 or RGBA16F, N x N x N, x = R, y = G, z = B), looked up with the display-space colour after the tonemap and blended by the intensity. Create it with `initialState = ShaderResource`. |

## Godot tonemappers and display adjustments (G4 R3)

`PostSettings::tonemap` 3..7 selects Godot's linear, Reinhard, filmic, ACES and AgX curves (`rhi/Tonemap.hpp` is the CPU twin and the
reference; `color.hlsli` the HLSL), with `tonemapWhite` (Reinhard, filmic, ACES) and three display-space adjustments, `adjustBrightness`,
`adjustContrast`, `adjustSaturation` (Godot's `apply_bcs`: scale, pivot on 0.5, pivot on the channel mean), run AFTER the tonemap and the gamma
encode and BEFORE the grading LUT. Project keys `RENDER.TONEMAP 0..7`, `RENDER.TONEMAPWHITE`, `RENDER.ADJUSTBRIGHTNESS / CONTRAST /
SATURATION`; post-volume keys `tonemap`, `tonemapwhite`, `adjbrightness`, `adjcontrast`, `adjsaturation`.

The code sits in a separate composite: `PSComposite` compiled with `AVER_POST_TONEMAP_EXT` (D3D12 and Vulkan), built the first frame
`rhi::postNeedsExtComposite(post)` is true (a Godot tonemapper, or an adjustment off 1). The ordinary composite, its defines and its
pipelines are untouched, and so is `averTonemap`; `PostCB` gained one row (`ext`) that only the EXT variant declares. Unlike the grade stage
above, the adjustments are in display space, as Godot's are.

## Depth of field

`c(d) = f^2 / (N (S - f)) * (d - S) / d` on a 36 mm sensor (3.6 cm), with `f` the focal length, `N` the f-stop, `S`
the focus distance and `d` the G-buffer view Z, all in cm. Positive is beyond the focus plane. It is converted to
pixels of the composite's source width. Sky (view Z 0) is at infinity. At 35 mm f/4 focused at 10 m the far plane
blurs 1.6 px at 1920 wide, so the lens, not a slider, sets the strength: open the aperture or focus close for a
visible effect. The radius is capped at 1% of the height in half-res pixels (4..12).

1. **Prep** (half res, RGBA16F): colour (Karis weighted, clamped by `maxRadiance`) and the signed CoC.
2. **Gather** (half res): 48 rotated Vogel taps. A tap counts when its own blur disc reaches the pixel; far taps
   behind a nearer pixel are rejected; near taps (blurred foreground) accumulate separately and come back as a
   coverage alpha, so a blurred foreground covers a sharp background.
3. **Composite** mixes the gathered colour over the sharp one by the pixel's own far CoC (smoothstep 0.5..2 px) and the near coverage.

Both buffers are made the first time DOF runs and remade when the composite source's size changes.

## The G-buffer dependency

DOF and motion blur read the G-buffer's view Z and velocity (`IDevice::gBufferViewZTexture`,
`gBufferVelocityTexture`). D3D12 keeps the G-buffer on while either is requested (`setGBufferEnabled` ORs it in) and skips both when
the frame has no readable G-buffer (MSAA resolve failed, wireframe view, the generated image of frame
interpolation). **The Vulkan backend has no G-buffer yet**, so DOF and motion blur are D3D12-only for now; every
other effect, including the LUT, is on both.

## Cost and defaults

Each effect is a uniform branch on a constant in `PostCB` (`lens`, `lens2`, `grade`, `wb`, `lift`, `gamma`, `gain`,
`dof`, filled for both backends by `rhi::fillPostLens` in `FrameConstants.hpp`). Left at the default there is no
extra pass, no extra texture and one compare per effect in the composite. `PostCB` grew from 144 to 272 bytes;
D3D12's per-frame constant ring doubled to 32 KB.

## Resource binding

D3D12: root parameter 3 is a four-descriptor table at `t3..t6` (view Z, velocity, grading LUT, DOF buffer) bound to
every post draw and dispatch. Passes that do not use it bind a table of nulls; the composite and the DOF passes bind
per-frame copies rewritten each frame (the LUT and the DOF targets can change between frames in flight).
Vulkan: the grading LUT is `t5` in set 3 of the post pipeline layout, written per frame in flight, and takes the
`AVER_POST_LUT` composite permutations; nothing else changes.

## Tests

`tests/rhi/src/PostLensTest.cpp` checks, with no device, that the defaults pack to the identity, the white-balance
direction and its exact neutral at 6500 K, the stage bitmask, the thin-lens CoC, and `postSettingsEqual` coverage.
