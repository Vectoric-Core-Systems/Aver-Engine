#pragma once
#include "aver/core/Types.hpp"

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
namespace aver::rhi {

// ---------------------------------------------------------------- formats & resources

enum class Format : u8 {
    Unknown,
    RGBA8Unorm,
    RGBA16F,      // radiance volumes, HDR targets
    R32Float,
    D32Float,     // depth-stencil view format
    R32Typeless,  // aliased depth: DSV sees D32Float, SRV sees R32Float
};

enum class TextureDim : u8 { Tex2D, Tex3D };

// Bitmask: how a resource may be bound. Drives the underlying resource flags and which views the
// backend creates.
enum class ResourceBind : u32 {
    None            = 0,
    ShaderResource  = 1u << 0,
    UnorderedAccess = 1u << 1,
    RenderTarget    = 1u << 2,
    DepthStencil    = 1u << 3,
};
inline ResourceBind  operator|(ResourceBind a, ResourceBind b) { return static_cast<ResourceBind>(static_cast<u32>(a) | static_cast<u32>(b)); }
inline bool          any(ResourceBind v, ResourceBind bit)     { return (static_cast<u32>(v) & static_cast<u32>(bit)) != 0; }

struct TextureDesc {
    TextureDim   dim    = TextureDim::Tex2D;
    u32          width  = 1;
    u32          height = 1;
    u32          depth  = 1;    // Tex3D only
    u32          mips   = 1;    // 0 = full chain
    Format       format = Format::RGBA8Unorm;
    ResourceBind bind   = ResourceBind::ShaderResource;
    const char*  debugName = nullptr;
};

// Where the buffer lives and what it is for. AccelStructure exists because acceleration structures
// have their own required resource state; the RHI does not interpret their contents.
enum class BufferKind : u8 {
    Default,         // GPU-local
    Upload,          // CPU-writable, GPU-readable (staging, instance descriptions)
    AccelStructure,  // GPU-local, created in the acceleration-structure state
};

struct BufferDesc {
    u64         bytes = 0;
    BufferKind  kind  = BufferKind::Default;
    bool        allowUnorderedAccess = false;   // e.g. scratch for acceleration-structure builds
    const char* debugName = nullptr;
};

using TextureHandle  = u32;  // 0 = invalid
using BufferHandle   = u32;
using ShaderHandle   = u32;
using PipelineHandle = u32;
using BlasHandle     = u32;
using TlasHandle     = u32;

// ---------------------------------------------------------------- shaders & pipelines

enum class ShaderStage : u8 { Vertex, Pixel, Geometry, Compute, Mesh };

struct ShaderDesc {
    const char* source = nullptr;   // HLSL text; the feature module owns its own shader source
    const char* entry  = nullptr;
    ShaderStage stage  = ShaderStage::Vertex;
    // Minimum shader model required, as major*10+minor (60 = SM 6.0, 65 = SM 6.5). The backend
    // may compile higher. A module asking for more than the device reports gets an invalid handle.
    u32         minShaderModel = 60;
    // Semicolon-separated preprocessor defines, e.g. "AVER_MS=1;AVER_RT=1".
    const char* defines = nullptr;
};

enum class CullMode : u8 { None, Back, Front };
enum class FillMode : u8 { Solid, Wireframe };
enum class CompareOp : u8 { Never, Less, LessEqual, Always };

struct DepthState {
    bool      test  = false;
    bool      write = false;
    CompareOp op    = CompareOp::Less;
};

struct GraphicsPipelineDesc {
    // Either (vs[,gs]) or ms must be set — a mesh pipeline has no input assembler, and the backend
    // picks the matching root-signature flavour automatically.
    ShaderHandle vs = 0, gs = 0, ms = 0, ps = 0;

    FillMode fill = FillMode::Solid;
    CullMode cull = CullMode::None;
    bool     depthClip = true;
    // Widens rasterisation so thin geometry still covers a pixel. Silently ignored where the
    // device does not support it — query DeviceCaps::conservativeRaster first if it matters.
    bool     conservativeRaster = false;

    DepthState depth{};

    // Zero render targets is legal and meaningful: a pass whose only output is a UAV write.
    u32    renderTargetCount = 0;
    Format renderTargets[4]  = {};
    Format depthFormat       = Format::Unknown;
    u32    sampleCount       = 1;

    // Vertex layout is implicit: position+normal interleaved (rhi::MeshVertex) when vs is set.
    bool usesMeshVertexLayout = true;
};

struct ComputePipelineDesc {
    ShaderHandle cs = 0;
};

// ---------------------------------------------------------------- binding model
//
// Explicit descriptor tables, NOT bindless. This keeps Resource Binding Tier 1 hardware working
// (Kepler / Maxwell gen 1 / Haswell), which SM 6.6 dynamic resources would drop. See
// docs/MINIMUM_SPECS.md before changing this.

// A binding set is a contiguous run of shader-resource slots plus a contiguous run of
// unordered-access slots, allocated once and rebound cheaply. Tier 1 requires every declared slot
// to hold a valid descriptor, so the backend null-fills any the module leaves unset.
using BindingSetHandle = u32;

struct BindingSetDesc {
    u32 srvCount = 0;   // t0..t(n-1)
    u32 uavCount = 0;   // u0..u(n-1)
};

// What a slot points at. A texture binding may target one mip or the whole chain; targeting a
// single mip is what makes read-one-level-while-writing-the-next legal on one resource.
constexpr u32 kAllMips = 0xFFFFFFFFu;

// ---------------------------------------------------------------- barriers

enum class ResourceState : u8 {
    Common,
    ShaderResource,          // readable by the pixel stage
    NonPixelShaderResource,  // readable by compute / non-pixel stages
    UnorderedAccess,
    RenderTarget,
    DepthWrite,
    CopySource,
    CopyDest,
};

constexpr u32 kAllSubresources = 0xFFFFFFFFu;

// ---------------------------------------------------------------- command recording
//
// A feature module records into the frame's command stream through this interface rather than
// touching a backend command list. It is handed one by the engine at a defined point in the frame
// (see IRenderFeature below), so modules never own submission, allocators or fences.
class IRenderContext {
public:
    virtual ~IRenderContext() = default;

    // ---- state ----
    virtual void setPipeline(PipelineHandle p) = 0;
    virtual void setViewport(u32 x, u32 y, u32 w, u32 h) = 0;
    // Bind render targets by handle. count == 0 with no depth is a UAV-only pass.
    virtual void setRenderTargets(const TextureHandle* colors, u32 count, TextureHandle depth) = 0;
    virtual void clearDepth(TextureHandle depth, f32 value) = 0;

    // ---- bindings ----
    virtual void setBindingSet(BindingSetHandle set) = 0;
    // Root constants, in 32-bit words. The per-object slot the raster pipelines share.
    virtual void setConstants(const void* data, u32 dwords) = 0;
    // Root descriptors: a buffer bound straight by address, no descriptor heap slot needed.
    // This is how a mesh shader reads vertex/index data.
    virtual void setBufferSRV(u32 rootSlot, BufferHandle b) = 0;

    // ---- draws ----
    virtual void drawIndexed(BufferHandle vb, BufferHandle ib, u32 indexCount) = 0;
    virtual void dispatch(u32 gx, u32 gy, u32 gz) = 0;
    virtual void dispatchMesh(u32 groups) = 0;
    virtual void drawFullscreen() = 0;   // 3-vertex fullscreen triangle, no vertex buffer

    // ---- synchronisation ----
    virtual void barrier(TextureHandle t, ResourceState from, ResourceState to,
                         u32 subresource = kAllSubresources) = 0;
    virtual void uavBarrier(TextureHandle t) = 0;
};

// ---------------------------------------------------------------- feature modules
//
// The hook a render-feature module implements. The backend calls these at fixed points in the
// frame; it does not know what the feature does, only when to give it the context.
class IRenderFeature {
public:
    virtual ~IRenderFeature() = default;
    virtual const char* name() const = 0;
    // Before the scene's render targets are bound — for passes that own their own targets
    // (shadow maps, volume rasterisation, acceleration-structure builds).
    virtual void prePass(IRenderContext& ctx) { (void)ctx; }
    // Whether the feature currently wants the scene drawn with its own pipelines.
    virtual bool overridesScenePipeline() const { return false; }
};

} // namespace aver::rhi
