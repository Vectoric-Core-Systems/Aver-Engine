// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
#include "aver/voxi/Voxi.hpp"
#include "aver/voxi/voxi_abi.h"
#include "aver/core/Log.hpp"

#include <algorithm>

// The Voxi settings service and the C ABI the C# scripting layer P/Invokes.
namespace aver::voxi {

// Returns the process-wide instance.
Renderer& Renderer::get() {
    static Renderer inst;
    return inst;
}

// Records what the device can do and re-clamps the current settings against it.
void Renderer::setDeviceInfo(const DeviceInfo& info) {
    device_ = info;
    refusalLogged_ = 0;
    setSettings(settings_);
}

// Applies what is legal for this device; unsupported requests are clamped and logged once.
void Renderer::setSettings(const Settings& s) {
    Settings n = s;

    auto refuse = [&](Feature f) {
        const u32 bit = 1u << static_cast<u32>(f);
        if (refusalLogged_ & bit) return;
        refusalLogged_ |= bit;
        // THE DEVICE IS NOT ALWAYS AT FAULT, and saying it is sends the reader to the wrong place.
        // Unsupported means this GPU cannot; NotImplemented means the engine does not, on any GPU.
        // The old wording blamed hardware for both, so a NotImplemented feature read as "your card is
        // too old" -- someone could reasonably go shopping over a line of missing code.
        AVER_INFO(status(f) == Status::NotImplemented
                      ? "[Voxi] {} was requested but this engine does not implement it yet ({}); it stays off"
                      : "[Voxi] {} was requested but this device cannot run it ({}); it stays off",
                  featureName(f), statusText(f));
    };

    u32 samples = static_cast<u32>(n.msaa);
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8) samples = 1;
    while (samples > 1 && !(device_.msaaMask & samples)) samples >>= 1;
    n.msaa = static_cast<Msaa>(samples);

    if (status(Feature::GlobalIllumination) != Status::Ready) {
        if (n.globalIllumination != Quality::Off) refuse(Feature::GlobalIllumination);
        n.globalIllumination = Quality::Off;
    }
    if (status(Feature::RayTracing) != Status::Ready) {
        if (n.rayTracing != Quality::Off) refuse(Feature::RayTracing);
        n.rayTracing = Quality::Off;
    }
    if (status(Feature::PathTracing) != Status::Ready) {
        if (n.pathTracing != Quality::Off) refuse(Feature::PathTracing);
        n.pathTracing = Quality::Off;
    }
    if (status(Feature::MeshShaders) != Status::Ready) {
        if (n.meshShaders) refuse(Feature::MeshShaders);
        n.meshShaders = false;
    }

    // Clamped exactly like the four above it, and for the same reason -- a setting that a device or a
    // half-built feature cannot honour must not keep a value that says otherwise. While
    // status(LayeredBsdf) is NotImplemented this pins the field to Off, so nothing downstream has to
    // ask whether the value it is reading is real, and a project manifest carrying a stale rung is
    // reported once by refuse() rather than acted on.
    if (status(Feature::LayeredBsdf) != Status::Ready) {
        if (n.layeredBsdf != Quality::Off) refuse(Feature::LayeredBsdf);
        n.layeredBsdf = Quality::Off;
    }

    // Quality tiers used to leave voxelResolution completely alone -- selecting Epic cost nothing
    // extra because nothing read the tier to size the grid. When the caller changes the GI tier and
    // leaves voxelResolution exactly as it already was -- the common case: the editor's Quality
    // combo touched alone, or a project manifest that states giQuality but not voxelResolution (see
    // applyProjectRenderSettings in SandboxApp.cpp) -- derive the grid edge from the new tier. An
    // explicit voxelResolution request arriving in the SAME call (the incoming value differs from
    // what is currently active) always wins over the derived one, so the editor's "Voxel grid"
    // combo and a manifest's explicit voxelResolution keep overriding it exactly as before.
    if (n.globalIllumination != settings_.globalIllumination && n.voxelResolution == settings_.voxelResolution)
        n.voxelResolution = voxelResolutionForQuality(n.globalIllumination);

    // The same derivation for HOW OFTEN the volume is rebuilt, by exactly the same rule and for the
    // same reason: the tier sized the grid but said nothing about the revoxelisation rate, so every
    // tier paid the full always-fresh cost. Revoxelising measured at 108 ms of a 229 ms frame -- 47%,
    // the single largest item in it -- and dropping to one rebuild in four took the frame from 121.1
    // to 104.5 ms on the Electric Dreams scene.
    //
    // EPIC STAYS AT 1, which is the point. Epic means "do not compromise", so its indirect light
    // remains bit-identical to the always-fresh behaviour every tier used to have; only the cheaper
    // tiers buy speed with latency. The trade is temporal, not spatial -- indirect light lags scene
    // changes by up to N-1 frames and a static scene converges to exactly the same image.
    if (n.globalIllumination != settings_.globalIllumination && n.giUpdateInterval == settings_.giUpdateInterval)
        n.giUpdateInterval = giUpdateIntervalForQuality(n.globalIllumination);

    // And the CONE COUNT, by the same rule -- the one that was missing, and the one that carries
    // most of the cost. See Settings::giCones.
    if (n.globalIllumination != settings_.globalIllumination && n.giCones == settings_.giCones)
        n.giCones = giConesForQuality(n.globalIllumination);

    // REFRACTION FOLLOWS THE RAY-TRACING TIER, by the same by-value rule as every derived knob here:
    // only when the tier MOVED and the caller did not set the mode itself in the same call. A caller
    // asking for the value the field already holds is indistinguishable from one who never asked --
    // that is the trap SandboxApp's two-call setSettings pattern exists to step around, and it
    // applies to this exactly as it does to rtShadowRays.
    if (n.rayTracing != settings_.rayTracing && n.refractionMode == settings_.refractionMode)
        n.refractionMode = refractionForQuality(n.rayTracing);

    // The RT sun-shadow knobs follow their own tier the same way, and for a sharper reason: with RT
    // on by default there is no longer any configuration in which these are inert, so a tier change
    // that left them alone would advertise Medium while running whatever the last tier paid for.
    // Same "only if the caller did not set it explicitly in this call" rule as GI above.
    if (n.rayTracing != settings_.rayTracing && n.rtShadowRays == settings_.rtShadowRays)
        n.rtShadowRays = rtShadowRaysForQuality(n.rayTracing);
    if (n.rayTracing != settings_.rayTracing && n.rtPixelsPerRayTile == settings_.rtPixelsPerRayTile)
        n.rtPixelsPerRayTile = rtPixelsPerRayTileForQuality(n.rayTracing);
    if (n.rayTracing != settings_.rayTracing && n.rtShadowDenoise == settings_.rtShadowDenoise)
        n.rtShadowDenoise = rtShadowDenoiseForQuality(n.rayTracing);
    if (n.rayTracing != settings_.rayTracing && n.rtRenderMode == settings_.rtRenderMode)
        n.rtRenderMode = rtRenderModeForQuality(n.rayTracing);
    // KEYED ON rayTracing, not globalIllumination, even though it is the AMBIENT term it corrects:
    // what it costs is a ray, and what makes it possible at all is the acceleration structure. A
    // project raising GI quality on hardware with ray tracing off must not start paying for rays.
    if (n.rayTracing != settings_.rayTracing && n.giSkyOcclusionRays == settings_.giSkyOcclusionRays)
        n.giSkyOcclusionRays = giSkyOcclusionRaysForQuality(n.rayTracing);
    if (n.rayTracing != settings_.rayTracing && n.giSkyOcclusionTile == settings_.giSkyOcclusionTile)
        n.giSkyOcclusionTile = giSkyOcclusionTileForQuality(n.rayTracing);
    // KEYED ON pathTracing, not rayTracing. A bounce budget is a path-tracing quantity; deriving
    // it from the ray-tracing tier is what let the two run out of step.
    if (n.pathTracing != settings_.pathTracing && n.ptBounces == settings_.ptBounces)
        n.ptBounces = ptBouncesForQuality(n.pathTracing);

    n.voxelResolution = std::clamp(n.voxelResolution, 32u, 512u);
    // At least the axial cone, or the gather returns nothing and GI silently switches itself off.
    // 16 is a ceiling on a per-pixel loop, for the same reason the bounce count has one.
    n.giCones         = std::clamp(n.giCones, 1u, 16u);
    // 1 is "a fresh direction per pixel" and is the no-op; 16 is well past where a tile stops being a
    // local neighbourhood and starts being a visible block. Clamped rather than rejected for the same
    // reason every other dial here is: a project asking for something silly gets the nearest sane
    // renderer, not a refusal to start.
    n.giSkyOcclusionTile = std::clamp(n.giSkyOcclusionTile, 1u, 16u);
    n.giIntensity     = std::clamp(n.giIntensity, 0.0f, 8.0f);
    n.giMaxDistance   = std::clamp(n.giMaxDistance, 1.0f, 100000.0f);
    // Mirrors VoxiRenderer::kMaxShadowRays / kMaxPixelsPerRayTile, restated rather than shared: this
    // library is core-only and must not depend on the RHI-backed renderer that owns those constants.
    // The renderer's own setters are the authority on the exact contract (kMaxPixelsPerRayTile also
    // rounds to a power of two); this is just enough to keep a wild request off the wire to it.
    n.rtShadowRays       = std::clamp(n.rtShadowRays, 1u, 32u);
    // 3 is a (2*3+1)^2 = 49-tap neighbourhood, which is already past the point where a wider
    // kernel buys anything a second iteration would not buy more cheaply. Kept low deliberately:
    // this runs per FRAGMENT inside the shading shader, so the tap count multiplies by overdraw.
    n.rtShadowDenoise    = std::clamp(n.rtShadowDenoise, 0u, 3u);
    // 1 is the only mode that exists besides raster; anything else is a manifest typo, and
    // clamping to 1 rather than 0 would turn a typo into a silent renderer swap.
    n.rtRenderMode       = n.rtRenderMode > 1u ? 0u : n.rtRenderMode;
    // 8 is arbitrary but finite: an unbounded bounce count in a shader loop is a hang, and the
    // useful range for a real-time path tracer is nowhere near it.
    n.ptBounces          = std::clamp(n.ptBounces, 1u, 8u);
    n.rtPixelsPerRayTile = std::clamp(n.rtPixelsPerRayTile, 1u, 16u);
    // Mirrors VoxiRenderer::kMaxGiUpdateInterval for the same reason as rtPixelsPerRayTile above.
    n.giUpdateInterval   = std::clamp(n.giUpdateInterval, 1u, 8u);

    if (n.msaa != settings_.msaa) msaaDirty_ = true;
    settings_ = n;
}

// Returns whether a feature is usable on this device.
Status Renderer::status(Feature f) const {
    switch (f) {
        case Feature::Msaa:
            return device_.maxMsaaSamples > 1 ? Status::Ready : Status::Unsupported;
        case Feature::GlobalIllumination:
            if (!device_.computeShaders) return Status::Unsupported;
            return Status::Ready;
        case Feature::RayTracing:
            if (device_.rayTracingTier < 11 || device_.shaderModel < 65 || !device_.dxcAvailable)
                return Status::Unsupported;
            return Status::Ready;
        case Feature::PathTracing:
            // MIRRORS aver::pt::PathTracer::init()'s own gate field for field (kRayTracingTier=11,
            // kShaderModel=65, caps.dxcAvailable, caps.computeShaders -- see PathTracer.cpp): this IS
            // the capability check for modules/render.pt's reference view (PtSceneView), the only
            // path tracer this engine has ever built. Used to read `return Status::NotImplemented;`
            // unconditionally here -- true the day this enum was declared, and left true long after
            // SandboxApp grew a real PtSceneView, so the Path Tracing settings-page combo that reads
            // this status stayed permanently grey and setSettings() below clamped whatever
            // Settings::pathTracing held back to Off, on every device, forever, regardless of
            // hardware -- a persisted, C#-scriptable setting with no relationship whatsoever to the
            // real path tracer. See SandboxApp.cpp's buildRenderingSettings(page==4), the only reader
            // of a Ready status here, for what actually reconciles this against PtSceneView now.
            if (device_.rayTracingTier < 11 || device_.shaderModel < 65 || !device_.dxcAvailable ||
                !device_.computeShaders)
                return Status::Unsupported;
            return Status::Ready;
        case Feature::MeshShaders:
            if (device_.meshShaderTier == 0 || device_.shaderModel < 65 || !device_.dxcAvailable)
                return Status::Unsupported;
            return Status::Ready;
        case Feature::LayeredBsdf:
            // Ready as of the commit that added averCoatTerms and wired AVER_LAYERED_BSDF into the
            // material define string. Before that this returned NotImplemented and the clamp below
            // pinned the setting to Off -- deliberately, so a combo could not change a persisted
            // value and alter nothing.
            //
            // NO DEVICE GATE, and that is a claim rather than an omission: the coat is arithmetic in
            // a pixel shader built from the same split-sum helpers the base BRDF already uses. It
            // needs no ray tracing, no mesh shaders, no compute, no shader model above what every
            // material-shaded draw already requires. If a future layer needs something the device
            // may not have, THIS is where the check goes.
            return Status::Ready;
        default: return Status::Unsupported;
    }
}

// Returns a readable reason for a feature's status.
const char* Renderer::statusText(Feature f) const {
    switch (status(f)) {
        case Status::Ready:          return "Ready";
        case Status::NotImplemented: return "Not implemented yet";
        case Status::Unsupported:
            switch (f) {
                case Feature::Msaa:               return "Device reports no MSAA";
                case Feature::GlobalIllumination: return "Device has no compute support";
                case Feature::MeshShaders:        return "Needs mesh-shader Tier 1 + SM 6.5 (D3D12 Ultimate)";
                default:                          return "Needs DXR 1.1 + SM 6.5 (D3D12 Ultimate)";
            }
        default: return "Unknown";
    }
}

// Returns a feature's display name.
const char* Renderer::featureName(Feature f) {
    switch (f) {
        case Feature::Msaa:               return "Anti-Aliasing (MSAA)";
        case Feature::GlobalIllumination: return "Global Illumination";
        case Feature::RayTracing:         return "Ray Tracing";
        case Feature::PathTracing:        return "Path Tracing";
        case Feature::MeshShaders:        return "Mesh Shaders";
        case Feature::LayeredBsdf:        return "Layered BSDF";
        default: return "?";
    }
}

// THE PER-PIXEL DIFFUSE GATHER'S CONE COUNT -- the GI setting that actually costs anything, and
// until this ladder existed the one number globalIllumination did not touch. globalIllumination
// already derived voxelResolution and giUpdateInterval below, both of which move the volume BUILD
// (measured at 0.4-0.5 ms); the per-pixel GATHER, measured at 1.3 ms and by far the larger half,
// was a hardcoded six for every tier. Turning GI down bought almost nothing, and turning it up to
// High made the frame SLOWER with no way to spend the extra budget (6.0 -> 6.4 ms: a bigger volume
// to sample, same number of samples). See Settings::giCones in Voxi.hpp for the field itself.
//
// (NOTE ON WHERE THIS COMMENT USED TO LIVE: this block sat above voxelResolutionForQuality until it
// was corrected -- giConesForQuality was inserted ahead of that function without moving that
// function's own VRAM-table comment down with it, leaving the table describing voxel grid memory
// attached to a function about cone count, and voxelResolutionForQuality with no comment at all.
// Fixed by relocating the VRAM table to its own function below, where it has sat unnoticed for one
// stage of this feature's history -- see the house rule this violated: trace the code, not the
// comment above it.)
//
// MEDIUM IS SIX, WHICH IS WHAT EVERY TIER USED TO TRACE. The default rung is deliberately the old
// hardcoded number: this ladder changes what the LADDER does, not what a default project looks
// like, and the struct default must equal the default tier's rung or the derivation never fires at
// all (it only runs on a tier CHANGE).
//
// Low buys speed and High/Epic spend it -- about 0.22 ms per cone, first measured by bisecting the
// gather pass directly. The gather is a weighted AVERAGE, normalised by the sum of the cosine
// weights, so changing the count changes how well the hemisphere is sampled rather than how bright
// the result is: fewer cones is a coarser estimate of the same quantity, not a darker one.
//
// MEASURED AGAINST THE LADDER ITSELF, not just the isolated pass: FirstPerson range, scene draw per
// tier -- Low (3 cones) 3.3 ms, Medium (6) 4.0 ms, High (9) 4.6 ms, Epic (13) 5.4 ms. A 2.1 ms
// spread across 10 cones is 0.21 ms/cone, an independent confirmation of the 0.22 ms/cone bisection
// above from a second experiment that agrees with the first.
//
// A NUMBER DELIBERATELY NOT REPEATED HERE, because this file cannot back it: a comment elsewhere in
// this tree (VoxiShaders.hpp, PSRayDriven's cost investigation, a file this one does not own) states
// "a prior measurement found dropping to two cones moved a probe by 2/255" and reads that as room to
// cut further below Low's 3-cone rung. That claim predates this ladder -- whatever it measured, it
// measured against the OLD behaviour, six hardcoded cones at every tier, not against the 3-cone Low
// rung this file now actually ships -- and neither the bisection nor the FirstPerson table above
// reproduces or contradicts it, because neither was run to test a two-cone rung. Restating it here
// as settled would be exactly the trap this project's own memory keeps a standing note against
// (aver-unbacked-verification.md): a described test result nobody re-ran against the code as it
// stands today. If two cones is ever proposed as a rung below Low, it needs its own probe capture
// against THIS derivation, not a number carried over from before the derivation existed.
// Which refraction the tier asks for. RayTraced only at the top two rungs, because it spends a ray
// per translucent pixel on the pass that is already the frame's bottleneck; ScreenSpace is nearly
// free (it reuses the backdrop copy the absorption path already takes) and is therefore the sensible
// middle. Off at Quality::Off keeps the "tier off means feature off" contract every other knob here
// honours -- with no ray tracing there is no thickness to bend by anyway.
//
// MEDIUM IS THE DEFAULT TIER, so its rung must equal Settings::refractionMode's own default or the
// change-gated derivation above can never fire on a default device. See that field's comment.
u32 Renderer::refractionForQuality(Quality q) {
    switch (q) {
        case Quality::Off:    return 0;   // Off
        case Quality::Low:    return 1;   // ScreenSpace
        case Quality::Medium: return 1;   // ScreenSpace  <- Settings::refractionMode's default
        case Quality::High:   return 2;   // RayTraced
        case Quality::Epic:   return 2;   // RayTraced
        default:              return 1;
    }
}

u32 Renderer::giConesForQuality(Quality q) {
    switch (q) {
        case Quality::Off:    return 6;   // inert: nothing gathers with GI off
        case Quality::Low:    return 3;
        case Quality::Medium: return 6;
        case Quality::High:   return 9;
        case Quality::Epic:   return 13;
        default:              return 6;
    }
}

// The voxel grid edge each GI quality tier resolves to (see setSettings for when this actually
// applies). Doubling the edge is an 8x jump in both memory and per-voxel GPU cost -- the volume is
// resolution CUBED -- so this ladder is deliberately conservative: Medium keeps the long-standing
// fixed default (128) so any project already tuned around Medium sees no change; Off and Low share
// the cheapest grid, since Off's volume is otherwise idle VRAM (voxelizePass/filterMips are skipped
// whenever GI is disabled -- see VoxiRenderer::prePass -- so a smaller grid there costs nothing in
// frame time, only in bytes reserved). Approximate VRAM for the radiance volume (RGBA16F, full mip
// chain) plus the R32_UINT injection accumulator that sits beside it, at each rung:
//   Off / Low (64):   ~6 MB
//   Medium    (128):  ~50 MB   (today's fixed default, unchanged)
//   High      (256):  ~400 MB
//   Epic      (512):  ~3.2 GB  -- by far the steepest rung; only for a GPU with gigabytes to spare
u32 Renderer::voxelResolutionForQuality(Quality q) {
    switch (q) {
        case Quality::Off:
        case Quality::Low:    return 64;
        case Quality::Medium: return 128;
        case Quality::High:   return 256;
        case Quality::Epic:   return 512;
        default:              return 128;
    }
}

// Returns how many frames apart a GI tier re-voxelises. See setSettings for why Epic is 1.
u32 Renderer::giUpdateIntervalForQuality(Quality q) {
    switch (q) {
        case Quality::Off:    return 1;   // GI is not running; the value is inert either way
        case Quality::Low:    return 8;   // kMaxGiUpdateInterval, the cheapest the clamp allows
        // MEDIUM IS 1 -- ALWAYS FRESH -- BECAUSE 4 IS WHAT MAKES LIGHTING TRAIL THE CAMERA. At 4 the
        // volume is revoxelised every fourth frame and the three in between reuse it, so indirect
        // light and the shadowing that comes with it lag scene and camera movement by up to three
        // frames. Standing still it converges to exactly the same image, which is why this reads as a
        // subtle "shadows lagging behind when moving around" rather than as an obvious fault, and why
        // it survived being measured: a still-camera benchmark cannot see it at all.
        //
        // The trade it was buying is real but belongs a rung down: Low still amortises at 8. Medium is
        // the default, the default is what people judge the renderer by, and latency in the lighting
        // is a worse first impression than a few milliseconds.
        case Quality::Medium: return 1;
        case Quality::High:   return 1;
        case Quality::Epic:   return 1;   // always fresh -- bit-identical to the old behaviour
        default:              return 1;   // an unknown tier must not silently degrade lighting
    }
}

// The RT sun-shadow rungs. MEASURED, not chosen by feel: ElectricDreams, windowed, --no-vsync, 200
// frames, whole-frame median, against 11.73 ms with rayTracing Off --
//   Low    (1 ray,  tile 4) 18.07 ms
//   Medium (1 ray,  tile 2) 18.36 ms
//   High   (2 rays, tile 2) 19.84 ms
//   Epic   (4 rays, tile 1) 23.15 ms
// Ray-traced sun shadows are not cheap on this hardware at any rung; the ladder buys back what it
// can. Tile 4 over tile 2 saves only 0.29 ms, so Low differs from Medium mostly in noise rather than
// cost -- which is why Low takes the wider tile and the same single ray, rather than pretending a
// meaningful gap exists.
// EPIC ONLY, and the zero below Epic is the whole point rather than caution. These rays are an
// ADDITION to a frame that already traces shadow rays and cone-marches a volume; the cone estimate
// they replace is wrong in enclosed geometry but costs nothing extra, so every rung that cannot
// afford another ray per pixel keeps it. A tier that silently started tracing four more rays would
// be the opposite of what a quality ladder is for.
u32 Renderer::giSkyOcclusionRaysForQuality(Quality q) {
    switch (q) {
        // OFF MUST BE 0, and not merely "inert": with no acceleration structure there is nothing to
        // trace against, and the shader falls back to the cone gather's occlusion on this value.
        case Quality::Off:    return 0;
        case Quality::Low:    return 0;
        case Quality::Medium: return 0;
        // ONE RAY AT THE TOP TWO TIERS, NOT FOUR AT ONE -- and the change is the accumulation, not
        // a re-tuning. This used to say FOUR, NOT ONE, because at one ray the estimator is
        // `open = hit ? 0 : 1`, a binary per-pixel mask whose MEAN was already right (a probe read
        // the same value at 1 and at 4 -- which is why a probe could not see the problem: a probe
        // cannot measure variance). Four samples bought five quantisation levels instead of two,
        // and the coherence tile paid for them by sharing one azimuth across a 4x4 block. That
        // comment ended by naming its own successor: "the real fix is temporal accumulation against
        // a reprojected history ... it needs a third history pair, and therefore a wider SRV/UAV
        // table, which is why it is not in this change". That pair now exists (rtAoHist_,
        // kVoxiSrvCount/kVoxiUavCount) and rtSkyOcclusionTemporal blends against it.
        //
        // So the sample count comes from FRAMES: ~10 effective samples at weight 0.9, more than
        // four rays ever gave, with independent per-pixel noise that averages away instead of
        // correlated noise that stacks into a visible block. One ray also costs less than four, so
        // High can afford the term that used to be Epic-only.
        //
        // WHY THIS MATTERS BEYOND NOISE: every tier that returns 0 here falls back to the voxel cone
        // gather's own occlusion, and MEASURED on Sponza that fallback is far too open -- turning
        // the rays off brightens the darkest 81% of the frame by 2.6x (mean luminance 20.71 -> 31.92).
        // That is why shadows only ever looked properly dark on Epic.
        //
        // LOW AND MEDIUM STAY ON THE CONE GATHER. The ray is cosine-distributed over the hemisphere,
        // so neighbouring lanes walk unrelated parts of the BVH and it is genuinely expensive --
        // accumulation fixes its VARIANCE, not its traversal cost. A budget tier should not pay it.
        case Quality::High:   return 1;
        case Quality::Epic:   return 1;
        default:              return 0;   // an unknown tier must not silently cost more
    }
}

// SHARED ACROSS A TILE ONLY WHERE THE RAYS EXIST: every rung that traces no sky-occlusion ray keeps 1,
// so the value is inert rather than merely unused, and a tier that later starts tracing does not
// inherit a coherence setting nobody chose for it.
u32 Renderer::giSkyOcclusionTileForQuality(Quality q) {
    switch (q) {
        case Quality::Off:    return 1;
        case Quality::Low:    return 1;
        case Quality::Medium: return 1;
        case Quality::High:   return 1;
        // 1 EVERYWHERE NOW, EPIC INCLUDED. The tile existed to make four incoherent hemisphere
        // rays affordable by pointing neighbouring lanes the same way, and its price was
        // correlated noise -- which is exactly what a 4x4 block of identical ambient occlusion
        // looks like on screen. With one accumulated ray there is nothing left to make coherent.
        // KEPT AS A DIAL rather than deleted: the measurements behind it still stand (+1.98 ms
        // for four rays at tile 4 against one at tile 1) and a future term tracing many
        // incoherent rays could want it. 1 is the identity, floor(p/1) == p.
        case Quality::Epic:   return 1;
        default:              return 1;   // an unknown tier gets the un-correlated, un-amortised path
    }
}

u32 Renderer::rtShadowRaysForQuality(Quality q) {
    switch (q) {
        case Quality::Off:    return 1;   // RT is not running; the value is inert either way
        case Quality::Low:    return 1;
        case Quality::Medium: return 1;
        case Quality::High:   return 2;
        case Quality::Epic:   return 4;
        default:              return 1;   // an unknown tier must not silently cost more
    }
}

// RAY-DRIVEN RENDERING IS 1 AT EVERY RUNG THAT RUNS RAY TRACING AT ALL -- Low, Medium, High, Epic
// -- BY EXPLICIT PRODUCT DECISION: this is now the default render path (the user's name for it is
// "the Wavefront Primary rays model"), not an experiment a quality preset has to avoid switching
// on behind the author's back. That decision replaces the one this comment used to record, which
// was the opposite.
//
// OFF STAYS 0, and must: a primary ray is traced through the same DXR 1.1 acceleration structure
// the RT sun shadow already requires (see status(Feature::RayTracing) above, and setSettings's
// forced clamp to Off when the device fails that gate) -- there is no BVH to traverse without it.
// Quality::Off means "no ray tracing", so answering 1 there would not be a cautious choice, it
// would be INCOHERENT: the renderer would have to detect and refuse its own default at runtime
// instead of the setting simply never claiming a mode the device has no path to run. Every other
// rung already implies that hardware is present and already pays for one kind of BVH traversal
// (the sun shadow); paying for a second kind (primary visibility) is coherent everywhere ray
// tracing itself is.
//
// It exists NOW rather than later for the same reason rtShadowDenoiseForQuality does: the
// derivation only fires on a tier CHANGE, so the function and the struct default have to agree
// from the first commit or the field never reaches the rung it claims. THE CHECK: Settings::
// rayTracing defaults to Quality::Medium, Settings::rtRenderMode defaults to 1, and
// rtRenderModeForQuality(Quality::Medium) below is 1 -- the struct default and the default tier's
// own rung agree, by construction, the same way voxelResolution's 128 agrees with Medium.
u32 Renderer::rtRenderModeForQuality(Quality q) {
    switch (q) {
        // No ray-tracing hardware path is guaranteed here -- there is nothing to traverse, and
        // claiming 1 would be a mode the renderer has no way to actually run.
        case Quality::Off:    return 0;
        case Quality::Low:    return 1;
        case Quality::Medium: return 1;
        case Quality::High:   return 1;
        case Quality::Epic:   return 1;
        // An unknown tier must not silently swap which thing finds the first surface out from
        // under a caller who did not ask for that; fall back to the path every version of this
        // engine before this setting ran.
        default:              return 0;
    }
}

// THE PATH-TRACING LADDER. Off is 1 -- one hit and direct lighting, which is ray tracing and not
// path tracing at all -- and every rung above it buys bounces. The struct default is 1 and the
// default pathTracing tier is Off, so the two agree by construction; derivation only fires on a
// tier CHANGE, and a default contradicting its own rung never reaches it.
//
// MEASURED before these were chosen, ElectricDreams at 2750x1639: 1 bounce 4.7ms, 2 bounces
// 5.9ms, 4 bounces 6.3ms. The ladder stops at 4 because bounces beyond the second cost 0.4ms
// and changed nothing measurable outdoors -- most paths escape to sky and terminate. A closed
// interior would price them differently, and this engine has none to test against yet.
u32 Renderer::ptBouncesForQuality(Quality q) {
    switch (q) {
        case Quality::Off:    return 1;   // one hit, direct lighting: ray tracing, not path tracing
        case Quality::Low:    return 2;
        case Quality::Medium: return 2;
        case Quality::High:   return 3;
        case Quality::Epic:   return 4;
        default:              return 1;   // an unknown tier must not silently start bouncing
    }
}

// THE RT SHADOW SPATIAL-FILTER RUNGS. Zero at every tier until ray-driven primary visibility
// (rtRenderMode) became the default render path and took the raster path's 4x MSAA with it -- see
// Settings::rtShadowDenoise in Voxi.hpp for the full account of why that flipped the trade. What
// follows is why EACH rung landed where it did, not just that they moved off zero.
//
// COST IS NOT THE DISCRIMINATOR HERE, AND SAYING SO ONCE SAVES REPEATING IT FOUR TIMES: the filter
// is a Gaussian gather whose tap count is (2*radius+1)^2, measured at +0.02 ms for the full 49-tap
// radius-3 kernel against +1.69 ms for one more traced ray (VoxiRenderer.cpp) -- about eighty-five
// times cheaper than the ray it stands in for, at the WIDEST rung the clamp allows. Every radius
// below 3 costs less still. So unlike rtShadowRays or giCones, this ladder is not buying frame time
// by asking for less at any rung; what actually varies by tier is how much raw noise there is to
// hide, and how much of the measured motion drift is worth risking to hide it.
//
// LOW, MEDIUM AND HIGH SHARE RADIUS 2. Low and Medium both trace exactly one ray (see
// rtShadowRaysForQuality above), so their raw shadow term is the same hard dither Settings::
// rtShadowDenoise's "THE PROBLEM IT IS FOR" describes -- there is no basis in this file for
// treating them differently on THIS knob, and the sibling shadow knobs already agree: rtShadowRays
// and rtPixelsPerRayTile both set Low equal to Medium for the identical reason (see
// rtPixelsPerRayTileForQuality's "LOW WAS 4" history for why Low's old habit of taking the cheaper,
// noisier rung was abandoned specifically for shadow quality). High traces two rays -- real
// variance reduction over one, but nowhere near the sixteen-ray convergence this file has an actual
// measurement for -- so it is still solidly in "needs real help" territory and gets the same radius
// rather than an invented intermediate value this file has no measurement to justify.
//
// EPIC IS 1, NOT 2 AND NOT 0. Epic already spends four rays specifically to be the closest this
// renderer gets to ground truth without path tracing (see ptBouncesForQuality and
// giUpdateIntervalForQuality's own Epic rungs for the same instinct applied elsewhere in this
// file) -- its raw signal has measurably less variance to begin with, so the marginal noise this
// filter would still remove is smaller while the drift it risks is not: drift scales with radius,
// not with how many rays fed the centre sample. 1 keeps a light touch on Epic's residual noise
// without reaching for the wider kernel the noisier tiers need. It is deliberately not 0: Epic's
// four rays still carry real per-pixel variance (nothing in this file's measurements converges
// below sixteen), and switching the filter off entirely on the one tier that already pays the most
// would be refusing an almost-free improvement out of a symmetry with rtPixelsPerRayTile that does
// not actually apply here. That knob went to 1 everywhere because widening it bought NOTHING real --
// its measured frame-time "saving" was noise, so refusing the drift it risked cost nothing in
// return. This filter is the opposite shape: it has no frame-time saving to weigh either way at any
// radius (see COST IS NOT THE DISCRIMINATOR above), only a real noise-reduction benefit against a
// real drift risk, both scaling with radius -- so the honest choice at Epic is the smallest radius
// that still buys a real improvement over 0, not the smallest number that happens to match a
// different knob's unrelated reason for landing on 1.
//
// NONE OF THE FOUR RUNGS REACH THE CLAMP'S MAXIMUM OF 3. Radius 3 is where "within one code of the
// sixteen-ray answer" was measured (VoxiShaders.hpp, rtShadowSpatial) -- but it is also where the
// wobble-drift measurement tops out, at the worst end of the 12-to-32-code range recorded there.
// Buying the last increment of stillness-accuracy at the largest recorded motion cost is not a
// trade this file should make silently, by default, on every tier; 3 stays reachable through an
// explicit override for whoever wants it, the same way an explicit voxelResolution request already
// overrides its own tier's derived rung.
u32 Renderer::rtShadowDenoiseForQuality(Quality q) {
    switch (q) {
        case Quality::Off:    return 0;   // RT is not running; the filter has nothing to filter
        case Quality::Low:    return 2;
        case Quality::Medium: return 2;
        case Quality::High:   return 2;
        case Quality::Epic:   return 1;
        default:              return 0;   // an unknown tier must not silently start smearing shadows
    }
}
// See rtShadowRaysForQuality for the measurements behind the ray-count rungs. The tile rungs below
// have their own history: see the LOW WAS 4 paragraph.
//
// 1 AT EVERY RUNG NOW, Low included: every pixel traces every frame, which is bit-identical to no
// denoiser at all -- no tiling, no reprojected history, no temporal blend. Anything above 1 reuses a
// reprojected sample for most pixels, and reprojection is what makes a shadow appear to trail the
// thing casting it while the camera moves. It is nearly free on this scene regardless of tile width
// (18.19 ms at tile 1 against 18.36 ms at tile 2, inside the noise), because amortisation saturates
// early at one ray per pixel -- there was never real frame time on the table for any rung to trade.
//
// LOW WAS 4, "one traced pixel per 4x4, the widest amortisation that pays." That reasoning measured
// only frame-time cost, on a still camera, and concluded the widest tile the clamp allows was free
// money. It was free money -- 22.94 ms (tile 1) vs 22.80 ms (tile 4), moving-camera medians, is noise
// -- but a still-camera benchmark cannot see what the tile actually spends: shadows visibly trailing
// the camera during Play-in-Editor, on the ONE tier that shipped with it. This is the same trap that
// already caught giUpdateInterval above (a still-camera benchmark reads amortisation as free because
// the thing it costs, motion, is exactly what it doesn't measure) and it is not a coincidence that it
// caught this knob the same way -- both are temporal-history amortisations, and both hide their cost
// from a benchmark that never pans. Re-measured this session with a wobbling camera against the
// ground-crop diff the giUpdateInterval work established: tile 1 vs tile 2 alone reproduces 0.80% of
// pixels over threshold from the reprojected-shadow trail, and tile 2 vs tile 4 adds only another
// 0.04% on top -- the artifact is already fully present at tile 2, so there is no partial-credit rung
// between "visible trail" and "none". Low is 1 now for the same reason Medium already was: the frame
// time was never real, and this was the one Low-specific amortisation that bought a visible fault
// rather than a latency nobody could see (see giUpdateIntervalForQuality and voxelResolutionForQuality
// above for the two that are real and stay).
u32 Renderer::rtPixelsPerRayTileForQuality(Quality q) {
    switch (q) {
        case Quality::Off:    return 1;
        // ONE AT EVERY RUNG, INCLUDING LOW, and the long argument for that lives with the field in
        // Voxi.hpp -- see Settings::rtPixelsPerRayTile's "ALL FOUR TIERS ARE 1" and "LOW WAS 4".
        // Short version: amortisation measured 0.14 ms moving / 0.09 ms static against tile 1, which
        // is inside the noise, while the motion trail it buys is already visible on 0.80% of pixels
        // at tile 2. It traded a fault anyone moving the camera can see for milliseconds nobody
        // could measure.
        //
        // THIS FUNCTION SAID 4 FOR LOW WHILE THAT HEADER SAID 1, which is a torn pair of exactly the
        // kind gates.baseline.txt's own header warns about -- the conclusion was written down and the
        // code was left behind. The gate suite caught it: `tier1-no-typed-uav` clamps ray tracing far
        // enough to land on the Low rung, and `rt`, `ms-rt` and `ms-rt-gi` each read 65,44,36 against
        // a recorded 65,45,37. That is the SAME three gates and the SAME one-code move this rung's
        // earlier Medium=2 experiment produced, for the same reason, which is what makes it a
        // recognisable signature rather than a mystery.
        //
        // A project that wants the amortisation at any tier can still ask: RENDER.RTPIXELSPERRAY and
        // --rt-pixels-per-ray are both honoured, and both outrank the manifest properly. Opting in is
        // a decision; a preset doing it silently is not.
        case Quality::Low:    return 1;
        case Quality::Medium: return 1;
        case Quality::High:   return 1;
        case Quality::Epic:   return 1;
        default:              return 1;   // an unknown tier must not silently reintroduce the history
    }
}

// Returns a quality level's display name.
const char* Renderer::qualityName(Quality q) {
    switch (q) {
        case Quality::Off:    return "Off";
        case Quality::Low:    return "Low";
        case Quality::Medium: return "Medium";
        case Quality::High:   return "High";
        case Quality::Epic:   return "Epic";
        default: return "?";
    }
}

// Returns true once after settings.msaa changes, then clears the flag.
bool Renderer::consumeMsaaDirty() {
    const bool d = msaaDirty_;
    msaaDirty_ = false;
    return d;
}

} // namespace aver::voxi

// ------------------------------------------------------------------ C ABI
using aver::voxi::Feature;
using aver::voxi::Quality;
using aver::voxi::Renderer;
using aver::voxi::Settings;

namespace {
// True when a feature id is in range.
bool validFeature(int32_t f) { return f >= 0 && f < AVER_VOXI_FEATURE_COUNT; }
// Returns the quality field a feature id names, or null.
Quality* qualitySlot(Settings& s, int32_t f) {
    switch (f) {
        case AVER_VOXI_FEATURE_GLOBAL_ILLUMINATION: return &s.globalIllumination;
        case AVER_VOXI_FEATURE_RAY_TRACING:         return &s.rayTracing;
        case AVER_VOXI_FEATURE_PATH_TRACING:        return &s.pathTracing;
        default: return nullptr;
    }
}
} // namespace

extern "C" {

// Returns the number of features.
int32_t aver_voxi_feature_count(void) { return AVER_VOXI_FEATURE_COUNT; }

// Returns a feature's display name.
const char* aver_voxi_feature_name(int32_t f) {
    return validFeature(f) ? Renderer::featureName(static_cast<Feature>(f)) : "?";
}
// Returns a feature's status id.
int32_t aver_voxi_feature_status(int32_t f) {
    return validFeature(f) ? static_cast<int32_t>(Renderer::get().status(static_cast<Feature>(f)))
                           : AVER_VOXI_STATUS_UNSUPPORTED;
}
// Returns a feature's status as readable text.
const char* aver_voxi_feature_status_text(int32_t f) {
    return validFeature(f) ? Renderer::get().statusText(static_cast<Feature>(f)) : "?";
}

// Returns the current MSAA sample count.
int32_t aver_voxi_get_msaa(void) { return static_cast<int32_t>(Renderer::get().settings().msaa); }

// Sets the MSAA sample count. Returns 0 if the request was rejected or clamped away.
int32_t aver_voxi_set_msaa(int32_t samples) {
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8) return 0;
    Settings s = Renderer::get().settings();
    s.msaa = static_cast<aver::voxi::Msaa>(samples);
    Renderer::get().setSettings(s);
    return static_cast<int32_t>(Renderer::get().settings().msaa) == samples ? 1 : 0;
}

// Returns the supported sample counts as a bit mask.
int32_t aver_voxi_msaa_mask(void) { return static_cast<int32_t>(Renderer::get().deviceInfo().msaaMask); }

// Returns a feature's quality level.
int32_t aver_voxi_get_quality(int32_t f) {
    Settings s = Renderer::get().settings();
    const Quality* q = qualitySlot(s, f);
    return q ? static_cast<int32_t>(*q) : AVER_VOXI_QUALITY_OFF;
}

// Sets a feature's quality level. Returns 0 if the request was rejected or clamped away.
int32_t aver_voxi_set_quality(int32_t f, int32_t quality) {
    if (quality < AVER_VOXI_QUALITY_OFF || quality > AVER_VOXI_QUALITY_EPIC) return 0;
    Settings s = Renderer::get().settings();
    Quality* slot = qualitySlot(s, f);
    if (!slot) return 0;
    *slot = static_cast<Quality>(quality);
    Renderer::get().setSettings(s);
    Settings after = Renderer::get().settings();
    const Quality* now = qualitySlot(after, f);
    return (now && static_cast<int32_t>(*now) == quality) ? 1 : 0;
}

// Returns the cubic voxel grid edge.
int32_t aver_voxi_get_voxel_resolution(void) { return static_cast<int32_t>(Renderer::get().settings().voxelResolution); }
// Sets the cubic voxel grid edge. Returns 0 if out of range.
int32_t aver_voxi_set_voxel_resolution(int32_t res) {
    if (res < 32 || res > 512) return 0;
    Settings s = Renderer::get().settings(); s.voxelResolution = static_cast<aver::u32>(res);
    Renderer::get().setSettings(s); return 1;
}
// Returns the indirect bounce multiplier.
float   aver_voxi_get_gi_intensity(void) { return Renderer::get().settings().giIntensity; }
// Sets the indirect bounce multiplier.
int32_t aver_voxi_set_gi_intensity(float v) {
    Settings s = Renderer::get().settings(); s.giIntensity = v; Renderer::get().setSettings(s); return 1;
}
// Returns the cone trace range in centimetres.
float   aver_voxi_get_gi_max_distance(void) { return Renderer::get().settings().giMaxDistance; }
// Sets the cone trace range, in centimetres.
int32_t aver_voxi_set_gi_max_distance(float cm) {
    Settings s = Renderer::get().settings(); s.giMaxDistance = cm; Renderer::get().setSettings(s); return 1;
}

// Returns how many frames apart the GI volume is re-voxelised (1 = every frame).
int32_t aver_voxi_get_gi_update_interval(void) { return static_cast<int32_t>(Renderer::get().settings().giUpdateInterval); }
// Sets the GI revoxelise interval, in frames. Returns 0 if out of range.
int32_t aver_voxi_set_gi_update_interval(int32_t frames) {
    if (frames < 1 || frames > 8) return 0;
    Settings s = Renderer::get().settings(); s.giUpdateInterval = static_cast<aver::u32>(frames);
    Renderer::get().setSettings(s); return 1;
}

// Returns the device's ray tracing tier.
int32_t aver_voxi_ray_tracing_tier(void) { return static_cast<int32_t>(Renderer::get().deviceInfo().rayTracingTier); }
// Returns the device's highest supported MSAA sample count.
int32_t aver_voxi_max_msaa(void)         { return static_cast<int32_t>(Renderer::get().deviceInfo().maxMsaaSamples); }
// Returns the device's mesh shader tier.
int32_t aver_voxi_mesh_shader_tier(void) { return static_cast<int32_t>(Renderer::get().deviceInfo().meshShaderTier); }
// Returns the device's shader model.
int32_t aver_voxi_shader_model(void)     { return static_cast<int32_t>(Renderer::get().deviceInfo().shaderModel); }
// Returns 1 when the mesh shader submission path is on.
int32_t aver_voxi_get_mesh_shaders(void) { return Renderer::get().settings().meshShaders ? 1 : 0; }
// Turns the mesh shader submission path on or off. Returns 0 if the device refused it.
int32_t aver_voxi_set_mesh_shaders(int32_t on) {
    Settings s = Renderer::get().settings(); s.meshShaders = on != 0;
    Renderer::get().setSettings(s);
    return Renderer::get().settings().meshShaders == (on != 0) ? 1 : 0;
}

} // extern "C"
