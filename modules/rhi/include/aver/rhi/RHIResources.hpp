// Generic GPU primitives — textures, buffers, shaders, pipelines, binding sets, barriers and
// command recording — that render-feature modules build on. Carries no feature vocabulary.
// BINDING MODEL: explicit descriptor tables, NOT bindless; see docs/MINIMUM_SPECS.md.
#pragma once
#include "aver/core/Types.hpp"

#include <string>
namespace aver::rhi {

// ---------------------------------------------------------------- handles
// All 0 = invalid. Distinct names, identical underlying type, so overload resolution can never
// tell them apart; every API below names the kind it wants.
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

// Pixel and vertex-attribute formats this RHI exposes.
enum class Format : u8 {
    Unknown,
    RGBA8Unorm,
    RGBA8UnormSrgb,  // the hardware does the sRGB decode on every tap
    RG8Unorm,
    R8Unorm,
    RGBA16F,      // radiance volumes, HDR targets
    R32Float,
    RG32Float,    // two floats; a vertex attribute, not a texture format here
    R32Uint,      // the only typed format D3D12 guarantees UAV atomics on
    D32Float,     // depth-stencil view format
    R32Typeless,  // aliased depth: DSV sees D32Float, SRV sees R32Float
    // Block-compressed, 4x4 texel blocks.
    BC1Unorm,
    BC1UnormSrgb,
    BC3Unorm,
    BC3UnormSrgb,
    BC5Unorm,
    BC7Unorm,
    BC7UnormSrgb,
};

// True for block-compressed formats, whose extents are counted in 4x4 blocks and not texels.
inline bool isBlockFormat(Format f) {
    return f >= Format::BC1Unorm && f <= Format::BC7UnormSrgb;
}

// Texture dimensionality.
enum class TextureDim : u8 { Tex2D, Tex3D };

// What a resource may be bound as. Flags.
enum class ResourceBind : u32 {
    None            = 0,
    ShaderResource  = 1u << 0,
    UnorderedAccess = 1u << 1,
    RenderTarget    = 1u << 2,
    DepthStencil    = 1u << 3,
};
inline ResourceBind operator|(ResourceBind a, ResourceBind b) { return static_cast<ResourceBind>(static_cast<u32>(a) | static_cast<u32>(b)); }
inline bool         any(ResourceBind v, ResourceBind bit)     { return (static_cast<u32>(v) & static_cast<u32>(bit)) != 0; }

// Resource states. THE MODULE OWNS RESOURCE STATE: nothing transitions implicitly, no promotion or
// decay is performed, and a resource's state CARRIES ACROSS FRAMES.
enum class ResourceState : u8 {
    Common,
    ShaderResource,          // readable by the pixel stage
    NonPixelShaderResource,  // readable by compute / non-pixel stages
    UnorderedAccess,
    RenderTarget,
    DepthWrite,
    CopySource,
    CopyDest,
    // TERMINAL: set at creation, never a valid barrier argument in either direction.
    AccelerationStructure,
};

// How to create a texture, including its initial state and optional initial contents.
struct TextureDesc {
    TextureDim   dim    = TextureDim::Tex2D;
    u32          width  = 1;
    u32          height = 1;
    u32          depth  = 1;    // Tex3D only
    u32          mips   = 1;    // 0 = full chain; query the resolved count with textureInfo()
    Format       format = Format::RGBA8Unorm;
    ResourceBind bind   = ResourceBind::ShaderResource;
    // The state the resource is created in, and the state the first barrier must transition FROM.
    ResourceState initialState = ResourceState::Common;
    bool        hasClearValue = false;
    f32         clearDepth    = 1.0f;
    f32         clearColor[4] = {};
    const char* debugName = nullptr;

    // ---- initial contents ----
    // One CPU pointer per subresource (== per mip). The backend uploads and then leaves the
    // resource in `initialState`, so a module never names a copy state.
    const void* const* initialData = nullptr;
    u32                initialDataCount = 0;   // subresources supplied; the rest are left undefined
    // Source bytes per row of subresource 0. Zero means tightly packed (width * texel size).
    // Subresources past the first are always taken as tightly packed for their own mip extent.
    u32                initialRowPitch = 0;
};

// Where a buffer's memory lives.
enum class BufferKind : u8 {
    Default,         // GPU-local
    Upload,          // CPU-writable, GPU-readable
    AccelStructure,  // GPU-local, created in the terminal AccelerationStructure state
};

// How to create a buffer. There is deliberately no initial state: a buffer is always Common at the
// top of a frame, so the first barrier a module writes must claim Common as its `from`.
struct BufferDesc {
    u64           bytes = 0;
    BufferKind    kind  = BufferKind::Default;
    bool          allowUnorderedAccess = false;
    const char*   debugName = nullptr;
};

// ---------------------------------------------------------------- samplers
// Static samplers declared on the pipeline, NOT as binding-set slots — the Tier-1-friendly choice.

// Texture filtering mode.
enum class Filter : u8 {
    Point,
    Linear,
    ComparisonLinear,   // depth comparison sampling (SampleCmpLevelZero)
    Anisotropic,        // trilinear plus anisotropy; reads maxAnisotropy
};
// How coordinates outside [0,1] are handled.
enum class AddressMode : u8 { Clamp, Wrap };
// Comparison used by depth tests and comparison samplers.
enum class CompareOp : u8 { Never, Less, LessEqual, Always };

// One static sampler on a pipeline.
struct SamplerDesc {
    Filter      filter  = Filter::Linear;
    AddressMode address = AddressMode::Clamp;
    CompareOp   compare = CompareOp::Never;   // ComparisonLinear only
    f32         maxLod  = 3.402823466e+38f;   // clamping it makes higher mips unreachable
    u8          maxAnisotropy = 1;            // Filter::Anisotropic only; D3D12 rejects zero
};

// ---------------------------------------------------------------- shaders & pipelines

// Which programmable stage a shader is compiled for.
enum class ShaderStage : u8 { Vertex, Pixel, Geometry, Compute, Mesh };

// One HLSL shader to compile.
struct ShaderDesc {
    const char* source = nullptr;   // HLSL text; the feature module owns its own shader source
    // Prepended verbatim before `source`. Use sharedShaderPrelude() so the shared cbuffer layouts
    // have exactly ONE owner.
    const char* prelude = nullptr;
    const char* entry   = nullptr;
    ShaderStage stage   = ShaderStage::Vertex;
    // Minimum shader model as major*10+minor (60 = SM 6.0, 65 = SM 6.5). Asking for more than the
    // device reports yields an invalid handle rather than a hard failure.
    u32         minShaderModel = 60;
    const char* defines = nullptr;  // semicolon-separated, e.g. "AVER_MS=1;AVER_RT=1"
};

// Which triangle facing is discarded.
enum class CullMode : u8 { None, Back, Front };

// How a pipeline's colour output combines with what is already in the render target.
enum class BlendMode : u8 {
    Opaque,        // no blending, all channels written
    // src.rgb * src.a + dst.rgb * (1 - src.a), and alpha as src.a + dst.a * (1 - src.a).
    AlphaBlend,
    // src.rgb + dst.rgb * (1 - src.a), for colour that ALREADY has its alpha folded in. Not
    // interchangeable with AlphaBlend, which would apply alpha a second time.
    PremultipliedAlpha,
    Additive,      // src.rgb + dst.rgb, for light, fire, and anything that only ever brightens
};
// Whether triangles are filled or drawn as wireframe.
enum class FillMode : u8 { Solid, Wireframe };

// Depth test and write configuration for a pipeline.
struct DepthState {
    bool      test  = false;
    bool      write = false;
    CompareOp op    = CompareOp::Less;
};

// Logical constant slots, mapping one-to-one onto b0..b(n-1). Reaches
// kFeatureFrameConstantRegister inclusive, so a feature can declare its frame constants.
constexpr u32 kMaxConstantSlots = 5;

// The binding layout a pipeline declares. The backend caches root signatures keyed by it, so
// pipelines of identical shape share one and switching between them does not invalidate bindings.
struct PipelineLayout {
    u32 srvCount = 0;             // table 0: t0..t(srvCount-1)
    u32 uavCount = 0;             // table 0: u0..u(uavCount-1)
    // A SECOND declarable table, based immediately above the first: t(srvCount).. and u(uavCount)..
    // Zero counts declare no second table at all. Table 0 holds descriptors a feature owns and
    // reallocates; table 1 is swapped per draw.
    u32 srvCount1 = 0;
    u32 uavCount1 = 0;
    // Logical constant slot k maps to register b(k). A non-zero word count makes it ROOT CONSTANTS,
    // written with setConstants; zero makes it a ROOT CBV, written with setConstantBuffer. A slot
    // cannot be both.
    //
    // Slot 0 is RESERVED for the engine per-frame block (kEngineFrameConstantRegister), which the
    // backend binds on every setPipeline. Leave constantDwords[0] at zero.
    u32 constantDwords[kMaxConstantSlots] = {};
    SamplerDesc samplers[4] = {};
    u32 samplerCount = 0;         // s0..s(n-1)
};

// How many declarable descriptor tables a layout has, and so the bound on setBindingSet's index.
constexpr u32 kBindingTableCount = 2;

// The register a layout's SRV declarations run out to, ACROSS BOTH TABLES. Anything the backend
// reserves above a layout must be placed with this, never with srvCount alone.
inline u32 declaredSrvCount(const PipelineLayout& l) { return l.srvCount + l.srvCount1; }
// The register a layout's UAV declarations run out to, across both tables.
inline u32 declaredUavCount(const PipelineLayout& l) { return l.uavCount + l.uavCount1; }

// ---------------------------------------------------------------- vertex layout
//
// For pipelines that draw geometry the CALLER owns. This does not replace MeshHandle; it exists
// beside it, so a feature building its own vertices per frame can own its own vertex format.

// What a vertex attribute means to the input assembler.
enum class VertexSemantic : u8 { Position, Normal, TexCoord, Color };

// One attribute in a caller-owned vertex.
struct VertexAttrib {
    VertexSemantic semantic = VertexSemantic::Position;
    u8             semanticIndex = 0;    // POSITION0, TEXCOORD1, ...
    Format         format = Format::Unknown;
    u32            offset = 0;           // bytes from the start of the vertex
};

constexpr u32 kMaxVertexAttribs = 8;

// A caller-owned vertex format: its attributes and its stride.
struct VertexLayout {
    VertexAttrib attribs[kMaxVertexAttribs] = {};
    u32          attribCount = 0;
    // Bytes per vertex. Required whenever attribCount is non-zero and NOT derived from the
    // attributes, because a layout may legally leave padding at the end.
    u32          stride = 0;
};

// How to create a graphics pipeline.
struct GraphicsPipelineDesc {
    // Either (vs[,gs]) or ms must be set. A mesh pipeline has no input assembler; the backend picks
    // the matching root-signature flavour automatically.
    ShaderHandle vs = 0, gs = 0, ms = 0, ps = 0;

    // The vertex format the input assembler reads. LEFT EMPTY, the backend uses the engine's own
    // MeshVertex. Ignored by a mesh-shader pipeline and by drawFullscreen.
    VertexLayout vertexLayout{};

    PipelineLayout layout{};

    FillMode fill = FillMode::Solid;
    CullMode cull = CullMode::None;
    bool     depthClip = true;
    // Widens rasterisation so thin geometry still covers a pixel. Silently ignored where
    // unsupported — check DeviceCaps::conservativeRaster if it matters.
    bool     conservativeRaster = false;
    // Constant bias is meaningless against a float depth buffer, so slope-scaled carries the load.
    f32      depthBias = 0.0f;
    f32      slopeScaledDepthBias = 0.0f;

    DepthState depth{};

    // Zero render targets is legal and meaningful: a pass whose only output is a UAV write.
    u32    renderTargetCount = 0;
    Format renderTargets[4]  = {};
    Format depthFormat       = Format::Unknown;
    u32    sampleCount       = 1;

    BlendMode blend = BlendMode::Opaque;
};

// How to create a compute pipeline.
struct ComputePipelineDesc {
    ShaderHandle   cs = 0;
    PipelineLayout layout{};
};

// ---------------------------------------------------------------- binding sets
//
// A binding set is a contiguous run of SRV slots plus a contiguous run of UAV slots. Tier 1
// requires every declared slot to hold a valid descriptor, so the backend null-fills any left unset
// with a view of the correct dimension. All sets suballocate from one shader-visible heap.

// The KIND of view a slot holds. Needed for null-filling: a Tier 1 device reading a null descriptor
// of the wrong dimension is undefined behaviour, not a warning.
enum class SlotKind : u8 {
    Texture2D,
    Texture3D,
    AccelerationStructure,   // SRV slots only; bound with a null resource and an address
};

// Slots per range. Enforced, not advisory: counts above this cannot declare a kind.
constexpr u32 kMaxBindingSlots = 16;

// How to create a binding set.
struct BindingSetDesc {
    u32 srvCount = 0;            // must be <= kMaxBindingSlots
    u32 uavCount = 0;            // must be <= kMaxBindingSlots
    SlotKind srvKinds[kMaxBindingSlots] = {};   // default-initialises to Texture2D
    SlotKind uavKinds[kMaxBindingSlots] = {};
    // The first shader register this set is meant to cover. Recorded, not used to build anything,
    // so the backend can check the set against the table it is bound at.
    u32 srvBaseRegister = 0;
    u32 uavBaseRegister = 0;
};

// Binds every mip of a texture as one view. Invalid for a UAV, which always targets one level.
constexpr u32 kAllMips = 0xFFFFFFFFu;

// Transitions the whole resource, which requires EVERY subresource to already be in `from`. For the
// single-slice Tex2D/Tex3D this interface supports, `subresource` is the mip index.
constexpr u32 kAllSubresources = 0xFFFFFFFFu;

// Triangles per mesh-shader thread group. The group count at every dispatch site is
// ceil(triangleCount / this) and the shader's own [numthreads] must agree.
constexpr u32 kMeshShaderTrisPerGroup = 64;

// ---------------------------------------------------------------- reserved registers
//
// dispatchMeshFor() binds geometry the mesh shader pulls itself. These registers are RESERVED by
// the backend; a feature module must not declare anything at them, and the matching declarations
// live in sharedShaderPrelude().
//   - vertices: t(declaredSrvCount), indices: t(declaredSrvCount + 1). They depend on the layout,
//     so the prelude takes them as -D macros from meshGeometryDefines() rather than as literals.
//   - triangle count: 4 root constants at b(kMeshGeometryConstantRegister).
constexpr u32 kMeshGeometryConstantRegister = 5;
constexpr u32 kFeatureFrameConstantRegister = 4;
// b1 is the per-draw block the shared prelude declares: the transform, then the shading constants.
// One owner for the size; a root signature and a shader that disagree is not a validation error.
constexpr u32 kObjectConstantRegister = 1;
constexpr u32 kObjectConstantDwords = 32;
// b2 is the constant block that travels WITH binding table 1, written as a root CBV so it can grow
// without every layout restating its size.
constexpr u32 kDrawConstantRegister = 2;
// Largest b2 block setDrawBinding will carry. A longer block is rejected, not truncated.
constexpr u32 kMaxDrawConstantBytes = 256;
// b0 is the engine's PerFrame block, bound by the backend on every pipeline bind.
constexpr u32 kEngineFrameConstantRegister = 0;
static_assert(kFeatureFrameConstantRegister < kMaxConstantSlots,
              "a feature must be able to DECLARE the register it is told to put frame constants at");
static_assert(kMeshGeometryConstantRegister >= kMaxConstantSlots,
              "the backend's mesh geometry constants must sit above every declarable slot");

// ---------------------------------------------------------------- acceleration structures

// One instance in a top-level acceleration structure. The backend owns sizing, scratch lifetime
// and instance packing.
struct TlasInstance {
    f32         world[16];   // ENGINE convention: row-major/row-vector, cm, +Z up. The backend does
                             // any transpose the underlying API needs — do not pre-transpose.
    u32         mask = 0xFF;
    BlasHandle  blas = 0;
};

// ---------------------------------------------------------------- resource factory

// Creates and destroys GPU resources. Reached with IDevice::resources(), which returns nullptr on
// backends without GPU support.
class IResourceFactory {
public:
    virtual ~IResourceFactory() = default;

    // Creation. Each returns 0 when the resource could not be made.
    virtual TextureHandle    createTexture(const TextureDesc& d) = 0;
    virtual BufferHandle     createBuffer(const BufferDesc& d) = 0;
    virtual ShaderHandle     createShader(const ShaderDesc& d) = 0;
    virtual PipelineHandle   createGraphicsPipeline(const GraphicsPipelineDesc& d) = 0;
    virtual PipelineHandle   createComputePipeline(const ComputePipelineDesc& d) = 0;
    virtual BindingSetHandle createBindingSet(const BindingSetDesc& d) = 0;
    // Builds the acceleration structure for one uploaded mesh.
    virtual BlasHandle       createBlas(MeshHandle mesh) = 0;
    // Allocates a top-level acceleration structure sized for `maxInstances`.
    virtual TlasHandle       createTlas(u32 maxInstances) = 0;

    // Destruction is DEFERRED BY CONTRACT: the backend retires the resource once the GPU has passed
    // every frame that could still reference it. Safe to call mid-frame.
    virtual void destroyTexture(TextureHandle h) = 0;
    virtual void destroyBuffer(BufferHandle h) = 0;
    virtual void destroyShader(ShaderHandle h) = 0;
    virtual void destroyPipeline(PipelineHandle h) = 0;
    virtual void destroyBindingSet(BindingSetHandle h) = 0;

    // Populates a binding set. Slots left unset are null-filled.
    virtual void setSrv(BindingSetHandle set, u32 slot, TextureHandle t, u32 mip = kAllMips) = 0;
    virtual void setUav(BindingSetHandle set, u32 slot, TextureHandle t, u32 mip) = 0;
    // Puts a TLAS in an SRV slot. Distinct name, not an overload: every handle is the same integer.
    virtual void setSrvTlas(BindingSetHandle set, u32 slot, TlasHandle tlas) = 0;

    // Writes bytes into a BufferKind::Upload buffer; rejected on any other kind. The write is
    // IMMEDIATE and unsynchronised, so the caller owns the N-buffering.
    virtual bool writeBuffer(BufferHandle h, const void* src, u64 bytes, u64 offset = 0) = 0;

    // Resolved description, with `mips` filled in when the desc asked for a full chain. The module
    // must not recompute the mip count.
    virtual bool textureInfo(TextureHandle h, TextureDesc& out) const = 0;

    // Blocks until the GPU is idle. Required before destroying and recreating a resource that
    // binding sets still point at.
    virtual void waitIdle() = 0;
};

// ---------------------------------------------------------------- command recording

// How a feature module records into the frame's command stream. It is handed one at defined points
// in the frame, so modules never own submission, allocators or fences.
class IRenderContext {
public:
    virtual ~IRenderContext() = default;

    // Selects the pipeline, its declared layout and the graphics-or-compute binding point. Must
    // precede setConstants / setBindingSet.
    virtual void setPipeline(PipelineHandle p) = 0;

    virtual void setViewport(u32 x, u32 y, u32 w, u32 h) = 0;
    // Mandatory, and NOT derived from the viewport: a pass that binds zero render targets has no
    // extent to fall back on and would otherwise inherit the last rectangle used.
    virtual void setScissor(u32 x, u32 y, u32 w, u32 h) = 0;

    // Binds render targets. The caller must already have transitioned these into RenderTarget /
    // DepthWrite.
    virtual void setRenderTargets(const TextureHandle* colors, u32 count, TextureHandle depth) = 0;
    virtual void clearDepth(TextureHandle depth, f32 value) = 0;

    // Binds a set to one of the pipeline's declared tables. Tier 1 requires EVERY declared table to
    // hold valid descriptors on every pass.
    virtual void setBindingSet(BindingSetHandle set, u32 table = 0) = 0;
    // Root constants at a logical slot. Always overwrites the whole declared block.
    virtual void setConstants(u32 slot, const void* data, u32 dwords) = 0;
    // Transient per-frame constants: suballocated from the frame's upload ring and bound as a root
    // CBV. Backend-owned memory, fresh every call.
    virtual void setConstantBuffer(u32 slot, const void* data, u32 bytes) = 0;

    // Sets binding table 1 plus its b2 constant block as one piece of sticky state, consumed by
    // every subsequent drawMesh / dispatchMeshFor and forwarded to IRenderFeature::submitDraw.
    // Silently ignored by a pipeline whose layout declares neither.
    virtual void setDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) {
        (void)set; (void)constants; (void)bytes;
    }

    // Draws one backend-owned mesh; the backend resolves buffers, counts and root SRVs internally.
    virtual void drawMesh(MeshHandle mesh) = 0;
    // Draws one backend-owned mesh through the mesh-shader path.
    virtual void dispatchMeshFor(MeshHandle mesh) = 0;
    // Dispatches a compute pipeline.
    virtual void dispatch(u32 gx, u32 gy, u32 gz) = 0;

    // ---- geometry the CALLER owns ----
    //
    // The counterpart to GraphicsPipelineDesc::vertexLayout, for a feature that builds its own
    // vertices per frame and so cannot express them as a MeshHandle.

    // Binds a caller-owned vertex buffer. `stride` is named here rather than taken from the
    // pipeline, because nothing checks the two against each other.
    virtual void setVertexBuffer(BufferHandle b, u32 stride) = 0;
    // Binds a caller-owned index buffer. Format must be R32Uint or R16Uint-equivalent.
    virtual void setIndexBuffer(BufferHandle b, Format indexFormat) = 0;
    // Draws from the currently bound vertex and index buffers. `baseVertex` is ADDED to every index
    // before the fetch, which lets many draws share one buffer without rewriting indices.
    virtual void drawIndexed(u32 indexCount, u32 firstIndex = 0, i32 baseVertex = 0) = 0;

    // Draws a 3-vertex fullscreen triangle. The pipeline supplies its OWN vertex shader generating
    // it from SV_VertexID.
    virtual void drawFullscreen() = 0;

    // Builds a bottom-level acceleration structure.
    virtual void buildBlas(BlasHandle blas) = 0;
    // Builds a top-level acceleration structure over `instances`.
    virtual void buildTlas(TlasHandle tlas, const TlasInstance* instances, u32 count) = 0;

    // Transitions a texture, or one of its mips, between states.
    virtual void textureBarrier(TextureHandle t, ResourceState from, ResourceState to,
                                u32 subresource = kAllSubresources) = 0;
    // Transitions a buffer between states.
    virtual void bufferBarrier(BufferHandle b, ResourceState from, ResourceState to) = 0;
    // Orders UAV writes to a texture against later reads.
    virtual void uavBarrierTexture(TextureHandle t) = 0;
    // Orders UAV writes to a buffer against later reads; the only synchronisation between an
    // acceleration-structure write and the RayQuery reads that consume it.
    virtual void uavBarrierBuffer(BufferHandle b) = 0;

    // Opens a named region in PIX / RenderDoc.
    virtual void pushMarker(const char* label) { (void)label; }
    // Closes the innermost open marker region.
    virtual void popMarker() {}
};

// ---------------------------------------------------------------- feature modules

// The hook a render-feature module implements. The backend calls these at fixed points; it does not
// know what the feature does, only when to call it. Registration is NON-owning.
class IRenderFeature {
public:
    virtual ~IRenderFeature() = default;
    virtual const char* name() const = 0;

    // Starts a frame's scene submission, so a feature can replay geometry into its own passes.
    virtual void beginScene() {}
    // One scene draw, with the WHOLE per-draw shading state: the b1 block, plus the sticky table-1
    // set and its b2 block. `drawConstants` is BORROWED and valid only for this call.
    virtual void submitDraw(MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                            f32 metallic, f32 roughness, BindingSetHandle drawBinding,
                            const void* drawConstants, u32 drawConstantBytes) {
        (void)mesh; (void)world; (void)baseColor; (void)metallic; (void)roughness;
        (void)drawBinding; (void)drawConstants; (void)drawConstantBytes;
    }

    // Runs before the scene's render targets are bound — for passes that own their own targets.
    virtual void prePass(IRenderContext& ctx) { (void)ctx; }

    // Whether the scene must be drawn with this feature's pipelines.
    virtual bool           overridesScenePipeline() const { return false; }
    // The pipeline to draw the scene with. wireframe is passed because it has no mesh-shader
    // variant, and a feature may fall back when a variant failed to build.
    virtual PipelineHandle scenePipeline(bool meshShaders, bool wireframe) const {
        (void)meshShaders; (void)wireframe; return 0;
    }
    // Bindings and constants the feature's scene shaders need, applied by the BACKEND to every
    // scene draw it records.
    virtual BindingSetHandle sceneBindingSet() const { return 0; }
    virtual bool sceneConstants(const void** data, u32* bytes) const { (void)data; (void)bytes; return false; }

    // Whether this feature replaces the scene entirely. Suppression covers line/overlay draws too.
    virtual bool suppressesScene() const { return false; }
    // Draws the replacement scene, after the colour target is bound.
    virtual void scenePass(IRenderContext& ctx) { (void)ctx; }

    // Draws onto the BACKBUFFER after the camera post chain, so a HUD authored in display colours
    // is not tonemapped, exposed or bloomed with the world. The backbuffer is already bound as the
    // sole render target with no depth, and the viewport and scissor are already its full extent.
    // Runs BEFORE the editor's own UI is recorded.
    virtual void overlayPass(IRenderContext& ctx, u32 width, u32 height) {
        (void)ctx; (void)width; (void)height;
    }

    // Rebuilds pipelines that bake sample count or target formats when those change.
    virtual void onRenderTargetsChanged(u32 sampleCount, Format color, Format depth) {
        (void)sampleCount; (void)color; (void)depth;
    }
};

// The shared HLSL prelude: cbuffer layouts, vertex structures and helpers used by BOTH the
// backend's own shaders and feature modules. One owner, so the two can never drift.
const char* sharedShaderPrelude();

// The -D list pinning the prelude's reserved mesh-geometry registers to a layout's own SRV count,
// e.g. "AVER_MS_VTX_REG=3;AVER_MS_IDX_REG=4". Semicolon-separated, so it appends straight onto
// ShaderDesc::defines. EVERY mesh-shader compile must pass it, for the layout its pipeline declares.
std::string meshGeometryDefines(const PipelineLayout& layout);

// The camera post chain's HLSL: bloom, eye adaptation, and the tonemap that ends the frame.
// Self-contained — it declares its own constant buffer and does NOT include the shared prelude.
//
// Entry points, in the order a frame uses them:
//   PostVS            fullscreen triangle from SV_VertexID, shared by every pixel entry below
//   PSBloomPrefilter  scene -> half res, soft-knee threshold + Karis average
//   PSBloomDown       one halving step of the pyramid
//   PSBloomUp         one tent-filtered upsample, ADDED by the blender into the level above
//   CSHistogram       256-bin log-luminance histogram of the scene
//   CSExposure        histogram -> one adapted exposure value, with temporal damping
//   PSComposite       scene + bloom -> exposure -> ACES -> gamma -> backbuffer
//
// The composite takes two optional defines, AVER_POST_BLOOM and AVER_POST_AUTOEXPOSURE.
const char* postShaderSource();

} // namespace aver::rhi
