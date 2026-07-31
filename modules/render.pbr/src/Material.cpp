// The material library's storage — slots, generations, sanitising — and the C ABI over it.
#include "aver/pbr/Material.hpp"
#include "aver/pbr/pbr_abi.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace aver::pbr {

// One slot per index ever handed out, recycled through a free list with the generation bumped.
struct MaterialLibrary::Impl {
    // One material's storage.
    struct Slot {
        MaterialDesc desc{};
        u32  generation = 1;   // never 0: that is what keeps a live handle non-zero
        bool live = false;
        bool dirty = false;
    };
    std::vector<Slot> slots;
    std::vector<u32>  freeIndices;
    std::vector<MaterialHandle> liveHandles;   // dense, for the editor's enumeration
};

namespace {

// Clamps every scalar the shading model divides by or raises to a power.
void sanitise(MaterialDesc& d) {
    for (f32& c : d.baseColorFactor) c = std::clamp(c, 0.0f, 1.0f);
    for (f32& c : d.emissiveFactor)  c = std::max(c, 0.0f);   // emissive is radiance, not a ratio
    d.metallicFactor    = std::clamp(d.metallicFactor, 0.0f, 1.0f);
    // Not clamped to 0: a perfect mirror collapses the GGX denominator. The floor matches
    // shadeSurface's, so the authored value and the shaded value agree.
    d.roughnessFactor   = std::clamp(d.roughnessFactor, 0.045f, 1.0f);
    d.normalScale       = std::clamp(d.normalScale, 0.0f, 8.0f);
    d.occlusionStrength = std::clamp(d.occlusionStrength, 0.0f, 1.0f);
    // Both are reflectances. F0 above 1 makes (f90 - F0) negative in the Schlick term.
    d.reflectance       = std::clamp(d.reflectance, 0.0f, 1.0f);
    d.f90               = std::clamp(d.f90, 0.0f, 1.0f);
    d.alphaCutoff       = std::clamp(d.alphaCutoff, 0.0f, 1.0f);
    if (static_cast<u32>(d.alphaMode) > static_cast<u32>(AlphaMode::Blend)) d.alphaMode = AlphaMode::Opaque;
}

// Removes one handle from the live list.
void eraseHandle(std::vector<MaterialHandle>& v, MaterialHandle h) {
    const auto it = std::find(v.begin(), v.end(), h);
    if (it != v.end()) v.erase(it);
}

} // namespace

MaterialLibrary::MaterialLibrary() : impl_(new Impl) {}
MaterialLibrary::~MaterialLibrary() { delete impl_; }

// The process-wide library.
MaterialLibrary& MaterialLibrary::get() {
    static MaterialLibrary inst;
    return inst;
}

// Creates a material from a description. 0 when the index space is exhausted.
MaterialHandle MaterialLibrary::create(const MaterialDesc& desc) {
    u32 index;
    if (!impl_->freeIndices.empty()) {
        index = impl_->freeIndices.back();
        impl_->freeIndices.pop_back();
    } else {
        if (impl_->slots.size() > kMaterialIndexMask) return 0;   // index space exhausted
        index = static_cast<u32>(impl_->slots.size());
        impl_->slots.emplace_back();
    }

    Impl::Slot& s = impl_->slots[index];
    s.desc = desc;
    sanitise(s.desc);
    s.live = true;
    s.dirty = true;   // never uploaded, so the first consumer owes it an upload

    const MaterialHandle h = makeMaterialHandle(index, s.generation);
    impl_->liveHandles.push_back(h);
    return h;
}

// True while `h` still names a live material.
bool MaterialLibrary::valid(MaterialHandle h) const {
    if (h == 0) return false;
    const u32 i = materialIndex(h);
    if (i >= impl_->slots.size()) return false;
    const Impl::Slot& s = impl_->slots[i];
    return s.live && s.generation == materialGeneration(h);
}

// Destroys a material and frees its slot for reuse.
bool MaterialLibrary::destroy(MaterialHandle h) {
    if (!valid(h)) return false;
    Impl::Slot& s = impl_->slots[materialIndex(h)];
    s.live = false;
    s.desc = MaterialDesc{};
    // Skip generation 0: it would make the handle for index 0 equal the invalid handle.
    s.generation = (s.generation + 1) & kMaterialGenerationMask;
    if (s.generation == 0) s.generation = 1;
    impl_->freeIndices.push_back(materialIndex(h));
    eraseHandle(impl_->liveHandles, h);
    return true;
}

// Reads a material, or nullptr for a stale handle.
const MaterialDesc* MaterialLibrary::desc(MaterialHandle h) const {
    return valid(h) ? &impl_->slots[materialIndex(h)].desc : nullptr;
}

// A mutable material, or nullptr for a stale handle. The caller must call touch() afterwards.
MaterialDesc* MaterialLibrary::mutableDesc(MaterialHandle h) {
    return valid(h) ? &impl_->slots[materialIndex(h)].desc : nullptr;
}

// Replaces the whole description and marks the material dirty.
bool MaterialLibrary::update(MaterialHandle h, const MaterialDesc& d) {
    if (!valid(h)) return false;
    Impl::Slot& s = impl_->slots[materialIndex(h)];
    s.desc = d;
    sanitise(s.desc);
    s.dirty = true;
    return true;
}

// Sanitises a material edited in place and marks it dirty.
void MaterialLibrary::touch(MaterialHandle h) {
    if (!valid(h)) return;
    Impl::Slot& s = impl_->slots[materialIndex(h)];
    sanitise(s.desc);
    s.dirty = true;
}

// True when this material still owes the GPU an upload. Reading it clears it.
bool MaterialLibrary::consumeDirty(MaterialHandle h) {
    if (!valid(h)) return false;
    Impl::Slot& s = impl_->slots[materialIndex(h)];
    const bool d = s.dirty;
    s.dirty = false;
    return d;
}

// How many materials are live.
u32 MaterialLibrary::count() const { return static_cast<u32>(impl_->liveHandles.size()); }

// The handle at a live index, or 0.
MaterialHandle MaterialLibrary::at(u32 i) const {
    return i < impl_->liveHandles.size() ? impl_->liveHandles[i] : 0;
}

// How far along a feature is.
Status MaterialLibrary::status(Feature f) {
    switch (f) {
        // Authored here and consumed: averEvalMaterial() reads the b2 block, samples the five maps
        // through the material table, and clips on the mask itself.
        case Feature::Factors:
        case Feature::BaseColorMap:
        case Feature::MetalRoughMap:
        case Feature::NormalMap:
        case Feature::OcclusionMap:
        case Feature::EmissiveMap:
        case Feature::AlphaMask:
            return Status::Ready;
        // Stored, not rendered: blending needs a blend state and a back-to-front draw order, both
        // of which belong to the renderer.
        case Feature::AlphaBlend:
            return Status::NotImplemented;
        default:
            return Status::Unsupported;
    }
}

// The human sentence for a feature's status.
const char* MaterialLibrary::statusText(Feature f) {
    switch (status(f)) {
        case Status::Ready:          return "Ready";
        case Status::NotImplemented: return "Authored and stored; no renderer consumes it yet";
        case Status::Unsupported:    return "Not a material feature";
        default:                     return "Unknown";
    }
}

// The human name of a feature.
const char* MaterialLibrary::featureName(Feature f) {
    switch (f) {
        case Feature::Factors:        return "Factors";
        case Feature::BaseColorMap:   return "Base Colour Map";
        case Feature::MetalRoughMap:  return "Metallic-Roughness Map";
        case Feature::NormalMap:      return "Normal Map";
        case Feature::OcclusionMap:   return "Occlusion Map";
        case Feature::EmissiveMap:    return "Emissive Map";
        case Feature::AlphaMask:      return "Alpha Mask";
        case Feature::AlphaBlend:     return "Alpha Blend";
        default: return "?";
    }
}

// The `.ocmat` TEX name of a slot.
const char* MaterialLibrary::textureSlotName(TextureSlot s) {
    switch (s) {
        case TextureSlot::BaseColor:  return "baseColor";
        case TextureSlot::MetalRough: return "metalRough";
        case TextureSlot::Normal:     return "normal";
        case TextureSlot::Occlusion:  return "occlusion";
        case TextureSlot::Emissive:   return "emissive";
        default: return "?";
    }
}

// The `.ocmat` BLEND name of an alpha mode.
const char* MaterialLibrary::alphaModeName(AlphaMode m) {
    switch (m) {
        case AlphaMode::Opaque: return "opaque";
        case AlphaMode::Mask:   return "masked";
        case AlphaMode::Blend:  return "translucent";
        default: return "?";
    }
}

} // namespace aver::pbr

// ------------------------------------------------------------------ C ABI
using aver::pbr::AlphaMode;
using aver::pbr::Feature;
using aver::pbr::MaterialDesc;
using aver::pbr::MaterialHandle;
using aver::pbr::MaterialLibrary;
using aver::pbr::TextureSlot;

namespace {

// True for a feature id the ABI declares.
bool validFeature(int32_t f) { return f >= 0 && f < AVER_PBR_FEATURE_COUNT; }
// True for a texture slot id the ABI declares.
bool validSlot(int32_t s)    { return s >= 0 && s < AVER_PBR_TEX_COUNT; }

// The library handle for an ABI handle. A negative one is rejected, not reinterpreted.
MaterialHandle handleOf(int32_t m) {
    return m > 0 ? static_cast<MaterialHandle>(m) : 0;
}

MaterialDesc* edit(int32_t m) { return MaterialLibrary::get().mutableDesc(handleOf(m)); }
const MaterialDesc* read(int32_t m) { return MaterialLibrary::get().desc(handleOf(m)); }

// How every setter ends: sanitise and mark dirty exactly once.
int32_t commit(int32_t m) { MaterialLibrary::get().touch(handleOf(m)); return 1; }

} // namespace

extern "C" {

// ---- feature introspection ----
int32_t aver_pbr_feature_count(void) { return AVER_PBR_FEATURE_COUNT; }

const char* aver_pbr_feature_name(int32_t f) {
    return validFeature(f) ? MaterialLibrary::featureName(static_cast<Feature>(f)) : "?";
}
int32_t aver_pbr_status(int32_t f) {
    return validFeature(f) ? static_cast<int32_t>(MaterialLibrary::status(static_cast<Feature>(f)))
                           : AVER_PBR_STATUS_UNSUPPORTED;
}
const char* aver_pbr_status_text(int32_t f) {
    return validFeature(f) ? MaterialLibrary::statusText(static_cast<Feature>(f)) : "?";
}
const char* aver_pbr_texture_slot_name(int32_t s) {
    return validSlot(s) ? MaterialLibrary::textureSlotName(static_cast<TextureSlot>(s)) : "?";
}
const char* aver_pbr_alpha_mode_name(int32_t mode) {
    return (mode >= AVER_PBR_ALPHA_OPAQUE && mode <= AVER_PBR_ALPHA_BLEND)
               ? MaterialLibrary::alphaModeName(static_cast<AlphaMode>(mode)) : "?";
}

// ---- lifetime and enumeration ----
aver_pbr_material aver_pbr_create(const char* name) {
    MaterialDesc d;
    if (name) d.name = name;
    const MaterialHandle h = MaterialLibrary::get().create(d);
    return static_cast<aver_pbr_material>(h);
}
int32_t aver_pbr_destroy(aver_pbr_material m) { return MaterialLibrary::get().destroy(handleOf(m)) ? 1 : 0; }
int32_t aver_pbr_valid(aver_pbr_material m)   { return MaterialLibrary::get().valid(handleOf(m)) ? 1 : 0; }

int32_t aver_pbr_count(void) { return static_cast<int32_t>(MaterialLibrary::get().count()); }
aver_pbr_material aver_pbr_at(int32_t index) {
    return index < 0 ? 0 : static_cast<aver_pbr_material>(MaterialLibrary::get().at(static_cast<aver::u32>(index)));
}

// ---- identity ----
const char* aver_pbr_get_name(aver_pbr_material m) {
    const MaterialDesc* d = read(m);
    return d ? d->name.c_str() : "";
}
int32_t aver_pbr_set_name(aver_pbr_material m, const char* name) {
    MaterialDesc* d = edit(m);
    if (!d || !name) return 0;
    d->name = name;
    return commit(m);
}

// ---- factors ----
int32_t aver_pbr_get_base_color_factor(aver_pbr_material m, float* out4) {
    const MaterialDesc* d = read(m);
    if (!d || !out4) return 0;
    std::memcpy(out4, d->baseColorFactor, sizeof(d->baseColorFactor));
    return 1;
}
int32_t aver_pbr_set_base_color_factor(aver_pbr_material m, float r, float g, float b, float a) {
    MaterialDesc* d = edit(m);
    if (!d) return 0;
    d->baseColorFactor[0] = r; d->baseColorFactor[1] = g; d->baseColorFactor[2] = b; d->baseColorFactor[3] = a;
    return commit(m);
}
int32_t aver_pbr_get_emissive_factor(aver_pbr_material m, float* out3) {
    const MaterialDesc* d = read(m);
    if (!d || !out3) return 0;
    std::memcpy(out3, d->emissiveFactor, sizeof(d->emissiveFactor));
    return 1;
}
int32_t aver_pbr_set_emissive_factor(aver_pbr_material m, float r, float g, float b) {
    MaterialDesc* d = edit(m);
    if (!d) return 0;
    d->emissiveFactor[0] = r; d->emissiveFactor[1] = g; d->emissiveFactor[2] = b;
    return commit(m);
}

float aver_pbr_get_metallic_factor(aver_pbr_material m) {
    const MaterialDesc* d = read(m); return d ? d->metallicFactor : 0.0f;
}
int32_t aver_pbr_set_metallic_factor(aver_pbr_material m, float v) {
    MaterialDesc* d = edit(m); if (!d) return 0; d->metallicFactor = v; return commit(m);
}
float aver_pbr_get_roughness_factor(aver_pbr_material m) {
    const MaterialDesc* d = read(m); return d ? d->roughnessFactor : 0.0f;
}
int32_t aver_pbr_set_roughness_factor(aver_pbr_material m, float v) {
    MaterialDesc* d = edit(m); if (!d) return 0; d->roughnessFactor = v; return commit(m);
}
float aver_pbr_get_normal_scale(aver_pbr_material m) {
    const MaterialDesc* d = read(m); return d ? d->normalScale : 0.0f;
}
int32_t aver_pbr_set_normal_scale(aver_pbr_material m, float v) {
    MaterialDesc* d = edit(m); if (!d) return 0; d->normalScale = v; return commit(m);
}
float aver_pbr_get_reflectance(aver_pbr_material m) {
    const MaterialDesc* d = read(m); return d ? d->reflectance : 0.0f;
}
int32_t aver_pbr_set_reflectance(aver_pbr_material m, float v) {
    MaterialDesc* d = edit(m); if (!d) return 0; d->reflectance = v; return commit(m);
}
float aver_pbr_get_f90(aver_pbr_material m) {
    const MaterialDesc* d = read(m); return d ? d->f90 : 0.0f;
}
int32_t aver_pbr_set_f90(aver_pbr_material m, float v) {
    MaterialDesc* d = edit(m); if (!d) return 0; d->f90 = v; return commit(m);
}
float aver_pbr_get_occlusion_strength(aver_pbr_material m) {
    const MaterialDesc* d = read(m); return d ? d->occlusionStrength : 0.0f;
}
int32_t aver_pbr_set_occlusion_strength(aver_pbr_material m, float v) {
    MaterialDesc* d = edit(m); if (!d) return 0; d->occlusionStrength = v; return commit(m);
}

// ---- blending and sidedness ----
int32_t aver_pbr_get_alpha_mode(aver_pbr_material m) {
    const MaterialDesc* d = read(m); return d ? static_cast<int32_t>(d->alphaMode) : AVER_PBR_ALPHA_OPAQUE;
}
int32_t aver_pbr_set_alpha_mode(aver_pbr_material m, int32_t mode) {
    if (mode < AVER_PBR_ALPHA_OPAQUE || mode > AVER_PBR_ALPHA_BLEND) return 0;
    MaterialDesc* d = edit(m); if (!d) return 0;
    d->alphaMode = static_cast<AlphaMode>(mode);
    return commit(m);
}
float aver_pbr_get_alpha_cutoff(aver_pbr_material m) {
    const MaterialDesc* d = read(m); return d ? d->alphaCutoff : 0.0f;
}
int32_t aver_pbr_set_alpha_cutoff(aver_pbr_material m, float v) {
    MaterialDesc* d = edit(m); if (!d) return 0; d->alphaCutoff = v; return commit(m);
}
int32_t aver_pbr_get_two_sided(aver_pbr_material m) {
    const MaterialDesc* d = read(m); return d && d->twoSided ? 1 : 0;
}
int32_t aver_pbr_set_two_sided(aver_pbr_material m, int32_t on) {
    MaterialDesc* d = edit(m); if (!d) return 0; d->twoSided = on != 0; return commit(m);
}
int32_t aver_pbr_get_cast_shadow(aver_pbr_material m) {
    const MaterialDesc* d = read(m); return d && d->castShadow ? 1 : 0;
}
int32_t aver_pbr_set_cast_shadow(aver_pbr_material m, int32_t on) {
    MaterialDesc* d = edit(m); if (!d) return 0; d->castShadow = on != 0; return commit(m);
}

// ---- texture mapping ----
int32_t aver_pbr_get_uv_mode(aver_pbr_material m) {
    const MaterialDesc* d = read(m);
    return d ? static_cast<int32_t>(d->uvMode) : AVER_PBR_UV_MESH;
}
int32_t aver_pbr_set_uv_mode(aver_pbr_material m, int32_t mode) {
    MaterialDesc* d = edit(m);
    if (!d || mode < AVER_PBR_UV_MESH || mode > AVER_PBR_UV_WORLD_ALIGNED) return 0;
    d->uvMode = static_cast<aver::pbr::UvMode>(mode);
    return commit(m);
}
float aver_pbr_get_uv_tiling(aver_pbr_material m) {
    const MaterialDesc* d = read(m);
    return d ? d->uvTiling : 0.0f;
}
// Sets world centimetres per tile. Zero or negative is rejected rather than clamped.
int32_t aver_pbr_set_uv_tiling(aver_pbr_material m, float cmPerTile) {
    MaterialDesc* d = edit(m);
    if (!d || !(cmPerTile > 0.0f)) return 0;
    d->uvTiling = cmPerTile;
    return commit(m);
}
const char* aver_pbr_uv_mode_name(int32_t mode) {
    switch (mode) {
        case AVER_PBR_UV_MESH:          return "mesh";
        case AVER_PBR_UV_WORLD_ALIGNED: return "world";
        default:                        return "?";
    }
}

// ---- texture references ----
const char* aver_pbr_get_texture_path(aver_pbr_material m, int32_t slot) {
    const MaterialDesc* d = read(m);
    return (d && validSlot(slot)) ? d->textures[slot].path.c_str() : "";
}
int32_t aver_pbr_set_texture_path(aver_pbr_material m, int32_t slot, const char* path) {
    MaterialDesc* d = edit(m);
    if (!d || !validSlot(slot) || !path) return 0;
    d->textures[slot].path = path;
    return commit(m);
}
int64_t aver_pbr_get_texture_id(aver_pbr_material m, int32_t slot) {
    const MaterialDesc* d = read(m);
    return (d && validSlot(slot)) ? static_cast<int64_t>(d->textures[slot].id) : 0;
}
int32_t aver_pbr_set_texture_id(aver_pbr_material m, int32_t slot, int64_t id) {
    MaterialDesc* d = edit(m);
    if (!d || !validSlot(slot)) return 0;
    d->textures[slot].id = static_cast<aver::u64>(id);
    return commit(m);
}
int32_t aver_pbr_clear_texture(aver_pbr_material m, int32_t slot) {
    MaterialDesc* d = edit(m);
    if (!d || !validSlot(slot)) return 0;
    d->textures[slot] = aver::pbr::TextureRef{};
    return commit(m);
}

// ---- upload bookkeeping ----
int32_t aver_pbr_consume_dirty(aver_pbr_material m) {
    return MaterialLibrary::get().consumeDirty(handleOf(m)) ? 1 : 0;
}

} // extern "C"
