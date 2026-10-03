# Frame interpolation — patent sweep (for counsel)

**Not legal advice.** This is an engineering sweep run 2026-10-03 by research agents with web access: they
searched by assignee and by technique, read independent claims, and mapped each claim element to the
design in [FRAME_INTERPOLATION.md](FRAME_INTERPOLATION.md) (elements E1–E15, listed below). A second agent
re-read each HIGH/MEDIUM finding and tried to show the first mapping was too lenient. It states element
mappings and risk, never infringement or clearance. A freedom-to-operate opinion from a patent attorney
is still required before shipping.

**Reliability caveats:**
- Google Patents rate-limited the agents part-way through. Several adversarial reviews could not re-fetch
  the claims and relied on the first agent's quotation (marked below).
- Many findings are **published applications**: not enforceable until granted, and their claims may
  narrow in prosecution. Each one's status must be re-checked in USPTO Patent Center / Espacenet.
- Leads not searched or not read: see §5.

## 1. Design elements the claims were mapped against

| | Element (as designed before this sweep) |
|---|---|
| E1 | Interpolation only: two rendered frames in, one in-between frame at t = 0.5 out |
| E2 | Engine motion vectors and depth for each real frame (none rendered for the in-between time) |
| E3 | Midpoint motion field by forward-splatting frame N's motion with a depth-ordered atomic resolve, then hole fill |
| E4 | Backward-warp both frames through that field; per-frame depth-consistency disocclusion masks |
| E5 | Engine-written material trust mask (glass, water, particles, emissives) |
| E6 | Milestone 1: heuristic blend of the two warps (no network) |
| E7 | Milestone 2: small U-Net on real-frame data → per-pixel blend weight + RGB fill for uncovered pixels |
| E8 | Training on ground truth rendered at 2× rate |
| E9 | HDR, before the spatial upscaler and tone mapping; exposure and bloom blended from the real frames |
| E10 | UI drawn after interpolation on both frames |
| E11 | Pacer thread presenting the generated frame at ~T/2 from a moving-average frame-time estimate |
| E12 | CPU frame-start delay from measured GPU time (latency reduction) |
| E13 | Scene-cut detection; skip generation on a cut |
| E14 | FSR3 v1.1.4 passes as the starting point (game-MV field via atomics, interpolated-depth estimate, inpainting pyramids, 8×8 SAD luma flow, two disocclusion masks, blend, inpaint, UI cleanup) |
| E15 | Editor preference for frame generation while editing |

## 2. HIGH risk (every claim element plausibly met by the design as it stood)

| Number | Assignee | Status | What the independent claim requires (plain words) | Hits | Design-around (reviewer's strongest) |
|---|---|---|---|---|---|
| **US 2025/0191120 A1** "Motion vector field generation for frame interpolation" | AMD | Pending (prio 2023-12-11) | Weight each current-frame motion vector that lands on a location of the interpolated frame; select one by weight to build the interpolated frame's motion field; generate the frame from that field. Dependents: weight = depth, atomic writes, hole fill from the neighbour furthest from the camera. | E3, E14 (FSR3's game-MV field) | **Do not build a midpoint motion field by scatter-and-select.** Build the in-between frame by *gather*: per output pixel, a fixed-point search through each real frame's own depth and motion (Yang et al. 2011, bidirectional reprojection, prior art). Or warp colour and depth from each real frame with its own motion and resolve conflicts in colour. Softmax-weighted splatting is judged a rename, not a removal. Drop FSR3's game-MV-field pass. *(Challenger could not re-read claims verbatim.)* |
| **US 2025/0299287 A1** "Time-adjusted interpolated frame display" | AMD | Pending (prio 2024-03-25) | Generate an interpolated frame from two rendered frames; determine its display timing from rendering metrics of those frames; present it on that timing. Claim 15 is broader (metrics of the interpolated frame). | E11, E14 (FSR3 pacing) | **Presentation timing must not be computed from measured quantities.** Queue generated + real frames as fixed back-to-back vsync presents (half-refresh cap), no frame-time estimator, no GPU or UI timing in the schedule. Disable generation on variable-refresh / uncapped output rather than adapting. Keep E12 strictly separate. Counsel: do present statistics count as "rendering metrics"? |
| **US 2025/0069319 A1** "Multi-channel disocclusion mask for interpolated frame recertification" | AMD | Pending (prio 2023-08-21) | Claims 1/8: recalculate colour values of the interpolated frame based on a multi-channel disocclusion mask associated with both rendered frames. Claim 15: mask channels derived from depth of a rendered frame and depth of the interpolated frame. | E4, E6, E14 | Raised from MEDIUM by the challenger. Keeping two masks in separate textures is a rename. For claim 15: derive occlusion **only** from real-frame depth consistency, never from a midpoint depth. For claims 1/8 no clean removal: options are a single per-pixel source decision not used to recompute colour afterwards, or occlusion learned inside the network with no explicit mask stage. |
| **GB 2620919 A** (granted UK) / **US 2024/0029196 A1** (pending) "Temporal upsampling image frames" | Arm | GB granted; US pending (prio 2022-07-21) | A neural network on pre-processed frames outputs a **mask and a residual**; the mask is applied to frame features; the result is combined with the residual. Description: MV interpolation to the intermediate time, depth-aware warps, U-Net predicting blend mask + residual. | E7 | **The network outputs only the blend weight** (optionally refined warp offsets) and **no image-valued output** at all; uncovered pixels are filled procedurally. A "learned fill that replaces a hole" is judged a rename. Milestone 1 (no network) is unaffected. *(Challenger could not re-read claims.)* |
| **US 12,568,184 B2** "Techniques to generate interpolated video frames" | NVIDIA | Granted 2026-03-03 | (Per first researcher; not re-read verbatim.) Forward motion vectors; an occlusion mask AND a dis-occlusion mask; time-distance weights; masked pixels taken from a single frame. | E3, E4, E6, E14 | One continuous confidence map (no separate occlusion/dis-occlusion pair); never snap masked pixels to one frame; weights from warp/depth error, not time distance. A second searcher rated this MEDIUM. |
| **US 2022/0038653 A1** (sibling of the above) | NVIDIA | Pending (prio 2020-07-30) | Generate a third frame based on one of a plurality of possible motions of objects between two frames. | Anything resolving competing motions | No clean design-around on the literal text. Watch prosecution; prior art before 2020-07-30: Softmax Splatting, DAIN, depth-ordered motion-compensated interpolation. |
| **US 2022/0398751 A1** "Computing motion of pixels among images" | NVIDIA | Pending (prio 2021-07-15) | Motion of pixels in one region computed from motion of an overlapping region. | Pyramidal/block-matching flow, motion hole-fill (E3, E14) | Raised from MEDIUM. Fill holes in colour/depth space, not by propagating motion; avoid coarse-to-fine motion seeding. Broad against prior art (Bergen 1992, Bouguet 2001); grant in this form doubtful. Watch prosecution. |

## 3. MEDIUM risk

| Number | Assignee | Status | Note |
|---|---|---|---|
| US 12,288,281 B2 | Disney / ILM | Granted | Networks on feature maps of both key frames **and the target frame**. One searcher MEDIUM, one LOW. Keep any midpoint data out of the network. |
| US 2025/0225705 A1 | Intel | Pending | Mixed-precision neural network for frame interpolation; design-around judged not to hold. Re-read before choosing fp16 for E7. |
| US 2026/0030797 A1 | Arm | Pending | Efficient interpolation of colour frames; design-around judged not to hold. |
| US 2026/0094228 A1 | AMD | Pending | Neural-network-based graphics rendering. |
| US 2024/0098216 A1, US 2025/0106355 A1, US 12,574,521 B1 | (see sweep output) | Mixed | Video frame blending / adaptive blending / motion information; design-around judged not to hold. |
| US 10,776,688 B2 | NVIDIA (Super SloMo) | Granted, expires 2039 | Network on bidirectional intermediate flows and warped inputs. One challenger lowered it to LOW given procedural flow and a weight-only network. |
| US 12,524,830 B2 | Qualcomm | Granted | Occlusion-aware forward warping. Lowered to LOW by the challenger. |

LOW findings (Disney 12,367,544 and 12,506,842; Valve 10,733,783; Meta 11,783,533 extrapolation; Qualcomm,
Samsung, Intel and Arm families) are in the sweep output and need no design change.

## 4. The FSR3 licence question

The FidelityFX SDK's FSR3 frame-interpolation, optical-flow, swapchain and Anti-Lag 2 sources are in the
SDK's MIT-licensed file list. MIT grants copyright permissions only: **no express patent grant**, and AMD
is actively filing on FSR3-style techniques (§2). Two coherent positions, which counsel should choose
between:

1. **Vendor the MIT-listed files verbatim** (notices kept) and rely on an implied-licence / estoppel
   argument for the methods those files practise. Covers only what the released code does, not later
   claims beyond it, and not our own pacer or additions.
2. **Reimplement and design around** (§2). Clean on copyright, but gives up any implied-licence argument,
   so the design-arounds must actually remove claim elements.

Mixing them (a modified algorithm derived from AMD's code) is the worst of both.

## 5. Gaps — still to search or read

- The claims of every pending application above, verbatim, and their current status (grant/abandonment,
  narrowed claims, continuations; KR/CN members of 2025/0069319).
- NVIDIA DLSS frame-generation filings (game motion vectors + optical flow accelerator; CPC H04N 7/0137,
  G06T 3/4007, 2021–2024) — likely closer to E2/E14 than the Reda/Sapra family. US 12,229,970 (NVIDIA,
  frame-rate up-conversion using optical flow) found but claims not read.
- **Intel US 12,057,090 B2** "Frame pacing for improved experiences in 3D applications": delaying CPU work
  to align with GPU availability — directly relevant to **E12**. Claims not read.
- AMD Anti-Lag / latency filings, UI/HUD-less composition for generated frames, AMD optical-flow filings.
- Continuations of US 10,776,688.
