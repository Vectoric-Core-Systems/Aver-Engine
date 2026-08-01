// Generic GPU primitives — textures, buffers, shaders, pipelines, binding sets, barriers and
// command recording — that render-feature modules build on. Carries no feature vocabulary.
// BINDING MODEL: explicit descriptor tables, NOT bindless; see docs/MINIMUM_SPECS.md.
#pragma once
#include "aver/core/Types.hpp"

#include <string>
namespace aver::rhi {

// ---------------------------------------------------------------- handles
// All 0 = invalid. Distinct names, identical underlying type: overloads cannot tell them apart.
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

// Resource states. Nothing transitions implicitly, and a resource's state carries across frames.
enum class ResourceState : u8 {
    Common,
    ShaderResource,          // readable by the pixel stage
    NonPixelShaderResource,  // readable by compute / non-pixel stages
    UnorderedAccess,
    RenderTarget,
    DepthWrite,
    CopySource,
    CopyDest,
    // A buffer being read by the input assembler. Needed so a compute pass that WRITES vertices can
    // hand them to a draw: RENDERING.md 7.3 specifies exactly this transition, and it could not be
    // expressed before.
    VertexBuffer,
    // A buffer read as GEOMETRY BY EVERY CONSUMER AT ONCE -- the input assembler, a shader doing
    // manual vertex fetch, and an acceleration-structure build.
    //
    // It is a combined state rather than three separate ones because a skinned vertex buffer is
    // genuinely read all three ways in a single frame, and there is no point in it between them at
    // which a transition could be inserted. VertexBuffer alone is what a BLAS build rejects: it
    // wants NON_PIXEL_SHADER_RESOURCE, and the debug layer is what says so. Being a read state it
    // cannot also be a UAV, so re-skinning mid-frame must pass back through UnorderedAccess.
    GeometryRead,
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
    // One CPU pointer per subresource (== per mip); uploaded, then left in `initialState`.
    const void* const* initialData = nullptr;
    u32                initialDataCount = 0;   // subresources supplied; the rest are left undefined
    // Source bytes per row of subresource 0; zero means tightly packed, as later mips always are.
    u32                initialRowPitch = 0;
};

// Where a buffer's memory lives.
enum class BufferKind : u8 {
    Default,         // GPU-local
    Upload,          // CPU-writable, GPU-readable
    AccelStructure,  // GPU-local, created in the terminal AccelerationStructure state
    Readback,        // GPU-writable by copy, CPU-readable; how a compute result is checked
};

// How to create a buffer. A buffer is always Common at the top of a frame.
struct BufferDesc {
    u64           bytes = 0;
    BufferKind    kind  = BufferKind::Default;
    bool          allowUnorderedAccess = false;
    const char*   debugName = nullptr;
};

// ---------------------------------------------------------------- samplers
// Static samplers declared on the pipeline, not as binding-set slots.

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
    // Prepended verbatim before `source`; use sharedShaderPrelude().
    const char* prelude = nullptr;
    const char* entry   = nullptr;
    ShaderStage stage   = ShaderStage::Vertex;
    // Minimum shader model as major*10+minor (60 = SM 6.0); too high yields an invalid handle.
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
    // src.rgb + dst.rgb * (1 - src.a), for colour that ALREADY has its alpha folded in.
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

// Logical constant slots, mapping one-to-one onto b0..b(n-1).
constexpr u32 kMaxConstantSlots = 5;

// The binding layout a pipeline declares.
struct PipelineLayout {
    u32 srvCount = 0;             // table 0: t0..t(srvCount-1)
    u32 uavCount = 0;             // table 0: u0..u(uavCount-1)
    // A SECOND declarable table, based immediately above the first: t(srvCount).., u(uavCount)..
    u32 srvCount1 = 0;
    u32 uavCount1 = 0;
    // Slot k maps to register b(k): a non-zero word count makes it root constants, zero a root CBV.
    // Slot 0 is RESERVED for the engine per-frame block; leave constantDwords[0] at zero.
    u32 constantDwords[kMaxConstantSlots] = {};
    SamplerDesc samplers[4] = {};
    u32 samplerCount = 0;         // s0..s(n-1)
};

// How many declarable descriptor tables a layout has, and so the bound on setBindingSet's index.
constexpr u32 kBindingTableCount = 2;

// The register a layout's SRV declarations run out to, ACROSS BOTH TABLES.
inline u32 declaredSrvCount(const PipelineLayout& l) { return l.srvCount + l.srvCount1; }
// The register a layout's UAV declarations run out to, across both tables.
inline u32 declaredUavCount(const PipelineLayout& l) { return l.uavCount + l.uavCount1; }

// ---------------------------------------------------------------- vertex layout
// For pipelines that draw geometry the CALLER owns, beside MeshHandle rather than replacing it.

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
    // Bytes per vertex. Required whenever attribCount is non-zero; not derived from the attributes.
    u32          stride = 0;
};

// How to create a graphics pipeline.
struct GraphicsPipelineDesc {
    // Either (vs[,gs]) or ms must be set.
    ShaderHandle vs = 0, gs = 0, ms = 0, ps = 0;

    // The vertex format the input assembler reads; left empty, the engine's own MeshVertex.
    VertexLayout vertexLayout{};

    PipelineLayout layout{};

    FillMode fill = FillMode::Solid;
    CullMode cull = CullMode::None;
    bool     depthClip = true;
    // Widens rasterisation so thin geometry still covers a pixel; ignored where unsupported.
    bool     conservativeRaster = false;
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
// A binding set is a contiguous run of SRV slots plus a contiguous run of UAV slots. Tier 1
// requires every declared slot to hold a valid descriptor, so unset slots are null-filled.

// The KIND of view a slot holds: a Tier 1 null descriptor of the wrong dimension is undefined.
enum class SlotKind : u8 {
    Texture2D,
    Texture3D,
    AccelerationStructure,   // SRV slots only; bound with a null resource and an address
    // A StructuredBuffer<T> / RWStructuredBuffer<T>. The element stride is given at BIND time
    // rather than declared here, because one slot serves whatever the pass puts in it.
    StructuredBuffer,
};

// Slots per range. Enforced: counts above this cannot declare a kind.
constexpr u32 kMaxBindingSlots = 16;

// How to create a binding set.
struct BindingSetDesc {
    u32 srvCount = 0;            // must be <= kMaxBindingSlots
    u32 uavCount = 0;            // must be <= kMaxBindingSlots
    SlotKind srvKinds[kMaxBindingSlots] = {};   // default-initialises to Texture2D
    SlotKind uavKinds[kMaxBindingSlots] = {};
    // The first shader register this set is meant to cover; recorded so the backend can check it.
    u32 srvBaseRegister = 0;
    u32 uavBaseRegister = 0;
};

// Binds every mip of a texture as one view. Invalid for a UAV, which always targets one level.
constexpr u32 kAllMips = 0xFFFFFFFFu;

// Whole-resource transition, requiring EVERY subresource to be in `from`; otherwise a mip index.
constexpr u32 kAllSubresources = 0xFFFFFFFFu;

// Triangles per mesh-shader thread group. The shader's own [numthreads] must agree.
constexpr u32 kMeshShaderTrisPerGroup = 64;

// ---------------------------------------------------------------- reserved registers
// Reserved by the backend for dispatchMeshFor(), and declared in sharedShaderPrelude(): vertices
// at t(declaredSrvCount), indices at t(declaredSrvCount + 1), triangle count as 4 root constants.
constexpr u32 kMeshGeometryConstantRegister = 5;
constexpr u32 kFeatureFrameConstantRegister = 4;
// b1 is the per-draw block the shared prelude declares: the transform, then shading constants.
constexpr u32 kObjectConstantRegister = 1;
constexpr u32 kObjectConstantDwords = 32;
// b2 is the constant block that travels WITH binding table 1, written as a root CBV.
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

// One instance in a top-level acceleration structure.
struct TlasInstance {
    f32         world[16];   // ENGINE: row-major/row-vector, cm, +Z up; do not pre-transpose.
    u32         mask = 0xFF;
    BlasHandle  blas = 0;
};

// ---------------------------------------------------------------- resource factory

// Creates and destroys GPU resources. Reached with IDevice::resources().
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

    // Destruction is DEFERRED BY CONTRACT: the resource retires once the GPU is past every frame
    // that could reference it.
    virtual void destroyTexture(TextureHandle h) = 0;
    virtual void destroyBuffer(BufferHandle h) = 0;
    virtual void destroyShader(ShaderHandle h) = 0;
    virtual void destroyPipeline(PipelineHandle h) = 0;
    virtual void destroyBindingSet(BindingSetHandle h) = 0;

    // Populates a binding set. Slots left unset are null-filled.
    virtual void setSrv(BindingSetHandle set, u32 slot, TextureHandle t, u32 mip = kAllMips) = 0;
    virtual void setUav(BindingSetHandle set, u32 slot, TextureHandle t, u32 mip) = 0;
    // Puts a TLAS in an SRV slot.
    virtual void setSrvTlas(BindingSetHandle set, u32 slot, TlasHandle tlas) = 0;
    // Puts a buffer in a SlotKind::StructuredBuffer slot. `stride` is the element size in bytes,
    // `count` the number of elements, `firstElement` the offset into it.
    virtual void setSrvBuffer(BindingSetHandle set, u32 slot, BufferHandle b,
                              u32 stride, u32 count, u32 firstElement = 0) = 0;
    virtual void setUavBuffer(BindingSetHandle set, u32 slot, BufferHandle b,
                              u32 stride, u32 count, u32 firstElement = 0) = 0;

    // Writes bytes into a BufferKind::Upload buffer; IMMEDIATE and unsynchronised.
    virtual bool writeBuffer(BufferHandle h, const void* src, u64 bytes, u64 offset = 0) = 0;

    // Reads bytes out of a BufferKind::Readback buffer. Does NO synchronisation: the caller is
    // responsible for the GPU having finished writing what it is about to read.
    virtual bool readBuffer(BufferHandle h, void* dst, u64 bytes, u64 offset = 0) = 0;

    // Resolved description, with `mips` filled in when the desc asked for a full chain.
    virtual bool textureInfo(TextureHandle h, TextureDesc& out) const = 0;

    // Blocks until the GPU is idle.
    virtual void waitIdle() = 0;
};

// ---------------------------------------------------------------- command recording

// How a feature module records into the frame's command stream.
class IRenderContext {
public:
    virtual ~IRenderContext() = default;

    // Selects the pipeline, its layout and the binding point. Precedes setConstants/setBindingSet.
    virtual void setPipeline(PipelineHandle p) = 0;

    virtual void setViewport(u32 x, u32 y, u32 w, u32 h) = 0;
    // Sets the scissor rectangle. Mandatory, and NOT derived from the viewport.
    virtual void setScissor(u32 x, u32 y, u32 w, u32 h) = 0;

    // Binds render targets; the caller must already have transitioned them.
    virtual void setRenderTargets(const TextureHandle* colors, u32 count, TextureHandle depth) = 0;
    virtual void clearDepth(TextureHandle depth, f32 value) = 0;

    // Binds a set to one of the pipeline's declared tables.
    virtual void setBindingSet(BindingSetHandle set, u32 table = 0) = 0;
    // Root constants at a logical slot. Always overwrites the whole declared block.
    virtual void setConstants(u32 slot, const void* data, u32 dwords) = 0;
    // Transient per-frame constants, suballocated from the upload ring and bound as a root CBV.
    virtual void setConstantBuffer(u32 slot, const void* data, u32 bytes) = 0;

    // Sets binding table 1 and its b2 constant block as sticky state for every later draw.
    virtual void setDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) {
        (void)set; (void)constants; (void)bytes;
    }

    // Draws one backend-owned mesh; the backend resolves buffers, counts and root SRVs internally.
    virtual void drawMesh(MeshHandle mesh) = 0;
    // Draws one backend-owned mesh through the mesh-shader path.
    virtual void dispatchMeshFor(MeshHandle mesh) = 0;
    // Dispatches a compute pipeline.
    virtual void dispatch(u32 gx, u32 gy, u32 gz) = 0;
    // Copies whole bytes between buffers. Both must already be in CopySource / CopyDest.
    virtual void copyBuffer(BufferHandle dst, BufferHandle src, u64 bytes) = 0;

    // ---- geometry the CALLER owns ----
    // The counterpart to GraphicsPipelineDesc::vertexLayout, for vertices a feature builds itself.

    // Binds a caller-owned vertex buffer with its stride.
    virtual void setVertexBuffer(BufferHandle b, u32 stride) = 0;
    // Binds a caller-owned index buffer. Format must be R32Uint or R16Uint-equivalent.
    virtual void setIndexBuffer(BufferHandle b, Format indexFormat) = 0;
    // Draws from the bound vertex and index buffers; `baseVertex` is ADDED to every index.
    virtual void drawIndexed(u32 indexCount, u32 firstIndex = 0, i32 baseVertex = 0) = 0;

    // Draws a 3-vertex fullscreen triangle; the pipeline supplies the vertex shader.
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
    // Orders UAV writes to a buffer against later reads.
    virtual void uavBarrierBuffer(BufferHandle b) = 0;

    // Opens a named region in PIX / RenderDoc.
    virtual void pushMarker(const char* label) { (void)label; }
    // Closes the innermost open marker region.
    virtual void popMarker() {}
};

// ---------------------------------------------------------------- feature modules

// The hook a render-feature module implements; the backend calls these at fixed points.
class IRenderFeature {
public:
    virtual ~IRenderFeature() = default;
    virtual const char* name() const = 0;

    // Starts a frame's scene submission, so a feature can replay geometry into its own passes.
    virtual void beginScene() {}
    // One scene draw with its whole per-draw shading state. `drawConstants` is BORROWED.
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
    // The pipeline to draw the scene with.
    virtual PipelineHandle scenePipeline(bool meshShaders, bool wireframe) const {
        (void)meshShaders; (void)wireframe; return 0;
    }
    // Bindings and constants the feature's scene shaders need, applied to every scene draw.
    virtual BindingSetHandle sceneBindingSet() const { return 0; }
    virtual bool sceneConstants(const void** data, u32* bytes) const { (void)data; (void)bytes; return false; }

    // Whether this feature replaces the scene entirely. Suppression covers line/overlay draws too.
    virtual bool suppressesScene() const { return false; }
    // Draws the replacement scene, after the colour target is bound.
    virtual void scenePass(IRenderContext& ctx) { (void)ctx; }

    // Draws onto the BACKBUFFER after the camera post chain, before the editor's own UI. The
    // backbuffer is already bound as the sole render target, viewport and scissor already set.
    virtual void overlayPass(IRenderContext& ctx, u32 width, u32 height) {
        (void)ctx; (void)width; (void)height;
    }

    // Rebuilds pipelines that bake sample count or target formats when those change.
    virtual void onRenderTargetsChanged(u32 sampleCount, Format color, Format depth) {
        (void)sampleCount; (void)color; (void)depth;
    }
};

// The shared HLSL prelude: cbuffer layouts, vertex structures and helpers.
const char* sharedShaderPrelude();

// The -D list pinning the prelude's mesh-geometry registers to a layout's own SRV count, e.g.
// "AVER_MS_VTX_REG=3;AVER_MS_IDX_REG=4". Every mesh-shader compile must pass it.
std::string meshGeometryDefines(const PipelineLayout& layout);

// The camera post chain's HLSL: bloom, eye adaptation, and the tonemap that ends the frame.
// Self-contained — it declares its own constant buffer and does NOT include the shared prelude.
const char* postShaderSource();

} // namespace aver::rhi
