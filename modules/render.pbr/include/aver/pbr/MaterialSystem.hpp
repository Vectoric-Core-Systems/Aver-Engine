#pragma once
#include "aver/pbr/Material.hpp"
#include "aver/pbr/MaterialGpu.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <string>
#include <unordered_map>

// The GPU residency of the material library: one binding set and one packed constant block per
// material, plus the identity textures every unset slot falls back to.
//
// Part of Aver.Render.PBR.Materials, so it may name the GENERIC RHI and nothing below it. It is not
// an rhi::IRenderFeature and never will be: a material is not a pass. A renderer asks this for the
// set and the constants a draw needs and hands them to rhi::IDevice::setDrawBinding.
namespace aver::pbr {

// A material's SRV table is exactly the glTF metallic-roughness set, in TextureSlot order, so a
// slot index IS its shader register offset and no separate mapping table can go stale.
inline constexpr u32 kMaterialSrvCount = kTextureSlotCount;
static_assert(kMaterialSrvCount <= rhi::kMaxBindingSlots,
              "a material table must fit in one binding set");

class MaterialSystem {
public:
    // A TextureRef is an authoring path or an opaque id, and resolving EITHER needs the asset
    // system — which sits a tier above this target and must not be linked from it. So the host
    // installs a resolver. Returning 0 means "not available", and the slot keeps its fallback,
    // which is a complete and correct surface rather than a black one.
    //
    // The SLOT is passed because it is the only thing that says what the pixels MEAN, and nothing in
    // an image file does: base colour and emissive are sRGB-encoded, metal-rough and occlusion are
    // linear data, and a normal map is a vector field that must be filtered as one. A resolver given
    // only the reference would have to guess, and guessing wrong is invisible — an sRGB-decoded
    // roughness map is merely a bit shinier than intended, everywhere, forever.
    //
    // OWNERSHIP: the handle returned becomes this system's. It is destroyed at shutdown(), so a
    // resolver must hand over a texture nothing else frees.
    using TextureResolver = rhi::TextureHandle (*)(const TextureRef& ref, TextureSlot slot, void* user);

    // `tableBaseRegister` is the SRV count of the consuming pipeline's table 0, i.e. the register
    // this table is based at. Carried on every set purely so the backend can catch a set bound at
    // the wrong table index — see rhi::BindingSetDesc::srvBaseRegister.
    bool init(rhi::IDevice& device, u32 tableBaseRegister);
    void shutdown();
    bool ready() const { return res_ != nullptr; }

    void setTextureResolver(TextureResolver fn, void* user) { resolve_ = fn; resolveUser_ = user; }

    // Drain MaterialLibrary::consumeDirty() and re-upload whatever changed. Once per frame, before
    // any draw: a set rewritten mid-frame is a shader-visible descriptor an in-flight frame may
    // still be reading.
    void update();

    // The set and the block a draw of `h` binds. An unknown or stale handle gets the fallback
    // material, because a draw with no valid material must still be a complete surface.
    rhi::BindingSetHandle bindingSet(MaterialHandle h);
    const MaterialConstants& constants(MaterialHandle h);

    // The identity material: white base colour, flat normal, full roughness, no metal, no emission.
    // This is what rhi::IDevice::setDefaultDrawBinding should be given, so a draw that names no
    // material renders as an untextured surface rather than inheriting the previous draw's.
    rhi::BindingSetHandle fallbackBindingSet() const { return fallbackSet_; }
    const MaterialConstants& fallbackConstants() const { return fallbackConstants_; }

    u32 textureCacheSize() const { return static_cast<u32>(cache_.size()); }

private:
    struct Entry {
        rhi::BindingSetHandle set = 0;
        MaterialConstants      constants{};
    };

    bool createFallbackTextures();
    // Fill EVERY slot: fallback where the material sets nothing. Never left to Tier 1 null-filling —
    // a null descriptor gives a view of the right DIMENSION, so behaviour is defined, but it reads
    // ZERO on most hardware and is nowhere contractually black. A material with no base-colour map
    // would render black and one with no normal map would have N = (0,0,0), which normalize() turns
    // into the NaN this project already has a TDR to its name for.
    //
    // Identity fallbacks also mean an untextured material is the SAME pipeline and the same
    // branch-free shader as a fully textured one: no permutation, no dynamic branch.
    void writeSlots(const MaterialDesc& d, rhi::BindingSetHandle set);
    rhi::TextureHandle resolveTexture(const TextureRef& ref, TextureSlot slot);
    Entry& entryFor(MaterialHandle h);

    rhi::IResourceFactory* res_ = nullptr;
    u32 tableBase_ = 0;

    // 1x1, created BEFORE any material so no set can ever be built without them. The metal-rough
    // one is (0,255,255,255) because glTF packs occlusion in R, roughness in G and metallic in B:
    // that reads roughness 1 and metallic 1, both of which multiply through their factors
    // unchanged, which is what makes "no map" and "factors only" the same code path.
    rhi::TextureHandle white_ = 0;       // base colour and occlusion
    rhi::TextureHandle flatNormal_ = 0;  // (128,128,255) — +Z in tangent space
    rhi::TextureHandle metalRough_ = 0;
    rhi::TextureHandle black_ = 0;       // emissive

    rhi::BindingSetHandle fallbackSet_ = 0;
    MaterialConstants     fallbackConstants_{};

    std::unordered_map<MaterialHandle, Entry> entries_;
    // Keyed by the REFERENCE, not by the resolved handle: two materials naming the same texture must
    // share one upload, and the id and the path are two spellings of one key (id wins when set).
    std::unordered_map<std::string, rhi::TextureHandle> cache_;

    TextureResolver resolve_ = nullptr;
    void*           resolveUser_ = nullptr;
};

} // namespace aver::pbr
