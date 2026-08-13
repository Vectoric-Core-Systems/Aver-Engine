// Generic GPU primitives — textures, buffers, shaders, pipelines, binding sets, barriers and
// command recording — that render-feature modules build on. Carries no feature vocabulary.
// BINDING MODEL: explicit descriptor tables, NOT bindless; see docs/MINIMUM_SPECS.md.
#pragma once
#include "aver/core/Types.hpp"

#include <cstring>
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

// Which programmable stage a shader is compiled for. Amplification runs BEFORE Mesh and decides,
// per group, whether and how many mesh-shader groups DispatchMesh spawns -- the stage a per-cluster
// LOD cut runs on, since it is one thread's local test with no dependency on any other cluster.
enum class ShaderStage : u8 { Vertex, Pixel, Geometry, Compute, Mesh, Amplification };

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
    // Either (vs[,gs]) or ms must be set. `as` is optional and valid ONLY alongside `ms`: it runs
    // ahead of the mesh shader and decides, per group, how many mesh-shader groups get dispatched.
    ShaderHandle vs = 0, gs = 0, ms = 0, ps = 0, as = 0;

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

    // Reserves ONE EXTRA root SRV, past every t-register `layout` itself declares (and past the
    // mesh-shader geometry SRVs too, when `ms` is also set): a per-instance StructuredBuffer that
    // IRenderContext::drawMeshInstanced binds and the shader indexes with SV_InstanceID. See that
    // method and kInstanceWorldRegister's comment below for the whole mechanism. Defaulted false so
    // every EXISTING pipeline gets the exact root signature it already had -- this is additive, not
    // a reinterpretation of anything `layout` already means.
    bool instanced = false;
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
    // A multisampled Texture2D -- SRV slots only, and the view dimension a genuinely multisampled
    // resource REQUIRES: D3D12 rejects a plain TEXTURE2D SRV over a resource whose SampleDesc.Count
    // is above 1. createTexture() itself never produces one (every TextureDesc it creates is forced
    // to 1 sample -- see D3D12ResourceFactory::createTexture), so the only resource that can ever
    // legally fill a slot declared this way is one a backend adopted from OUTSIDE the ordinary
    // texture-creation path, the way D3D12Device::sceneDepthTexture() wraps the live scene depth
    // buffer. modules/occlusion is the one consumer today (its HZB seed pass reads the scene's own,
    // possibly-multisampled, depth buffer -- see OcclusionCuller.cpp's top comment for why an
    // ordinary Texture2D read would be invalid there, not merely wrong).
    Texture2DMS,
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

// Clusters per amplification-shader thread group, for a cluster-culling mesh-shader pipeline
// dispatched with dispatchMeshClusters(). The shader's own [numthreads] must agree. Kept separate
// from kMeshShaderTrisPerGroup: that one sizes a group of TRIANGLES inside a mesh shader that reads
// one flat index buffer; this one sizes a group of CLUSTERS inside the amplification shader ahead
// of it, where each thread does one cluster's local LOD-cut test (ownError/parentError vs budget,
// frustum, cone) and DispatchMesh()'s the survivors -- unrelated units, unrelated shaders.
constexpr u32 kClusterAmplificationGroupSize = 32;

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

// Reserved for GraphicsPipelineDesc::instanced == true, and declared in the CALLER's own shader (not
// the shared prelude -- unlike PerObject, not every consumer wants this, so it is opt-in per pipeline
// rather than universal): a StructuredBuffer<float4x4> of per-instance world matrices, one draw's
// worth of transforms, indexed with SV_InstanceID. Its t-register is t(declaredSrvCount(layout)),
// or t(declaredSrvCount(layout) + 2) when the SAME pipeline is also a mesh-shader one (`ms` set),
// since dispatchMeshFor's own vertex/index SRVs already claim declaredSrvCount and +1 in that case.
// A caller compiling its instanced shader must pass the SAME NUMBER as a #define -- see
// VoxiShaders.hpp's VSShadowInstanced and VoxiRenderer.cpp's AVER_INSTANCE_SRV for the pattern; the
// backend has no way to push a register number INTO already-compiled HLSL text, so this is computed
// identically on both sides from the same layout rather than shared any other way.

// ---------------------------------------------------------------- acceleration structures

// One instance in a top-level acceleration structure.
struct TlasInstance {
    f32         world[16];   // ENGINE: row-major/row-vector, cm, +Z up; do not pre-transpose.
    u32         mask = 0xFF;
    BlasHandle  blas = 0;
    // The caller's own id for this instance, readable from a hit as HLSL's CommittedInstanceID().
    //
    // THE ONLY WAY A HIT CAN SAY WHAT IT HIT. Everything a shader needs after an intersection --
    // which mesh, where its vertices start, which material -- is looked up from this. Without it,
    // ray tracing can answer "is something there" and nothing else, which is why shadows were all
    // the engine could do with it.
    //
    // Do NOT use CommittedInstanceIndex() for that job. It is a position in the built structure,
    // and buildTlas SKIPS instances naming an invalid acceleration structure, so one failure
    // silently shifts every later index by one and every subsequent lookup reads its neighbour's
    // geometry. This field survives that compaction; the index does not.
    //
    // 24 BITS: DXR declares it as a bitfield, so a larger value is rejected rather than truncated.
    u32         instanceId = 0;
};
// The largest value TlasInstance::instanceId can carry.
constexpr u32 kMaxTlasInstanceId = 0xFFFFFFu;

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

    // Releases an acceleration structure. Normally reached through IDevice::destroyMesh, which
    // destroys whatever it built from the mesh it is freeing -- a BLAS left behind would keep
    // pointing ray tracing at that mesh's freed vertex and index memory.
    //
    // NOT PURE, unlike its siblings above, and deliberately: tests/render.ui and
    // tests/render.actorpreview each implement this interface with a MockFactory, and a new `= 0`
    // would break both for a method neither has any use for. A backend that grew acceleration
    // structures without growing a way to release them is a bug in that backend, not here.
    virtual void destroyBlas(BlasHandle h) { (void)h; }

    // The mesh a BLAS was built from, or 0 if it is dead or was never built.
    //
    // EXISTS SO A CACHE CAN SELF-HEAL. VoxiRenderer memoises MeshHandle -> BlasHandle, and after a
    // mesh is destroyed that entry names a structure over freed memory. Asking the factory what a
    // BLAS is actually for lets the cache notice on its own, rather than needing every caller of
    // destroyMesh to remember to tell it -- which is the "one missed site" shape this codebase has
    // been bitten by before.
    virtual MeshHandle blasMesh(BlasHandle h) const { (void)h; return 0; }

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
    // Clears a colour target to an RGBA value. `color` is 4 floats.
    virtual void clearColor(TextureHandle target, const f32 color[4]) = 0;

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

    // Draws `instanceCount` copies of one backend-owned mesh in a SINGLE call, with per-instance
    // world transforms read from a StructuredBuffer the shader indexes with SV_InstanceID -- see
    // kInstanceWorldRegister's comment above GraphicsPipelineDesc::instanced for the whole mechanism.
    // `worlds` is `instanceCount` row-major 4x4 matrices back to back (ENGINE convention: cm, +Z up,
    // do not pre-transpose -- the same layout drawMesh's caller already writes into PerObject via
    // setConstants(kObjectConstantRegister, ...)); COPIED, so the caller may reuse its buffer
    // immediately. The bound pipeline must have been built with GraphicsPipelineDesc::instanced =
    // true, exactly as dispatchMeshFor requires a mesh-shader pipeline.
    //
    // This is deliberately narrower than a full per-instance PerObject block (world plus base
    // colour, material, shading model, emissive): the depth-only shadow pass is this mechanism's
    // first caller and reads nothing else per instance -- see VoxiRenderer::shadowPass. The
    // SRV-indexed-by-SV_InstanceID SHAPE generalises to any other per-instance payload a future
    // caller wants; widen the element type and the shader that reads it then, not this entry point.
    //
    // NOT PURE, same reasoning as dispatchMeshClusters below: adding a new virtual here must not
    // break an existing IRenderContext that never asked for instancing. The default below is a
    // correct, unaccelerated fallback -- one setConstants + drawMesh per instance, exactly what a
    // caller would otherwise write by hand -- so MockContext and any other override keep compiling
    // and behaving correctly, unmodified, the moment this method exists; only D3D12RenderContext
    // turns it into an actual DrawIndexedInstanced with an instance count above 1.
    virtual void drawMeshInstanced(MeshHandle mesh, const f32* worlds, u32 instanceCount) {
        f32 consts[kObjectConstantDwords] = {};
        for (u32 i = 0; i < instanceCount; ++i) {
            std::memcpy(consts, worlds + static_cast<size_t>(i) * 16, 16 * sizeof(f32));
            setConstants(kObjectConstantRegister, consts, kObjectConstantDwords);
            drawMesh(mesh);
        }
    }
    // Dispatches a compute pipeline.
    virtual void dispatch(u32 gx, u32 gy, u32 gz) = 0;

    // Dispatches an amplification+mesh-shader pipeline over `clusterCount` clusters -- one
    // amplification-shader thread per cluster, in groups of kClusterAmplificationGroupSize -- for a
    // per-cluster LOD cut. `mesh`, if non-zero and alive, has its plain vertex buffer bound the same
    // way dispatchMeshFor binds one: every cluster's MeshletVertices are GLOBAL indices into that
    // SAME buffer (FORMAT_SPECS 5.7 -- every LOD level shares LOD 0's vertex array), so the mesh
    // shader still needs it to resolve a cluster's vertices to positions. The cluster arrays
    // themselves (MeshletDesc/Bounds/Vertices/Triangles) are NOT resolved here: unlike a MeshHandle's
    // vertex/index pair, a cluster CUT has no single backend-owned source, so the caller binds them
    // explicitly first (setBindingSet/setSrvBuffer), exactly as any other feature binds its own data
    // ahead of a draw or dispatch call.
    //
    // Deliberately a SEPARATE entry point from dispatchMeshFor(MeshHandle), not an overload of it:
    // dispatchMeshFor dispatches by TRIANGLE COUNT over one flat, immutable index buffer and knows
    // nothing about clusters. Giving it a second, cluster-shaped meaning would make one function
    // answer two different questions depending on which pipeline happened to be bound -- exactly the
    // kind of silent double-duty this codebase has been bitten by before.
    //
    // NOT PURE: adding it here must not break every existing IRenderContext (MockContext in
    // tests/render.ui and tests/render.actorpreview implement this interface and have no use for a
    // cluster-culling path). A backend that grows cluster support without growing this override is a
    // bug in that backend, not here -- see IResourceFactory::destroyBlas for the identical reasoning.
    virtual void dispatchMeshClusters(MeshHandle mesh, u32 clusterCount) { (void)mesh; (void)clusterCount; }

    // Copies whole bytes between buffers. Both must already be in CopySource / CopyDest.
    // Copies a range between buffers. The offsets are what let several sources be CONCATENATED
    // into one destination -- which is how a set of separate meshes becomes the single flat table
    // a shader can index after a ray hit. Without them this could only ever copy a whole buffer to
    // the start of another.
    virtual void copyBuffer(BufferHandle dst, BufferHandle src, u64 bytes,
                            u64 dstOffset = 0, u64 srcOffset = 0) = 0;

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

// This engine's SCOPED_GPU_STAT: opens a pushMarker region at construction and closes it (via
// popMarker) at destruction, so a timing scope over a pass is one line at the top of its block and
// cannot be left open by an early return -- the exact failure mode a hand-written pushMarker/
// popMarker pair invites the moment the function it brackets grows a second exit. VoxiRenderer::
// buildAccelerationStructures had exactly one such exit (an early return for "nothing to build this
// frame") BEFORE this existed, and its popMarker() had to be repeated by hand at that exit as well as
// at the bottom of the function -- correct only because someone remembered both times. A forgotten
// one is not cosmetic: it leaves the backend's tsOpen_ stack (D3D12Device.cpp) off by one for the
// rest of the run, so every span opened afterward inherits a parent that never closes and the frame's
// own top-level bracket ends up permanently nested one level too deep.
//
// PURE RAII, NOT A "MAYBE" ONE: pushMarker/popMarker are declared with inert default bodies above
// precisely so a backend that has not wired up GPU timing (or a MockContext in a test) can still
// take this class -- it costs two virtual calls that no-op, not a missing feature.
//
// DOES DOUBLE DUTY ON PURPOSE, same as the pushMarker/popMarker pair it wraps: on D3D12 this opens
// both a PIX/RenderDoc debug-event region AND a GPU timestamp span nested under whatever is already
// open (see D3D12RenderContext::pushMarker). One call, one scope, one mechanism -- there is
// deliberately no separate "just the marker" or "just the timing" variant to keep two systems in
// sync by hand.
class ScopedGpuStat {
public:
    ScopedGpuStat(IRenderContext& ctx, const char* label) : ctx_(ctx) { ctx_.pushMarker(label); }
    ~ScopedGpuStat() { ctx_.popMarker(); }
    ScopedGpuStat(const ScopedGpuStat&) = delete;
    ScopedGpuStat& operator=(const ScopedGpuStat&) = delete;

private:
    IRenderContext& ctx_;
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
    // The pipeline to draw the scene with. `depthPrepassed` is true for an instance whose depth THIS
    // SAME FRAME's depthPrepassPipeline() already wrote, via a separate depth-only draw earlier in
    // the frame -- see D3D12Device::drawMesh's own comment for the whole mechanism (who sets it, and
    // why it is auto-consumed rather than sticky). DEFAULTED so every pre-existing caller and every
    // OTHER override compiles and behaves exactly as before: a feature that never looks at the third
    // argument returns the identical pipeline it always did, prepass or not.
    virtual PipelineHandle scenePipeline(bool meshShaders, bool wireframe, bool depthPrepassed = false) const {
        (void)meshShaders; (void)wireframe; (void)depthPrepassed; return 0;
    }
    // The DEPTH-ONLY pipeline for a same-frame depth prepass. A caller pairs this with
    // scenePipeline(..., depthPrepassed=true) for the SAME instance later in the frame: this one
    // writes depth (test=Less, write=true, matching scenePipeline()'s own default depth state
    // exactly), the other only TESTS it (LessEqual, write=false) and skips shading wherever the two
    // disagree. THE VERTEX TRANSFORM MUST BE BIT-IDENTICAL BETWEEN THE TWO -- an implementation
    // should build this from the SAME compiled vertex shader scenePipeline() uses, not a hand-copied
    // one, or the depth values the two passes produce will not agree and the EQUAL-ish test above
    // will drop or duplicate pixels. 0 (the default) means this feature offers no prepass, which is
    // the correct answer for every feature except one that implements this: IDevice::
    // drawMeshDepthPrepass is then a no-op, and nothing calls scenePipeline with depthPrepassed=true.
    virtual PipelineHandle depthPrepassPipeline() const { return 0; }
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

    // Rebuilds pipelines that bake sample count or target formats, and any screen-resolution-sized
    // resource of its own, when those change.
    virtual void onRenderTargetsChanged(u32 sampleCount, Format color, Format depth, u32 width, u32 height) {
        (void)sampleCount; (void)color; (void)depth; (void)width; (void)height;
    }
};

// ---------------------------------------------------------------- upscaling
// Scene-resolution colour in, present-resolution colour out. See docs/AVERSR.md for the full design
// (naming, module boundaries, quality tiers) -- this is the seam that design names AverSR: the
// INTERFACE lives here, beside IRenderFeature, in the generic RHI; every actual implementation
// (Aver's own spatial resample, a vendored FSR, a DLSS slot that stays empty on this hardware)
// lives in a module that links Aver.RHI and is never linked BY it, so a renderer holds an
// IUpscaler* that may be null and calls it if it is not, exactly as it holds registered render
// features today -- without this module ever knowing any of them exist.

// What an upscaler reads besides the scene colour target. A caller (the post chain) queries this
// ONCE, ahead of the scene pass, so it knows whether to pay for a motion-vector target or a
// jittered projection matrix at all -- work only a temporal upscaler will ever read. Same idiom as
// ResourceBind above: an implementation ORs together whatever it actually consumes.
enum class UpscalerNeeds : u32 {
    None          = 0,
    // Scene-resolution depth, same frame, same format as the scene's own depth buffer.
    Depth         = 1u << 0,
    // Scene-resolution, screen-space motion in texels/frame (RG; destination texel minus source
    // texel). Nothing in this engine produces this today -- FSR2/3 and DLSS both need it; the
    // built-in upscaler does not ask for it.
    MotionVectors = 1u << 1,
    // The sub-pixel offset THIS frame's scene was rendered with, so a temporal accumulator can
    // un-jitter a sample before blending it into history. Nothing jitters the camera today either.
    Jitter        = 1u << 2,
    // Last frame's OWN present-resolution output, for temporal accumulation across frames.
    History       = 1u << 3,
};
inline UpscalerNeeds operator|(UpscalerNeeds a, UpscalerNeeds b) {
    return static_cast<UpscalerNeeds>(static_cast<u32>(a) | static_cast<u32>(b));
}
inline bool any(UpscalerNeeds v, UpscalerNeeds bit) {
    return (static_cast<u32>(v) & static_cast<u32>(bit)) != 0;
}

// One frame's upscaler input. `color` is always valid; every other field is populated ONLY when
// needs() asked for the matching flag -- a caller that skips producing what nothing reads leaves
// the rest at their zero defaults, and an implementation must not read a field it did not ask for.
struct UpscalerInput {
    TextureHandle color = 0;             // scene-resolution colour, scene-resolution sized
    u32 srcWidth = 0, srcHeight = 0;     // the scene's own size, whatever produced it
    u32 dstWidth = 0, dstHeight = 0;     // the present size to produce

    TextureHandle depth         = 0;     // valid only if needs() has UpscalerNeeds::Depth
    TextureHandle motionVectors = 0;     // valid only if needs() has UpscalerNeeds::MotionVectors
    f32 jitterX = 0.0f, jitterY = 0.0f;  // valid only if needs() has UpscalerNeeds::Jitter (texels)
    TextureHandle history       = 0;     // valid only if needs() has UpscalerNeeds::History
};

// The seam itself, declared the way IRenderFeature just above is: a couple of pure virtuals for
// identity and the one thing every implementation must do, defaulted hooks for everything a simple
// implementation can ignore. That split is what lets a plain spatial resample, a future vendored
// FSR2/3, and a DLSS slot that stays empty on this hardware all compile against the SAME interface,
// with nothing here changing when any of them arrives -- see docs/AVERSR.md for why the empty slot
// is deliberate (no source to integrate, and hardware/licence this repo cannot use regardless).
class IUpscaler {
public:
    virtual ~IUpscaler() = default;
    virtual const char* name() const = 0;

    // Declared ONCE, not re-queried per frame: an implementation's input needs are a property of
    // what algorithm it is, not of any particular frame, so the renderer can decide before the
    // scene pass even runs whether to produce motion vectors or a jitter offset at all. A plain
    // spatial resample returns None -- it reads nothing but the scene colour.
    virtual UpscalerNeeds needs() const { return UpscalerNeeds::None; }

    // True for an upscaler that accumulates state across frames -- reprojected history, an
    // exponential moving average -- and so needs it thrown away on a cut: a camera teleport, a
    // level load, a change of render scale. False (the default) means reset() is never called,
    // because there is nothing to throw away.
    virtual bool isTemporal() const { return false; }
    virtual void reset() {}

    // Scene-resolution colour in `in.color`, present-resolution colour out at `outTarget`. `ctx` is
    // the SAME command-recording context the rest of the frame draws with. The CALLER has already
    // bound `outTarget` as the sole render target and already set the viewport and scissor to
    // (0, 0, in.dstWidth, in.dstHeight) -- the identical contract IRenderFeature::overlayPass above
    // already uses for the backbuffer it is handed. An implementation only records its own pipeline
    // bind and draw; it does not transition `outTarget` before or after.
    virtual void execute(IRenderContext& ctx, const UpscalerInput& in, TextureHandle outTarget) = 0;
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
