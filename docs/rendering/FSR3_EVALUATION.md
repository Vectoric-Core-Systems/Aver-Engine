# FSR3 frame generation as NeuraFI's replacement: evaluation (2026-10-03)

The question: vendor AMD's FSR3 frame generation (MIT) instead of NeuraFI, and run it on any GPU.
Read from upstream (GitHub `GPUOpen-LibrariesAndSDKs/FidelityFX-SDK`) on 2026-10-03.

## Licence: yes, the source can be vendored

| Release | What is MIT | Notes |
|---|---|---|
| **v1.1.4** (tag) | The whole SDK, `LICENSE.txt` MIT (c) 2024 AMD | FSR 3.1.4 source: `sdk/src/components/frameinterpolation`, `opticalflow`, `fsr3`, `ffx-api`, DX12 `FrameInterpolationSwapchain`. DX12 **and Vulkan**. |
| **v2.3.0** "Redstone" (main) | Only the files listed in `docs/license.md`; FSR3 frame generation 3.1.6 and its swapchain 3.1.7 are listed | The default licence of 2.x is **binary-only, no reverse engineering** (it covers FSR 4 / ML parts and the signed DLLs). 2.3 is DX12 only ("Vulkan is currently not supported"). |

Our existing FidelityFX vendoring (`third_party/fidelityfx-fsr`, `fidelityfx-denoiser`) is the same
shape: MIT source, copied unmodified.

**Patents are a separate matter.** MIT grants copyright permission only, and AMD's readme says:
"No license, including implied or arising by estoppel, to any intellectual property rights is granted
by this document." Using FSR3 practises AMD's own pending claims:
- US 2025/0299287 (18/615,997): pacing from measured frame times. FSR3's pacing thread "keeps track of
  average frame time" and "calculates the target presentation time delta". The application was
  allowed on 2026-05-12, then reopened with an RCE; all 20 claims were rejected on 2026-09-16.
- US 2025/0191120: game motion-vector field built by scatter-and-select.
- US 2025/0069319: disocclusion masks.

The NVIDIA applications in NEURAFI_PATENTS.md §6 are not addressed by any AMD licence either.
Counsel's call. In practice AMD publishes it for adoption, but the disclaimer removes the implied-licence
argument.

## Hardware: agnostic in practice

Needs typed UAV loads and R16G16B16A16_UNORM, shader model 6.2 (6.6 for wave64). FSR3 frame
generation runs on AMD RDNA 2+, NVIDIA RTX 20+ and Intel Arc. Since FSR 3.1, frame generation is
decoupled from FSR's upscaler, so it would work after AverSR.

## Technical fit

- It **replaces the DXGI swapchain** with AMD's proxy, which adds two CPU threads (a pacing thread and
  a present thread), an async present queue and an optional async compute queue. Our device's
  two-presents-per-frame path (`submitGeneratedImage`, `presentPass` twice) would be removed.
- UI: per-present callback (fits the editor's ImGui), a separate UI texture, or a HUD-less input.
- **Windowed mode has the same limit as NeuraFI:** "VRR and tearing are only available in fullscreen
  mode". For windowed apps AMD recommends capping the frame rate at half the refresh rate. In the
  editor window on a 60 Hz panel, the most it can show is 30 real + 30 generated frames per second.
- Why FSR3 does not stall like NeuraFI does today: it presents from its own thread on a separate
  present queue. The render thread never blocks in Present. NeuraFI presents both images on the
  render thread, and a windowed Present waits for the previous image to be shown. Measured on
  2026-10-03 (Sponza, 0.67): the real Present blocked 52 ms and the frame took 67 ms, against 24 ms
  with interpolation off. That is an engineering defect, fixable without FSR3, with a present
  thread or queue-ordered presents.

## Options

1. **Vendor FSR3 (v1.1.4 for Vulkan + DX12, or 2.3 for DX12 only).** Fastest route to a tuned,
   shipped frame generator on any modern GPU. Accepts the AMD patent exposure above, and gives up
   NeuraFI's patent-aware design.
2. **Fix NeuraFI's presentation:** a present thread or queue-ordered mid-frame presents (the owner's
   "one frame back" idea). Keeps the clean design; about a day's work.
3. **Both**, as a user-selectable backend behind `rhi::IFrameInterpolator`.
