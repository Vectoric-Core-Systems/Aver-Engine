// Aver.Render.NRD -- translation between NRD's descriptors and the engine's.
//
// This is the ONLY translation unit in the engine that includes an NRD header, and that is the
// point (see NrdDenoiser.hpp for why). Everything here is bookkeeping: NRD's structures are
// stable, plain data, and the work is naming things in Aver's vocabulary and being honest about
// the places where Aver has no name.
//
// AVER_WITH_NRD is a build-system fact, not a runtime one. When it is off this file still compiles
// and every entry point fails cleanly, so a caller never needs an #if of its own -- the fallback
// path is `if (!denoiser.create(...))`, which is a path it has to have anyway.
#include <aver/render/nrd/NrdDenoiser.hpp>

#include <vector>

#if AVER_WITH_NRD
    #include <NRD.h>
#endif

namespace aver::render::nrd {

#if AVER_WITH_NRD

namespace {

// nrd::Format -> rhi::Format. Deliberately NOT exhaustive over NRD's 48 entries: an entry here is a
// claim that the engine can allocate that texture, and claiming one it cannot would move the
// failure from this table to a device-removal several frames later. Unmapped returns Unknown, which
// PoolTexture documents as "the RHI has no equivalent" and the caller reports with the name below.
rhi::Format toRhiFormat(::nrd::Format f) {
    switch (f) {
        case ::nrd::Format::R8_UNORM:              return rhi::Format::R8Unorm;
        case ::nrd::Format::RG8_UNORM:             return rhi::Format::RG8Unorm;
        case ::nrd::Format::RGBA8_UNORM:           return rhi::Format::RGBA8Unorm;
        case ::nrd::Format::RGBA8_SRGB:            return rhi::Format::RGBA8UnormSrgb;
        case ::nrd::Format::R32_SFLOAT:            return rhi::Format::R32Float;
        case ::nrd::Format::RG32_SFLOAT:           return rhi::Format::RG32Float;
        case ::nrd::Format::R32_UINT:              return rhi::Format::R32Uint;
        case ::nrd::Format::RG16_SFLOAT:           return rhi::Format::RG16F;
        case ::nrd::Format::RGBA16_SFLOAT:         return rhi::Format::RGBA16F;
        case ::nrd::Format::R10_G10_B10_A2_UNORM:  return rhi::Format::RGB10A2Unorm;
        // REBLUR's own pool formats. rhi::Format gained these two FOR this table -- see the note on
        // the enum in modules/rhi/include/aver/rhi/RHIResources.hpp.
        case ::nrd::Format::R16_UNORM:             return rhi::Format::R16Unorm;
        case ::nrd::Format::R16_SFLOAT:            return rhi::Format::R16F;
        case ::nrd::Format::R8_UINT:               return rhi::Format::R8Uint;
        case ::nrd::Format::R16_UINT:              return rhi::Format::R16Uint;
        default:                                   return rhi::Format::Unknown;
    }
}

// For the diagnostic in PoolTexture. NRD's own GetResourceTypeString has no format counterpart, so
// the few names a report would need are spelled out; everything else is identified by number,
// which is enough to look up in NRDDescs.h.
const char* nrdFormatName(::nrd::Format f) {
    switch (f) {
        case ::nrd::Format::R8_UNORM:              return "R8_UNORM";
        case ::nrd::Format::R8_SNORM:              return "R8_SNORM";
        case ::nrd::Format::R8_UINT:               return "R8_UINT";
        case ::nrd::Format::RG8_UNORM:             return "RG8_UNORM";
        case ::nrd::Format::RG8_SNORM:             return "RG8_SNORM";
        case ::nrd::Format::RGBA8_UNORM:           return "RGBA8_UNORM";
        case ::nrd::Format::RGBA8_SNORM:           return "RGBA8_SNORM";
        case ::nrd::Format::RGBA8_SRGB:            return "RGBA8_SRGB";
        case ::nrd::Format::R16_UNORM:             return "R16_UNORM";
        case ::nrd::Format::R16_SNORM:             return "R16_SNORM";
        case ::nrd::Format::R16_UINT:              return "R16_UINT";
        case ::nrd::Format::R16_SFLOAT:            return "R16_SFLOAT";
        case ::nrd::Format::RG16_UNORM:            return "RG16_UNORM";
        case ::nrd::Format::RG16_SNORM:            return "RG16_SNORM";
        case ::nrd::Format::RG16_UINT:             return "RG16_UINT";
        case ::nrd::Format::RG16_SFLOAT:           return "RG16_SFLOAT";
        case ::nrd::Format::RGBA16_UNORM:          return "RGBA16_UNORM";
        case ::nrd::Format::RGBA16_SNORM:          return "RGBA16_SNORM";
        case ::nrd::Format::RGBA16_UINT:           return "RGBA16_UINT";
        case ::nrd::Format::RGBA16_SFLOAT:         return "RGBA16_SFLOAT";
        case ::nrd::Format::R32_UINT:              return "R32_UINT";
        case ::nrd::Format::R32_SFLOAT:            return "R32_SFLOAT";
        case ::nrd::Format::RG32_UINT:             return "RG32_UINT";
        case ::nrd::Format::RG32_SFLOAT:           return "RG32_SFLOAT";
        case ::nrd::Format::RGBA32_UINT:           return "RGBA32_UINT";
        case ::nrd::Format::RGBA32_SFLOAT:         return "RGBA32_SFLOAT";
        case ::nrd::Format::R10_G10_B10_A2_UNORM:  return "R10_G10_B10_A2_UNORM";
        case ::nrd::Format::R11_G11_B10_UFLOAT:    return "R11_G11_B10_UFLOAT";
        case ::nrd::Format::R9_G9_B9_E5_UFLOAT:    return "R9_G9_B9_E5_UFLOAT";
        default:                                   return "<nrd::Format>";
    }
}

::nrd::Denoiser toNrdDenoiser(DenoiserKind k) {
    switch (k) {
        case DenoiserKind::ReblurDiffuseOcclusion: return ::nrd::Denoiser::REBLUR_DIFFUSE_OCCLUSION;
        case DenoiserKind::ReblurDiffuse:          return ::nrd::Denoiser::REBLUR_DIFFUSE;
        case DenoiserKind::ReblurSpecular:         return ::nrd::Denoiser::REBLUR_SPECULAR;
        case DenoiserKind::SigmaShadow:            return ::nrd::Denoiser::SIGMA_SHADOW;
    }
    return ::nrd::Denoiser::REBLUR_DIFFUSE_OCCLUSION;
}

SlotRole toSlotRole(::nrd::ResourceType t) {
    switch (t) {
        case ::nrd::ResourceType::IN_VIEWZ:                 return SlotRole::InViewZ;
        case ::nrd::ResourceType::IN_MV:                    return SlotRole::InMotionVectors;
        case ::nrd::ResourceType::IN_NORMAL_ROUGHNESS:      return SlotRole::InNormalRoughness;
        case ::nrd::ResourceType::IN_DIFF_HITDIST:          return SlotRole::InDiffuseHitDistance;
        case ::nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:  return SlotRole::InDiffuseRadianceHitDistance;
        case ::nrd::ResourceType::IN_PENUMBRA:              return SlotRole::InPenumbra;
        case ::nrd::ResourceType::IN_TRANSLUCENCY:          return SlotRole::InTranslucency;
        case ::nrd::ResourceType::OUT_DIFF_HITDIST:         return SlotRole::OutDiffuseHitDistance;
        case ::nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST: return SlotRole::OutDiffuseRadianceHitDistance;
        case ::nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY:  return SlotRole::OutShadowTranslucency;
        case ::nrd::ResourceType::PERMANENT_POOL:           return SlotRole::PermanentPool;
        case ::nrd::ResourceType::TRANSIENT_POOL:           return SlotRole::TransientPool;
        default:                                            return SlotRole::Other;
    }
}

ResourceClass toResourceClass(::nrd::DescriptorType d) {
    return d == ::nrd::DescriptorType::STORAGE_TEXTURE ? ResourceClass::StorageTexture
                                                       : ResourceClass::Texture;
}

}  // namespace

// The translated views. Vectors rather than spans into NRD's own memory because the shapes differ:
// NRD's ResourceDesc carries a descriptor type and an index-in-pool, Aver's SlotBinding carries a
// role. Rebuilt in place on every call so the allocation is paid once.
struct Denoiser::Scratch {
    std::vector<PipelineInfo>  pipelines;
    std::vector<ResourceRange> ranges;      // flat; PipelineInfo::ranges points into it
    std::vector<size_t>        rangeOffsets;
    std::vector<PoolTexture>   permanent;
    std::vector<PoolTexture>   transient;
    std::vector<Dispatch>      dispatches;
    std::vector<SlotBinding>   bindings;    // flat; Dispatch::bindings points into it
    std::vector<size_t>        bindingOffsets;
};

Denoiser::~Denoiser() { destroy(); }

bool Denoiser::create(const DenoiserKind* kinds, u32 count) {
    destroy();
    if (kinds == nullptr || count == 0) {
        return false;
    }

    std::vector<::nrd::DenoiserDesc> descs(count);
    for (u32 i = 0; i < count; ++i) {
        descs[i].identifier = static_cast<::nrd::Identifier>(i);
        descs[i].denoiser   = toNrdDenoiser(kinds[i]);
    }

    ::nrd::InstanceCreationDesc desc{};
    desc.denoisers    = descs.data();
    desc.denoisersNum = count;

    ::nrd::Instance* inst = nullptr;
    if (::nrd::CreateInstance(desc, inst) != ::nrd::Result::SUCCESS) {
        return false;
    }
    instance_ = inst;
    scratch_  = new Scratch();
    return true;
}

void Denoiser::destroy() {
    if (instance_ != nullptr) {
        ::nrd::DestroyInstance(*static_cast<::nrd::Instance*>(instance_));
        instance_ = nullptr;
    }
    delete scratch_;
    scratch_ = nullptr;
}

InstanceLayout Denoiser::layout() {
    InstanceLayout out{};
    if (!valid()) {
        return out;
    }
    const ::nrd::InstanceDesc& d = *::nrd::GetInstanceDesc(*static_cast<::nrd::Instance*>(instance_));
    Scratch& s = *scratch_;

    // TWO PASSES OVER THE RANGES, and the reason is pointer stability: PipelineInfo::ranges points
    // into s.ranges, so the vector must not reallocate after the first pointer is taken. Counting
    // first and reserving exactly is cheaper than a vector-of-vectors and cannot dangle.
    u32 rangeTotal = 0;
    for (u32 i = 0; i < d.pipelinesNum; ++i) {
        rangeTotal += d.pipelines[i].resourceRangesNum;
    }
    s.ranges.clear();
    s.ranges.reserve(rangeTotal);
    s.rangeOffsets.clear();
    s.rangeOffsets.reserve(d.pipelinesNum);
    s.pipelines.clear();
    s.pipelines.reserve(d.pipelinesNum);

    for (u32 i = 0; i < d.pipelinesNum; ++i) {
        const ::nrd::PipelineDesc& p = d.pipelines[i];
        s.rangeOffsets.push_back(s.ranges.size());
        for (u32 r = 0; r < p.resourceRangesNum; ++r) {
            s.ranges.push_back({toResourceClass(p.resourceRanges[r].descriptorType),
                                p.resourceRanges[r].descriptorsNum});
        }
        PipelineInfo info{};
        info.dxil            = {static_cast<const u8*>(p.computeShaderDXIL.bytecode),
                                p.computeShaderDXIL.size};
        info.spirv           = {static_cast<const u8*>(p.computeShaderSPIRV.bytecode),
                                p.computeShaderSPIRV.size};
        info.rangeCount      = p.resourceRangesNum;
        info.hasConstantData = p.hasConstantData;
        info.debugName       = p.shaderIdentifier;
        s.pipelines.push_back(info);
    }
    // Offsets, not pointers, until here: s.ranges grew during the loop above and every push_back
    // could have reallocated it. Taking the address only once the vector is final is the whole
    // reason the offsets exist.
    for (size_t i = 0; i < s.pipelines.size(); ++i) {
        s.pipelines[i].ranges = s.ranges.data() + s.rangeOffsets[i];
    }

    auto fillPool = [](std::vector<PoolTexture>& dst, const ::nrd::TextureDesc* src, u32 n) {
        dst.clear();
        dst.reserve(n);
        for (u32 i = 0; i < n; ++i) {
            dst.push_back({toRhiFormat(src[i].format), src[i].downsampleFactor,
                           nrdFormatName(src[i].format)});
        }
    };
    fillPool(s.permanent, d.permanentPool, d.permanentPoolSize);
    fillPool(s.transient, d.transientPool, d.transientPoolSize);

    out.pipelines         = s.pipelines.data();
    out.pipelineCount     = static_cast<u32>(s.pipelines.size());
    out.permanentPool     = s.permanent.data();
    out.permanentPoolSize = static_cast<u32>(s.permanent.size());
    out.transientPool     = s.transient.data();
    out.transientPoolSize = static_cast<u32>(s.transient.size());

    out.binding.constantBufferAndSamplersSpace = d.constantBufferAndSamplersSpaceIndex;
    out.binding.resourcesSpace                 = d.resourcesSpaceIndex;
    out.binding.constantBufferRegister         = d.constantBufferRegisterIndex;
    out.binding.samplersBaseRegister           = d.samplersBaseRegisterIndex;
    out.binding.resourcesBaseRegister          = d.resourcesBaseRegisterIndex;
    out.binding.constantBufferMaxDataSize      = d.constantBufferMaxDataSize;
    out.binding.entryPoint                     = d.shaderEntryPoint;
    const u32 samplerMax = static_cast<u32>(sizeof(out.binding.samplers) / sizeof(out.binding.samplers[0]));
    out.binding.samplerCount = d.samplersNum < samplerMax ? d.samplersNum : samplerMax;
    for (u32 i = 0; i < out.binding.samplerCount; ++i) {
        out.binding.samplers[i] = d.samplers[i] == ::nrd::Sampler::LINEAR_CLAMP
                                      ? SamplerKind::LinearClamp
                                      : SamplerKind::NearestClamp;
    }
    return out;
}

bool Denoiser::setFrameSettings(const FrameSettings& s) {
    if (!valid()) {
        return false;
    }
    ::nrd::CommonSettings c{};
    for (u32 i = 0; i < 16; ++i) {
        c.viewToClipMatrix[i]      = s.viewToClip[i];
        c.viewToClipMatrixPrev[i]  = s.viewToClipPrev[i];
        c.worldToViewMatrix[i]     = s.worldToView[i];
        c.worldToViewMatrixPrev[i] = s.worldToViewPrev[i];
    }
    for (u32 i = 0; i < 3; ++i) {
        c.motionVectorScale[i] = s.motionVectorScale[i];
    }
    c.cameraJitter[0]     = s.cameraJitter[0];
    c.cameraJitter[1]     = s.cameraJitter[1];
    c.cameraJitterPrev[0] = s.cameraJitterPrev[0];
    c.cameraJitterPrev[1] = s.cameraJitterPrev[1];

    // RESOURCE SIZE AND RECT SIZE ARE NOT THE SAME THING and conflating them is how a denoiser
    // reads outside the rendered region under dynamic resolution. resourceSize is what was
    // ALLOCATED; rectSize is what was RENDERED into this frame. A caller that leaves rect at zero
    // means "the whole thing", which is the common case and is filled in here rather than left for
    // NRD to reject.
    const u32 rectW = s.rectWidth != 0 ? s.rectWidth : s.resourceWidth;
    const u32 rectH = s.rectHeight != 0 ? s.rectHeight : s.resourceHeight;
    c.resourceSize[0] = static_cast<uint16_t>(s.resourceWidth);
    c.resourceSize[1] = static_cast<uint16_t>(s.resourceHeight);
    c.rectSize[0]     = static_cast<uint16_t>(rectW);
    c.rectSize[1]     = static_cast<uint16_t>(rectH);
    // PREV sizes: this seam does not track history, so the honest answer for a caller that has not
    // told us otherwise is "the same as now". A resolution change is signalled by resetHistory.
    c.resourceSizePrev[0] = c.resourceSize[0];
    c.resourceSizePrev[1] = c.resourceSize[1];
    c.rectSizePrev[0]     = c.rectSize[0];
    c.rectSizePrev[1]     = c.rectSize[1];

    c.denoisingRange           = s.denoisingRange;
    c.timeDeltaBetweenFrames   = s.timeDeltaSeconds;
    c.frameIndex               = s.frameIndex;
    c.accumulationMode         = s.resetHistory ? ::nrd::AccumulationMode::CLEAR_AND_RESTART
                                                : ::nrd::AccumulationMode::CONTINUE;
    c.isMotionVectorInWorldSpace = s.motionVectorsAreWorldSpace;

    return ::nrd::SetCommonSettings(*static_cast<::nrd::Instance*>(instance_), c)
           == ::nrd::Result::SUCCESS;
}

// See ReblurTuning in the header for why hitDistanceParameters.A must be restated in engine units
// and for the shader-side mirror that has to agree with it.
bool Denoiser::setReblurTuning(u32 denoiserIndex, const ReblurTuning& s) {
    if (!valid()) {
        return false;
    }
    // DEFAULT-CONSTRUCTED FIRST, then only the fields this seam names are overwritten: nrd::
    // ReblurSettings carries roughly twenty tuned fields and SetDenoiserSettings replaces the whole
    // block, so building it from {} is what keeps the other nineteen at NVIDIA's values instead of
    // silently zeroing them.
    ::nrd::ReblurSettings r{};
    r.hitDistanceParameters.A = s.hitDistA;
    r.hitDistanceParameters.B = s.hitDistB;
    r.hitDistanceParameters.C = s.hitDistC;
    r.enableAntiFirefly       = s.enableAntiFirefly;
    r.diffusePrepassBlurRadius = s.diffusePrepassBlurRadius;
    r.maxStabilizedFrameNum    = s.maxStabilizedFrameNum;
    r.maxAccumulatedFrameNum   = s.maxAccumulatedFrameNum;
    return ::nrd::SetDenoiserSettings(*static_cast<::nrd::Instance*>(instance_),
                                      static_cast<::nrd::Identifier>(denoiserIndex), &r)
           == ::nrd::Result::SUCCESS;
}

bool Denoiser::dispatches(const u32* denoiserIndices, u32 indexCount,
                          const Dispatch*& out, u32& outCount) {
    out      = nullptr;
    outCount = 0;
    if (!valid() || denoiserIndices == nullptr || indexCount == 0) {
        return false;
    }

    std::vector<::nrd::Identifier> ids(indexCount);
    for (u32 i = 0; i < indexCount; ++i) {
        ids[i] = static_cast<::nrd::Identifier>(denoiserIndices[i]);
    }

    const ::nrd::DispatchDesc* descs = nullptr;
    uint32_t                   n     = 0;
    if (::nrd::GetComputeDispatches(*static_cast<::nrd::Instance*>(instance_), ids.data(),
                                    indexCount, descs, n)
        != ::nrd::Result::SUCCESS) {
        return false;
    }

    Scratch& s = *scratch_;
    u32 bindingTotal = 0;
    for (u32 i = 0; i < n; ++i) {
        bindingTotal += descs[i].resourcesNum;
    }
    s.bindings.clear();
    s.bindings.reserve(bindingTotal);
    s.bindingOffsets.clear();
    s.bindingOffsets.reserve(n);
    s.dispatches.clear();
    s.dispatches.reserve(n);

    for (u32 i = 0; i < n; ++i) {
        const ::nrd::DispatchDesc& d = descs[i];
        s.bindingOffsets.push_back(s.bindings.size());
        for (u32 r = 0; r < d.resourcesNum; ++r) {
            const ::nrd::ResourceDesc& res = d.resources[r];
            s.bindings.push_back({toResourceClass(res.descriptorType), toSlotRole(res.type),
                                  res.indexInPool, static_cast<u32>(res.type)});
        }
        Dispatch dp{};
        dp.name               = d.name;
        dp.bindingCount       = d.resourcesNum;
        dp.constants          = d.constantBufferData;
        dp.constantsSize      = d.constantBufferDataSize;
        dp.constantsUnchanged = d.constantBufferDataMatchesPreviousDispatch;
        dp.pipelineIndex      = d.pipelineIndex;
        dp.groupsX            = d.gridWidth;
        dp.groupsY            = d.gridHeight;
        s.dispatches.push_back(dp);
    }
    for (size_t i = 0; i < s.dispatches.size(); ++i) {
        s.dispatches[i].bindings = s.bindings.data() + s.bindingOffsets[i];
    }

    out      = s.dispatches.data();
    outCount = static_cast<u32>(s.dispatches.size());
    return true;
}

bool Denoiser::available() { return true; }

// Stringified from the header this file was compiled against, which is the number that matters: it
// is where every struct layout, every encoding and the whole ABI below came from. NRD exposes the
// same three numbers at runtime through LibraryDesc, and NrdLinkTest checks the two agree -- a
// mismatch means a stale static library, which is otherwise completely silent.
#define AVER_NRD_STR2(x) #x
#define AVER_NRD_STR(x)  AVER_NRD_STR2(x)

const char* Denoiser::versionString() {
    return "NRD " AVER_NRD_STR(NRD_VERSION_MAJOR) "." AVER_NRD_STR(NRD_VERSION_MINOR) "."
           AVER_NRD_STR(NRD_VERSION_BUILD);
}

bool Denoiser::libraryVersion(u32& major, u32& minor, u32& build) {
    const ::nrd::LibraryDesc* d = ::nrd::GetLibraryDesc();
    if (d == nullptr) {
        return false;
    }
    major = d->versionMajor;
    minor = d->versionMinor;
    build = d->versionBuild;
    return true;
}

bool Denoiser::encodings(u32& normalEncoding, u32& roughnessEncoding) {
    const ::nrd::LibraryDesc* d = ::nrd::GetLibraryDesc();
    if (d == nullptr) {
        return false;
    }
    normalEncoding    = static_cast<u32>(d->normalEncoding);
    roughnessEncoding = static_cast<u32>(d->roughnessEncoding);
    return true;
}

#else  // !AVER_WITH_NRD

Denoiser::~Denoiser() = default;
bool Denoiser::create(const DenoiserKind*, u32) { return false; }
void Denoiser::destroy() {}
InstanceLayout Denoiser::layout() { return {}; }
bool Denoiser::setFrameSettings(const FrameSettings&) { return false; }
bool Denoiser::setReblurTuning(u32, const ReblurTuning&) { return false; }
bool Denoiser::dispatches(const u32*, u32, const Dispatch*& out, u32& outCount) {
    out = nullptr;
    outCount = 0;
    return false;
}
bool Denoiser::available() { return false; }
const char* Denoiser::versionString() { return nullptr; }
bool Denoiser::libraryVersion(u32&, u32&, u32&) { return false; }
bool Denoiser::encodings(u32&, u32&) { return false; }

#endif

}  // namespace aver::render::nrd
