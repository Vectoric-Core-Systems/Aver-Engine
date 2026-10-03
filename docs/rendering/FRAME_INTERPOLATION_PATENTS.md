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
- Leads not searched or not read: see §7. Round 2 (revised design) is §5; round 3 (NVIDIA status and
  prior art) is §6.

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

## 5. Round 2 — the revised design (2026-10-03)

Re-checked against the design after the §2 design-arounds (gather, one continuous confidence, weight-only
network, fixed vsync cadence):

- **All §2 items read LOW** against the revised design: AMD 2025/0191120, 2025/0299287, 2025/0069319;
  Arm GB 2620919 / US 2024/0029196; NVIDIA 12,568,184 and 12,700,107.
- **New HIGH, granted — Georgia Tech US 9,094,660 B2** "Hierarchical hole-filling for depth-based view
  synthesis" (priority 2010-11-11, nominal expiry ~2033-12). Claim 1: reduce the resolution of a 3D-warped
  image until holes fall below a threshold, expand it, fill the warped image's holes from the expanded
  image. A push-pull pyramid fill met every element. **Design change: full-resolution fill, no mip chain**
  (FRAME_INTERPOLATION.md §3.4). Prior art for counsel: Gortler et al. 1996 (push-pull), Grossman & Dally
  1998, Marroquim et al. 2007. Threshold wording was read from partly garbled OCR.
- **Granted — Intel US 12,057,090 B2** "Frame pacing…": delaying CPU work to align with GPU availability.
  **Design change: the measured CPU frame-start delay (E12) is dropped.**
- **New HIGH, pending, no clean design-around — four broad NVIDIA applications** (§6).

## 6. Round 3 — status and prior art for the broad NVIDIA applications (2026-10-03)

Prior-art dates are each reference's first public date against the family's earliest priority date. The
table's status column came from Google Patents; the **verified file histories below supersede it**.

### 6.1 USPTO file histories, read 2026-10-03 (Global Dossier, public)

Read in USPTO Global Dossier (`globaldossier.uspto.gov/details/US/<appl>/A/…`), which needs no sign-in.
Patent Center refused its own data calls from an automated browser, and the Open Data Portal has needed a
USPTO.gov sign-in since 2026-06-18. Claim text below was read from the filed claim documents (image PDFs).

| Application | Verified history | Current claim 1 (plain words; quoted text verbatim) | §122(e) third-party submission |
|---|---|---|---|
| 16/559,312 (US 2021/0067735) | Non-final rejection 2022-05-26; final rejection 2022-12-08; appeal notice 2023-06-08; RCE 2023-08-08 ruled non-responsive 2023-08-17; **Abandonment 2024-03-22.** | — (abandoned; no revival petition seen) | n/a |
| 17/949,153 (US 2024/0098216, blending) | Rejections 2024-06-10 and 2024-11-04; amendments; **Notice of Allowance 2025-06-12**, issue fee paid 2025-09-11; then an **RCE 2025-10-13** (with a Quick Path IDS) pulled it back; **non-final rejection 2026-04-10**; examiner interviews requested 2026-06-26 and 2026-08-20; IDS 2026-09-03. No reply filed yet: the six-month statutory limit for the reply is **2026-10-10**. | (Amended 2025-05-05, the allowed text) one or more processors with circuitry "to use one or more neural networks to blend two or more intermediate video frames generated from a first video frame and a second video frame to generate a blended intermediate video frame between the first video frame and the second video frame." Dependents: blending factors, object motion vectors, optical flow, motion types, depths of pixels, camera position. | **Closed** (6 months after publication, 2024-09-21, has passed and a rejection has issued). Post-grant routes only. |
| 17/949,156 (US 2025/0106355, depth) | Non-final rejection 2025-05-21; interview 2025-08-20; amendment 2025-11-21; **final rejection 2026-03-17**; **RCE with amendment 2026-07-16**; awaiting the next action. | (Amended 2026-07-16) circuitry to identify pixels of the first or second frame having similar depth values "within a threshold of a depth value corresponding to a pixel location to be filled, wherein the one or more pixels are determined to correspond to a same object based on the depth values", and generate pixel values of intermediate frames using them. In effect a **depth-guided hole fill**. Dependent 3: generated using a neural network. | **Closed** (2025-09-28 passed; rejections issued). |
| 19/643,218 (US 2026/0245168, continuation of 12,632,916) | Filed 2026-04-09; **preliminary amendment 2026-06-16**; published 2026-08-20; **no examiner action yet**. | (Amended 2026-06-16, **broadened**: "intercepting", "using a swap chain buffer" and "outside of the swap chain buffer" were struck) in response to one or more API calls to output an application-generated frame: store it and interpolated frames generated from it in a first buffer; provide them from the first buffer to a second buffer comprising a swap chain buffer; cause them to be presented from the second buffer. Interception survives only in dependents 7, 16 and 19. | **Open** until the later of 2027-02-20 or the first rejection, and only before allowance. |

**Consequences for the design.**
- *Depth application:* the G4 fill already takes frame N's colour plus a plain normalised convolution and
  never selects source pixels by depth similarity to the hole. Keep it so. The G2 depth-agreement check
  scores a candidate continuously against the other real frame; it does not choose pixels to fill a
  location. Counsel to confirm that distinction.
- *Blending application:* the allowed claim reads closely on the held-back milestone 2 (a network
  weighting two gathered candidates, each an intermediate frame from one real frame). It stays held back.
- *Continuation:* with interception and "outside the swap chain" struck, the §5 mitigation (composite
  straight into the back buffer, no interception) is weaker than first thought. Prior art for
  application-side buffering of real + synthesized frames ahead of presentation (Andreev 2010, Oculus ASW,
  Valve motion smoothing) is the main route, and the only one where a pre-issuance submission is still
  open.

| Application | Claim as published (plain words) | Status / family | Best prior art found |
|---|---|---|---|
| **US 2021/0067735 A1** (prio 2019-09-03) | Any processor/system/medium that generates higher-frame-rate video from lower using one or more neural networks; or trains networks to | **Abandoned** (US). Live members: DE 112020003165 (pending), CN 114303160 (pending). GB withdrawn, PCT ceased. | **TOFlow**, Xue et al., arXiv 1711.09078 (2017-11-24), not NVIDIA-affiliated: "generates a high frame rate video" with a trained network. Super SloMo (2017-11-30) matches fully but two authors are inventors here (outside the 1-year grace period). Cycle-consistency dependents: Liu et al. AAAI-19 (abstract only). |
| **US 2024/0098216 A1** "Video frame blending" (prio 2022-09-20) | A neural network blends frames into an in-between frame | Pending. DE 102023125188, CN 117750070. No PCT. | Super SloMo (CNN visibility maps blending two warps), RIFE (2020-11-12), IFRNet (2022-05-29), BMBC (2020). **Reads on the held-back milestone 2.** |
| **US 2025/0106355 A1** "Adaptive video frame blending" (prio 2022-09-20) | In-between frames generated based at least in part on pixel depth | Pending. DE 102023125185, CN 117880561. | **Yang et al. 2011** (bidirectional scene reprojection, the basis of this design): every element. Briedis et al. 2021 (Disney, neural interpolation of rendered content with engine depth). DAIN 2019. Mark, McMillan & Bishop 1997 (not read in full). |
| **US 2026/0245168 A1** (filed 2026-04-09), continuation of **US 12,632,916 B2** (granted 2026-05-19, expiry ~2043) | In response to API calls to output a frame: store it and interpolated frames in a first buffer, provide them to a swap-chain buffer, present. Dependents: copying, API interception, choosing the count from frame rate vs goal rate | Continuation pending; parent granted. CN 117853306, DE 102023126457. | Not yet collected. Leads: Oculus ASW and Valve motion smoothing (2016–19), ReShade / Special K present hooks. **Design change:** generated frame composited straight into the back buffer, no store-then-copy, no interception (FRAME_INTERPOLATION.md §5). |

Related, not in the four: NVIDIA **US 12,568,184 B2** (granted 2026-03-03; LOW against the revised design)
and its pending sibling US 2022/0038653; NVIDIA US 12,574,521 B1 and **12,524,850 B1** (granted 2026,
priority 2022-09-20, edge-enhanced blending, not yet charted).

**Territory.** No family above has a Singapore, EP, KR or JP member. IPOS can act only on Singapore patents
and applications, so IPOS observations, re-examination and revocation do not apply unless an SG member
turns up (confirm with an IPOS search and Patentscope national-phase data). Exposure remains in the US,
China and Germany for anything distributed there.

**Procedures and dates (for counsel to confirm).**
- *US third-party pre-issuance submission* (35 U.S.C. 122(e), 37 CFR 1.290): before allowance and before
  the later of 6 months after publication or the first rejection. Open now for **US 2026/0245168**
  (six-month point 2027-02-20). **Closed** for 2024/0098216 and 2025/0106355 (verified, §6.1). Fee about
  $195 per ten items (free for three or fewer with the 1.290(g) statement); re-check the live schedule.
- *Provisional rights* (35 U.S.C. 154(d)): if a patent issues with claims substantially identical to the
  published claims, the owner may claim a reasonable royalty from the publication date for use with
  actual notice. "Pending" is therefore not the same as "no exposure" (counsel to assess).
- *Post-grant review* (35 U.S.C. 321, any grounds, within 9 months of grant): **US 12,568,184 closes
  2026-12-03**; US 12,632,916 closes 2027-02-19. IPR opens after those dates. Ex parte reexamination
  (any time; printed prior art) is the low-cost option.
- *Watch:* set Patent Center / Espacenet alerts on 19/643,218, 17/949,153 and 17/949,156. Continuations
  let claims be redrafted toward a later product.

### 6.2 The present chain: granted parent claims and pre-2022 prior art (2026-10-03)

**US 12,632,916 B2 claim 1 (verbatim, Google Patents):** "A method, comprising: intercepting one or more
application programming interface (API) calls from a host application indicative of a request to output
an application-generated frame using a swap chain buffer; storing the application-generated frame, to be
rendered by a first process, in a buffer outside of a swap chain buffer; initiating a second process
that, at least partially in parallel with the first process as the first process is rendering the
application-generated frame, generates one or more interpolated frames based at least on the
application-generated frame and stored in the buffer outside of the swap chain buffer; identifying a goal
frame rate; and providing the application-generated frame and the one or more interpolated frames to the
swap chain buffer from the buffer outside of the swap chain buffer to output the application-generated
frame and the one or more interpolated frames in accordance with the goal frame rate." Claims 8/14 are the
system/processor forms. Dependents: a pacer process (4, 9, 10, 19), the next frame in parallel (5, 15), a
machine-learning model (6, 18), the count from the rate gap (7, 11), a goal rate from display
characteristics (12). Examiner-cited: NVIDIA US 2014/0092109 A1 ("GPU driver-generated interpolated
frames", 2012), not yet read.

**Against milestone 1 as designed:** no interception (the engine is the application); no finished frame
stored outside the swap chain (each frame is composited straight into its back buffer); no second process
generating in parallel (the gather runs in the same command list, after the scene); no goal frame rate
(fixed 2×, one generated frame per real frame, the refresh is the clock). The broadened continuation
(§6.1) drops interception, parallelism and the goal rate, which is why the prior art below matters there.

| Reference | Public | Discloses | Licence |
|---|---|---|---|
| Andreev, *Real-time frame rate up-conversion for video games*, SIGGRAPH 2010 Talks (LucasArts, *The Force Unleashed 2*), DOI 10.1145/1837026.1837047 | 2010-07 | Interpolated frame built as a new front buffer and flipped mid-frame, triple buffered; generated in parallel with the next frame. | ACM copyright: cite only |
| Mark, McMillan, Bishop, *Post-Rendering 3D Warping*, I3D 97 | 1997-04 | Low-rate reference frames depth-warped into higher-rate displayed frames | ACM: cite |
| Didyk et al., Eurographics 2010; Yang et al. 2011; Bowles et al. 2012 | 2010–12 | Synthesised in-between frames for high-refresh display | Publisher: cite |
| Oculus Asynchronous Timewarp (2016-03-25), ASW (2016-11-10), ASW 2.0 (2019-04-04) | 2016–19 | App submits frames through an API; runtime buffers, synthesises (extrapolation) and presents; ASW engages when the app misses rate | Proprietary: cite |
| Valve SteamVR Motion Smoothing (beta 2018-10-17) | 2018 | App at half rate; "synthesize 2 frames or even 3 frames for every 1 frame delivered" (count from the rate gap) | Proprietary; OpenVR headers BSD-3 |
| Windows Mixed Reality motion reprojection (2018-04-01; Auto mode 2019) | 2018–19 | Half-rate rendering with synthesised frames; switches on the achieved rate | Proprietary: cite |
| Meta Application SpaceWarp (2021-11-04) | 2021 | App submits colour, motion and depth at half rate; the OS synthesises every other frame | Runtime proprietary |
| ReShade (Present-hooking proxy, releases from ~2015) / OBS game-capture hook (2014) | 2014–15 | Intercepting Present and the back buffer (no frame insertion) | BSD-3 / GPL-2.0 |

No pre-2022 tool was found that combines a third-party Present hook with frame insertion.

**Permissively licensed interpolation / reprojection code public before 2022-09-20** (reusable under its
licence; prior-art status depends only on the date): DAIN (MIT, 2019-03-22, depth-aware), RIFE (MIT,
2020-11-12), rife-ncnn-vulkan (MIT, 2020-11-24), IFRNet (MIT, 2022-03-17), Super-SloMo reimplementation
(MIT, 2018-12-25), FLAVR (Apache-2.0, 2020-12-24), ABME (MIT, 2021-08-24), ST-MFNet (MIT, 2022-02-09), AMD
FSR2 (MIT, 2022-06-22, depth + motion reprojection for upscaling), Microsoft MiniEngine (MIT, camera
reprojection 2015, temporal effects 2017). Not permissive (cite only): softmax-splatting (academic use
only), MVTools (GPL), Blender EEVEE (GPL), Special K (GPL-3.0).

### 6.3 The neural milestone: claims read and candidate designs (2026-10-03)

Claims read from the USPTO PDFs (page images): 12,524,850; 12,574,521; 12,229,970; 2026/0094228;
2026/0030797; 2025/0225705. From Google Patents pages: 10,776,688; 12,288,281; US 2024/0029196; GB 2620919
**A** (the granted **B** claims are still unread).

| Document | Status | Claim 1 (plain words) |
|---|---|---|
| NVIDIA US 12,524,850 B1 | Granted 2026-01-13 | Filter pixels to locate an edge, convert the filtered images to luma, then down-sample; dependents: a network uses the result for blending information |
| NVIDIA US 12,574,521 B1 | Granted 2026-03-10 | Find pixels with no motion vector; estimate their motion from nearby pixels with motion vectors, the same depth and the same object |
| NVIDIA US 12,229,970 B2 | Granted 2025-02-18 | Two reference frames' vectors conflict at one intermediate pixel; use the one more similar to a global motion vector |
| AMD US 2026/0094228 A1 | Pending, unexamined | A rendering pipeline in which at least one stage is a trained network transforming the previous stage's data (very broad as published) |
| Arm US 2026/0030797 A1 | Pending | Reduced-resolution interpolated optical flow AND motion-vector data; nearest-in-depth vector per output pixel (scatter); gather colour |
| Intel US 2025/0225705 A1 | Pending | Forward motion plus generated optical flow "representing non-geometric changes" plus motion to the timepoint. **No precision element in any claim** |
| NVIDIA US 10,776,688 B2 | Granted | A flow-interpolation network predicts occlusion data and generates the intermediate frame |
| Disney/ILM US 12,288,281 B2 | Granted | A network on features of key frames AND of the target frame, producing pixel mappings |

Candidate designs, ranked: **N1, a learned trajectory prior** (network outputs only a per-pixel
acceleration that bends the gather path; never touches the blend), then N3 (single-image residual
restoration), N4 (learned constants, no network at runtime), N2 (learned reliability feeding the blend
weight, closest to 17/949,153's allowed claim, dropped). The binding design rules R1–R10 are in
FRAME_INTERPOLATION.md §3.5.

**Milestone 1 flags from this review:**
- The **two search starts per frame** with the more confident result kept could be read against NVIDIA
  US 2022/0038653 ("generate a third frame based on one of a plurality of possible motions", pending since
  2020). Counsel to decide; a single-start search is the fallback (thin fast objects suffer).
- **AMD US 2026/0094228** as published reads on any trained network stage — the neural radiance cache
  stage 2 included. Pre-2024 prior art: DLSS 2.0 (2020), Chaitanya et al. 2017, Xiao et al. 2020,
  ExtraNet 2021.

**AMD US 2026/0094228 A1 in full (read from the publication; file history from Global Dossier,
2026-10-03).** "Neural Network Based Graphics Rendering", Appl. 18/901,288, filed 2024-09-30, inventor
Kunal Tyagi, published 2026-04-02. The disclosure is **cloud gaming**: a server ray traces, denoises and
encodes with a trained network; a client decodes, denoises and upscales with one and interpolates or
extrapolates frames with another, fed local and predicted user input to hide lag. Independent claims 1
(method), 10 (system), 20 (medium) need only: a multi-stage rendering pipeline receives scene data; at
least one stage is a trained network transforming the previous stage's data; the frame is rendered at
least partly from that output. Dependents: denoise/encode/decode/upscale (2, 11); training on
original/transformed frame pairs (3, 12); a network generating extra frames by interpolation or
extrapolation (4, 9, 13, 18); the server/client split with lag adjustment from local input (5–8, 14–17);
predicted user input to the frame network (9, 19). **Status: non-final rejection 2026-08-12**; reply due
2026-11-12 (2027-02-12 with extensions). The §122(e) window closed 2026-10-02. No SG member found.
**Decision (owner, 2026-10-03): no design change now** — the independent claims as filed are anticipated
by the prior art above and already rejected; the likely survivors (cloud split, predicted/local user
input) are features Aver does not use. Watch AMD's reply.

## 7. Gaps — still to search or read

- GB 2620919 **B** (granted) claims; 2022/0038653's current claims and status.

- NVIDIA US 2014/0092109 A1 (examiner-cited on the parent): not read.

- USPTO file wrappers for every application above (abandonment of 2021/0067735 and any revival or
  continuation; whether 2024/0098216 and 2025/0106355 have a first rejection).
- The parent US 12,632,916 claims verbatim (only Google's summary was read). Prior art for the
  store-then-copy present chain before 2022-10-04.
- Quotes from TOFlow, DAIN, MEMC-Net, AdaConv and others were found by keyword search; re-check against
  the PDFs before anything goes to counsel. Mark/McMillan/Bishop 1997 and Liu et al. AAAI-19 were not
  read in full.
- NVIDIA DLSS frame-generation filings (game motion vectors + optical flow accelerator; CPC H04N 7/0137,
  G06T 3/4007, 2021–2024). US 12,229,970 (frame-rate up-conversion using optical flow): claims not read.
  US 12,524,850 (edge-enhanced blending): not charted.
- AMD Anti-Lag / latency filings, UI/HUD-less composition for generated frames, AMD optical-flow filings.
- Continuations of US 10,776,688.
