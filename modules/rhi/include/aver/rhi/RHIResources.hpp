// Generic GPU primitives — textures, buffers, shaders, pipelines, binding sets and barriers.
// BINDING MODEL: explicit descriptor tables, NOT bindless; see docs/MINIMUM_SPECS.md.
#pragma once
#include "aver/core/Types.hpp"

#include <cstring>
#include <string>
namespace aver::rhi {

// ---------------------------------------------------------------- handles
// All 0 = invalid. Distinct names, identical underlying type.
using MeshHandle       = u32;
using LineHandle       = u32;
using TextureHandle    = u32;
using BufferHandle     = u32;
using ShaderHandle     = u32;
using PipelineHandle   = u32;
using BindingSetHandle = u32;
using BlasHandle       = u32;
using TlasHandle       = u32;
using BindlessTableHandle = u32;   // ray path's texture array

// ---------------------------------------------------------------- formats & resources

// Pixel and vertex-attribute formats this RHI exposes.
enum class Format : u8 {
    Unknown,
    RGBA8Unorm,
    RGBA8UnormSrgb,  // sRGB decode on tap
    RG8Unorm,
    R8Unorm,
    RGBA16F,      // radiance volumes, HDR targets
    R32Float,
    RG32Float,    // vertex attribute, not texture
    R32Uint,      // D3D12 UAV atomics guarantee
    D32Float,     // depth-stencil view format
    R32Typeless,  // aliased depth: DSV D32Float, SRV R32Float
    RG16F,        // screen-space motion vectors
    RGB10A2Unorm, // world-space normal + roughness, 4 bytes/pixel
    // Block-compressed, 4x4 texel blocks.
    BC1Unorm,
    BC1UnormSrgb,
    BC3Unorm,
    BC3UnormSrgb,
    BC5Unorm,
    BC7Unorm,
    BC7UnormSrgb,
    // 16-bit (uint/unorm), for denoiser-style accumulation.
    R16Unorm,
    R16Uint,
    R16F,         // single-channel half float
    R8Uint,       // single-channel 8-bit uint
};

// True for block-compressed formats, whose extents are counted in 4x4 blocks.
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

// Resource states. Nothing transitions implicitly; state carries across frames.
enum class ResourceState : u8 {
    Common,
    ShaderResource,          // readable by pixel stage
    NonPixelShaderResource,  // readable by compute/non-pixel stages
    UnorderedAccess,
    RenderTarget,
    DepthWrite,
    CopySource,
    CopyDest,
    // Input-assembler read; lets compute that writes vertices hand them to a draw.
    VertexBuffer,
    // Read by input assembler AND acceleration-structure build at once (skinned mesh).
    GeometryRead,
    // TERMINAL: set at creation, never a valid barrier argument.
    AccelerationStructure,
};

// How to create a texture, including its initial state and optional initial contents.
struct TextureDesc {
    TextureDim   dim    = TextureDim::Tex2D;
    u32          width  = 1;
    u32          height = 1;
    u32          depth  = 1;    // Tex3D only
    u32          mips   = 1;    // 0 = full chain
    Format       format = Format::RGBA8Unorm;
    ResourceBind bind   = ResourceBind::ShaderResource;
    // State at creation, state first barrier transitions from.
    ResourceState initialState = ResourceState::Common;
    bool        hasClearValue = false;
    f32         clearDepth    = 1.0f;
    f32         clearColor[4] = {};
    const char* debugName = nullptr;

    // ---- initial contents ----
    // One CPU pointer per subresource (== per mip); uploaded then left in initialState.
    const void* const* initialData = nullptr;
    u32                initialDataCount = 0;
    // Source bytes per row; zero = tightly packed.
    u32                initialRowPitch = 0;
};

// How a texture mip is laid out inside a buffer for copyTextureToBuffer / copyBufferToTexture.
// Not necessarily tightly packed (D3D12 requires 256-byte row alignment).
struct TextureCopyFootprint {
    u64 totalBytes   = 0;   // buffer capacity needed
    u32 rowPitch     = 0;   // bytes row-to-row, including padding
    u32 rowBytes     = 0;   // real data per row
    u32 rows         = 0;
    u32 depth        = 1;
};

enum class BufferKind : u8 {
    Default,         // GPU-local
    Upload,          // CPU-writable, GPU-readable
    AccelStructure,  // GPU-local, terminal AccelerationStructure state
    Readback,        // GPU-writable, CPU-readable
};

// How to create a buffer. Always Common at frame top.
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
    ComparisonLinear,   // depth comparison sampling
    Anisotropic,
};
// How coordinates outside [0,1] are handled.
enum class AddressMode : u8 { Clamp, Wrap };
// Comparison used by depth tests and comparison samplers.
enum class CompareOp : u8 { Never, Less, LessEqual, Always };

// One static sampler on a pipeline.
struct SamplerDesc {
    Filter      filter  = Filter::Linear;
    AddressMode address = AddressMode::Clamp;
    CompareOp   compare = CompareOp::Never;
    f32         maxLod  = 3.402823466e+38f;
    u8          maxAnisotropy = 1;
};

// ---------------------------------------------------------------- shaders & pipelines

// Which programmable stage a shader is compiled for.
enum class ShaderStage : u8 { Vertex, Pixel, Geometry, Compute, Mesh, Amplification };

// One shader to create: either HLSL to compile, or pre-compiled bytecode.
struct ShaderDesc {
    const char* source = nullptr;   // HLSL text
    // Prepended before source; use sharedShaderPrelude().
    const char* prelude = nullptr;
    const char* entry   = nullptr;
    ShaderStage stage   = ShaderStage::Vertex;
    // Minimum shader model as major*10+minor (60 = SM 6.0).
    u32         minShaderModel = 60;
    const char* defines = nullptr;  // semicolon-separated

    // ---- the precompiled path ----
    // Pre-compiled bytecode (DXIL/SPIR-V). Setting this makes source/prelude/entry/defines irrelevant.
    // Bytes are copied, not borrowed.
    const void* bytecode     = nullptr;
    u64         bytecodeSize = 0;

    [[nodiscard]] bool precompiled() const { return bytecode != nullptr && bytecodeSize != 0; }
};

// Which triangle facing is discarded.
enum class CullMode : u8 { None, Back, Front };

// How a pipeline's colour output combines with the render target.
enum class BlendMode : u8 {
    Opaque,
    AlphaBlend,                // src.rgb * src.a + dst.rgb * (1 - src.a)
    PremultipliedAlpha,        // src.rgb + dst.rgb * (1 - src.a)
    Additive,
};
// Whether triangles are filled or drawn as wireframe.
enum class FillMode : u8 { Solid, Wireframe };

// Depth test and write configuration.
struct DepthState {
    bool      test  = false;
    bool      write = false;
    CompareOp op    = CompareOp::Less;
};

// Logical constant slots, mapping to b0..b(n-1).
constexpr u32 kMaxConstantSlots = 5;

// Slots per range; defined here because PipelineLayout also sizes its slot-kind arrays with it.
constexpr u32 kMaxBindingSlots = 24;

// Forward-declared; defined below beside BindingSetDesc.
enum class SlotKind : u8;

struct PipelineLayout {
    u32 srvCount = 0;             // table 0: t0..t(srvCount-1)
    u32 uavCount = 0;             // table 0: u0..u(uavCount-1)
    // A SECOND declarable table, above the first.
    u32 srvCount1 = 0;
    u32 uavCount1 = 0;
    // Slot k maps to register b(k): non-zero word count makes it root constants, zero a root CBV.
    // Slot 0 is reserved for the engine per-frame block.
    u32 constantDwords[kMaxConstantSlots] = {};
    SamplerDesc samplers[4] = {};
    u32 samplerCount = 0;         // s0..s(n-1)

    // Kind of resource each declared slot holds; must agree with BindingSetDesc.
    // Declared because Vulkan types every binding in a descriptor set layout.
    u32 bindlessTextureCount = 0; // texture SRVs in REGISTER SPACE 1

    // ---- register spaces ----
    // Space for constant slots and static samplers; 0 for every pipeline this engine compiles.
    u32 constantSpace = 0;
    u32 samplerSpace  = 0;

    bool slotKindsDeclared = false;
    SlotKind srvKinds[kMaxBindingSlots]  = {};   // table 0
    SlotKind uavKinds[kMaxBindingSlots]  = {};
    SlotKind srvKinds1[kMaxBindingSlots] = {};   // table 1
    SlotKind uavKinds1[kMaxBindingSlots] = {};
};

// How many declarable descriptor tables a layout has.
constexpr u32 kBindingTableCount = 2;

// The register a layout's SRV declarations run out to, across both tables.
inline u32 declaredSrvCount(const PipelineLayout& l) { return l.srvCount + l.srvCount1; }

// ---------------------------------------------------------------- vertex layout
// For pipelines that draw geometry the caller owns, beside MeshHandle.

// What a vertex attribute means to the input assembler.
enum class VertexSemantic : u8 { Position, Normal, TexCoord, Color };

// One attribute in a caller-owned vertex.
struct VertexAttrib {
    VertexSemantic semantic = VertexSemantic::Position;
    u8             semanticIndex = 0;    // POSITION0, TEXCOORD1, ...
    Format         format = Format::Unknown;
    u32            offset = 0;           // bytes from vertex start
};

constexpr u32 kMaxVertexAttribs = 8;

// A caller-owned vertex format: its attributes and stride.
struct VertexLayout {
    VertexAttrib attribs[kMaxVertexAttribs] = {};
    u32          attribCount = 0;
    // Bytes per vertex.
    u32          stride = 0;
};

// How to create a graphics pipeline.
struct GraphicsPipelineDesc {
    // Either (vs[,gs]) or ms must be set. as is optional and valid ONLY with ms.
    ShaderHandle vs = 0, gs = 0, ms = 0, ps = 0, as = 0;

    // Vertex format for the input assembler; empty = engine's MeshVertex.
    VertexLayout vertexLayout{};

    PipelineLayout layout{};

    FillMode fill = FillMode::Solid;
    CullMode cull = CullMode::None;
    bool     depthClip = true;
    // Widens rasterisation so thin geometry covers a pixel.
    bool     conservativeRaster = false;
    f32      depthBias = 0.0f;
    f32      slopeScaledDepthBias = 0.0f;

    DepthState depth{};

    // Zero render targets is legal: pass whose only output is UAV write.
    u32    renderTargetCount = 0;
    Format renderTargets[4]  = {};
    Format depthFormat       = Format::Unknown;
    u32    sampleCount       = 1;

    BlendMode blend = BlendMode::Opaque;

    // Reserves one extra root SRV past every t-register layout declares:
    // per-instance StructuredBuffer of world matrices, indexed by SV_InstanceID.
    bool instanced = false;
};

// How to create a compute pipeline.
struct ComputePipelineDesc {
    ShaderHandle   cs = 0;
    PipelineLayout layout{};
};

// ---------------------------------------------------------------- binding sets
// A binding set is contiguous SRV slots plus contiguous UAV slots. Tier 1 requires every declared slot to hold a valid descriptor.

// The kind of view a slot holds: Tier 1 null descriptor of wrong dimension is undefined.
enum class SlotKind : u8 {
    Texture2D,
    Texture3D,
    AccelerationStructure,   // SRV slots only
    StructuredBuffer,        // element stride given at bind time
    // Multisampled Texture2D (SRV only); only backend-adopted resources can fill this.
    Texture2DMS,
};

// How to create a binding set.
struct BindingSetDesc {
    u32 srvCount = 0;            // must be <= kMaxBindingSlots
    u32 uavCount = 0;
    SlotKind srvKinds[kMaxBindingSlots] = {};   // default Texture2D
    SlotKind uavKinds[kMaxBindingSlots] = {};
    // First shader register this set is meant to cover.
    u32 srvBaseRegister = 0;
    u32 uavBaseRegister = 0;
};

// Binds every mip of a texture as one view. Invalid for UAV, which targets one level.
constexpr u32 kAllMips = 0xFFFFFFFFu;

// Whole-resource transition; otherwise a mip index.
constexpr u32 kAllSubresources = 0xFFFFFFFFu;

// Triangles per mesh-shader thread group; shader's [numthreads] must agree.
constexpr u32 kMeshShaderTrisPerGroup = 64;

// Clusters per amplification-shader thread group; shader's [numthreads] must agree.
constexpr u32 kClusterAmplificationGroupSize = 32;

// ---------------------------------------------------------------- reserved registers
// Reserved by the backend for dispatchMeshFor(), declared in sharedShaderPrelude().
constexpr u32 kMeshGeometryConstantRegister = 5;
constexpr u32 kFeatureFrameConstantRegister = 4;
// b1: per-draw block (transform, then shading constants).
constexpr u32 kObjectConstantRegister = 1;
constexpr u32 kObjectConstantDwords = 32;
// b2: constant block traveling with binding table 1, root CBV.
constexpr u32 kDrawConstantRegister = 2;
// Largest b2 block setDrawBinding will carry.
constexpr u32 kMaxDrawConstantBytes = 256;
// b0: engine's PerFrame block, bound by backend on every pipeline bind.
constexpr u32 kEngineFrameConstantRegister = 0;
static_assert(kFeatureFrameConstantRegister < kMaxConstantSlots,
              "a feature must be able to DECLARE the register it is told to put frame constants at");
static_assert(kMeshGeometryConstantRegister >= kMaxConstantSlots,
              "the backend's mesh geometry constants must sit above every declarable slot");

// Reserved for GraphicsPipelineDesc::instanced == true: StructuredBuffer<float4x4> of per-instance world matrices.

// ---------------------------------------------------------------- acceleration structures

// One instance in a top-level acceleration structure.
struct TlasInstance {
    f32         world[16];   // row-major, cm, +Z up
    u32         mask = 0xFF;
    BlasHandle  blas = 0;
    // Caller's id for this instance, read from hit via HLSL CommittedInstanceID().
    // 24 BITS: DXR rejects larger values.
    u32         instanceId = 0;

    // Per-instance behaviour (TlasInstanceFlags). At the instance, not the geometry.
    u32         flags = 0;
};

// TlasInstance::flags. Values match D3D12_RAYTRACING_INSTANCE_FLAGS.
enum TlasInstanceFlags : u32 {
    TlasInstanceFlag_None                = 0,
    TlasInstanceFlag_TriangleCullDisable = 1u << 0,
    TlasInstanceFlag_TriangleFrontCcw    = 1u << 1,
    TlasInstanceFlag_ForceOpaque         = 1u << 2,  // candidate, not commit (for tinted shadows)
    TlasInstanceFlag_ForceNonOpaque      = 1u << 3,
};
// The largest value TlasInstance::instanceId can carry.
constexpr u32 kMaxTlasInstanceId = 0xFFFFFFu;
// The most instances one TLAS may hold; D3D12 and Vulkan floor.
constexpr u32 kMaxTlasInstances = 1u << 24;
// One instance as TLAS reads it (D3D12_RAYTRACING_INSTANCE_DESC / VkAccelerationStructureInstanceKHR).
constexpr u32 kTlasInstanceDescBytes = 64;

// One geometry of a multi-geometry BLAS.
struct BlasGeometry {
    MeshHandle mesh = 0;
    // OPAQUE geometry flag or none, per geometry.
    bool opaque = true;
};

// ---------------------------------------------------------------- resource factory

// Creates and destroys GPU resources.
class IResourceFactory {
public:
    virtual ~IResourceFactory() = default;

    // Creation. Each returns 0 when the resource could not be made.
    virtual TextureHandle    createTexture(const TextureDesc& d) = 0;
    virtual BufferHandle     createBuffer(const BufferDesc& d) = 0;
    virtual ShaderHandle     createShader(const ShaderDesc& d) = 0;
    virtual PipelineHandle   createGraphicsPipeline(const GraphicsPipelineDesc& d) = 0;
    virtual PipelineHandle   createComputePipeline(const ComputePipelineDesc& d) = 0;
    // ---- the ray path's bindless texture table ----
    // Fixed-size array of texture SRVs, indexed by computed value (not bound register).
    virtual BindlessTableHandle createBindlessTextureTable(u32 capacity) = 0;
    virtual void destroyBindlessTextureTable(BindlessTableHandle h) = 0;

    // Writes one texture into one slot. Returns false and logs on out-of-range.
    virtual bool setBindlessTexture(BindlessTableHandle h, u32 index, TextureHandle t) = 0;

    // The capacity the table was created with, or 0 for invalid handle.
    virtual u32 bindlessTableCapacity(BindlessTableHandle h) const = 0;

    virtual BindingSetHandle createBindingSet(const BindingSetDesc& d) = 0;
    // Builds the acceleration structure for one uploaded mesh.
    virtual BlasHandle       createBlas(MeshHandle mesh) = 0;
    // Allocates a top-level acceleration structure.
    virtual TlasHandle       createTlas(u32 maxInstances) = 0;

    // Updatable twins of createBlas/createTlas: refittable in place.
    virtual BlasHandle createBlasUpdatable(MeshHandle mesh) { return createBlas(mesh); }
    virtual TlasHandle createTlasUpdatable(u32 maxInstances) { return createTlas(maxInstances); }

    // One BLAS over several meshes, one geometry each, opacity per geometry.
    virtual BlasHandle createBlasMulti(const BlasGeometry* geometries, u32 count) {
        (void)geometries; (void)count;
        return 0;
    }

    // Static instance prefix: instances packed once, occupying TLAS slots [0, count).
    virtual bool setTlasStaticInstances(TlasHandle tlas, const TlasInstance* instances, u32 count) {
        (void)tlas; (void)instances; (void)count;
        return false;
    }
    // The prefix's device-local buffer, or 0 when tlas has no prefix.
    virtual BufferHandle tlasStaticInstanceBuffer(TlasHandle tlas) const { (void)tlas; return 0; }

    // Resident bytes behind an acceleration structure, or 0 for dead handle.
    virtual u64 blasMemoryBytes(BlasHandle h) const { (void)h; return 0; }
    virtual u64 tlasMemoryBytes(TlasHandle h) const { (void)h; return 0; }

    // Destruction is deferred by contract.
    virtual void destroyTexture(TextureHandle h) = 0;
    virtual void destroyBuffer(BufferHandle h) = 0;
    virtual void destroyShader(ShaderHandle h) = 0;
    virtual void destroyPipeline(PipelineHandle h) = 0;
    virtual void destroyBindingSet(BindingSetHandle h) = 0;

    // Releases an acceleration structure. Not pure.
    virtual void destroyBlas(BlasHandle h) { (void)h; }

    // The mesh a BLAS was built from, or 0 if dead. Lets a cache self-heal.
    virtual MeshHandle blasMesh(BlasHandle h) const { (void)h; return 0; }

    // A structure already built from this mesh, or 0 if none. Avoids duplicate builds.
    virtual BlasHandle blasForMesh(MeshHandle mesh) const { (void)mesh; return 0; }

    // Populates a binding set. Slots left unset are null-filled.
    virtual void setSrv(BindingSetHandle set, u32 slot, TextureHandle t, u32 mip = kAllMips) = 0;
    virtual void setUav(BindingSetHandle set, u32 slot, TextureHandle t, u32 mip) = 0;
    // Returns an SRV slot to null-filled state (textures may be destroyed without unbinding).
    virtual void clearSrv(BindingSetHandle set, u32 slot) = 0;
    // Puts a TLAS in an SRV slot.
    virtual void setSrvTlas(BindingSetHandle set, u32 slot, TlasHandle tlas) = 0;
    // Puts a buffer in a StructuredBuffer slot. stride = element size, count = elements.
    virtual void setSrvBuffer(BindingSetHandle set, u32 slot, BufferHandle b,
                              u32 stride, u32 count, u32 firstElement = 0) = 0;
    virtual void setUavBuffer(BindingSetHandle set, u32 slot, BufferHandle b,
                              u32 stride, u32 count, u32 firstElement = 0) = 0;

    // Writes bytes into a BufferKind::Upload buffer; immediate and unsynchronised.
    virtual bool writeBuffer(BufferHandle h, const void* src, u64 bytes, u64 offset = 0) = 0;

    // Reads bytes from a BufferKind::Readback buffer. Caller is responsible for GPU sync.
    virtual bool readBuffer(BufferHandle h, void* dst, u64 bytes, u64 offset = 0) = 0;

    // Fills out with the layout copyTextureToBuffer/copyBufferToTexture use for one mip.
    virtual bool textureCopyFootprint(TextureHandle t, u32 mip, TextureCopyFootprint& out) const {
        (void)t; (void)mip; (void)out;
        return false;
    }

    // Resolved description, with mips filled when the desc asked for a full chain.
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
    // Sets the scissor rectangle. Mandatory, not derived from viewport.
    virtual void setScissor(u32 x, u32 y, u32 w, u32 h) = 0;

    // Binds render targets; caller must already have transitioned them.
    virtual void setRenderTargets(const TextureHandle* colors, u32 count, TextureHandle depth) = 0;
    virtual void clearDepth(TextureHandle depth, f32 value) = 0;
    // Clears a colour target to an RGBA value (4 floats).
    virtual void clearColor(TextureHandle target, const f32 color[4]) = 0;

    // Binds a set to one of the pipeline's declared tables.
    virtual void setBindingSet(BindingSetHandle set, u32 table = 0) = 0;

    // Binds the ray path's bindless texture table. No-op on pipelines that declared none.
    virtual void setBindlessTable(BindlessTableHandle table) = 0;
    // Root constants at a logical slot.
    virtual void setConstants(u32 slot, const void* data, u32 dwords) = 0;
    // Transient per-frame constants, suballocated from upload ring, root CBV.
    virtual void setConstantBuffer(u32 slot, const void* data, u32 bytes) = 0;

    // Sets binding table 1 and its b2 constant block as sticky state.
    virtual void setDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) {
        (void)set; (void)constants; (void)bytes;
    }

    // Draws one backend-owned mesh.
    virtual void drawMesh(MeshHandle mesh) = 0;
    // Draws one backend-owned mesh through the mesh-shader path.
    virtual void dispatchMeshFor(MeshHandle mesh) = 0;

    // Draws instanceCount copies of one mesh in a single call, per-instance transforms from StructuredBuffer.
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

    // Dispatches amplification+mesh-shader pipeline over clusterCount clusters.
    virtual void dispatchMeshClusters(MeshHandle mesh, u32 clusterCount) { (void)mesh; (void)clusterCount; }

    // Copies a range between buffers; offsets let concatenation.
    virtual void copyBuffer(BufferHandle dst, BufferHandle src, u64 bytes,
                            u64 dstOffset = 0, u64 srcOffset = 0) = 0;

    // Copies the whole of one texture into another.
    virtual void copyTexture(TextureHandle dst, TextureHandle src) = 0;

    // Copies one mip of a texture to/from a buffer. Query textureCopyFootprint for layout first.
    virtual void copyTextureToBuffer(BufferHandle dst, u64 dstOffset, TextureHandle src, u32 mip) {
        (void)dst; (void)dstOffset; (void)src; (void)mip;
    }
    virtual void copyBufferToTexture(TextureHandle dst, u32 mip, BufferHandle src, u64 srcOffset) {
        (void)dst; (void)mip; (void)src; (void)srcOffset;
    }

    // ---- geometry the caller owns ----

    // Binds a caller-owned vertex buffer with its stride.
    virtual void setVertexBuffer(BufferHandle b, u32 stride) = 0;
    // Binds a caller-owned index buffer (R32Uint or R16Uint-equivalent).
    virtual void setIndexBuffer(BufferHandle b, Format indexFormat) = 0;
    // Draws from the bound vertex and index buffers.
    virtual void drawIndexed(u32 indexCount, u32 firstIndex = 0, i32 baseVertex = 0) = 0;

    // Draws a 3-vertex fullscreen triangle.
    virtual void drawFullscreen() = 0;

    // Builds a bottom-level acceleration structure.
    virtual void buildBlas(BlasHandle blas) = 0;
    // Builds a top-level acceleration structure over instances.
    virtual void buildTlas(TlasHandle tlas, const TlasInstance* instances, u32 count) = 0;

    // ---- in-place updates (refit) ----
    // Both return true when they updated in place, false on full build; structure is valid either way.
    virtual bool refitBlas(BlasHandle blas) { buildBlas(blas); return false; }
    // Updates a TLAS in place when instances have the same count, BLAS, flags and mask.
    virtual bool refitTlas(TlasHandle tlas, const TlasInstance* instances, u32 count) {
        buildTlas(tlas, instances, count);
        return false;
    }

    // Transitions a texture between states.
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

// Scoped GPU stat: opens pushMarker at construction, closes at destruction via RAII.
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

// The hook a render-feature module implements.
class IRenderFeature {
public:
    virtual ~IRenderFeature() = default;
    virtual const char* name() const = 0;

    // Starts a frame's scene submission.
    virtual void beginScene() {}
    // One scene draw with per-draw shading state. drawConstants is borrowed.
    // blended marks a translucent draw captured for sorted, blend-enabled replay.
    virtual void submitDraw(MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                            f32 metallic, f32 roughness, BindingSetHandle drawBinding,
                            const void* drawConstants, u32 drawConstantBytes,
                            bool blended = false) {
        (void)mesh; (void)world; (void)baseColor; (void)metallic; (void)roughness;
        (void)drawBinding; (void)drawConstants; (void)drawConstantBytes; (void)blended;
    }

    // Runs before the scene's render targets are bound.
    virtual void prePass(IRenderContext& ctx) { (void)ctx; }

    // Whether the scene must be drawn with this feature's pipelines.
    virtual bool overridesScenePipeline() const { return false; }
    // The pipeline to draw the scene with. depthPrepassed = true when depth was already written.
    virtual PipelineHandle scenePipeline(bool meshShaders, bool depthPrepassed = false,
                                         bool blended = false) const {
        (void)meshShaders; (void)depthPrepassed; (void)blended; return 0;
    }

    // The bindless texture table the pipelines expect, or 0 for pipelines that declare none.
    virtual BindlessTableHandle sceneBindlessTable() const { return 0; }
    // Depth-only pipeline for same-frame depth prepass (paired with scenePipeline(..., depthPrepassed=true)).
    virtual PipelineHandle depthPrepassPipeline() const { return 0; }
    // Bindings and constants the scene shaders need.
    virtual BindingSetHandle sceneBindingSet() const { return 0; }
    virtual bool sceneConstants(const void** data, u32* bytes) const { (void)data; (void)bytes; return false; }

    // Whether a blended draw actually samples the backdrop texture.
    virtual bool blendedDrawReadsBackdrop(const void* materialConstants, u32 bytes) const {
        (void)materialConstants; (void)bytes; return true;
    }

    // Whether this feature draws the scene geometry itself.
    virtual bool suppressesScene() const { return false; }

    // Whether suppression extends to everything else: sky, lines, transparent pass.
    virtual bool suppressesWholeFrame() const { return suppressesScene(); }

    // Draws the replacement scene.
    virtual void scenePass(IRenderContext& ctx) { (void)ctx; }

    // Draws depth-tested, blended geometry into the scene after opaque drawMesh and sky.
    virtual void transparentPass(IRenderContext& ctx) { (void)ctx; }

    // Draws onto the backbuffer after the camera post chain, before editor UI.
    // Scene depth is readable (IDevice::sceneDepthTexture).
    virtual void overlayPass(IRenderContext& ctx, u32 width, u32 height) {
        (void)ctx; (void)width; (void)height;
    }

    // Rebuilds pipelines and screen-resolution resources when targets/sample count changes.
    virtual void onRenderTargetsChanged(u32 sampleCount, Format color, Format depth, u32 width, u32 height) {
        (void)sampleCount; (void)color; (void)depth; (void)width; (void)height;
    }
};

// ---------------------------------------------------------------- upscaling
// Scene-resolution colour in, present-resolution colour out. See docs/AVERSR.md.

// What an upscaler reads besides scene colour.
enum class UpscalerNeeds : u32 {
    None          = 0,
    Depth         = 1u << 0,     // scene-resolution depth
    MotionVectors = 1u << 1,     // screen-space motion in texels/frame
    Jitter        = 1u << 2,     // sub-pixel offset for un-jittering
    History       = 1u << 3,     // last frame's present-resolution output
};
inline UpscalerNeeds operator|(UpscalerNeeds a, UpscalerNeeds b) {
    return static_cast<UpscalerNeeds>(static_cast<u32>(a) | static_cast<u32>(b));
}
inline bool any(UpscalerNeeds v, UpscalerNeeds bit) {
    return (static_cast<u32>(v) & static_cast<u32>(bit)) != 0;
}

// One frame's upscaler input. Fields beyond color are valid only when needs() requested them.
struct UpscalerInput {
    TextureHandle color = 0;             // scene-resolution colour
    u32 srcWidth = 0, srcHeight = 0;
    u32 dstWidth = 0, dstHeight = 0;

    TextureHandle depth         = 0;
    TextureHandle motionVectors = 0;
    f32 jitterX = 0.0f, jitterY = 0.0f;
    TextureHandle history       = 0;
};

// Upscaler interface. See docs/AVERSR.md.
class IUpscaler {
public:
    virtual ~IUpscaler() = default;
    virtual const char* name() const = 0;

    // Input needs (constant per algorithm).
    virtual UpscalerNeeds needs() const { return UpscalerNeeds::None; }

    // Whether this upscaler accumulates state across frames.
    virtual bool isTemporal() const { return false; }
    virtual void reset() {}

    // Upscale in.color to outTarget.
    virtual void execute(IRenderContext& ctx, const UpscalerInput& in, TextureHandle outTarget) = 0;
};

// Frame interpolation (docs/rendering/NEURAFI.md): one frame generated between every two real frames.

// One real frame handed to the generator (frame N).
struct FrameInterpInput {
    TextureHandle color    = 0;   // HDR scene colour
    TextureHandle velocity = 0;   // RG16F texel motion
    TextureHandle viewZ    = 0;   // R32F linear view depth
    u32  width = 0, height = 0;
    // Discontinuity between N-1 and N (level load, teleport): nothing may be interpolated.
    bool sceneCut = false;
};

class IFrameInterpolator {
public:
    virtual ~IFrameInterpolator() = default;
    virtual const char* name() const = 0;

    // Records generation of frame halfway between previous and current, stores current as history.
    // Returns generated HDR image or 0 when no valid previous frame.
    virtual TextureHandle generate(IRenderContext& ctx, const FrameInterpInput& in) = 0;

    // Forgets the previous frame.
    virtual void reset() = 0;
};

// The shared HLSL prelude: cbuffer layouts, vertex structures and helpers.
const char* sharedShaderPrelude();

// Defines pinning mesh-geometry registers to a layout's SRV count.
std::string meshGeometryDefines(const PipelineLayout& layout);

// The camera post chain's HLSL: bloom, eye adaptation, tonemap.
const char* postShaderSource();

} // namespace aver::rhi
