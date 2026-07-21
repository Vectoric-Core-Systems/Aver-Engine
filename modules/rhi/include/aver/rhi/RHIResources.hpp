#pragma once
#include "aver/core/Types.hpp"

#include <string>
// Generic GPU primitives — the foundation layer that render-feature modules build on.
//
// This header is deliberately free of render-feature vocabulary. It knows about textures, buffers,
// shaders, pipelines, barriers and command recording; it knows nothing about global illumination,
// voxels, shadow maps or ray tracing. Feature modules (e.g. Aver.Render.Voxi) own those concepts
// and express them in terms of what is here.
//
// Scope: this abstracts the operations feature modules actually perform, not the whole of D3D12.
// It is expected to grow when a second feature module needs something — that is cheaper and more
// honest than speculatively designing a universal RHI.
//
// BINDING MODEL: explicit descriptor tables, NOT bindless. This keeps Resource Binding Tier 1
// hardware working (Kepler / Maxwell gen 1 / Haswell), which SM 6.6 dynamic resources would drop.
// See docs/MINIMUM_SPECS.md before changing this.
namespace aver::rhi {

// ---------------------------------------------------------------- handles
// All 0 = invalid. Distinct names, identical underlying type — so they can NEVER be distinguished
// by overload resolution. Every API below that takes one says which kind it wants in its name.
using MeshHandle       = u32;
using LineHandle       = u32;
using TextureHandle    = u32;
using BufferHandle     = u32;
using ShaderHandle     = u32;
using PipelineHandle   = u32;
using BindingSetHandle = u32;
using BlasHandle       = u32;
using TlasHandle       = u32;

// ---------------------------------------------------------------- formats & resources

enum class Format : u8 {
    Unknown,
    RGBA8Unorm,
    RGBA8UnormSrgb,  // the hardware does the sRGB decode on every tap; base colour wants this
    RG8Unorm,
    R8Unorm,
    RGBA16F,      // radiance volumes, HDR targets
    R32Float,
    R32Uint,      // the only typed format D3D12 guarantees UAV atomics on
    D32Float,     // depth-stencil view format
    R32Typeless,  // aliased depth: DSV sees D32Float, SRV sees R32Float
    // Block-compressed, 4x4 texel blocks. Nothing emits these yet, but they are cheap to add now and
    // expensive to retrofit once a material file carries a baked format field. BC5 is the correct
    // tangent-space normal format (two channels, no sRGB variant); BC7 is universal on FL11_0.
    BC1Unorm,
    BC1UnormSrgb,
    BC3Unorm,
    BC3UnormSrgb,
    BC5Unorm,
    BC7Unorm,
    BC7UnormSrgb,
};

// A block format's extents are counted in 4x4 blocks, not texels, everywhere a pitch is computed —
// which is why this is a question of its own rather than a zero returned from a bytes-per-texel
// helper that a caller could multiply by anyway.
inline bool isBlockFormat(Format f) {
    return f >= Format::BC1Unorm && f <= Format::BC7UnormSrgb;
}

enum class TextureDim : u8 { Tex2D, Tex3D };

enum class ResourceBind : u32 {
    None            = 0,
    ShaderResource  = 1u << 0,
    UnorderedAccess = 1u << 1,
    RenderTarget    = 1u << 2,
    DepthStencil    = 1u << 3,
};
inline ResourceBind operator|(ResourceBind a, ResourceBind b) { return static_cast<ResourceBind>(static_cast<u32>(a) | static_cast<u32>(b)); }
inline bool         any(ResourceBind v, ResourceBind bit)     { return (static_cast<u32>(v) & static_cast<u32>(bit)) != 0; }

// THE MODULE OWNS RESOURCE STATE. No IRenderContext method transitions implicitly: setRenderTargets
// and clearDepth require the caller to have already placed the target. The backend performs no
// promotion or decay on module-created resources. A resource is exactly in the state its desc or
// its last barrier named, and that state CARRIES ACROSS FRAMES.
enum class ResourceState : u8 {
    Common,
    ShaderResource,          // readable by the pixel stage
    NonPixelShaderResource,  // readable by compute / non-pixel stages
    UnorderedAccess,
    RenderTarget,
    DepthWrite,
    CopySource,
    CopyDest,
    // TERMINAL: set at creation, never a valid barrier argument in either direction. The underlying
    // API rejects transitioning into or out of it; acceleration structures live here for life.
    AccelerationStructure,
};

struct TextureDesc {
    TextureDim   dim    = TextureDim::Tex2D;
    u32          width  = 1;
    u32          height = 1;
    u32          depth  = 1;    // Tex3D only
    u32          mips   = 1;    // 0 = full chain; query the resolved count with textureInfo()
    Format       format = Format::RGBA8Unorm;
    ResourceBind bind   = ResourceBind::ShaderResource;
    // The state the resource is created in. Pick the state the first barrier of frame 0 will
    // transition FROM, or that first barrier is a lie and the debug layer will say so.
    ResourceState initialState = ResourceState::Common;
    // A depth/render target created without a clear value that matches the value actually cleared
    // to loses fast clear and warns every frame.
    bool        hasClearValue = false;
    f32         clearDepth    = 1.0f;
    f32         clearColor[4] = {};
    const char* debugName = nullptr;

    // ---- initial contents ----
    // One CPU pointer per subresource (== per mip, since this interface exposes only single-slice
    // textures). Supplying it does NOT change the contract above: the backend creates the resource
    // in whatever copy state it needs, uploads, transitions to `initialState`, and seeds its state
    // tracking from `initialState`. So a module never sees, names, or has to reason about a copy
    // state, and the first barrier it writes is still checked against `initialState`.
    //
    // Carried on the desc rather than offered as a separate updateTexture() precisely to keep that
    // invariant: a post-creation upload would have to move the resource out of `initialState` and
    // back, which is a window in which the tracker and the resource disagree.
    const void* const* initialData = nullptr;
    u32                initialDataCount = 0;   // subresources supplied; the rest are left undefined
    // Source bytes per row of subresource 0. Zero means tightly packed (width * texel size). The
    // DESTINATION pitch is the hardware's and is generally larger, so the backend copies row by row.
    // Subresources past the first are always taken as tightly packed for their own mip extent —
    // which is what every decoder and mip generator produces.
    u32                initialRowPitch = 0;
};

enum class BufferKind : u8 {
    Default,         // GPU-local
    Upload,          // CPU-writable, GPU-readable
    AccelStructure,  // GPU-local, created in the terminal AccelerationStructure state
};

struct BufferDesc {
    u64           bytes = 0;
    BufferKind    kind  = BufferKind::Default;
    bool          allowUnorderedAccess = false;
    ResourceState initialState = ResourceState::Common;
    const char*   debugName = nullptr;
};

// ---------------------------------------------------------------- samplers
// Static samplers declared on the pipeline, NOT as binding-set slots — the Tier-1-friendly choice.

enum class Filter : u8 {
    Point,
    Linear,
    // Depth comparison sampling (SampleCmpLevelZero). Substituting Linear here compiles, runs, and
    // returns raw depth per tap — it reads as "shadows too weak", never as an error.
    ComparisonLinear,
};
enum class AddressMode : u8 { Clamp, Wrap };
enum class CompareOp : u8 { Never, Less, LessEqual, Always };

struct SamplerDesc {
    Filter      filter  = Filter::Linear;
    AddressMode address = AddressMode::Clamp;
    CompareOp   compare = CompareOp::Never;   // ComparisonLinear only
    // Leave unclamped for anything that samples a mip chain at a fractional level: clamping this
    // makes every level past the clamp unreachable.
    f32         maxLod  = 3.402823466e+38f;
};

// ---------------------------------------------------------------- shaders & pipelines

enum class ShaderStage : u8 { Vertex, Pixel, Geometry, Compute, Mesh };

struct ShaderDesc {
    const char* source = nullptr;   // HLSL text; the feature module owns its own shader source
    // Prepended verbatim before `source`. Use sharedShaderPrelude() so the shared cbuffer layouts
    // and helpers have exactly ONE owner — duplicating them across modules is a silent cross-module
    // ABI where reordering a field corrupts the other side with no compile error anywhere.
    const char* prelude = nullptr;
    const char* entry   = nullptr;
    ShaderStage stage   = ShaderStage::Vertex;
    // Minimum shader model as major*10+minor (60 = SM 6.0, 65 = SM 6.5). Asking for more than the
    // device reports yields an invalid handle rather than a hard failure.
    u32         minShaderModel = 60;
    const char* defines = nullptr;  // semicolon-separated, e.g. "AVER_MS=1;AVER_RT=1"
};

enum class CullMode : u8 { None, Back, Front };
enum class FillMode : u8 { Solid, Wireframe };

struct DepthState {
    bool      test  = false;
    bool      write = false;
    CompareOp op    = CompareOp::Less;
};

// Logical constant slots, mapping one-to-one onto b0..b(n-1). The count reaches
// kFeatureFrameConstantRegister INCLUSIVE: a feature declares its frame constants at that reserved
// register, and a layout that could not describe it would leave every pipeline reading that cbuffer
// impossible to create — the shader names a register the root signature never declared.
constexpr u32 kMaxConstantSlots = 5;

// Every pipeline DECLARES its binding layout. The backend caches root signatures keyed by that
// layout, so pipelines declaring identical shapes share one and switching between them does not
// invalidate bindings — which is what lets a feature's binding set stay live across draws the
// backend records itself (mandatory at Tier 1, where every declared table must be bound).
struct PipelineLayout {
    u32 srvCount = 0;             // table 0: t0..t(srvCount-1)
    u32 uavCount = 0;             // table 0: u0..u(uavCount-1)
    // A SECOND declarable table, based immediately above the first: t(srvCount).. and u(uavCount)..
    // Zero counts declare no second table at all, which is what every existing pipeline gets.
    //
    // Two tables rather than one wider one because the two have different LIFETIMES. Table 0 holds
    // descriptors a feature owns and reallocates (the voxel volume is recreated on a resolution
    // change); table 1 is swapped per draw. Folding them together would mean either rewriting every
    // per-draw set whenever the feature's resources move — while a frame in flight may still be
    // reading them — or rewriting the feature's descriptors per draw. Both are the hazard the TLAS
    // creation comment in VoxiRenderer already documents.
    //
    // Deliberately named "table 1" and not after any consumer: the RHI must not learn the word
    // "material" (see the header preamble).
    u32 srvCount1 = 0;
    u32 uavCount1 = 0;
    // Logical constant slot k maps to register b(k). A non-zero word count makes it ROOT CONSTANTS,
    // written with setConstants; zero makes it a ROOT CBV, written with setConstantBuffer. A slot
    // cannot be both, and setConstants on a CBV slot (or the reverse) is a binding error.
    //
    // Slot 0 is RESERVED for the engine per-frame block (kEngineFrameConstantRegister). It is a root
    // CBV that the BACKEND binds on every setPipeline, because binding a root signature discards
    // every root argument and a feature has no way to supply the engine's own frame data. Leave
    // constantDwords[0] at zero; a feature must never be able to observe b0 unbound.
    u32 constantDwords[kMaxConstantSlots] = {};
    SamplerDesc samplers[4] = {};
    u32 samplerCount = 0;         // s0..s(n-1)
};

// How many declarable descriptor tables a layout has. Tables are addressed by index at
// setBindingSet, so this is the bound on that argument.
constexpr u32 kBindingTableCount = 2;

// The register a layout's SRV/UAV declarations run out to, ACROSS BOTH TABLES. Anything the backend
// reserves above a layout must be placed with these, never with srvCount alone — table 1 now
// occupies exactly the registers a lone srvCount would have pointed at.
inline u32 declaredSrvCount(const PipelineLayout& l) { return l.srvCount + l.srvCount1; }
inline u32 declaredUavCount(const PipelineLayout& l) { return l.uavCount + l.uavCount1; }

struct GraphicsPipelineDesc {
    // Either (vs[,gs]) or ms must be set. A mesh pipeline has no input assembler; the backend picks
    // the matching root-signature flavour automatically.
    ShaderHandle vs = 0, gs = 0, ms = 0, ps = 0;

    PipelineLayout layout{};

    FillMode fill = FillMode::Solid;
    CullMode cull = CullMode::None;
    bool     depthClip = true;
    // Widens rasterisation so thin geometry still covers a pixel. Silently ignored where
    // unsupported — check DeviceCaps::conservativeRaster if it matters.
    bool     conservativeRaster = false;
    // Depth bias is per-triangle and slope-dependent; a shader-side bias cannot substitute for it.
    // Constant bias is meaningless against a float depth buffer, so slope-scaled carries the load.
    f32      depthBias = 0.0f;
    f32      slopeScaledDepthBias = 0.0f;

    DepthState depth{};

    // Zero render targets is legal and meaningful: a pass whose only output is a UAV write.
    // Blending is always off and all colour channels are written.
    u32    renderTargetCount = 0;
    Format renderTargets[4]  = {};
    Format depthFormat       = Format::Unknown;
    u32    sampleCount       = 1;
};

struct ComputePipelineDesc {
    ShaderHandle   cs = 0;
    PipelineLayout layout{};
};

// ---------------------------------------------------------------- binding sets
//
// A binding set is a contiguous run of shader-resource slots plus a contiguous run of
// unordered-access slots. Tier 1 requires every declared slot to hold a valid descriptor, so the
// backend null-fills any left unset with a view OF THE CORRECT DIMENSION. All sets suballocate from
// one device-owned shader-visible heap, so binding one never costs a heap switch.
// The KIND of view a slot holds. Null-filling needs this: a Tier 1 device reading a null descriptor
// whose dimension does not match what the shader declared is undefined behaviour, not a warning —
// and it cannot be reproduced on hardware that binds by descriptor heap. Counts alone are not
// enough information for the backend to fill a slot correctly.
enum class SlotKind : u8 {
    Texture2D,
    Texture3D,
    AccelerationStructure,   // SRV slots only; bound with a null resource and an address
};

// Slots per range. Counts above this cannot declare a kind, so the backend could only guess exactly
// the thing SlotKind exists to stop it guessing — the limit is therefore enforced, not advisory.
// 16, not 8: a material set is roughly seven textures before anything exotic, and Resource Binding
// Tier 1 permits 128 SRVs in a table, so the headroom is free.
constexpr u32 kMaxBindingSlots = 16;

struct BindingSetDesc {
    u32 srvCount = 0;            // must be <= kMaxBindingSlots
    u32 uavCount = 0;            // must be <= kMaxBindingSlots
    SlotKind srvKinds[kMaxBindingSlots] = {};   // default-initialises to Texture2D
    SlotKind uavKinds[kMaxBindingSlots] = {};
    // The first shader register this set is meant to cover. A set is a bare run of descriptors and
    // carries no registers of its own, so these are NOT used to build anything — they are recorded
    // so the backend can check, when the set is bound at a table index, that it was built for the
    // run that table actually covers. Binding a table-1 set at table 0 is otherwise completely
    // silent: the descriptors are valid, the count fits, and the shader simply reads the wrong
    // textures.
    u32 srvBaseRegister = 0;
    u32 uavBaseRegister = 0;
};

// Bind every mip of a texture as one view. Invalid for a UAV, which always targets one level.
constexpr u32 kAllMips = 0xFFFFFFFFu;

// A whole-resource transition requires EVERY subresource to already be in `from`. Mixing whole and
// per-subresource transitions without first reconciling every subresource is invalid; the backend
// validates `from` against its own tracking in debug builds and reports through the engine log.
// For the single-slice Tex2D/Tex3D this interface supports, `subresource` is the mip index.
constexpr u32 kAllSubresources = 0xFFFFFFFFu;

// Triangles per mesh-shader thread group. One owner, because the group count at every dispatch site
// is ceil(triangleCount / this) and the shader's own [numthreads] must agree.
constexpr u32 kMeshShaderTrisPerGroup = 64;

// ---------------------------------------------------------------- reserved registers
//
// dispatchMeshFor() binds geometry the mesh shader pulls itself. These registers are RESERVED by the
// backend; a feature module must not declare anything at them. The declarations that match live in
// sharedShaderPrelude(), so both sides are anchored to one owner rather than to a convention nobody
// enforces.
//   - vertices: t(declaredSrvCount), indices: t(declaredSrvCount + 1)  — placed after the declared
//     SRVs of BOTH tables so they can never collide with a layout however many SRVs it declares.
//     Because those two DEPEND on the layout, the prelude cannot write them as literals: it takes
//     them as -D macros, which meshGeometryDefines() below computes from the very same fields the
//     root signature uses.
//   - triangle count: 4 root constants at b(kMeshGeometryConstantRegister).
// Logical constant slots 0..4 map to b0..b4, the top one being a feature's own frame constants, so
// the mesh geometry block sits above both.
constexpr u32 kMeshGeometryConstantRegister = 5;
constexpr u32 kFeatureFrameConstantRegister = 4;
// b1 is the per-draw block the shared prelude declares: the transform, then the shading constants
// each draw carries. ONE owner for the size, because a root signature and a shader that disagree
// about it is not a validation error — it is a GPU-side read of whatever the last draw left behind,
// which looks like a plausible image with the wrong parameters. Every pipeline that draws geometry
// declares exactly this many dwords at slot 1, and every draw writes exactly this many.
constexpr u32 kObjectConstantRegister = 1;
constexpr u32 kObjectConstantDwords = 32;
// b2 is the constant block that travels WITH binding table 1, written as a root CBV rather than
// root constants: the two are set together by setDrawBinding and are meaningless apart, and a root
// CBV lets the block grow without every layout having to restate its size. Kept a reserved register
// of its own so a feature declaring its own constants cannot land on it by accident.
constexpr u32 kDrawConstantRegister = 2;
// Largest b2 block setDrawBinding will carry. The sticky state is COPIED, so this bounds a fixed
// buffer rather than a heap allocation on a per-draw path; a longer block is rejected and reported
// rather than truncated, because a truncated constant block reads as plausible wrong shading.
constexpr u32 kMaxDrawConstantBytes = 256;
// b0 is the engine's PerFrame block (gViewProj, gCamPos, gLightDir, gLightColor, gAmbient, gSky*).
// Owned and bound by the backend on every pipeline bind — see PipelineLayout::constantDwords.
constexpr u32 kEngineFrameConstantRegister = 0;
static_assert(kFeatureFrameConstantRegister < kMaxConstantSlots,
              "a feature must be able to DECLARE the register it is told to put frame constants at");
static_assert(kMeshGeometryConstantRegister >= kMaxConstantSlots,
              "the backend's mesh geometry constants must sit above every declarable slot");

// ---------------------------------------------------------------- acceleration structures
//
// Inline RayQuery (DXR 1.1) still needs built acceleration structures. The backend owns sizing,
// scratch lifetime and instance packing: result/scratch sizes come from a driver-dependent prebuild
// query, scratch must outlive the command list that consumed it, and the instance descriptor is a
// backend-specific bit layout that Vulkan packs differently.
struct TlasInstance {
    f32         world[16];   // ENGINE convention: row-major/row-vector, cm, +Z up. The backend does
                             // any transpose the underlying API needs — do not pre-transpose.
    u32         mask = 0xFF;
    BlasHandle  blas = 0;
};

// ---------------------------------------------------------------- resource factory
//
// Creation and destruction. Reached with IDevice::resources(), which returns nullptr on backends
// without GPU support so a feature module can decline to initialise instead of failing the engine.
class IResourceFactory {
public:
    virtual ~IResourceFactory() = default;

    virtual TextureHandle    createTexture(const TextureDesc& d) = 0;
    virtual BufferHandle     createBuffer(const BufferDesc& d) = 0;
    virtual ShaderHandle     createShader(const ShaderDesc& d) = 0;
    virtual PipelineHandle   createGraphicsPipeline(const GraphicsPipelineDesc& d) = 0;
    virtual PipelineHandle   createComputePipeline(const ComputePipelineDesc& d) = 0;
    virtual BindingSetHandle createBindingSet(const BindingSetDesc& d) = 0;
    virtual BlasHandle       createBlas(MeshHandle mesh) = 0;
    virtual TlasHandle       createTlas(u32 maxInstances) = 0;

    // Destruction is DEFERRED BY CONTRACT: the backend retires the resource once the GPU has passed
    // every frame that could still reference it. Safe to call mid-frame.
    virtual void destroyTexture(TextureHandle h) = 0;
    virtual void destroyBuffer(BufferHandle h) = 0;
    virtual void destroyShader(ShaderHandle h) = 0;
    virtual void destroyPipeline(PipelineHandle h) = 0;
    virtual void destroyBindingSet(BindingSetHandle h) = 0;

    // Populate a binding set. Slots left unset are null-filled.
    virtual void setSrv(BindingSetHandle set, u32 slot, TextureHandle t, u32 mip = kAllMips) = 0;
    virtual void setUav(BindingSetHandle set, u32 slot, TextureHandle t, u32 mip) = 0;
    // Distinct name, not an overload: every handle type is the same underlying integer.
    virtual void setSrvTlas(BindingSetHandle set, u32 slot, TlasHandle tlas) = 0;

    // Resolved description, with `mips` filled in when the desc asked for a full chain. The module
    // must not recompute the mip count: it drives the descriptor loop, the per-mip barrier sequence
    // and the dispatch loop, and an off-by-one makes the closing whole-resource transition illegal.
    virtual bool textureInfo(TextureHandle h, TextureDesc& out) const = 0;

    // Block until the GPU is idle. Required before destroying and recreating a resource that
    // binding sets still point at.
    virtual void waitIdle() = 0;
};

// ---------------------------------------------------------------- command recording
//
// A feature module records into the frame's command stream through this interface rather than
// touching a backend command list. It is handed one at defined points in the frame (see
// IRenderFeature), so modules never own submission, allocators or fences.
class IRenderContext {
public:
    virtual ~IRenderContext() = default;

    // setPipeline must precede setConstants / setBindingSet: it selects the pipeline's declared
    // layout AND whether the graphics or compute binding point is used. Those are wholly
    // independent state, and getting it backwards is silent corruption, not an error.
    virtual void setPipeline(PipelineHandle p) = 0;

    virtual void setViewport(u32 x, u32 y, u32 w, u32 h) = 0;
    // The scissor is mandatory and is NOT derived from the viewport. A pass that binds zero render
    // targets has no extent to fall back on, so it must set this or inherit whatever rectangle was
    // last used — which silently clips the pass.
    virtual void setScissor(u32 x, u32 y, u32 w, u32 h) = 0;

    // The caller must already have transitioned these into RenderTarget / DepthWrite.
    virtual void setRenderTargets(const TextureHandle* colors, u32 count, TextureHandle depth) = 0;
    virtual void clearDepth(TextureHandle depth, f32 value) = 0;

    // Bind a set to one of the pipeline's declared tables. Tier 1 requires EVERY declared table to
    // hold valid descriptors on every pass, so a pipeline declaring table 1 must have both bound
    // before it draws — the backend cannot invent the second one.
    virtual void setBindingSet(BindingSetHandle set, u32 table = 0) = 0;
    // Root constants at a logical slot. Always overwrites the whole declared block, so a partial
    // write can never inherit the previous pass's values.
    virtual void setConstants(u32 slot, const void* data, u32 dwords) = 0;
    // Transient per-frame constants: suballocated from the frame's upload ring and bound as a root
    // CBV. Backend-owned memory, fresh every call — a module cannot get the N-buffering wrong, and
    // republishing mid-frame (once a pass has computed its own matrices) is free.
    virtual void setConstantBuffer(u32 slot, const void* data, u32 bytes) = 0;

    // Binding table 1 plus its b2 constant block, as ONE piece of sticky state consumed by every
    // subsequent drawMesh / dispatchMeshFor. Sticky rather than an argument to drawMesh because the
    // draw entry points are also what IRenderFeature::submitDraw mirrors, and widening those would
    // change every feature that replays geometry — setWireframe / setLineDepth / setViewportRect are
    // the established idiom for exactly this.
    //
    // Silently ignored by a pipeline whose layout declares neither, which is what lets a pass that
    // has no use for per-draw resources leave the state alone instead of clearing it.
    virtual void setDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) {
        (void)set; (void)constants; (void)bytes;
    }

    // Draws. Geometry is addressed by MeshHandle: the backend resolves vertex/index buffers, index
    // count, triangle count, mesh-shader group count and root SRVs internally.
    virtual void drawMesh(MeshHandle mesh) = 0;
    virtual void dispatchMeshFor(MeshHandle mesh) = 0;
    virtual void dispatch(u32 gx, u32 gy, u32 gz) = 0;
    // 3-vertex fullscreen triangle. The pipeline supplies its OWN vertex shader generating the
    // triangle from SV_VertexID — no backend vertex shader is implied.
    virtual void drawFullscreen() = 0;

    // Acceleration-structure builds.
    virtual void buildBlas(BlasHandle blas) = 0;
    virtual void buildTlas(TlasHandle tlas, const TlasInstance* instances, u32 count) = 0;

    // Synchronisation. Textures and buffers have distinct names because the handle types are
    // indistinguishable to the compiler.
    virtual void textureBarrier(TextureHandle t, ResourceState from, ResourceState to,
                                u32 subresource = kAllSubresources) = 0;
    virtual void bufferBarrier(BufferHandle b, ResourceState from, ResourceState to) = 0;
    virtual void uavBarrierTexture(TextureHandle t) = 0;
    // The only synchronisation between an acceleration-structure write and the RayQuery reads that
    // consume it later in the same command list.
    virtual void uavBarrierBuffer(BufferHandle b) = 0;

    // Debug markers. Without these a feature's whole pass chain is attributed to the backend in
    // PIX / RenderDoc. No correctness impact; annoying to retrofit.
    virtual void pushMarker(const char* label) { (void)label; }
    virtual void popMarker() {}
};

// ---------------------------------------------------------------- feature modules
//
// The hook a render-feature module implements. The backend calls these at fixed points; it does not
// know what the feature does, only when to call it. Registration is NON-owning.
class IRenderFeature {
public:
    virtual ~IRenderFeature() = default;
    virtual const char* name() const = 0;

    // Scene submission, forwarded by the backend so a feature can replay geometry into its own
    // passes (shadow maps, volume rasterisation, acceleration structures).
    virtual void beginScene() {}
    virtual void submitDraw(MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                            f32 metallic, f32 roughness) {
        (void)mesh; (void)world; (void)baseColor; (void)metallic; (void)roughness;
    }

    // Before the scene's render targets are bound — for passes that own their own targets.
    virtual void prePass(IRenderContext& ctx) { (void)ctx; }

    // Whether the scene must be drawn with this feature's pipelines. wireframe is passed because it
    // has no mesh-shader variant, and a feature may fall back when a variant failed to build.
    virtual bool           overridesScenePipeline() const { return false; }
    virtual PipelineHandle scenePipeline(bool meshShaders, bool wireframe) const {
        (void)meshShaders; (void)wireframe; return 0;
    }
    // Bindings and constants the feature's scene shaders need, applied by the BACKEND to every
    // scene draw it records — including draws that read none of them, which Tier 1 requires anyway.
    virtual BindingSetHandle sceneBindingSet() const { return 0; }
    virtual bool sceneConstants(const void** data, u32* bytes) const { (void)data; (void)bytes; return false; }

    // Replace the scene entirely, after the colour target is bound (e.g. a debug visualisation).
    // Suppression must cover line/overlay draws too, or they float over the replacement.
    virtual bool suppressesScene() const { return false; }
    virtual void scenePass(IRenderContext& ctx) { (void)ctx; }

    // Pipelines that bake sample count or target formats must be rebuilt when those change.
    // setSampleCount can only rebuild the ones the backend owns.
    virtual void onRenderTargetsChanged(u32 sampleCount, Format color, Format depth) {
        (void)sampleCount; (void)color; (void)depth;
    }
};

// The shared HLSL prelude: cbuffer layouts, vertex structures and helpers used by BOTH the
// backend's own shaders and feature modules. One owner, so the two can never drift.
const char* sharedShaderPrelude();

// The -D list pinning the prelude's reserved mesh-geometry registers to a layout's own SRV count,
// e.g. "AVER_MS_VTX_REG=3;AVER_MS_IDX_REG=4" for a layout declaring three SRVs. Semicolon-separated,
// so it appends straight onto ShaderDesc::defines.
//
// EVERY mesh-shader compile must pass this, and must pass it for the SAME layout the pipeline
// declares. The prelude #errors without it rather than guessing, because the failure it prevents is
// invisible: the shader would ask for a register the root signature has since moved a material SRV
// into, which at best CreateRootSignature rejects and at worst reads a texture as a vertex buffer —
// no compile error, no debug-layer message, and only on the mesh-shader path.
std::string meshGeometryDefines(const PipelineLayout& layout);

} // namespace aver::rhi
