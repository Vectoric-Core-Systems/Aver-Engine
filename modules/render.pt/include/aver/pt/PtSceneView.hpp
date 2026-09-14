#pragma once
// PtSceneView -- an opt-in, progressive REFERENCE render of the REAL scene, through the same
// brute-force path tracer PtFurnaceTest already proves correct. Where PtFurnaceTest brings its own
// geometry to check the integrator's arithmetic, this is the other half: it is handed the scene the
// RASTER path already draws (through rhi::IRenderFeature::submitDraw, the exact hook VoxiRenderer's
// own draw-list capture uses) and the REAL camera (through rhi::IDevice::camera(), the exact accessor
// VoxiRenderer::fitCascades already reads), and turns them into a small, still-camera accumulator
// that converges toward a physically-based image the raster path cannot produce on its own -- soft
// indirect light, with no cheats and no denoiser hiding the noise.
//
// WHAT IT IS: a reference view, not a render mode. It suppresses the raster scene while registered
// and able to paint (rhi::IRenderFeature::suppressesScene(), which used to be unconditionally true
// and is now gated on the same four terms scenePass() checks -- see there) and draws its accumulator
// instead of it, at a small FIXED resolution independent of the swapchain, accumulating a bounded
// number of samples every still frame and resetting the moment the camera moves or the STATIC draw
// list changes -- an image that keeps accumulating across a camera move is not a reference image, it
// is a smear. It is registered ONLY when asked for -- SandboxApp's --pt-scene flag at startup, or the
// editor's own Path Tracing settings-page Quality combo (voxi::Settings::pathTracing, real as of the
// gap that used to leave it clamped to Off on every device -- see Voxi.cpp's status(Feature::
// PathTracing)) at any later frame, both funnelled through the SAME syncPtSceneView() reconciler --
// never by default: while unregistered it costs nothing, exactly like PtFurnaceTest.
//
// WHAT IT DELIBERATELY DOES NOT DO, stated here because a first-time user hitting any of these should
// read this comment before filing a bug:
//   - LIGHT SOURCE. Two sources reach this view, and only two. ptEnvironment() (PtShaders.hpp) is
//     skyColor() and nothing else -- INDIRECT/ambient light, collected when a bounce MISSES.
//     ptDirectSun() (PtShaders.hpp) is a next-event shadow ray fired at the one authored directional
//     light on every HIT -- DIRECT sun light, including on surfaces the sky itself cannot see (an
//     overhang, a wall facing away from open sky). Together that is "outdoors, or an interior that
//     sees sky and/or sun through a real opening". There is still no CLight (point/spot/area) and no
//     emissive term: a room lit only by placed lights or glowing materials, with no sky above it and
//     no line of sight to the sun, still correctly, honestly renders BLACK. That is not a bug in
//     this feature -- it is exactly what those two sources not existing here means.
//   - GEOMETRY. Only draws whose mesh has no compute-written vertex buffer are included -- the same
//     predicate VoxiRenderer::buildAccelerationStructures already applies (VoxiRenderer.cpp, gated on
//     IDevice::meshVertexBuffer). Skinned characters, particles, and anything else that writes its
//     own vertices every frame are silently absent: including them would mean the scene "changes"
//     every frame and the accumulator would never progress past sample 0.
//   - MATERIALS. PtSurface carries one flat linear albedo, produced by the host's AlbedoResolver
//     when it recognises the draw's binding, and by the legacy per-draw base colour otherwise (which
//     is also what every draw gets when no resolver is installed at all). No textures are sampled,
//     no metallic/roughness, no emissive. A textured OPAQUE material renders as its flat, decoded
//     base colour and is traced as pure Lambertian, exactly as before.
//
//     A BLENDED DRAW (submitDraw's `blended` flag, see IRenderFeature's own contract) is now
//     ACCEPTED rather than dropped, and marked a smooth, non-absorbing DIELECTRIC -- see
//     PtSurface::ior and PtShaders.hpp's ptScatterDielectric. ITS IOR IS A SINGLE FIXED CONSTANT
//     (kDefaultGlassIor), NOT THE REAL MATERIAL'S, and that is a stated approximation, not an
//     oversight: this module links only Core and the GENERIC RHI (see CMakeLists.txt) and so cannot
//     read pbr::MaterialConstants::ior out of drawConstants any more than it can read baseColorFactor
//     without AlbedoResolver's help. A future IorResolver mirroring AlbedoResolver's own shape would
//     let the host vary it per material; until one exists, every pane of glass in a traced scene
//     reads as ordinary soda-lime glass regardless of what its author actually set.
//   - PERFORMANCE. No denoiser, no importance sampling of lights, no spectral anything, not
//     real-time. It is a progressive, still-camera-only reference; see kAccumWidth/kAccumHeight/
//     kMaxBounces/kSamplesPerStep/kMaxSamples below for the bounded numbers it stays inside.
#include "aver/pt/PathTracer.hpp"

#include <functional>
#include <vector>

namespace aver::pt {

class PtSceneView final : public rhi::IRenderFeature {
public:
    ~PtSceneView() override;

    // Compiles the integrator. False means this device cannot run it (no ray tracing, or the compute
    // pipeline would not build) -- reported as "unavailable", never as a wrong answer, exactly like
    // PtFurnaceTest::init(). Allocates no scene or accumulator yet: those are built lazily, from the
    // first frame's real draw list, inside prePass().
    bool init(rhi::IDevice& dev);
    void shutdown();
    bool available() const { return pt_.available(); }

    // Turns one draw's OPAQUE binding into a flat linear albedo, or returns false to leave the draw
    // on its legacy per-draw base colour.
    //
    // THE HOST RESOLVES, NOT THIS MODULE -- the same division LandscapeRenderer::setSurfaceBinding
    // already uses, and for the same reason: submitDraw() is handed a binding handle and a block of
    // raw bytes with no type attached, and only the composition root knows what bound them. This
    // module links Core and the GENERIC RHI alone (see CMakeLists.txt) and so cannot name a pbr::
    // type to check against even if it wanted to. Guessing from the bytes instead -- "it is a
    // material if the block is the right SIZE" -- is what this seam exists to avoid: any unrelated
    // block of the same size would pass, silently, and the only symptom would be a wrong colour.
    //
    // Left unset, every draw uses its base colour, which is exactly right for a caller that has no
    // material system at all (PtFurnaceTest brings its own geometry and never sets one).
    //
    // ONE STRUCT RATHER THAN A GROWING OUT-PARAMETER LIST. This started as a single float3 and has
    // since needed a base-colour texture, a metal-rough texture, a normal map and three scalars;
    // threading each as its own pointer makes every future field a signature change at both ends,
    // and makes it easy to fill six of seven and leave the last reading whatever was on the stack.
    struct ResolvedMaterial {
        // THE FACTOR when baseColorTex is set, THE FINISHED COLOUR when it is not. That ambiguity is
        // deliberate and is why one call answers both: a resolver that hands back the texture's mean
        // AND its handle applies the texture twice, which reads as a uniformly too-dark scene with
        // no assert and nothing logged.
        f32 albedo[3] = {1, 1, 1};
        // NEGATIVE MEANS LAMBERTIAN, matching PtSurface::roughness -- a resolver that knows nothing
        // about gloss leaves this alone and the surface keeps the diffuse-only BSDF the tracer had
        // before specular existed. When metalRoughTex is set these two are FACTORS multiplying it,
        // exactly as the .ocmat authoring means them.
        f32 roughness = -1.0f;
        f32 metallic = 0.0f;
        f32 normalScale = 1.0f;
        rhi::TextureHandle baseColorTex = 0;
        rhi::TextureHandle metalRoughTex = 0;
        rhi::TextureHandle normalTex = 0;
    };
    using AlbedoResolver = std::function<bool(rhi::BindingSetHandle set, const void* constants,
                                              u32 bytes, ResolvedMaterial& out)>;
    void setAlbedoResolver(AlbedoResolver r) { resolveAlbedo_ = std::move(r); }

    const char* name() const override { return "Aver.PathTracer.SceneView"; }

    // ---- rhi::IRenderFeature ----
    void beginScene() override;
    void submitDraw(rhi::MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                    f32 metallic, f32 roughness, rhi::BindingSetHandle drawBinding,
                    const void* drawConstants, u32 drawConstantBytes, bool blended) override;
    void prePass(rhi::IRenderContext& ctx) override;
    // On whenever this feature is registered AND can actually paint. Registration is still the real
    // switch -- the caller decides (see the class comment), not a runtime toggle here.
    //
    // WHY THIS IS NOT SIMPLY `return true`, which is what it was: suppressesScene() is a PROMISE to
    // paint the whole scene, and scenePass() below opens with the exact same four-way guard and
    // silently returns when it fails. The two disagreeing means the backend drops every drawMesh()
    // for the frame (D3D12Device::drawMesh) and then nothing paints in their place -- a cleared
    // target, from a feature that said it had this covered. Testing the same condition in both
    // places is what makes the promise honest; the guard in scenePass() is now a belt-and-braces
    // repeat rather than the only check.
    //
    // Read LIVE, once per drawMesh() call, all through onRender() -- so every term here has to be
    // stable across a frame. All four are: target_ and sceneReady_ are written by rebuildScene()
    // from prePass(), which runs before any drawMesh(), and the two present handles are built once
    // in init()/onRenderTargetsChanged(). See SandboxApp::syncPtSceneView's comment on why the
    // REGISTRATION itself is likewise only ever mutated from onUpdate().
    bool suppressesScene() const override {
        return presentPso_ && presentSet_ && sceneReady_ && target_.valid();
    }
    void scenePass(rhi::IRenderContext& ctx) override;
    void onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth,
                                u32 width, u32 height) override;

    // Samples accumulated toward the CURRENT image (resets to 0 whenever the camera or the static
    // scene changes). For reporting convergence, not for anything this class reads itself.
    u32 samplesAccumulated() const { return sampleCursor_; }
    bool sceneReady() const { return sceneReady_; }

private:
    // One replayed STATIC draw: enough to rebuild a PtSurface from, nothing else. Captured a frame
    // behind the real draw list for the same reason VoxiRenderer::draws_/drawsPrev_ is: a frame's
    // list is not known complete until every drawMesh() call for it has happened.
    struct Draw {
        rhi::MeshHandle mesh = 0;
        f32 world[16];
        f32 albedo[3];
        // The material this draw's binding resolved to. Textures are kept as RHI HANDLES rather than
        // resolved bindless indices because submitDraw runs during the frame's draw walk while
        // residency is a PathTracer question answered at rebuildScene time; storing handles keeps
        // the two apart and keeps drawsKey() hashing something stable (a handle is never recycled,
        // an index could in principle be renumbered).
        f32 roughness = -1.0f;
        f32 metallic = 0.0f;
        f32 normalScale = 1.0f;
        rhi::TextureHandle baseColorTex = 0;
        rhi::TextureHandle metalRoughTex = 0;
        rhi::TextureHandle normalTex = 0;
        // 0.0 = opaque Lambertian (every draw the backend never marked `blended`); kDefaultGlassIor
        // for a blended one. See PtSurface::ior for why one field carries both the kind and the
        // value, and this class's own MATERIALS comment for why the value is a fixed constant.
        f32 ior = 0.0f;
    };
    std::vector<Draw> draws_, drawsPrev_;

    // Tears down the OLD target and scene, rebuilds pt_'s scene from drawsPrev_, and creates a fresh
    // target and accumulator. Returns false when nothing static survived the filter, or the device
    // refused an acceleration structure or the accumulator itself.
    bool rebuildScene(rhi::IRenderContext& ctx);
    // (Re)writes the present binding set's SRV over target_.accum. Called after every rebuild, since
    // rebuildScene() always hands back a NEW buffer handle.
    bool ensurePresentResources();
    // Reads the REAL camera (rhi::IDevice::camera()) and turns it into a PtCamera with the
    // ACCUMULATOR's own aspect -- never the real viewport's, see the class comment. False when there
    // is no camera to read yet, or the projection is degenerate.
    bool deriveCamera(PtCamera& out) const;

public:
    // Picks a rung of kAccumLadder, 0..3. Anything higher clamps to the top.
    //
    // Takes effect on the next frame, by forcing the same re-arm path a scene change takes: the
    // accumulator is a GPU buffer sized at createTarget() time, so a resolution change is a
    // reallocation, not a uniform. Idempotent -- setting the rung it is already on does nothing at
    // all, which is what makes it safe for the editor to call every frame from its settings read.
    void setQuality(u32 rung);
    u32  quality() const { return quality_; }

    // R5/F6 (contrast-fix plan): true restores the pre-fix reference sky (skyColor() on every miss,
    // unmatched to the raster's calibrated diffuse ambient); false (the default PtDispatch::
    // legacyEnvironment already carries) is the matched, corrected environment. Idempotent, like
    // setQuality above -- safe to call every frame from wherever the caller stores the console/CLI
    // slot. A change RE-ARMS ACCUMULATION (resets sampleCursor_, not the scene or the target: no
    // acceleration structure or resolution is affected) because every sample already summed into the
    // buffer was drawn under the OTHER environment, same reasoning as the sun/sky-change branch in
    // prePass(). Logs one AVER_INFO line on an actual change.
    void setLegacyEnvironment(bool legacy);
    bool legacyEnvironment() const { return legacyEnvironment_; }

private:
    // An order-sensitive FNV-1a hash of drawsPrev_ (mesh, world bits, albedo bits) -- the exact same
    // shape and constants VoxiRenderer::giDrawsKey() uses, for the same reason: a cache whose
    // invalidation rule is "close enough" is a cache that goes stale silently, so this is bit-exact,
    // not tolerance-based.
    u64 drawsKey() const;

    // Empty unless the host installed one; see setAlbedoResolver.
    AlbedoResolver resolveAlbedo_;

    rhi::IDevice*          dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    PathTracer              pt_;
    PtTarget                target_;

    PtCamera curCam_{};
    // The rung in force, 0..3. Changing it re-creates the accumulator, which costs a re-arm.
    u32 quality_ = 0;
    u32 accumWidth_  = kAccumWidthLow;
    u32 accumHeight_ = kAccumHeightLow;
    bool     haveCam_ = false;
    // The lighting this accumulator's samples were drawn under. accumulate() packs only the camera
    // into its constants -- the sun and sky reach the integrator through the shared per-frame block
    // inside the shader -- so without this a moving sun blends two times of day into one mean with
    // no reset and no warning. Compared the way VoxiRenderer::giSnapshotUnchanged compares it, whose
    // comment records the two traps (padding in a by-value return, and cloudTime being a clock).
    rhi::SkyAtmosphere sky_{};
    bool     haveSky_ = false;
    // R5/F6: mirrors into every accumulate() dispatch's PtDispatch::legacyEnvironment; see
    // setLegacyEnvironment's own comment above.
    bool     legacyEnvironment_ = false;
    // Said once per restart cause, so a permanently-restarting accumulator is distinguishable from a
    // working one in a log. It was silent before, which is why nobody could tell.
    bool     camResetLogged_ = false;

    u64  sceneKey_ = 0;
    bool sceneReady_ = false;
    u32  sampleCursor_ = 0;

    // ---- the DENOISER ------------------------------------------------------------------------
    //
    // An edge-aware a-trous (Dammertz et al.) wavelet filter over the 480x270 accumulator, run
    // after accumulation and before presentation.
    //
    // WHY THIS EXISTS, measured rather than assumed: a path-traced SHADOW carries roughly 18x the
    // relative error of the sunlit floor beside it, at every sample count (10.9% vs 0.55% at 64
    // spp). That is not a bug in the shadow test -- ptDirectSun is a single deterministic ray at a
    // delta light, and it returns exactly zero when occluded. It is that a lit pixel gets a large
    // noise-free direct term diluting its indirect noise, while a shadowed pixel's ENTIRE signal
    // is the cosine-sampled indirect bounce. Shadows are structurally the noisiest thing in the
    // image, and they are what an author notices.
    //
    // ITS STRENGTH IS DRIVEN BY THE PER-PIXEL SAMPLE COUNT, which is the part worth keeping. The
    // expected noise of a Monte Carlo mean falls as 1/sqrt(n), so the colour edge-stop is widened
    // by exactly that: strong filtering on the frame after a camera move (8 spp), and effectively
    // identity once the image has converged. That matters here more than in a game renderer,
    // because this view exists to be a REFERENCE -- blurring a converged reference would destroy
    // the thing it is for. It also means there is no pop when convergence completes: the filter
    // fades out continuously rather than switching off.
    //
    // NO GEOMETRY BUFFER, deliberately. A full SVGF stops on normal and depth as well as colour,
    // and doing that here would mean either widening the accumulator (which the furnace test reads
    // back at a hard-coded stride) or adding a second UAV to the integrator. Both change code this
    // module is measured by, to buy edge preservation that the 1/sqrt(n) colour stop already
    // provides most of at this resolution. If a later pass needs true geometric stops, THAT is
    // when to pay for the G-buffer.
    //
    // PathTracer ITSELF IS UNTOUCHED by all of this -- the accumulator keeps its exact layout and
    // semantics, so PtFurnaceTest's oracle reads the same bytes it always did. The denoiser reads
    // that buffer and writes its own.
    rhi::PipelineHandle   denoisePso_ = 0;
    rhi::BufferHandle     denoiseBuf_[2] = {0, 0};
    // HOW MANY PIXELS denoiseBuf_ WAS ACTUALLY ALLOCATED FOR, which is not the same question as
    // "how many pixels does the accumulator have" the moment anything changes the quality rung.
    // Keeping the two apart is the whole fix: see ensureDenoiseResources().
    u32                   denoisePixels_ = 0;
    // Three sets, because the chain is accum -> A -> B -> A -> ... : the first pass RESOLVES (sum
    // divided by count) out of the two-element accumulator, and the rest ping-pong between two
    // one-element mean buffers. A binding set is read when the command EXECUTES, so these cannot
    // be one set rebound between dispatches -- the same rule PathTracer::createTarget states.
    rhi::BindingSetHandle denoiseSetResolve_ = 0;   // accum -> A
    rhi::BindingSetHandle denoiseSetAB_ = 0;        // A -> B
    rhi::BindingSetHandle denoiseSetBA_ = 0;        // B -> A
    bool denoiseReady_ = false;
    // Which of denoiseBuf_ holds the finished image after the last pass. Presentation binds it.
    u32  denoiseResult_ = 0;
    bool ensureDenoiseResources();
    void runDenoise(rhi::IRenderContext& ctx);

    // The a-trous step sizes, in source texels, one dispatch each. 1,2,4 gives a 5x5 kernel an
    // effective support of about 33x33 for three passes rather than the 1089 taps a direct kernel
    // that wide would cost -- which is the entire point of the a-trous construction.
    static constexpr u32 kDenoisePasses = 3;
    // How many multiples of the estimated noise level count as "the same surface". Tuned by
    // measurement on the --pt-scene cast shadow, not by eye; see the .cpp.
    static constexpr f32 kDenoiseColorSigma = 1.5f;

    // ---- the presentation pass: gPtAccum (a StructuredBuffer) drawn to the scene colour target ----
    rhi::PipelineHandle   presentPso_ = 0;
    rhi::BindingSetHandle presentSet_ = 0;
    u32        presentSampleCount_ = 0;
    rhi::Format presentColorFormat_ = rhi::Format::Unknown;

    bool loggedUnavailable_ = false;
    bool loggedFirstFrame_  = false;
    bool dropCapLogged_     = false;
    bool convergedLogged_   = false;

    // ---- the bounded numbers this stays inside; see the class comment's PERFORMANCE point ----
    //
    // Bounded and independent of the swapchain: its cost must not scale with however large the
    // editor's viewport happens to be. But it is no longer FIXED -- see setQuality() and kAccumLadder
    // for why a single hardcoded 480x270 was the single most visible thing wrong with this view.
    //
    // THIS USED TO READ "a reference view, NEVER a render mode", and that is now an open question
    // rather than a settled one. It is still true of the class as it stands -- accumulation restarts
    // on any camera change (see prePass), the denoiser is three spatial a-trous passes with no
    // temporal component, and what a moving viewport shows is exactly one 8-sample step. Those are
    // the things that would have to change, not this sentence. It is softened rather than deleted so
    // that a reader does not take the old absolute as a reason not to ask.
    static constexpr u32 kAccumWidthLow  = 480;
    static constexpr u32 kAccumHeightLow = 270;
    static constexpr u32 kMaxBounces  = 4;
    // The first bounce Russian roulette may terminate a path at; see PtDispatch::rouletteDepth for
    // the technique and for why it is off unless a caller asks. THIS view asks and PtFurnaceTest does
    // not, which is the whole reason the switch is a dispatch field rather than a shader define.
    //
    // 2, NOT 0, and with kMaxBounces at 4 that leaves bounces 2, 3 and 4 eligible. Bounces 0 and 1
    // carry nearly all of a pixel's energy, so rouletting them buys almost no traversal -- a path
    // killed at b=0 skips at most four iterations the miss/horizon exits often skip anyway -- while
    // adding variance exactly where the image is brightest and any noise is most visible.
    //
    // MEASURED, and it is the largest single win available to this integrator. Sponza at Epic
    // (1280x720, 4 bounces, 8 spp/step), --no-vsync, --gpu-timing, same camera, only this constant
    // differing:
    //
    //     kRouletteDepth 0 (off) -- "PT accumulate" 44.72 ms
    //     kRouletteDepth 2       -- "PT accumulate" 28.68 ms      36% cheaper
    //
    // AND THE IMAGE DID NOT MOVE: the probe read 10,9,9 in both runs. That pairing -- a third of the
    // cost gone with a bit-identical probe -- is what "unbiased" means in practice, and it is the
    // check to repeat if this constant is ever changed. A roulette that CHANGED the probe would mean
    // the throughput division was wrong, not that the estimator had been tuned.
    static constexpr u32 kRouletteDepth = 2;
    // 8 samples per still frame. THE TRACE COUNT IS TWO PER BOUNCE, NOT ONE, and this comment said
    // one for as long as next-event estimation has existed: the bounce ray is joined by ptDirectSun's
    // shadow ray at every non-dielectric hit, which pt_pathtrace.hlsl:306 states outright ("COSTS ONE
    // EXTRA RayQuery PER DIFFUSE HIT"). So the budget is up to (bounces+1)*2 = 10 traces per sample
    // and 80 per pixel per dispatch -- 480x270 = ~10.4M traces/dispatch, twice the 5.2M this used to
    // claim. The old figure was not a rounding error; it halved the headline number in every
    // comparison drawn against it. See PtSceneView.cpp's own comment at the accumulate() call site
    // for how the corrected budget sits beside VoxiRenderer's measured, TDR-conscious ray budget.
    static constexpr u32 kSamplesPerStep = 8;
    // Stops issuing further accumulate() dispatches once this many samples have landed, so leaving
    // the camera still for a long unattended session does not keep issuing GPU work forever. The
    // image stays on screen; it simply stops changing. 200 steps * 8 spp = 1600 samples/pixel.
    static constexpr u32 kMaxSamples = 1600;
    // Warn-and-drop past this many static instances in one snapshot, mirroring
    // VoxiRenderer::kMaxDraws's own reasoning: addScene()'s createTlas(count) sizes to the snapshot's
    // EXACT count, with no headroom, so an unbounded snapshot would try to build one BLAS per
    // instance with no limit.
    static constexpr u32 kMaxInstances = 4096;

    // THE FIXED IOR EVERY BLENDED DRAW GETS, ordinary soda-lime glass -- see the class comment's
    // MATERIALS point for why this is one constant rather than the real material's own value: this
    // module has no path to pbr::MaterialConstants::ior without linking render.pbr, the same reason
    // AlbedoResolver exists as a callback instead of this module reading baseColorFactor itself.
    static constexpr f32 kDefaultGlassIor = 1.5f;

    // THE LADDER the Path Tracing quality combo drives. Four rungs, 16:9, each roughly double the
    // pixels of the one below.
    //
    // WHY RESOLUTION AND NOT SAMPLES. While the camera is moving the accumulator restarts every
    // frame, so what is on screen is always exactly ONE step -- kSamplesPerStep samples, no more,
    // whatever the tier. What the tier CAN change is how far that image is stretched: 480x270 shown
    // in a 2750-wide viewport is a 5.7x magnification, and that blow-up, not the sample count, is
    // what reads as a shimmering mess while flying. Spending the tier on samples instead would
    // sharpen an image nobody can see the pixels of.
    //
    // COST: WITHDRAWN. This comment used to carry a ladder --
    //
    //     Low 480x270  4.5 ms | Medium 640x360  5.0 ms | High 960x540  5.5 ms | Epic 1280x720  6.9 ms
    //
    // -- against "a raster frame of 9.5 ms", and concluded that EVERY rung is cheaper than the view
    // it suppresses. That conclusion does not survive reading the code behind it, and it is recorded
    // here rather than deleted because it was quoted for weeks:
    //
    //   THE NUMBERS ARE CPU WHOLE-FRAME PERIODS from --frame-time, not GPU spans, and they were set
    //   beside GPU pass spans from the other renderer. Different quantities.
    //   THE TRACED FRAME DOES STRICTLY LESS WORK. This class does not override suppressesWholeFrame(),
    //   so it inherits `return suppressesScene()` and sets frameSuppressed_, which skips the deferred
    //   sky, the blended replay and all particles/lines/chrome (D3D12Device.cpp :4545, :4578, :4786).
    //   VoxiRenderer's own override is `debugViewActive()` -- false in ray-driven -- so THAT path
    //   pays all three. The whole 2.6 ms margin is of the same order as the passes being skipped.
    //   IT MEASURED A DIFFERENT TRACER. At the commit that recorded it (18210ce0) this shader had
    //   zero texture fetches, no bindless, and no GGX lobe -- its own message says "still shades flat
    //   per surface". Three mip-0 bindless samples per hit, an energy-LUT fetch and a two-lobe
    //   specular branch have been added since.
    //   AND IT WAS AGAINST THE RASTERISER. rtRenderMode defaulted to 0 then; ray-driven primary
    //   visibility did not become the default until six days later.
    //
    // The rungs are still the right SHAPE -- resolution, not sample count, is what the tier should
    // buy while the camera moves -- and that reasoning above is untouched by any of this. What is
    // gone is the claim that any of it is cheaper than raster.
    //
    // AND THE REAL NUMBER, now that "PT accumulate" and "PT denoise" exist as GPU spans. Sponza (112
    // entities), --no-vsync, --gpu-timing, Epic on both sides:
    //
    //     PT accumulate (1280x720)                    28.68 ms
    //     Voxi ray-driven primary (2750x1639)         12.25 ms
    //
    // THE TRACER IS 2.3x SLOWER THAN THE PASS IT WAS SAID TO BEAT BY 5x, and per PIXEL it is worse
    // still -- 0.92 M pixels against 4.5 M, so roughly 11x the cost each. This is with Russian
    // roulette already saving it 36% (see kRouletteDepth). The old figure was not merely measured
    // badly; it had the sign backwards.
    //
    // NOTE WHAT IS AND IS NOT BEING COMPARED. "Voxi ray-driven primary" is the WHOLE deferred shade
    // in one draw, not just visibility, and the PT frame skips the sky, the blended replay and all
    // chrome because it suppresses them. Neither of those rescues the ratio at this magnitude, but
    // both belong in any statement of it.
    struct AccumRung { u32 width; u32 height; };
    static constexpr AccumRung kAccumLadder[4] = {
        { 480, 270},   // Low    -- 1.0x, what this view always used to be
        { 640, 360},   // Medium -- 1.8x
        { 960, 540},   // High   -- 4.0x
        {1280, 720},   // Epic   -- 7.1x
    };
    // Root CBV register for the presentation pixel shader's own tiny constant block (accumulator
    // width/height). This pipeline never includes rhi::sharedShaderPrelude() on the PIXEL side (only
    // the vertex shader borrows its VSky entry point), so b1 here is unrelated to what b1 means to a
    // pipeline that does -- same reasoning AverSrFxaa.cpp's kFxaaConstantRegister gives.
    static constexpr u32 kPresentConstantRegister = 1;
};

} // namespace aver::pt
