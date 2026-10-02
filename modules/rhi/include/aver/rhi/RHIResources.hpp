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
    // Two-channel half-float; screen-space motion vectors at scene resolution (see IDevice::gBufferVelocityTexture,
    // texels/frame, destination minus source). RG not RGBA16F: a padding channel would cost
    // 2 bytes/pixel across a bandwidth-counted target (~54 MB at 2750x1639 for all three new targets).
    RG16F,
    // 10-10-10-2 unorm: world-space normal + roughness, 4 bytes/pixel. Encoding lives in
    // IDevice::gBufferNormalRoughnessTexture -- do not restate it here (two descriptions, two
    // chances to drift, no way to notice). NOT RGBA8Unorm: 8 bits/normal component bands
    // visibly under directional light, which a denoiser/temporal filter can't afford.
    RGB10A2Unorm,
    // Block-compressed, 4x4 texel blocks.
    BC1Unorm,
    BC1UnormSrgb,
    BC3Unorm,
    BC3UnormSrgb,
    BC5Unorm,
    BC7Unorm,
    BC7UnormSrgb,
    // Single-channel 16-bit (uint / unorm), for denoiser-style pools: accumulation counters as
    // R16_UINT and normalised hit distances as R16_UNORM. Appended here rather than filed
    // near R8Unorm because appending can't change an existing enumerator's value -- nothing stores
    // a Format as a number today (checked), but a silent shift would be free once something does.
    R16Unorm,
    R16Uint,
    // R16F: single-channel half float (R16_SFLOAT), the same gap R16Unorm/R16Uint closed.
    R16F,
    // R8Uint: single-channel 8-bit unsigned integer (R8_UINT), e.g. small counters or masks.
    R8Uint,
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
    // Input-assembler read state; lets a compute pass that WRITES vertices hand them to a draw
    // (RENDERING.md 7.3). Could not be expressed before this existed.
    VertexBuffer,
    // Read as geometry by the input assembler, manual vertex fetch, AND an acceleration-structure
    // build AT ONCE -- combined because a skinned vertex buffer is genuinely read all three ways in
    // one frame with no seam to insert a transition. VertexBuffer alone is rejected by a BLAS build
    // (wants NON_PIXEL_SHADER_RESOURCE, per the debug layer). A read state, so re-skinning mid-frame
    // must pass back through UnorderedAccess.
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

// How a texture mip is laid out inside a buffer for copyTextureToBuffer / copyBufferToTexture.
// NOT NECESSARILY TIGHTLY PACKED (the reason this exists instead of width*height*depth*bpp):
// D3D12 requires each row to start on a 256-byte boundary (a 16-wide RGBA16F mip, 128 real
// bytes/row, occupies 256); Vulkan reports the tight pitch. Repack via rowPitch either direction.
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

// One shader to create: either HLSL to compile, or bytecode somebody else already compiled.
struct ShaderDesc {
    const char* source = nullptr;   // HLSL text; the feature module owns its own shader source
    // Prepended verbatim before `source`; use sharedShaderPrelude().
    const char* prelude = nullptr;
    const char* entry   = nullptr;
    ShaderStage stage   = ShaderStage::Vertex;
    // Minimum shader model as major*10+minor (60 = SM 6.0); too high yields an invalid handle.
    u32         minShaderModel = 60;
    const char* defines = nullptr;  // semicolon-separated, e.g. "AVER_MS=1;AVER_RT=1"

    // ---- the precompiled path ----
    // Already-compiled bytecode (DXIL for D3D12, SPIR-V for Vulkan). Setting it makes
    // source/prelude/entry/defines/minShaderModel irrelevant; `stage` is still required so the
    // backend knows which pipeline kind may consume it (neither bytecode form is inspected to find
    // out). Runtime HLSL-via-DXC is the right default otherwise -- it drives `--shader-source`
    // reload and lets one .hlsl serve both backends -- but can't serve a
    // library that ships precompiled shaders with no source. Bytes are COPIED, not borrowed: caller
    // may free right after createShader returns (sizes are small, hundreds of KiB for a large
    // permutation set); a dangling pointer would surface as PSO corruption or device removal, not a clean
    // crash. A backend refuses a mismatched format (e.g. DXIL handed to Vulkan) with an invalid handle
    // and a log, same as an unreachable shader model; a caller with both formats (a library shipping DXIL and
    // SPIR-V side by side) picks by asking the device which one it wants.
    const void* bytecode     = nullptr;
    u64         bytecodeSize = 0;

    [[nodiscard]] bool precompiled() const { return bytecode != nullptr && bytecodeSize != 0; }
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

// Slots per range; counts above this cannot declare a kind. Defined here (not beside
// BindingSetDesc) because PipelineLayout also sizes its slot-kind arrays with it.
//
// 16 -> 24 (optimisation-wave-2, U1/2.9, C2-8, integration cross-lane fix): kVoxiSrvCount
// (VoxiRenderer.cpp) grew to 17 with the half-res ReSTIR visibility history slot (t16), one past
// the old ceiling -- would have refused Voxi's own binding set (BindingSetDesc::srvCount is checked
// against this on every backend: D3D12Device.cpp's createBindingSet, VulkanResourceFactory.cpp's
// identical check) and made `srv[16] = ...` in VoxiRenderer::giTableKinds an out-of-bounds write.
// Every array/loop/static_assert sized off this constant widens with it automatically (incl.
// VulkanCommon.hpp's kVkUavBindingBase, VulkanRegisterMap.hpp's kMaxRegisterBinds; checked by grep:
// no other file hardcodes 16). Raised to 24, not 17, for headroom -- second wave in a row to grow
// Voxi's SRV table, and pbr::kMaterialSrvCount has its own independent budget against this same
// ceiling that this headroom also protects.
constexpr u32 kMaxBindingSlots = 24;

// The binding layout a pipeline declares. Forward-declared here (defined in full further down,
// beside BindingSetDesc) because PipelineLayout needs it for its slot-kind arrays; an opaque enum
// with a fixed underlying type is a COMPLETE type, so arrays of it are legal.
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

    // WHAT KIND OF RESOURCE EACH DECLARED SLOT HOLDS -- must agree slot-for-slot with BindingSetDesc's
    // own declaration (srvKinds/uavKinds below). DECLARED RATHER THAN INFERRED because Vulkan types
    // every binding in a descriptor set LAYOUT: the backend used to recover kinds by REFLECTING the
    // shader, which misses a slot the shader doesn't use (DXC eliminates it, so it defaults to
    // Texture2D) -- Voxi's t2 TLAS is exactly that slot, declared for the table but used by only some
    // pipelines sharing it, and the resulting Vulkan validation error ("... is
    // VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE but ... trying to bind, is
    // VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR") names the wrong descriptor type, three steps
    // from the real cause, at the draw. D3D12 ignores these (root-signature ranges are typed by class,
    // not dimension), so this is additive there. Leave slotKindsDeclared false to fall back to
    // reflection (correct when every declared slot is used); the backend warns when it has to guess.
    // Non-zero appends ONE more descriptor table to the root signature: this many texture SRVs in
    // its own REGISTER SPACE 1 (so it can't collide with the two ordinary tables' t-registers),
    // appended last so a layout leaving this at 0 serialises byte-identically to before. Part of
    // the root-signature cache key; see sameLayout in the D3D12 backend.
    u32 bindlessTextureCount = 0;

    // ---- register spaces ----
    // Register space for the constant slots and the static samplers; 0 for every pipeline this
    // engine compiles (separate fields only by accident, not design). Exist because HLSL lets a
    // shader put b0/t0 in different spaces, and a shader this engine didn't compile may already
    // have made that choice: a precompiled library may keep SRVs/UAVs in space 0 but its CBV
    // and samplers in space 1 -- without these fields a root signature built here can't describe
    // such shaders. NOT srvSpace/uavSpace: space 1 is already spoken for on the SRV side
    // (bindlessTextureCount's ray-path texture table); add a movable SRV/UAV space when something
    // actually needs one. Non-zero moves ALL kMaxConstantSlots slots, not just the ones used -- the
    // unused ones go unbound (garbage if read), same contract slot 0 already has.
    u32 constantSpace = 0;
    u32 samplerSpace  = 0;

    bool slotKindsDeclared = false;
    SlotKind srvKinds[kMaxBindingSlots]  = {};   // table 0
    SlotKind uavKinds[kMaxBindingSlots]  = {};
    SlotKind srvKinds1[kMaxBindingSlots] = {};   // table 1
    SlotKind uavKinds1[kMaxBindingSlots] = {};
};

// How many declarable descriptor tables a layout has, and so the bound on setBindingSet's index.
constexpr u32 kBindingTableCount = 2;

// The register a layout's SRV declarations run out to, across both tables. Serves one convention:
// the instance buffer binds at t(declaredSrvCount), so both backends and the shared HLSL must agree
// where declarations end. NO UAV TWIN -- a declaredUavCount() sat here unused purely for symmetry;
// write `l.uavCount + l.uavCount1` at the first site that actually needs it, not before.
inline u32 declaredSrvCount(const PipelineLayout& l) { return l.srvCount + l.srvCount1; }

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

    // Reserves ONE EXTRA root SRV past every t-register `layout` declares (and past the mesh-shader
    // geometry SRVs too, when `ms` is set): a per-instance StructuredBuffer that
    // IRenderContext::drawMeshInstanced binds, indexed by SV_InstanceID. See that method and the
    // per-instance register paragraph below for the whole mechanism. Defaulted false so every
    // existing pipeline keeps the exact root signature it already had.
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
    // A multisampled Texture2D -- SRV only, and the view dimension a genuinely multisampled resource
    // REQUIRES (D3D12 rejects a plain TEXTURE2D SRV when SampleDesc.Count > 1). createTexture() never
    // produces one itself (forced to 1 sample -- D3D12ResourceFactory::createTexture), so only a
    // backend-adopted resource from OUTSIDE the normal creation path can fill this slot, e.g.
    // D3D12Device::sceneDepthTexture(). modules/occlusion is the one consumer today (its HZB seed
    // pass reads the scene's own possibly-multisampled depth buffer -- see OcclusionCuller.cpp's top
    // comment for why an ordinary Texture2D read would be invalid there, not merely wrong).
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

// Clusters per amplification-shader thread group for dispatchMeshClusters(); the shader's own
// [numthreads] must agree. Separate from kMeshShaderTrisPerGroup: that sizes a group of TRIANGLES
// in a mesh shader reading one flat index buffer, this sizes a group of CLUSTERS in the
// amplification shader ahead of it (each thread does one cluster's LOD-cut test -- ownError/
// parentError vs budget, frustum, cone -- and DispatchMesh()'s the survivors); unrelated units.
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

// Reserved for GraphicsPipelineDesc::instanced == true, declared in the CALLER's own shader (not the
// shared prelude -- opt-in per pipeline, unlike PerObject): a StructuredBuffer<float4x4> of one
// draw's per-instance world matrices, indexed by SV_InstanceID. Its t-register is
// t(declaredSrvCount(layout)), or +2 when the same pipeline is also mesh-shader (`ms` set, since
// dispatchMeshFor's vertex/index SRVs claim declaredSrvCount and +1). The caller's instanced shader
// must pass the SAME NUMBER as a #define -- see VoxiShaders.hpp's VSShadowInstanced /
// VoxiRenderer.cpp's AVER_INSTANCE_SRV -- since the backend can't push a register number into
// already-compiled HLSL text.

// ---------------------------------------------------------------- acceleration structures

// One instance in a top-level acceleration structure.
struct TlasInstance {
    f32         world[16];   // ENGINE: row-major/row-vector, cm, +Z up; do not pre-transpose.
    u32         mask = 0xFF;
    BlasHandle  blas = 0;
    // The caller's own id for this instance, read from a hit via HLSL's CommittedInstanceID(). THE
    // ONLY WAY A HIT CAN SAY WHAT IT HIT -- mesh, vertex start, material are all looked up from this;
    // without it ray tracing can only answer "is something there" (why shadows were all it could do).
    // Do NOT use CommittedInstanceIndex() for buildTlas's instances: it SKIPS instances naming an
    // invalid acceleration structure, so one failure silently shifts every later index and its lookups
    // read the wrong geometry -- this field survives that compaction, the index does not. The one
    // exception is a STATIC PREFIX (IResourceFactory::setTlasStaticInstances), which refuses rather
    // than skips, so its slot index is stable. 24 BITS: DXR's bitfield rejects a larger value rather
    // than truncating it.
    u32         instanceId = 0;

    // Per-instance behaviour, as TlasInstanceFlags below. AT THE INSTANCE, NOT THE GEOMETRY -- the
    // reason a translucent surface can cast a shadow at all: createBlas hardcodes OPAQUE on every
    // geometry it builds (D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE / VK_GEOMETRY_OPAQUE_BIT_KHR), so the
    // hardware may skip any-hit entirely unless ForceNonOpaque overrides it per instance -- keeping
    // the fast path untouched for opaque geometry, paid only by the panes that need interception.
    // Changing createBlas instead would have made every mesh in the scene non-opaque. (createBlasMulti
    // is the one per-geometry exception: see BlasGeometry.)
    u32         flags = 0;
};

// TlasInstance::flags. Values match D3D12_RAYTRACING_INSTANCE_FLAGS, and the Vulkan backend maps
// them to the corresponding VkGeometryInstanceFlagBitsKHR rather than assuming the numbers agree.
enum TlasInstanceFlags : u32 {
    TlasInstanceFlag_None                = 0,
    TlasInstanceFlag_TriangleCullDisable = 1u << 0,
    TlasInstanceFlag_TriangleFrontCcw    = 1u << 1,
    // Makes every hit on this instance a CANDIDATE rather than a commit, so an inline RayQuery's
    // Proceed() loop can inspect the material and decide -- what a tinted, attenuated shadow needs:
    // the ray passes THROUGH a pane, multiplying transmittance, instead of stopping at the first hit.
    TlasInstanceFlag_ForceOpaque         = 1u << 2,
    TlasInstanceFlag_ForceNonOpaque      = 1u << 3,
};
// The largest value TlasInstance::instanceId can carry.
constexpr u32 kMaxTlasInstanceId = 0xFFFFFFu;
// The most instances one TLAS may hold, static prefix included: DXR's
// D3D12_RAYTRACING_MAX_INSTANCES_PER_TOP_LEVEL_ACCELERATION_STRUCTURE, which is also the floor Vulkan
// guarantees for maxInstanceCount -- so neither backend has to ask its device.
constexpr u32 kMaxTlasInstances = 1u << 24;
// One instance as a TLAS build reads it, and as tlasStaticInstanceBuffer() hands it to a shader:
// D3D12_RAYTRACING_INSTANCE_DESC and VkAccelerationStructureInstanceKHR, identical byte for byte --
// a 3x4 row-major float transform (the TRANSPOSE of TlasInstance::world's upper 4x3, translation in
// column 3), then [instanceId:24 | mask:8], [hit-group offset:24 | flags:8], the u64 BLAS address.
constexpr u32 kTlasInstanceDescBytes = 64;

// One geometry of a multi-geometry BLAS (IResourceFactory::createBlasMulti). A hit reports which one
// through GeometryIndex(), which is its position in the array handed to createBlasMulti.
struct BlasGeometry {
    MeshHandle mesh = 0;
    // OPAQUE geometry flag or none, PER GEOMETRY -- the one place a BLAS is not uniformly opaque. Pass
    // false only for an alpha-masked part: then only ITS triangles reach a Proceed() loop as candidates,
    // and the rest of the object keeps the hardware's any-hit skip without an instance-wide
    // ForceNonOpaque.
    bool opaque = true;
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
    // ---- the ray path's bindless texture table ----
    // A FIXED-SIZE ARRAY OF TEXTURE SRVs a shader indexes by a COMPUTED value, not a bound register
    // -- the one exception to the "explicit descriptor tables, NOT bindless" rule at the top of this
    // file. Deliberately not an extension of BindingSetDesc, which hard-refuses past kMaxBindingSlots
    // (24) and every raster pipeline depends on staying that small and explicit. Exists because a ray
    // hit has no "current draw" to bind a per-material table for -- one fullscreen pass shades every
    // material in the scene; the raster path keeps its cheaper per-draw tables, which work on the
    // FL 11_0 floor. Only shaders gated on
    // DeviceCaps::rtBindlessTextures (DXR 1.1, binding tier 3) may use this. FIXED capacity, not
    // unbounded: serialises under root signature v1.0, needs no VARIABLE_DESCRIPTOR_COUNT on Vulkan;
    // slots past what a scene fills stay null, so an unwritten slot reads zeros, not undefined memory.
    virtual BindlessTableHandle createBindlessTextureTable(u32 capacity) = 0;
    virtual void destroyBindlessTextureTable(BindlessTableHandle h) = 0;

    // Writes one texture into one slot. RETURNS FALSE AND LOGS rather than writing out of range -- a
    // bad index here is read by a shader with no bounds check at all, and this engine has already
    // hung a GPU once on exactly that shape of bug (a view sized for less than the buffer behind it).
    // Caller should record kUnboundTexture on refusal.
    virtual bool setBindlessTexture(BindlessTableHandle h, u32 index, TextureHandle t) = 0;

    // The capacity the table was created with, or 0 for an invalid handle.
    virtual u32 bindlessTableCapacity(BindlessTableHandle h) const = 0;

    virtual BindingSetHandle createBindingSet(const BindingSetDesc& d) = 0;
    // Builds the acceleration structure for one uploaded mesh.
    virtual BlasHandle       createBlas(MeshHandle mesh) = 0;
    // Allocates a top-level acceleration structure sized for `maxInstances`.
    virtual TlasHandle       createTlas(u32 maxInstances) = 0;

    // UPDATABLE twins of createBlas/createTlas: built with ALLOW_UPDATE (D3D12) /
    // ALLOW_UPDATE_BIT_KHR (Vulkan) and a scratch buffer sized for BOTH a build and an update, so
    // IRenderContext::refitBlas/refitTlas can update them in place instead of rebuilding from
    // scratch. A little more memory, and a refitted structure traces a little slower the further its
    // contents drift from its last full build -- callers that refit also rebuild periodically.
    // NOT PURE, defaulting to the plain create: a backend (or test mock) without refit support simply
    // hands back a structure every refit call fully rebuilds, which is always correct.
    virtual BlasHandle createBlasUpdatable(MeshHandle mesh) { return createBlas(mesh); }
    virtual TlasHandle createTlasUpdatable(u32 maxInstances) { return createTlas(maxInstances); }

    // ONE BLAS OVER SEVERAL MESHES, one geometry each in `geometries` order, opacity per geometry (see
    // BlasGeometry). Allocated, not built -- IRenderContext::buildBlas builds it like any other; never
    // updatable (refitBlas does a full build). For an object whose material parts are separate meshes but
    // which is instanced as ONE thing (foliage: one TLAS instance per plant, not per part). NOT a
    // blasForMesh candidate -- it is a function of the whole list, not of one mesh -- but blasMesh reports
    // its first geometry's mesh, and destroying ANY of its meshes destroys it, exactly as destroying a
    // mesh destroys its own BLAS. 0 when any mesh is invalid or has no indices. NOT PURE, same reason as
    // destroyBlas.
    virtual BlasHandle createBlasMulti(const BlasGeometry* geometries, u32 count) {
        (void)geometries; (void)count;
        return 0;
    }

    // THE STATIC INSTANCE PREFIX: `count` instances packed ONCE into a device-local buffer that occupies
    // TLAS slots [0, count); every later buildTlas/refitTlas copies that frame's instances in behind it
    // on the GPU and builds over count + n. Exists for millions of instances that never move (foliage):
    // the per-frame path uploads and CPU-packs every instance every build, which at that count is the
    // frame. The TLAS's structure/scratch grow to hold count + its own maxInstances (the TLAS's `as` is
    // REALLOCATED, so every setSrvTlas naming it must be redone before the next ray traverses it, and
    // it must be rebuilt first -- a build is due anyway, since this invalidates the last one). Slot
    // indices ARE stable here, unlike buildTlas's filtered list: every instance must name a live BLAS and
    // fit a 24-bit id, and a list that doesn't is REFUSED whole (false, the previous prefix kept) rather
    // than compacted. Every BLAS named must still be live at each build; one destroyed since drops the
    // whole prefix from that build, logged, rather than letting a ray traverse freed memory. `count` 0
    // removes the prefix and returns the TLAS to its own size. refitTlas's eligibility ignores the prefix
    // slots (they cannot change between builds). Everything the prefix allocated is released when it is
    // removed or replaced, and at device shutdown. NOT PURE, same reason as destroyBlas.
    virtual bool setTlasStaticInstances(TlasHandle tlas, const TlasInstance* instances, u32 count) {
        (void)tlas; (void)instances; (void)count;
        return false;
    }
    // The prefix's device-local buffer, for a shader to read as a StructuredBuffer of
    // kTlasInstanceDescBytes-stride descs (element i is TLAS slot i -- CommittedInstanceIndex()), or 0
    // when `tlas` has no prefix. REPLACED by every setTlasStaticInstances call: rebind after each one.
    virtual BufferHandle tlasStaticInstanceBuffer(TlasHandle tlas) const { (void)tlas; return 0; }

    // Resident bytes behind an acceleration structure -- the structure and its scratch, and for a TLAS
    // its instance buffers and static prefix too -- or 0 for a dead handle or a backend that does not
    // say. For memory reports, not for sizing anything.
    virtual u64 blasMemoryBytes(BlasHandle h) const { (void)h; return 0; }
    virtual u64 tlasMemoryBytes(TlasHandle h) const { (void)h; return 0; }

    // Destruction is DEFERRED BY CONTRACT: the resource retires once the GPU is past every frame
    // that could reference it.
    virtual void destroyTexture(TextureHandle h) = 0;
    virtual void destroyBuffer(BufferHandle h) = 0;
    virtual void destroyShader(ShaderHandle h) = 0;
    virtual void destroyPipeline(PipelineHandle h) = 0;
    virtual void destroyBindingSet(BindingSetHandle h) = 0;

    // Releases an acceleration structure. Normally reached via IDevice::destroyMesh, which destroys
    // whatever it built from the freed mesh -- a leftover BLAS would keep ray tracing pointed at
    // freed vertex/index memory. NOT PURE, unlike its siblings above: tests/render.ui and
    // tests/render.actorpreview implement this interface with a MockFactory that has no use for it,
    // and a new `= 0` would break both. A backend that grows acceleration structures without a way
    // to release them is a bug in that backend, not here.
    virtual void destroyBlas(BlasHandle h) { (void)h; }

    // The mesh a BLAS was built from, or 0 if dead or never built. EXISTS SO A CACHE CAN SELF-HEAL:
    // VoxiRenderer memoises MeshHandle -> BlasHandle, and after a mesh is destroyed that entry names
    // freed memory. Asking the factory what a BLAS is for lets the cache notice on its own, instead
    // of needing every destroyMesh caller to remember to tell it -- the "one missed site" shape this
    // codebase has been bitten by before.
    virtual MeshHandle blasMesh(BlasHandle h) const { (void)h; return 0; }

    // A structure already BUILT from this mesh, or 0 if none yet -- the reverse of blasMesh, so two
    // features stop paying for the same geometry twice. createBlas ALLOCATES, it does not dedupe:
    // VoxiRenderer and PathTracer each kept their own MeshHandle -> BlasHandle map, so a shared scene
    // built TWO bottom-level structures per mesh (MEASURED on Sponza: 220 meshes, 154.3 ms extra
    // allocation at load, double resident BLAS memory, for byte-identical geometry). Sharing is
    // correct, not just cheaper: A BLAS IS A PURE FUNCTION OF ITS MESH -- the build hardcodes OPAQUE
    // and takes only the vertex/index buffers, with everything per-use (transform, material,
    // non-opaque) applied at TLAS build time instead. BUILT, NOT MERELY ALLOCATED is the contract: a
    // created-but-unbuilt handle points at uninitialised memory and a reuser would race the build or
    // trace garbage; a caller getting 0 creates and builds its own, as before. OWNERSHIP STAYS WITH
    // THE MESH -- destroyMesh -> destroyBlasForMesh frees every structure for it, so a reused handle
    // must not be destroyed by whoever reused it.
    virtual BlasHandle blasForMesh(MeshHandle mesh) const { (void)mesh; return 0; }

    // Populates a binding set. Slots left unset are null-filled.
    virtual void setSrv(BindingSetHandle set, u32 slot, TextureHandle t, u32 mip = kAllMips) = 0;
    virtual void setUav(BindingSetHandle set, u32 slot, TextureHandle t, u32 mip) = 0;
    // Returns an SRV slot to the null-filled state it had when the set was created. EXISTS BECAUSE A
    // BOUND DESCRIPTOR OUTLIVES ITS TEXTURE, A GPU CRASH RATHER THAN A WRONG PIXEL: destroying a
    // texture doesn't unbind the slot, so a
    // shader sampling it faults and takes the device down with it ("the GPU stopped responding", no
    // other message). Written for a target that comes and goes across resize -- D3D12Device's
    // resize() releases post-process targets without recreating them in the same call, so a held
    // handle reads 0 for a frame or more. `if (h) setSrv(...)` looks like the right guard and is
    // exactly wrong: it skips the update and leaves the dead descriptor in place. Call this instead
    // when the handle a slot tracks becomes 0.
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
    // one mip of `t`. False for a bad handle, a mip past the chain, an unknown format byte size, or
    // an unimplemented backend -- also the check a caller should make BEFORE issuing either copy.
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

    // Binds the ray path's bindless texture table (PipelineLayout::bindlessTextureCount). A no-op on
    // a pipeline that declared none, so a caller need not know which variant is bound. Sticky, like
    // setBindingSet.
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

    // Draws `instanceCount` copies of one backend-owned mesh in a SINGLE call, per-instance world
    // transforms read from a StructuredBuffer indexed by SV_InstanceID (see the per-instance
    // register paragraph above GraphicsPipelineDesc::instanced). `worlds` is `instanceCount`
    // row-major 4x4 matrices back to back (ENGINE convention: cm, +Z up, no pre-transpose, same as
    // PerObject); COPIED, so the caller may reuse its buffer immediately. Requires
    // GraphicsPipelineDesc::instanced = true. Narrower than a full PerObject block (world plus base
    // colour, material, shading model, emissive) on purpose: the depth-only shadow pass is the first
    // caller and needs nothing else per instance (VoxiRenderer::shadowPass). The SRV-indexed-by-
    // SV_InstanceID shape generalises to any other per-instance payload a future caller wants -- widen
    // the element type and the shader reading it then, not this entry point. NOT PURE: the default is
    // an unaccelerated fallback (setConstants + drawMesh per instance) so overrides keep working; only
    // D3D12RenderContext uses a real DrawIndexedInstanced.
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
    // amplification-shader thread per cluster (groups of kClusterAmplificationGroupSize) for a
    // per-cluster LOD cut. `mesh`, if alive, has its plain vertex buffer bound like dispatchMeshFor:
    // MeshletVertices are GLOBAL indices into that SAME buffer (FORMAT_SPECS 5.7 -- every LOD level
    // shares LOD 0's vertex array). The cluster arrays (MeshletDesc/Bounds/Vertices/Triangles) are NOT
    // resolved here -- a cluster CUT has no single backend-owned source, so the caller binds them
    // first (setBindingSet/setSrvBuffer). SEPARATE from dispatchMeshFor(MeshHandle), not an overload:
    // that dispatches by TRIANGLE COUNT over a flat index buffer and knows nothing about clusters; a
    // second, cluster-shaped meaning on the same function would be the silent double-duty this
    // codebase has been bitten by before. NOT PURE, same reasoning as
    // IResourceFactory::destroyBlas: MockContext has no use for cluster culling, so a new `= 0`
    // would break it.
    virtual void dispatchMeshClusters(MeshHandle mesh, u32 clusterCount) { (void)mesh; (void)clusterCount; }

    // Copies a range between buffers; both must already be in CopySource / CopyDest. The offsets
    // let several sources be CONCATENATED into one destination -- how separate meshes become the
    // single flat table a shader indexes after a ray hit. Without them this could only copy a whole
    // buffer to the start of another.
    virtual void copyBuffer(BufferHandle dst, BufferHandle src, u64 bytes,
                            u64 dstOffset = 0, u64 srcOffset = 0) = 0;

    // Copies the WHOLE of one texture into another; same CopySource/CopyDest precondition as
    // copyBuffer. Exists because the RHI could copy buffers but not textures, so a rendered target
    // was gone once the next frame reused it -- blocking asset thumbnails, cached reflections, a
    // reused UI element; the content browser wanted rendered thumbnails and could not have them for
    // exactly this reason. WHOLE-RESOURCE, NOT A REGION: a region copy needs matching subresource
    // indices/offsets/extents, more ways to be wrong for a capability nothing has asked for. The two
    // textures must agree on dimension, size, format and mip count -- checked by the backends, which
    // log and no-op rather than record a rejected copy.
    virtual void copyTexture(TextureHandle dst, TextureHandle src) = 0;

    // Copies ONE MIP of a texture into a buffer, and back. WHY: the RHI could move bytes
    // texture-to-texture and buffer-to-buffer, but had no route to the CPU or from it
    // (createTexture's initialData was in-only) -- reaching past the RHI into a backend (D3D12Device
    // ::selfTest, 2D only) or using a structured buffer instead (PcgVolume) were the only workarounds.
    // Voxi's GI derived-data cache needs a Tex3D mip chain both ways, and neither fits.
    // THE BUFFER LAYOUT IS THE BACKEND'S, NOT TIGHTLY PACKED -- ask textureCopyFootprint first and
    // repack. NOT PURE, same reasoning as drawMeshInstanced: the default no-ops.
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
    // Builds a top-level acceleration structure over `instances`, placed after the TLAS's static
    // prefix when it has one (IResourceFactory::setTlasStaticInstances).
    virtual void buildTlas(TlasHandle tlas, const TlasInstance* instances, u32 count) = 0;

    // ---- in-place updates (refit) ----
    // Both return TRUE when they updated in place and FALSE when they did a full build instead; the
    // structure is valid either way, so a caller only needs the result for its own bookkeeping (when to
    // force the next periodic full rebuild). Defaults do the full build: correct for any backend.
    //
    // refitBlas: updates a BLAS from its mesh's CURRENT vertices -- what a compute-skinned mesh needs
    // every frame, at a fraction of a full build. Refits only when the BLAS was created updatable, has
    // been built, and its mesh still has the vertex and index counts it was built with; otherwise builds.
    virtual bool refitBlas(BlasHandle blas) { buildBlas(blas); return false; }
    // refitTlas: updates a TLAS in place when `instances` (after the same filtering buildTlas applies)
    // has the SAME COUNT as this TLAS's last build or refit and every slot names the SAME BLAS with the
    // same flags and mask; transforms and instance ids may differ (a pure transform change is the
    // expected case: Voxi's mover patch lane refits every frame after rewriting a few instances'
    // worlds, and still hands over the WHOLE list -- the backend repacks it on every call). Anything else -- created
    // non-updatable, never built, a count/BLAS/flags/mask change, a static prefix set, removed or
    // dropped since -- falls back to a full build. The prefix's own slots never enter the comparison.
    // REQUIRED after any buildBlas/refitBlas of a BLAS this TLAS references: DXR and Vulkan both require
    // a TLAS to be rebuilt or updated before rays traverse it once a referenced BLAS was modified (the
    // TLAS caches each instance's bounds), so "the BLAS was rebuilt in place at the same address" is
    // NOT a reason to leave the TLAS alone.
    virtual bool refitTlas(TlasHandle tlas, const TlasInstance* instances, u32 count) {
        buildTlas(tlas, instances, count);
        return false;
    }

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

// This engine's SCOPED_GPU_STAT: opens a pushMarker region at construction, closes it via popMarker
// at destruction, so a timing scope is one line and can't be left open by an early return -- the
// failure mode a hand-written push/pop pair invites once its function grows a second exit.
// VoxiRenderer::buildAccelerationStructures had exactly one such exit (an early return for "nothing
// to build this frame") before this existed, needing popMarker() repeated by hand at both places; a
// forgotten one leaves the backend's tsOpen_ stack (D3D12Device.cpp) off by one for the rest of the
// run, so every span opened afterward inherits a parent that never closes and the frame's own
// top-level bracket ends up permanently nested one level too deep. PURE RAII: pushMarker/popMarker
// have inert
// default bodies (above) so a backend without GPU timing, or a MockContext, still takes this class
// for free. DOES DOUBLE DUTY ON PURPOSE: on D3D12 this opens both a PIX/RenderDoc region AND a
// nested GPU timestamp span in one call (D3D12RenderContext::pushMarker) -- no separate variant to
// keep in sync by hand.
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
    // `blended` marks a TRANSLUCENT draw the backend is about to capture for sorted, blend-enabled
    // replay instead of the opaque path (IDevice::setDrawBlended). Offered rather than withheld
    // because THE RIGHT ANSWER DIFFERS PER FEATURE:
    //   - RASTER should ignore it -- voxelising blocks indirect light, the shadow cascade casts a
    //     solid black shadow, the RT acceleration structure makes every reflection opaque; all worse
    //     than the surface being absent, so VoxiRenderer drops these.
    //   - A PATH TRACER must NOT ignore it -- a dielectric is the one thing it models properly
    //     (Fresnel reflection/refraction, real TIR), so PtSceneView takes these and marks the
    //     instance dielectric.
    // Withholding it was the first design and was wrong (blended draws returned from drawMesh before
    // this loop, silently dropping glass from the path tracer's scene); one flag, each feature
    // deciding, is the fix. DEFAULTED so an override that doesn't name the parameter behaves as before.
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
    // SAME FRAME's depthPrepassPipeline() already wrote via a separate depth-only draw earlier in the
    // frame (see D3D12Device::drawMesh for who sets it and why it's auto-consumed, not sticky).
    // DEFAULTED so ignoring the third argument returns the same pipeline as before. `blended` asks
    // for the SAME shading via alpha-blend with depth-write off; never combined with `depthPrepassed`
    // (a blended draw writes no depth -- setDrawBlended). Returning 0 legitimately means "no blended
    // variant": the backend DROPS the draw rather than drawing it opaque, worse and harder to
    // attribute than a missing surface. Never asked in the wireframe view: the device queues those
    // draws for EditorLines instead (IDevice::setWireframe).
    virtual PipelineHandle scenePipeline(bool meshShaders, bool depthPrepassed = false,
                                         bool blended = false) const {
        (void)meshShaders; (void)depthPrepassed; (void)blended; return 0;
    }

    // The bindless texture table the pipelines above expect bound, or 0 for pipelines that declare
    // none. The backend has to ask because a blended draw is CAPTURED and replayed by the device
    // after the deferred sky, when the feature is no longer on the stack to bind anything itself and
    // the device doesn't own the table -- it asks, same as it asks for the pipeline. Returning 0
    // costs one no-op virtual call, since setBindlessTable no-ops on a pipeline that declared none.
    virtual BindlessTableHandle sceneBindlessTable() const { return 0; }
    // The DEPTH-ONLY pipeline for a same-frame depth prepass, paired with scenePipeline(...,
    // depthPrepassed=true) for the SAME instance later: this one writes depth (test=Less, write=true,
    // matching scenePipeline()'s own default depth state), the other only tests it (LessEqual,
    // write=false). THE VERTEX TRANSFORM MUST BE BIT-IDENTICAL between the two -- build both from the
    // SAME compiled vertex shader, or the depth values disagree and drop/duplicate pixels. 0 (default)
    // means no prepass: IDevice::drawMeshDepthPrepass is then a no-op and scenePipeline is never
    // called with depthPrepassed=true.
    virtual PipelineHandle depthPrepassPipeline() const { return 0; }
    // Bindings and constants the feature's scene shaders need, applied to every scene draw.
    virtual BindingSetHandle sceneBindingSet() const { return 0; }
    virtual bool sceneConstants(const void** data, u32* bytes) const { (void)data; (void)bytes; return false; }

    // Whether a BLENDED draw (material constants `materialConstants`/`bytes`) actually SAMPLES the
    // pre-draw backdrop the backend captures for it (IDevice::sceneColorBackdropTexture) -- a decal
    // never does, a refractive/attenuating surface does. Lets the backend skip the capture (a
    // full-target MSAA resolve) for draws that can't read it. DEFAULTED true: an unknown feature, or
    // a caller with no constants to check, keeps today's always-capture behaviour.
    virtual bool blendedDrawReadsBackdrop(const void* materialConstants, u32 bytes) const {
        (void)materialConstants; (void)bytes; return true;
    }

    // Whether this feature draws the scene GEOMETRY itself, so drawMesh stands aside. Says nothing
    // about the rest of the frame -- see suppressesWholeFrame. ONLY THE FIRST CLAIMANT IN
    // REGISTRATION ORDER PAINTS (D3D12Device/VulkanDevice::beginFrame, which returns at it); two
    // features answering true is a real, silently-decided configuration. That once cost a
    // raster-vs-ray-driven comparison: PtSceneView answered true unconditionally, so turning
    // ray-driven OFF handed the frame to the path tracer instead of the rasteriser, and its 0.1 ms
    // blit measured as "the raster path" while drawMesh() was dropped. The backends now LOG the
    // collision and name the winner; a feature that cannot paint this frame should answer false
    // rather than rely on losing the race.
    virtual bool suppressesScene() const { return false; }

    // Whether the suppression above extends to EVERYTHING ELSE: sky, line draws, the transparent
    // pass. Conflating this with suppressesScene once cost the sky -- two different meanings:
    //   1. "THE FRAME IS MINE" -- a debug view painting every pixel; nothing else should run
    //      (the default).
    //   2. "THE FIRST SURFACE IS MINE" -- ray-driven primary visibility, replacing only the RASTER;
    //      sky, gizmos and particles must still draw (suppressing them once left the ray-driven
    //      viewport with no clouds, no atmosphere, no sun disc and no gizmos).
    // A feature in case 2 overrides this to false.
    virtual bool suppressesWholeFrame() const { return suppressesScene(); }

    // Draws the replacement scene, after the colour target is bound.
    virtual void scenePass(IRenderContext& ctx) { (void)ctx; }

    // Draws depth-tested, blended geometry into the SCENE colour target, after opaque drawMesh AND
    // the deferred sky (see D3D12Device::endFrame) -- moved here (DECIDED 4's own investigation) from
    // an original position BEFORE the sky, after finding that ordering erased any particle not backed
    // by an opaque occluder (the sky's own opaque, depth-EQUAL-clear fill overwrote it). THE ENGINE'S
    // FIRST DEPTH-TESTED TRANSPARENT PASS -- for particles (smoke, dust, rain, sparks) and anything
    // that must sit IN the scene against real occluders, unlike overlayPass's paste-over. A SEAM, not
    // a special case: any feature may implement it. Scene colour/depth and viewport/scissor are
    // already set (same contract as overlayPass); the PIPELINE is NOT preset -- use your own with
    // depth-write OFF (see D3D12Device::endFrame for what depth-write ON would break). DEFAULTED TO A
    // NO-OP; no shipped feature overrides it.
    virtual void transparentPass(IRenderContext& ctx) { (void)ctx; }

    // Draws onto the BACKBUFFER after the camera post chain, before the editor's own UI. The
    // backbuffer is already bound as the sole render target, viewport and scissor already set.
    // DISPLAY space: what is written is what is shown -- no exposure, tonemap or bloom follows.
    //
    // THE SCENE DEPTH IS READABLE HERE: IDevice::sceneDepthTexture() is in a shader-resource state
    // for the whole overlay stage (after the device's own line replay, before the UI), so a feature
    // can occlude against the scene by sampling it -- viewport sprites do. It is SCENE-sized, which
    // differs from width x height under a render scale: map a pixel with svPos.xy * sceneSize /
    // (width, height), taking sceneSize from the texture's own dimensions. Multisampled when
    // IDevice::sampleCount() > 1 (load sample 0 through a Texture2DMS slot). Standard depth
    // (cleared to 1, LESS), the same projection the scene used.
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
// -- this is the seam design names AverSR. The INTERFACE lives here, beside IRenderFeature; every
// implementation (Aver's own spatial resample, a vendored FSR, an empty DLSS slot) lives in a module
// that links Aver.RHI and is never linked BY it, so a renderer holds a nullable IUpscaler* and calls
// it if set, the same way it holds registered render features, without this module knowing any exist.

// What an upscaler reads besides the scene colour target. Queried ONCE by the post chain ahead of
// the scene pass, so it knows whether to pay for a motion-vector target or jittered projection
// matrix at all -- work only a temporal upscaler will ever read. Same idiom as ResourceBind: an
// implementation ORs together what it consumes.
enum class UpscalerNeeds : u32 {
    None          = 0,
    // Scene-resolution depth, same frame, same format as the scene's own depth buffer.
    Depth         = 1u << 0,
    // Scene-resolution, screen-space motion in texels/frame (RG; destination minus source texel).
    // A producer now exists -- IDevice::gBufferVelocityTexture() (RHI.hpp) writes exactly this, but
    // ONLY while setGBufferEnabled(true) (default OFF), so this flag still yields nothing on an
    // unmodified build. Wiring it into UpscalerInput::motionVectors below is NOT done here -- the
    // next step, not this one (docs/rendering/DENOISING.md) -- so FSR2/3 and DLSS still can't be
    // driven by this flag even though the data could now be produced.
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

// The seam itself, declared like IRenderFeature above: a couple of pure virtuals for identity and
// the one thing every implementation must do, defaulted hooks for what a simple one can ignore --
// letting a plain spatial resample, a future FSR2/3, and an empty DLSS slot all compile against the
// SAME interface with nothing here changing when any arrives (see docs/AVERSR.md for why the empty
// slot is deliberate: no source to integrate, and hardware/licence this repo can't use regardless).
class IUpscaler {
public:
    virtual ~IUpscaler() = default;
    virtual const char* name() const = 0;

    // Declared ONCE, not re-queried per frame: input needs are a property of the algorithm, not the
    // frame, so the renderer can decide before the scene pass runs whether to produce motion vectors
    // or a jitter offset. A plain spatial resample returns None (reads only scene colour).
    virtual UpscalerNeeds needs() const { return UpscalerNeeds::None; }

    // True for an upscaler that accumulates state across frames (reprojected history, an EMA) and
    // needs it thrown away on a cut -- camera teleport, level load, render-scale change. False (the
    // default): reset() is never called, since there's nothing to throw away.
    virtual bool isTemporal() const { return false; }
    virtual void reset() {}

    // Scene-resolution colour in `in.color`, present-resolution colour out at `outTarget`. `ctx` is
    // the SAME context the rest of the frame draws with. The CALLER has already bound `outTarget` as
    // the sole render target with viewport/scissor at (0, 0, in.dstWidth, in.dstHeight) -- the same
    // contract IRenderFeature::overlayPass uses for the backbuffer. An implementation only records
    // its own pipeline bind and draw; it does not transition `outTarget` before or after.
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
