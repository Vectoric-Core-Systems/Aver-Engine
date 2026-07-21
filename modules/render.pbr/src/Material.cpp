#include "aver/pbr/Material.hpp"
#include "aver/pbr/pbr_abi.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace aver::pbr {

// One slot per index ever handed out. Slots are recycled through a free list and their generation
// bumped, so a handle to a destroyed material fails valid() instead of addressing its successor.
struct MaterialLibrary::Impl {
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

// Every scalar the shading model divides by, or raises to a power, is clamped here rather than in
// the renderer: a material is authored once and consumed by several passes, so a value that is only
// legal in one of them is a bug waiting for the second consumer.
void sanitise(MaterialDesc& d) {
    for (f32& c : d.baseColorFactor) c = std::clamp(c, 0.0f, 1.0f);
    for (f32& c : d.emissiveFactor)  c = std::max(c, 0.0f);   // emissive is radiance, not a ratio
    d.metallicFactor    = std::clamp(d.metallicFactor, 0.0f, 1.0f);
    // Not clamped to 0: a perfect mirror makes the GGX denominator collapse. The floor matches the
    // one shadeSurface applies, so the authored value and the shaded value agree.
    d.roughnessFactor   = std::clamp(d.roughnessFactor, 0.045f, 1.0f);
    d.normalScale       = std::clamp(d.normalScale, 0.0f, 8.0f);
    d.occlusionStrength = std::clamp(d.occlusionStrength, 0.0f, 1.0f);
    // Both are reflectances, so both are ratios in [0,1]. The upper bound matters more than it
    // looks: F0 above 1 makes (f90 - F0) negative in the Schlick term, and the surface reflects
    // NEGATIVE radiance at grazing angles, which the tonemap then clamps to black -- a dark rim on
    // a bright material, with nothing in the image to suggest a reflectance was the cause.
    d.reflectance       = std::clamp(d.reflectance, 0.0f, 1.0f);
    d.f90               = std::clamp(d.f90, 0.0f, 1.0f);
    d.alphaCutoff       = std::clamp(d.alphaCutoff, 0.0f, 1.0f);
    if (static_cast<u32>(d.alphaMode) > static_cast<u32>(AlphaMode::Blend)) d.alphaMode = AlphaMode::Opaque;
}

void eraseHandle(std::vector<MaterialHandle>& v, MaterialHandle h) {
    const auto it = std::find(v.begin(), v.end(), h);
    if (it != v.end()) v.erase(it);
}

} // namespace

MaterialLibrary::MaterialLibrary() : impl_(new Impl) {}
MaterialLibrary::~MaterialLibrary() { delete impl_; }

MaterialLibrary& MaterialLibrary::get() {
    static MaterialLibrary inst;
    return inst;
}

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

bool MaterialLibrary::valid(MaterialHandle h) const {
    if (h == 0) return false;
    const u32 i = materialIndex(h);
    if (i >= impl_->slots.size()) return false;
    const Impl::Slot& s = impl_->slots[i];
    return s.live && s.generation == materialGeneration(h);
}

bool MaterialLibrary::destroy(MaterialHandle h) {
    if (!valid(h)) return false;
    Impl::Slot& s = impl_->slots[materialIndex(h)];
    s.live = false;
    s.desc = MaterialDesc{};
    // Bump past the wrap rather than through zero: generation 0 would make the handle for index 0
    // compare equal to the invalid handle.
    s.generation = (s.generation + 1) & kMaterialGenerationMask;
    if (s.generation == 0) s.generation = 1;
    impl_->freeIndices.push_back(materialIndex(h));
    eraseHandle(impl_->liveHandles, h);
    return true;
}

const MaterialDesc* MaterialLibrary::desc(MaterialHandle h) const {
    return valid(h) ? &impl_->slots[materialIndex(h)].desc : nullptr;
}

MaterialDesc* MaterialLibrary::mutableDesc(MaterialHandle h) {
    return valid(h) ? &impl_->slots[materialIndex(h)].desc : nullptr;
}

bool MaterialLibrary::update(MaterialHandle h, const MaterialDesc& d) {
    if (!valid(h)) return false;
    Impl::Slot& s = impl_->slots[materialIndex(h)];
    s.desc = d;
    sanitise(s.desc);
    s.dirty = true;
    return true;
}

void MaterialLibrary::touch(MaterialHandle h) {
    if (!valid(h)) return;
    Impl::Slot& s = impl_->slots[materialIndex(h)];
    sanitise(s.desc);
    s.dirty = true;
}

bool MaterialLibrary::consumeDirty(MaterialHandle h) {
    if (!valid(h)) return false;
    Impl::Slot& s = impl_->slots[materialIndex(h)];
    const bool d = s.dirty;
    s.dirty = false;
    return d;
}

u32 MaterialLibrary::count() const { return static_cast<u32>(impl_->liveHandles.size()); }

MaterialHandle MaterialLibrary::at(u32 i) const {
    return i < impl_->liveHandles.size() ? impl_->liveHandles[i] : 0;
}

// Nothing consumes a material on the GPU yet: this step stands the system up, and the renderer edge
// lands with the Materials target. Reporting Ready here would be exactly the lie the status enum
// exists to prevent — an editor would offer a texture slot that shades nothing.
Status MaterialLibrary::status(Feature f) {
    switch (f) {
        case Feature::Factors:
        case Feature::BaseColorMap:
        case Feature::MetalRoughMap:
        case Feature::NormalMap:
        case Feature::OcclusionMap:
        case Feature::EmissiveMap:
        case Feature::AlphaMask:
        case Feature::AlphaBlend:
            return Status::NotImplemented;
        default:
            return Status::Unsupported;
    }
}

const char* MaterialLibrary::statusText(Feature f) {
    switch (status(f)) {
        case Status::Ready:          return "Ready";
        case Status::NotImplemented: return "Authored and stored; no renderer consumes it yet";
        case Status::Unsupported:    return "Not a material feature";
        default:                     return "Unknown";
    }
}

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

// These spell the `.ocmat` TEX slot names, so what the editor shows and what the file says match.
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

bool validFeature(int32_t f) { return f >= 0 && f < AVER_PBR_FEATURE_COUNT; }
bool validSlot(int32_t s)    { return s >= 0 && s < AVER_PBR_TEX_COUNT; }

MaterialHandle handleOf(int32_t m) {
    // A negative handle can only come from a caller that stored an error code, so it is rejected
    // rather than reinterpreted into some other material's index.
    return m > 0 ? static_cast<MaterialHandle>(m) : 0;
}

MaterialDesc* edit(int32_t m) { return MaterialLibrary::get().mutableDesc(handleOf(m)); }
const MaterialDesc* read(int32_t m) { return MaterialLibrary::get().desc(handleOf(m)); }

// Every setter ends the same way: sanitise and mark dirty exactly once.
int32_t commit(int32_t m) { MaterialLibrary::get().touch(handleOf(m)); return 1; }

} // namespace

extern "C" {

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

int32_t aver_pbr_consume_dirty(aver_pbr_material m) {
    return MaterialLibrary::get().consumeDirty(handleOf(m)) ? 1 : 0;
}

} // extern "C"
