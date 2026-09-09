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
using BindlessTableHandle = u32;   // the ray path's texture array; see createBindlessTextureTable

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
    // Two-channel half-float. Scene-resolution screen-space motion vectors are the first consumer
    // -- see IDevice::gBufferVelocityTexture in RHI.hpp for the exact units (texels/frame,
    // destination minus source) -- and the reason this is RG rather than RGBA16F's four channels
    // truncated to two: a fourth, unused channel would be 2 bytes/pixel of pure padding across
    // every pixel of a target that already exists only because bandwidth was being counted (see
    // that feature's own task brief: ~54 MB at 2750x1639 for all three new targets together).
    RG16F,
    // 10-10-10-2 unorm. Packs a world-space normal (xyz) and a roughness (w) into 4 bytes/pixel --
    // see IDevice::gBufferNormalRoughnessTexture for the exact encoding (xyz maps [-1,1] to [0,1];
    // w is roughness, already [0,1], stored as-is). NOT an RGBA8Unorm: 8 bits per normal component
    // bands visibly on a smoothly curved surface under directional light, which is exactly the
    // artifact a G-buffer feeding a denoiser or a temporal filter cannot afford to introduce
    // upstream of the very passes meant to clean an image up, not add a new defect to it.
    RGB10A2Unorm,
    // Block-compressed, 4x4 texel blocks.
    BC1Unorm,
    BC1UnormSrgb,
    BC3Unorm,
    BC3UnormSrgb,
    BC5Unorm,
    BC7Unorm,
    BC7UnormSrgb,
    // Single-channel 16-bit, one integer and one normalised. Added for NVIDIA NRD's internal
    // texture pools (modules/render.nrd): REBLUR stores its per-pixel accumulation counters as
    // R16_UINT and its normalised hit distances as R16_UNORM, and a pool texture the engine cannot
    // allocate is a denoiser that cannot run -- NrdLinkTest fails on exactly that, by name.
    //
    // APPENDED HERE rather than filed beside R8Unorm where they read better, because appending is
    // the only edit to an enum that cannot change an existing enumerator's value. Nothing today
    // stores or transmits a Format as a number (checked), so this is precaution rather than a
    // constraint -- but the cost of the precaution is a comment, and the cost of being wrong is a
    // silent format shift in anything that ever starts to.
    R16Unorm,
    R16Uint,
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
// How a texture mip is laid out inside a buffer for copyTextureToBuffer / copyBufferToTexture.
//
// NOT NECESSARILY TIGHTLY PACKED, which is the whole reason this exists rather than the caller
// computing width*height*depth*bpp. D3D12 requires each ROW of a copy footprint to start on a
// 256-byte boundary, so a 16-wide RGBA16F mip -- 128 bytes of real data per row -- occupies 256.
// Vulkan has no such rule and reports the tight pitch. A caller that wants tightly packed bytes
// (a file, a hash) must repack using rowPitch, and one that has tightly packed bytes to upload
// must expand into it.
struct TextureCopyFootprint {
    u64 totalBytes   = 0;   // what the buffer must be able to hold for this mip
    u32 rowPitch     = 0;   // bytes from one row to the next, INCLUDING any padding
    u32 rowBytes     = 0;   // bytes of real data in a row; <= rowPitch
    u32 rows         = 0;   // rows per depth slice
    u32 depth        = 1;   // slices (1 for a 2D texture, the mip's depth for a Tex3D)
};

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

// Slots per range. Enforced: counts above this cannot declare a kind. Defined HERE, above
// PipelineLayout, because that struct sizes its slot-kind arrays with it; BindingSetDesc further
// down uses the same one.
constexpr u32 kMaxBindingSlots = 16;

// The binding layout a pipeline declares.
// Declared ahead of PipelineLayout, which now carries slot kinds; defined in full further down,
// next to BindingSetDesc, which has always carried them. An opaque enum declaration with a fixed
// underlying type is a COMPLETE type, so arrays of it are legal here.
enum class SlotKind : u8;

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

    // WHAT KIND OF RESOURCE EACH DECLARED SLOT HOLDS -- the same declaration BindingSetDesc makes,
    // and it must agree with it slot for slot.
    //
    // WHY IT HAS TO BE DECLARED RATHER THAN INFERRED. Vulkan types every binding in a descriptor set
    // LAYOUT, and a set is only bindable to a pipeline whose layout declares the same types. The
    // Vulkan backend used to recover the kinds by REFLECTING the shader, which cannot see a slot the
    // shader does not use: DXC eliminates it, reflection finds nothing, the slot defaults to
    // Texture2D, and the pipeline then declares SAMPLED_IMAGE where the binding set holds, say, an
    // acceleration structure. The layer's report is
    //     "Binding 2 ... is VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE but binding 2 ... trying to bind, is
    //      VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR"
    // three steps away from the cause, at the draw. Voxi's t2 TLAS is exactly that slot: declared
    // for the table, used by only some of the pipelines that share the table.
    //
    // D3D12 ignores these -- a root signature's descriptor ranges are typed by class (SRV/UAV), not
    // by resource dimension -- so this is additive there.
    //
    // Leave slotKindsDeclared false and the backend falls back to reflection, which is correct for
    // any layout whose shaders use every slot they declare. It warns when it has to guess.
    // Non-zero appends ONE more descriptor table to the root signature, holding this many texture
    // SRVs in REGISTER SPACE 1 -- its own space so it cannot collide with any t-register the two
    // ordinary tables above already claim, and appended last so every layout that leaves this at 0
    // serialises byte-identically to before this field existed. Part of the root-signature cache
    // key; see sameLayout in the D3D12 backend.
    u32 bindlessTextureCount = 0;
    bool slotKindsDeclared = false;
    SlotKind srvKinds[kMaxBindingSlots]  = {};   // table 0
    SlotKind uavKinds[kMaxBindingSlots]  = {};
    SlotKind srvKinds1[kMaxBindingSlots] = {};   // table 1
    SlotKind uavKinds1[kMaxBindingSlots] = {};
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

    // Per-instance behaviour, as TlasInstanceFlags below.
    //
    // AT THE INSTANCE, NOT THE GEOMETRY, and that choice is the reason a translucent surface can cast
    // a shadow at all here. createBlas hardcodes D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE (and Vulkan's
    // VK_GEOMETRY_OPAQUE_BIT_KHR) on every geometry it builds, which tells the hardware it may skip
    // any-hit entirely -- so a Proceed() loop would never be handed a candidate to inspect. The
    // instance-level ForceNonOpaque flag OVERRIDES that geometry flag for this instance only, which
    // means opaque geometry keeps the fast path untouched and only the panes that need interception
    // pay for it. Changing createBlas instead would have made every mesh in the scene non-opaque.
    u32         flags = 0;
};

// TlasInstance::flags. Values match D3D12_RAYTRACING_INSTANCE_FLAGS, and the Vulkan backend maps
// them to the corresponding VkGeometryInstanceFlagBitsKHR rather than assuming the numbers agree.
enum TlasInstanceFlags : u32 {
    TlasInstanceFlag_None                = 0,
    TlasInstanceFlag_TriangleCullDisable = 1u << 0,
    TlasInstanceFlag_TriangleFrontCcw    = 1u << 1,
    // Makes every hit on this instance a CANDIDATE rather than a commit, so an inline RayQuery's
    // Proceed() loop can look at the material and decide. This is what a tinted, attenuated shadow
    // needs: the ray must be able to pass THROUGH a pane, multiplying transmittance as it goes,
    // instead of stopping at the first triangle.
    TlasInstanceFlag_ForceOpaque         = 1u << 2,
    TlasInstanceFlag_ForceNonOpaque      = 1u << 3,
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
    // ---- the ray path's bindless texture table ----
    //
    // A FIXED-SIZE ARRAY OF TEXTURE SRVs a shader may index by a value it COMPUTED, rather than by
    // a register the pipeline bound. This is the one exception to the "explicit descriptor tables,
    // NOT bindless" rule at the top of this file, and it is deliberately not an extension of
    // BindingSetDesc: that path hard-refuses anything past kMaxBindingSlots (16) and every raster
    // pipeline in the engine depends on it staying exactly as small and explicit as it is.
    //
    // WHY IT EXISTS. A ray hit has no "current draw", so there is no per-material descriptor table
    // to bind -- one fullscreen pass shades every material in the scene. The raster path keeps its
    // per-draw tables, which are cheaper and work on the FL 11_0 floor; only shaders gated behind
    // DeviceCaps::rtBindlessTextures (DXR 1.1, hence always binding tier 3) may use this.
    //
    // FIXED capacity, not unbounded: a fixed range serialises under root signature version 1.0 and
    // needs no VARIABLE_DESCRIPTOR_COUNT on Vulkan. Slots past what a scene fills stay null, which
    // is why a sampler reading an unwritten slot gets zeros rather than undefined memory.
    virtual BindlessTableHandle createBindlessTextureTable(u32 capacity) = 0;
    virtual void destroyBindlessTextureTable(BindlessTableHandle h) = 0;

    // Writes one texture into one slot. RETURNS FALSE AND LOGS rather than writing out of range --
    // an index past the table is how this engine has already hung a GPU once (a view sized for less
    // than the buffer behind it), and a bad index here is read by a shader with no bounds check at
    // all. The caller is expected to record kUnboundTexture for a refused slot, not to ignore this.
    virtual bool setBindlessTexture(BindlessTableHandle h, u32 index, TextureHandle t) = 0;

    // The capacity the table was created with, or 0 for an invalid handle.
    virtual u32 bindlessTableCapacity(BindlessTableHandle h) const = 0;

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

    // A structure already BUILT from this mesh, or 0 if there is none yet -- the reverse of
    // blasMesh, and the thing that stops two features paying for the same geometry twice.
    //
    // createBlas ALLOCATES; it does not deduplicate, so every caller that asks gets its own pair of
    // buffers. That was invisible while one feature ray-traced, and stopped being invisible when a
    // second did: VoxiRenderer and PathTracer each keep a MeshHandle -> BlasHandle map of their own,
    // so a scene both of them touch built, sized and kept TWO bottom-level structures per mesh.
    // MEASURED on Sponza: 220 meshes, 154.3 ms of allocation at load, and double the resident BLAS
    // memory, for structures that describe byte-identical geometry.
    //
    // A BLAS IS A PURE FUNCTION OF ITS MESH, which is what makes sharing correct rather than merely
    // cheaper: the build hardcodes OPAQUE and takes only the vertex/index buffers, and everything
    // per-use -- transform, material, and whether the instance is non-opaque -- is applied at TLAS
    // build time. So there is no per-consumer state in one to disagree about.
    //
    // BUILT, NOT MERELY ALLOCATED, and that qualifier is the contract. A handle whose structure has
    // been created but not yet built points at uninitialised memory, and a second feature reusing it
    // would trace garbage or race the first feature's build over the same scratch buffer. Returning
    // only built structures means the caller can use one as-is; a caller that gets 0 creates and
    // builds its own, exactly as before.
    //
    // OWNERSHIP DOES NOT CHANGE HANDS: a shared structure belongs to the MESH, and destroyMesh ->
    // destroyBlasForMesh already frees every structure for a mesh. A caller that reuses a handle it
    // did not create must therefore not destroy it.
    virtual BlasHandle blasForMesh(MeshHandle mesh) const { (void)mesh; return 0; }

    // Populates a binding set. Slots left unset are null-filled.
    virtual void setSrv(BindingSetHandle set, u32 slot, TextureHandle t, u32 mip = kAllMips) = 0;
    virtual void setUav(BindingSetHandle set, u32 slot, TextureHandle t, u32 mip) = 0;
    // Returns an SRV slot to the null-filled state it had when the set was created.
    //
    // EXISTS BECAUSE A BOUND DESCRIPTOR OUTLIVES ITS TEXTURE, AND THAT IS A GPU CRASH RATHER THAN A
    // WRONG PIXEL. Once a slot has been pointed at a texture, destroying that texture does not
    // unbind it: the descriptor keeps naming memory the factory has retired and will free. A shader
    // that then samples the slot faults, and a fault in a shader removes the device -- the window
    // dies with no message beyond "the GPU stopped responding".
    //
    // The case this was written for is a target that comes and goes across a resize. D3D12Device's
    // resize() releases the post-process targets and does NOT recreate them in the same call, so
    // anything holding one of their handles sees it go to 0 for a frame or more. `if (h) setSrv(...)`
    // is the natural-looking guard and it is exactly wrong: it skips the update and leaves the dead
    // descriptor in place. Call this instead when the handle a slot tracks becomes 0.
    virtual void clearSrv(BindingSetHandle set, u32 slot) = 0;
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

    // Fills `out` with the layout IRenderContext::copyTextureToBuffer / copyBufferToTexture use for
    // one mip of `t`. False when the handle is bad, the mip is past the chain, the format's byte
    // size is not known, or this backend has not implemented the pair -- which is also the check a
    // caller should make BEFORE issuing either copy.
    virtual bool textureCopyFootprint(TextureHandle t, u32 mip, TextureCopyFootprint& out) const {
        (void)t; (void)mip; (void)out;
        return false;
    }

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

    // Binds the ray path's bindless texture table for the pipelines that declared one
    // (PipelineLayout::bindlessTextureCount). A no-op on a pipeline that did not, so a caller does
    // not have to know which variant is bound. Sticky, like setBindingSet.
    virtual void setBindlessTable(BindlessTableHandle table) = 0;
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

    // Copies the WHOLE of one texture into another. Both must already be in CopySource / CopyDest,
    // exactly as copyBuffer requires of its two buffers.
    //
    // WHY THIS EXISTS, because "copy a texture" sounds too obvious to need a reason: until now the
    // RHI could copy buffers and not textures, so anything rendered into a target was gone the
    // moment the next frame reused that target. That is not an inconvenience, it is a hard block on
    // a whole class of feature -- an asset thumbnail, a cached reflection, a UI element painted once
    // and reused -- because a render target is transient by construction and there was NO WAY to
    // move its pixels somewhere that outlives it. The content browser wanted rendered thumbnails and
    // could not have them for exactly this reason.
    //
    // WHOLE-RESOURCE, NOT A REGION, and deliberately so: a region copy needs matching subresource
    // indices, offsets and extents on both sides, which is four more ways to be wrong for a
    // capability nothing has asked for yet. The two textures must agree on dimension, size, format
    // and mip count -- checked by the backends, which log and do nothing rather than record a copy
    // the debug layer would reject.
    virtual void copyTexture(TextureHandle dst, TextureHandle src) = 0;

    // Copies ONE MIP of a texture into a buffer, and back.
    //
    // WHY THESE EXIST. Until they did, this RHI could move bytes texture-to-texture and
    // buffer-to-buffer, but had no way at all to get a texture's contents to the CPU or to put CPU
    // bytes into an existing texture -- createTexture's initialData was the only route in, and there
    // was no route out. Anything wanting to bake, cache or verify a rendered volume had to either
    // reach past the RHI into a backend (which D3D12Device::selfTest does, for 2D only) or make its
    // output a structured buffer instead of a texture (which PcgVolume does, for that reason).
    // Voxi's GI derived-data cache needs a Tex3D mip chain both ways, and neither workaround fits.
    //
    // THE BUFFER LAYOUT IS THE BACKEND'S, NOT TIGHTLY PACKED -- ask textureCopyFootprint first and
    // repack. See TextureCopyFootprint for why that is not an implementation detail worth hiding.
    //
    // NOT PURE, same reasoning as drawMeshInstanced: adding a virtual here must not break an
    // IRenderContext that never asked for it. The default is an honest no-op that logs nothing and
    // copies nothing; a caller checks textureCopyFootprint first, which returns false on a backend
    // that has not implemented the pair.
    virtual void copyTextureToBuffer(BufferHandle dst, u64 dstOffset, TextureHandle src, u32 mip) {
        (void)dst; (void)dstOffset; (void)src; (void)mip;
    }
    virtual void copyBufferToTexture(TextureHandle dst, u32 mip, BufferHandle src, u64 srcOffset) {
        (void)dst; (void)mip; (void)src; (void)srcOffset;
    }

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
    //
    // `blended` marks a TRANSLUCENT draw -- one the backend is about to capture for its own sorted,
    // blend-enabled replay rather than send down the opaque path (see IDevice::setDrawBlended). It
    // is offered here rather than withheld because THE RIGHT ANSWER DIFFERS PER FEATURE, and only
    // the feature knows it:
    //
    //   - A RASTER feature should ignore it. Voxelising a pane of glass makes it block indirect
    //     light; putting it in the sun-shadow cascade makes it cast a solid black shadow; putting it
    //     in the ray-tracing acceleration structure makes every reflection of it opaque. All three
    //     are worse than the surface being absent, which is why VoxiRenderer drops these.
    //   - A PATH TRACER must NOT ignore it. A dielectric is the one thing a path tracer models
    //     properly -- Fresnel-weighted reflection and refraction with real total internal reflection
    //     -- and a path tracer that cannot see the glass in the scene it is tracing is not a
    //     reference for anything. PtSceneView takes these and marks the instance dielectric.
    //
    // WITHHOLDING IT WAS THE FIRST DESIGN AND IT WAS WRONG. Blended draws originally returned from
    // drawMesh BEFORE this loop, which excluded them from every feature at once -- correct for the
    // three raster consumers above and silently fatal for the path tracer, which then traced a scene
    // with the glass simply missing. One flag, each feature deciding, is the fix.
    //
    // DEFAULTED so an override that does not name the parameter behaves exactly as it did.
    virtual void submitDraw(MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                            f32 metallic, f32 roughness, BindingSetHandle drawBinding,
                            const void* drawConstants, u32 drawConstantBytes,
                            bool blended = false) {
        (void)mesh; (void)world; (void)baseColor; (void)metallic; (void)roughness;
        (void)drawBinding; (void)drawConstants; (void)drawConstantBytes; (void)blended;
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
    //
    // `blended` asks for the SAME shading through an alpha-blend blend state with depth-write off --
    // the pipeline a translucent material needs. It is never combined with `depthPrepassed`: a
    // blended draw writes no depth, so there is nothing for a prepass to have written, and
    // IDevice::drawMesh never routes one down the prepass path (see setDrawBlended). Returning 0 for
    // it is a legitimate answer meaning "this feature has no blended variant"; the backend then
    // DROPS the draw rather than silently drawing it opaque, because an opaque pane of glass is a
    // worse failure than a missing one and a great deal harder to attribute.
    virtual PipelineHandle scenePipeline(bool meshShaders, bool wireframe, bool depthPrepassed = false,
                                         bool blended = false) const {
        (void)meshShaders; (void)wireframe; (void)depthPrepassed; (void)blended; return 0;
    }

    // The bindless texture table the pipelines returned above expect to have bound, or 0 for a
    // feature whose pipelines declare none.
    //
    // WHY THE BACKEND HAS TO ASK. A blended draw is CAPTURED and replayed by the device, after the
    // deferred sky -- see scenePipeline's own comment. The feature is not on the stack at that
    // moment, so it cannot bind anything itself, and the device does not own the table. It asks,
    // exactly as it already asks for the pipeline. Returning 0 is answered by setBindlessTable
    // being a no-op on a pipeline that declared no range, so a feature that has no table and a
    // pipeline that wants none cost one virtual call between them.
    virtual BindlessTableHandle sceneBindlessTable() const { return 0; }
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

    // Whether this feature draws the scene GEOMETRY itself, so the device's own drawMesh path
    // should stand aside. Says nothing about the rest of the frame -- see suppressesWholeFrame.
    //
    // ONLY THE FIRST CLAIMANT IN REGISTRATION ORDER PAINTS (D3D12Device/VulkanDevice::beginFrame,
    // which returns at it). Two features answering true is therefore a real configuration, not an
    // impossible one, and it is silently decided by the order the host happened to register them in.
    // That cost a whole raster-versus-ray-driven performance comparison: PtSceneView answered true
    // unconditionally, so turning ray-driven OFF did not fall back to the rasteriser at all -- it
    // handed the frame to the path tracer, whose fullscreen blit then measured as "the raster path"
    // at 0.1ms while every drawMesh() was being dropped. The backends now LOG the collision and name
    // the winner; a feature that cannot paint this frame should answer false rather than rely on
    // losing the race.
    virtual bool suppressesScene() const { return false; }

    // Whether the suppression above extends to EVERYTHING ELSE IN THE FRAME: the sky pass, the
    // line draws, the transparent pass.
    //
    // THE TWO USED TO BE ONE PREDICATE, AND CONFLATING THEM COST THE SKY. There are genuinely two
    // different things a feature can mean by "I am drawing the scene":
    //
    //   1. "THE FRAME IS MINE" -- a debug visualisation or a research view that paints every pixel
    //      from its own model and would be corrupted by anything else drawing into it. Nothing else
    //      should run. This is the default, so a feature that says nothing keeps the old behaviour.
    //
    //   2. "THE FIRST SURFACE IS MINE" -- ray-driven primary visibility. It replaces the RASTER,
    //      and nothing more: the frame still has a sky above it, still has editor gizmos in it,
    //      still has particles in front of it. Suppressing those as well left the ray-driven
    //      viewport with no clouds, no physical atmosphere, no sun disc and no gizmos -- and the
    //      cloud loss is not subtle, it is the whole sky.
    //
    // A feature in case 2 overrides this to false. Defaulting to suppressesScene() means every
    // existing feature behaves exactly as it did.
    virtual bool suppressesWholeFrame() const { return suppressesScene(); }

    // Draws the replacement scene, after the colour target is bound.
    virtual void scenePass(IRenderContext& ctx) { (void)ctx; }

    // Draws depth-tested, blended geometry into the SCENE colour target, after every opaque drawMesh
    // call this frame AND after the deferred sky -- see D3D12Device::endFrame's own comment for
    // exactly where this sits and why (particles DECIDED 4's own investigation moved it here, from an
    // original position BEFORE the sky, after finding that ordering silently erased any particle not
    // also backed by an opaque occluder -- the sky's own opaque, depth-EQUAL-clear fill overwrote it).
    // THE ENGINE'S FIRST DEPTH-TESTED TRANSPARENT PASS, and the seam particles (smoke, dust, rain,
    // sparks -- anything that must sit IN the scene, tested against real occluders, rather than pasted
    // over it the way overlayPass is) will use. A SEAM, not a special case: nothing about "particle"
    // appears anywhere near this declaration, and any feature may implement it.
    //
    // The scene colour AND depth targets are already bound, and the viewport/scissor already set to
    // the scene rect -- the same contract overlayPass documents for the backbuffer. What is NOT preset
    // is the pipeline: draw here with a pipeline of your own (ctx.setPipeline), because the standard
    // opaque scene pipeline is exactly that -- opaque -- and has no blend state that would do anything
    // useful. Depth-test that pipeline WITH depth-write OFF; see D3D12Device::endFrame's comment at the
    // call site for what depth-write ON would break, and why.
    //
    // DEFAULTED TO A NO-OP, so every feature this engine ships today -- none of which override it --
    // draws nothing here and the frame is exactly what it always was.
    virtual void transparentPass(IRenderContext& ctx) { (void)ctx; }

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
    // texel). UPDATE, now that a producer exists where none did before: IDevice::
    // gBufferVelocityTexture() (RHI.hpp) writes exactly this quantity, in exactly this layout --
    // but ONLY while IDevice::setGBufferEnabled(true) is in effect, which defaults to OFF, so an
    // upscaler asking for this flag against an unmodified build still gets nothing, precisely as
    // before this existed. Turning the G-buffer on and copying its velocity texture into
    // UpscalerInput::motionVectors below is deliberately NOT done here -- that wiring is the next
    // step, not this one (see docs/rendering/DENOISING.md) -- so FSR2/3 and DLSS still cannot be
    // driven by this flag today even though the data they would need can now be produced.
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
