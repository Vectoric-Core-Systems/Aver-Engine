// Aver.Render.NRD -- the engine's seam onto NVIDIA Real-Time Denoisers.
//
// NRD IS NOT A RENDERER. It never touches a device, a command list or a texture. It is a planner:
// you tell it what your camera did this frame, and it hands back a list of compute dispatches --
// pipeline index, resource slots, constant bytes, thread-group counts -- for someone else to
// record. Everything GPU-shaped in this header is therefore a DESCRIPTION, and the backend that
// consumes it is free to be D3D12, Vulkan or a test that never creates a device at all. That is
// also why this module links Aver.RHI (for the format vocabulary) and no backend: no D3D12 or
// Vulkan type may cross this boundary, same rule as Aver.Occlusion.
//
// WHY NRD's OWN HEADERS DO NOT APPEAR HERE. They are not permissively licensed (see
// third_party/nrd/AVER_README.md), so they stay PRIVATE to this module's .cpp and the include
// directory is never exported. A consumer of this header compiles whether or not NRD is in the
// tree; only the link edge is conditional. The cost is a translation layer, which is paid in
// NrdDenoiser.cpp and nowhere else.
//
// WHAT THIS MODULE DOES NOT DO YET, stated here rather than discovered: nothing records these
// dispatches. Two gaps in Aver.RHI stand between this and pixels, both real and both measured --
// see README.md. This module is the half that can be written and tested without them.
#pragma once

#include <aver/core/Types.hpp>
#include <aver/rhi/RHIResources.hpp>

namespace aver::render::nrd {

// The denoisers this engine actually asks for. NRD ships around thirty; enumerating all of them
// here would be a mirror to maintain for no reader. Add one when something calls for it.
enum class DenoiserKind : u32 {
    // AMBIENT/SKY OCCLUSION -- the reason NRD is here at all. Takes a normalised hit distance per
    // pixel and returns a filtered occlusion term. This is the signal measured at 78% of the
    // engine's remaining speckle (third_party/nrd/AVER_README.md).
    ReblurDiffuseOcclusion,
    // Diffuse radiance, for a future GI denoise.
    ReblurDiffuse,
    // Specular radiance, for the reflection path.
    ReblurSpecular,
    // Binary shadow visibility, the SIGMA family. The engine's sun shadow is exactly this shape.
    SigmaShadow,
};

// Which of NRD's two immutable samplers a pipeline expects at a given slot.
enum class SamplerKind : u8 { NearestClamp, LinearClamp };

// A slot's access class. NRD only ever asks for these two.
enum class ResourceClass : u8 { Texture, StorageTexture };

// One shader, as bytecode. Owned by NRD; valid for the lifetime of the Denoiser.
struct Bytecode {
    const u8* data = nullptr;
    u64       size = 0;

    [[nodiscard]] bool valid() const { return data != nullptr && size != 0; }
};

// A contiguous run of same-class slots inside one pipeline's binding table.
struct ResourceRange {
    ResourceClass cls   = ResourceClass::Texture;
    u32           count = 0;
};

// One compute pipeline NRD wants created up front. Both bytecodes are populated only if the build
// embedded them (cmake/AverNRD.cmake decides that per backend); a backend takes the one it can use
// and must check valid() rather than assume.
struct PipelineInfo {
    Bytecode             dxil{};
    Bytecode             spirv{};
    const ResourceRange* ranges     = nullptr;
    u32                  rangeCount = 0;
    bool                 hasConstantData = false;
    // "fileName|macro=value|..." -- NRD's own permutation name. Debug labels and nothing else.
    const char*          debugName = nullptr;
};

// One texture NRD wants allocated. `downsampleFactor` divides the render resolution (1 = full).
//
// `format` is Unknown when the RHI has no equivalent for what NRD asked for. That is a REPORTED
// condition, not an assertion: NRD's format list is wider than this engine's, the gap is a fact
// about Aver.RHI rather than a programming error, and a caller that cannot allocate the pool needs
// to say which format it could not allocate. `nrdFormatName` is that diagnostic.
struct PoolTexture {
    rhi::Format format           = rhi::Format::Unknown;
    u32         downsampleFactor = 1;
    const char* nrdFormatName    = nullptr;
};

// Where NRD's shaders expect their bindings. These come from the compiled bytecode, so a backend
// building a root signature or descriptor set layout must honour them exactly -- they are not
// preferences.
struct BindingModel {
    u32 constantBufferAndSamplersSpace = 0;
    u32 resourcesSpace                 = 0;
    u32 constantBufferRegister         = 0;
    u32 samplersBaseRegister           = 0;
    u32 resourcesBaseRegister          = 0;
    u32 constantBufferMaxDataSize      = 0;   // bytes; upper bound over every dispatch
    SamplerKind samplers[8]{};
    u32         samplerCount = 0;
    const char* entryPoint   = nullptr;       // "NRD_CS_MAIN"
};

// NRD's own name for a texture slot, translated to the engine's vocabulary only where the engine
// has one. Slots outside this list belong to NRD's internal pools and are addressed by index.
enum class SlotRole : u8 {
    // Inputs the engine must supply.
    InViewZ,
    InMotionVectors,
    InNormalRoughness,
    InDiffuseHitDistance,
    // RADIANCE AND HIT DISTANCE IN ONE TEXTURE, which is REBLUR_DIFFUSE's input and a different
    // signal from InDiffuseHitDistance above rather than a superset of it: rgb carries the diffuse
    // radiance arriving at the pixel and a carries the NORMALISED distance it travelled.
    //
    // THE PRODUCING SHADER MUST ENCODE BOTH CHANNELS; NRD DOES NOT DO IT FOR YOU. This comment used
    // to say NRD packed and unpacked the pair itself and that the engine could write the two
    // channels raw, and that was simply wrong -- REBLUR_FrontEnd_PackRadianceAndNormHitDist and
    // REBLUR_BackEnd_UnpackRadianceAndNormHitDist (Shaders/NRD.hlsli) are helpers the PRODUCER and
    // the CONSUMER call, not anything NRD's own passes apply on the way in. Two obligations follow,
    // and neither fails loudly: rgb must be YCoCg, not linear RGB (the pack converts
    // unconditionally, and REBLUR_Config.hlsli sets REBLUR_USE_YCOCG 1, so REBLUR filters chroma in
    // that basis) and must be converted back by whoever reads the output; and a must be a hit
    // distance normalised by REBLUR's OWN divisor, saturate(dist / ((A + |viewZ|*B) * lerp(C,1,smc)))
    // with A/B/C from nrd::ReblurHitDistanceParameters -- not by any distance of the engine's
    // choosing, or REBLUR sizes its blur kernel and its disocclusion logic off a number that means
    // something else. See giRestirIndirect in voxi_restir.hlsli, which does both.
    InDiffuseRadianceHitDistance,
    InPenumbra,
    InTranslucency,
    // The denoised result.
    OutDiffuseHitDistance,
    OutDiffuseRadianceHitDistance,
    OutShadowTranslucency,
    // NRD's own storage. `poolIndex` says which.
    PermanentPool,
    TransientPool,
    // Something this translation does not name yet; `nrdResourceType` carries the raw value.
    Other,
};

// One binding inside a dispatch.
struct SlotBinding {
    ResourceClass cls  = ResourceClass::Texture;
    SlotRole      role = SlotRole::Other;
    u32           poolIndex       = 0;   // index into permanentPool/transientPool when role says so
    u32           nrdResourceType = 0;   // raw nrd::ResourceType, for diagnostics and Other
};

// One compute dispatch to record. Every pointer is owned by the Denoiser and is invalidated by the
// next dispatches() call -- NRD reuses one internal buffer, and this type does not copy it.
struct Dispatch {
    const char*        name        = nullptr;
    const SlotBinding* bindings    = nullptr;
    u32                bindingCount = 0;
    const u8*          constants    = nullptr;
    u32                constantsSize = 0;
    // True when the bytes are identical to the previous dispatch's, so the upload can be skipped.
    bool               constantsUnchanged = false;
    u32                pipelineIndex = 0;
    u32                groupsX = 0;
    u32                groupsY = 0;
};

// Everything a backend needs once, at creation.
struct InstanceLayout {
    const PipelineInfo* pipelines     = nullptr;
    u32                 pipelineCount = 0;
    const PoolTexture*  permanentPool = nullptr;
    u32                 permanentPoolSize = 0;
    const PoolTexture*  transientPool = nullptr;
    u32                 transientPoolSize = 0;
    BindingModel        binding{};
};

// What the camera did this frame. A subset of nrd::CommonSettings -- the fields the engine can
// actually fill. Everything omitted keeps NRD's default, which is the documented behaviour for a
// caller that has no better answer.
//
// MATRICES ARE COLUMN-MAJOR, 16 floats, the same convention nrd::CommonSettings documents. Aver's
// own math type is not used here so that this header stays free of a math dependency the RHI does
// not already impose.
struct FrameSettings {
    f32 viewToClip[16]{};
    f32 viewToClipPrev[16]{};
    f32 worldToView[16]{};
    f32 worldToViewPrev[16]{};
    // Screen-space motion in texels; set z to 0 for a 2D motion buffer.
    f32 motionVectorScale[3] = {1.0f, 1.0f, 0.0f};
    f32 cameraJitter[2]{};
    f32 cameraJitterPrev[2]{};
    u32 resourceWidth = 0, resourceHeight = 0;   // the allocated texture size
    u32 rectWidth = 0, rectHeight = 0;           // the region actually rendered into
    f32 denoisingRange = 500000.0f;
    f32 timeDeltaSeconds = 0.0f;
    u32 frameIndex = 0;
    // Throw the temporal history away -- a teleport, a level load, a resolution change.
    bool resetHistory = false;
    bool motionVectorsAreWorldSpace = false;
};

// The instance. Non-copyable; owns NRD state including its temporal history bookkeeping.
//
// One instance can host several denoisers; `dispatches()` takes the subset to run this frame, so a
// tier that wants ambient occlusion denoised but not shadows pays for one and not the other.
class Denoiser {
public:
    Denoiser() = default;
    ~Denoiser();
    Denoiser(const Denoiser&)            = delete;
    Denoiser& operator=(const Denoiser&) = delete;

    // Creates the NRD instance. `kinds[i]` becomes identifier `i`, which is what dispatches() and
    // the Dispatch stream refer to. Returns false and leaves the object unusable on failure --
    // including when the build has NRD compiled out, so a caller does not need its own #if.
    bool create(const DenoiserKind* kinds, u32 count);
    void destroy();

    [[nodiscard]] bool valid() const { return instance_ != nullptr; }

    // Valid only while the instance is. Pointers inside are owned by NRD or by this object's own
    // translation buffers, which is why this is not const: it fills those buffers.
    [[nodiscard]] InstanceLayout layout();

    // Per frame, before dispatches(). False if NRD rejected the settings.
    bool setFrameSettings(const FrameSettings& s);

    // ---- per-denoiser REBLUR tuning, and the hit-distance half of it is NOT optional ----
    //
    // Until this existed nothing in this seam ever called nrd::SetDenoiserSettings, so every REBLUR
    // denoiser ran on nrd::ReblurSettings' own defaults. For most fields that is the right answer and
    // still is -- these are NVIDIA's tuned numbers and this struct deliberately exposes only the two
    // that the engine knows something NRD cannot.
    //
    // hitDistA IS A LENGTH, AND NRD'S DEFAULT IS IN METRES WHILE THIS ENGINE IS IN CENTIMETRES.
    // REBLUR turns a hit distance into the normalised [0,1] value it filters by dividing by
    // (A + |viewZ| * B) * lerp(C, 1, smc) -- NRD.hlsli's _REBLUR_GetHitDistanceNormalization -- and
    // the producing shader must divide by the SAME thing before it writes the alpha channel. B is a
    // ratio and C is a roughness scale, so both are unit-free; A is 3.0, meaning three METRES, and
    // left at that against a centimetre viewZ it becomes three centimetres. The B term then
    // dominates everywhere past 30cm of depth, so at 2m from the camera the divisor is 23cm and
    // every GI bounce longer than that saturates to 1.0 -- the signal REBLUR sizes its blur kernel
    // and its disocclusion logic from goes flat exactly where the geometry is closest. 300 is the
    // same three metres, said in the engine's units.
    //
    // MIRRORED IN voxi_restir.hlsli (kNrdHitDistA/B/C in giRestirIndirect), because a shader cannot
    // include this header and NRD's front-end pack helper lives in a vendored tree that is
    // deliberately confined to this module. The two must agree; they are commented at each other.
    struct ReblurTuning {
        float hitDistA = 300.0f;   // cm -- NRD's 3.0 metres in engine units
        float hitDistB = 0.1f;     // unit-free viewZ scale
        float hitDistC = 20.0f;    // unit-free roughness scale; collapses to 1 at roughness 1
        // Cheap and unbiased in most cases by NRD's own description, and on by default in
        // nrd::ReblurSettings already -- named here so turning it OFF is a decision someone made
        // rather than a default nobody saw.
        bool  enableAntiFirefly = true;

        // ---- THE THREE KNOBS THAT REMOVE WORK RATHER THAN MERELY RETUNE IT ----
        //
        // Every default below is NRD'S OWN, so a caller that constructs this struct and changes
        // nothing gets exactly the behaviour it had before these fields existed. They are exposed
        // because the denoiser measured at roughly 3x the frame cost of running without it and
        // nothing had ever been tuned -- not because any particular value is known to be better.
        // DO NOT change a default here on reasoning alone; the whole point of the seam is that the
        // trade can be measured.
        //
        // TWO OF THEM GENUINELY SKIP A DISPATCH, which is why they are worth more than the quality
        // dials NRD also offers. Source/Reblur.cpp decides both:
        //     skipPrePass               = diffusePrepassBlurRadius == 0 (and the specular twin)
        //     skipTemporalStabilization = maxStabilizedFrameNum == 0
        // so each is a whole pass off the plan, not a pass that runs and does nothing.
        //
        // AND TURNING STABILIZATION OFF IS WORTH MORE THAN ITS DISPATCH. That pass is the ONLY thing
        // in REBLUR_DIFFUSE that writes IN_MV -- see Recorder::mvScratch_, which exists solely to give
        // NRD a motion-vector texture it may scribble on. With stabilization off, the full-resolution
        // CopyResource that feeds mvScratch_ every frame has nothing left to protect against.
        //
        // WHAT EACH COSTS, so the measurement is not run blind: the pre-pass is a spatial pre-blur
        // that matters most for a NOISY input, and ReSTIR GI is exactly that; temporal stabilization
        // is what suppresses frame-to-frame flicker, though this signal is already resampled
        // temporally by RTXDI before NRD ever sees it, so the two may overlap more than usual here.
        float diffusePrepassBlurRadius = 30.0f;   // 0 skips the Pre-pass dispatch entirely
        // 63 is NRD's REBLUR_MAX_HISTORY_FRAME_NUM, restated because NRD's headers cannot be included
        // outside this module. 0 skips the Temporal stabilization dispatch entirely.
        u32   maxStabilizedFrameNum = 63;
        u32   maxAccumulatedFrameNum = 30;        // history depth; latency/noise, not dispatch count
    };

    // `denoiserIndex` is the index into the kinds array create() was given -- the same index
    // dispatches() selects by. False if NRD rejected the settings or this build has no NRD.
    bool setReblurTuning(u32 denoiserIndex, const ReblurTuning& s);

    // Plan this frame's work for the denoisers named by index into the create() array. `out` and
    // `outCount` are set to a buffer owned by this object, valid until the next call.
    bool dispatches(const u32* denoiserIndices, u32 indexCount,
                    const Dispatch*& out, u32& outCount);

    // True when this build can actually create an instance -- i.e. NRD was compiled in. Lets a
    // caller log the reason for falling back to the hand-written filter without linking NRD.
    [[nodiscard]] static bool available();

    // The version of the HEADER this module was compiled against, e.g. "NRD 4.18.0". nullptr when
    // NRD is not in the build.
    [[nodiscard]] static const char* versionString();

    // The version the LINKED LIBRARY reports at runtime. Separate from versionString() on purpose:
    // the two disagreeing is the signature of a stale static library, which is otherwise entirely
    // silent -- the structs still have the shapes the old header described, and the first symptom
    // is garbage in a constant buffer. NrdLinkTest checks they agree.
    [[nodiscard]] static bool libraryVersion(u32& major, u32& minor, u32& build);

    // The normal and roughness encodings the shaders were BUILT with (cmake/AverNRD.cmake sets
    // them). These matter and are silent when wrong: the G-buffer handed to NRD must be packed to
    // match, and a mismatch produces a plausible-looking wrong image rather than an error.
    [[nodiscard]] static bool encodings(u32& normalEncoding, u32& roughnessEncoding);

private:
    // Translation buffers, refilled by layout()/dispatches(). Defined in the .cpp so NRD's types
    // stay out of every translation unit that includes this header.
    struct Scratch;

    void*    instance_ = nullptr;   // nrd::Instance*
    Scratch* scratch_  = nullptr;
};

}  // namespace aver::render::nrd
