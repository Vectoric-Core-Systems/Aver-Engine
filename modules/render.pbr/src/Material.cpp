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
    // Floored at 1.0 (vacuum); nothing in this engine models a surface less dense than the medium
    // it sits in. Ceiling is generous headroom past diamond's ~2.42, not a physical limit -- wide
    // enough that a mis-typed value clamps to something plausible instead of silently producing a
    // negative or NaN reflectance somewhere a future derivation reads it.
    d.ior               = std::clamp(d.ior, 1.0f, 4.0f);
    d.transmission      = std::clamp(d.transmission, 0.0f, 1.0f);
    d.alphaCutoff       = std::clamp(d.alphaCutoff, 0.0f, 1.0f);
    d.subsurfaceWeight  = std::clamp(d.subsurfaceWeight, 0.0f, 1.0f);
    d.subsurfaceRadius  = std::clamp(d.subsurfaceRadius, 0.0f, 1.0f);
    // Authored sRGB tint, same [0,1] range as baseColorFactor -- it is decoded through the same
    // pow(2.2) as that field and a channel outside [0,1] has no meaning on either side of it.
    for (f32& c : d.subsurfaceColor) c = std::clamp(c, 0.0f, 1.0f);
    // A multiplier on the light the material's own glow and size already cast, floored at 0 with no
    // ceiling. There is no upper bound the way subsurfaceWeight's [0,1] has: a lamp can legitimately
    // want to be far brighter than what its own glow and size alone would cast.
    d.lightIntensity    = std::max(d.lightIntensity, 0.0f);
    // Volume absorption. attenuationColor is a TRANSMITTANCE, so [0,1] per channel -- and the low end
    // is floored just above zero rather than at it, because the shader takes -log(colour) and a
    // channel of exactly 0 is an infinite extinction, i.e. an inf that propagates into the whole
    // pixel. 1e-4 is about nine attenuation lengths, far past visually opaque, so the floor costs
    // nothing anyone can see and removes the only way this pair can produce a non-finite result.
    for (f32& ch : d.attenuationColor) ch = std::clamp(ch, 1e-4f, 1.0f);
    // Negative is meaningless and would flip the sign of the extinction; clamping to 0 lands it on
    // the documented "no volume" state rather than on something inside-out.
    d.attenuationDistance = d.attenuationDistance < 0.0f ? 0.0f : d.attenuationDistance;
    if (static_cast<u32>(d.alphaMode) > static_cast<u32>(AlphaMode::Blend)) d.alphaMode = AlphaMode::Opaque;
}

// Removes one handle from the live list.
void eraseHandle(std::vector<MaterialHandle>& v, MaterialHandle h) {
    const auto it = std::find(v.begin(), v.end(), h);
    if (it != v.end()) v.erase(it);
}

} // namespace

// See Material.hpp for why these live here rather than being re-spelled at each call site.
bool isTranslucent(const MaterialDesc& d) {
    return d.alphaMode == AlphaMode::Blend || d.transmission > 0.0f;
}

void shadowTransmittance(const MaterialDesc& d, f32 outRgb[3]) {
    // castShadow wins outright, and it is checked FIRST rather than folded into the arithmetic: an
    // author who ticked it off means "this thing casts nothing", not "scale its shadow by something".
    if (!d.castShadow) { outRgb[0] = outRgb[1] = outRgb[2] = 1.0f; return; }

    // max(), not a sum or a product, of the two independent ways a surface can be see-through:
    // coverage (baseColorFactor.a) and substrate transmission. They are alternative descriptions of
    // the same physical fact, and an author who sets both means the more transmissive of the two --
    // multiplying them would make a pane that is 90% transparent by BOTH measures nearly opaque.
    const f32 a = std::clamp(d.baseColorFactor[3], 0.0f, 1.0f);
    const f32 t = std::clamp(d.transmission, 0.0f, 1.0f);
    const f32 k = std::max(1.0f - a, t);
    for (int i = 0; i < 3; ++i)
        outRgb[i] = std::clamp(d.baseColorFactor[i], 0.0f, 1.0f) * k;
}

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
        // Both pieces this used to be missing now exist in the renderer: IDevice::setDrawBlended
        // (aver/rhi/RHI.hpp) captures a blended drawMesh call instead of submitting it through the
        // normal opaque path, and IDevice::endFrame sorts that per-frame list BACK-TO-FRONT by
        // camera distance and replays it through scenePipeline(..., blended=true) -- with a real
        // blend state, depth-test on and depth-write off -- after the deferred sky draw and before
        // the transparent (particle) pass. PbrShaders.cpp's averBuildSurface also now raises alpha
        // toward 1 with the view Fresnel term under this same flag, so a blended dielectric reads as
        // glass rather than as uniformly-dimmed fog. That is the feature: alpha blending is Ready.
        //
        // WHAT STILL DOES NOT WORK, so a caller does not read "Ready" as "physically correct glass":
        //   - a blended draw is captured and replayed OUTSIDE IRenderFeature::submitDraw, which is
        //     the same loop that voxelises geometry, inserts it into the ray-tracing acceleration
        //     structure and feeds the shadow cascade. A blended surface is therefore never voxelised,
        //     never in the TLAS and never in the shadow map: it casts no ray-traced shadow, is absent
        //     from ray-traced reflections, and contributes no GI bounce. These are the SAME exclusion
        //     for the SAME reason a translucent mesh is skipped by drawMesh's opaque path in the
        //     first place -- see the contract at IDevice::setDrawBlended's declaration -- not
        //     separate oversights to fix later.
        //   - MaterialDesc now HAS ior/transmission (packed into MaterialConstants alongside
        //     everything else), but the only place either is read is averBuildSurface's alpha
        //     computation: transmission pulls blended coverage toward (1 - transmission) before the
        //     view-Fresnel term lifts it back at grazing angles, and ior is carried for completeness
        //     without being read by anything yet. So there is STILL no refraction and no light
        //     actually passes through the surface; what this status covers is alpha COMPOSITING (a
        //     weighted blend of the surface colour over whatever was drawn before it, with coverage
        //     shaped by transmission), not physical transmission. A window that should bend the view
        //     of what is behind it will not -- it will only show a translucent, unbent copy.
        case Feature::AlphaBlend:
            return Status::Ready;
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
        case TextureSlot::Layer1BaseColor:  return "layer1BaseColor";
        case TextureSlot::Layer1MetalRough: return "layer1MetalRough";
        case TextureSlot::Layer1Normal:     return "layer1Normal";
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
float aver_pbr_get_ior(aver_pbr_material m) {
    // 1.5 rather than 0 on a bad handle: it is MaterialDesc::ior's own default, and an ior of zero is
    // not a dim material, it is a nonsensical one that would drive the TIR test to nonsense too.
    const MaterialDesc* d = read(m); return d ? d->ior : 1.5f;
}
int32_t aver_pbr_set_ior(aver_pbr_material m, float v) {
    MaterialDesc* d = edit(m); if (!d) return 0; d->ior = v; return commit(m);
}
float aver_pbr_get_transmission(aver_pbr_material m) {
    const MaterialDesc* d = read(m); return d ? d->transmission : 0.0f;
}
int32_t aver_pbr_set_transmission(aver_pbr_material m, float v) {
    MaterialDesc* d = edit(m); if (!d) return 0; d->transmission = v; return commit(m);
}
float aver_pbr_get_subsurface_weight(aver_pbr_material m) {
    const MaterialDesc* d = read(m); return d ? d->subsurfaceWeight : 0.0f;
}
int32_t aver_pbr_set_subsurface_weight(aver_pbr_material m, float v) {
    MaterialDesc* d = edit(m); if (!d) return 0; d->subsurfaceWeight = v; return commit(m);
}
// A multiplier on the light the material's own glow (emissiveFactor) and size (its bounding sphere)
// already, physically, cast at 1 metre in the sun's own units (SkyAtmosphere::sunIntensity): 1 is
// exactly that output, 2 is twice it. 0 = not a light. See MaterialDesc::lightIntensity for what sets
// it apart from emissiveFactor.
float aver_pbr_get_light_intensity(aver_pbr_material m) {
    const MaterialDesc* d = read(m); return d ? d->lightIntensity : 0.0f;
}
int32_t aver_pbr_set_light_intensity(aver_pbr_material m, float v) {
    MaterialDesc* d = edit(m); if (!d) return 0; d->lightIntensity = v; return commit(m);
}
float aver_pbr_get_subsurface_radius(aver_pbr_material m) {
    const MaterialDesc* d = read(m); return d ? d->subsurfaceRadius : 0.0f;
}
int32_t aver_pbr_set_subsurface_radius(aver_pbr_material m, float v) {
    MaterialDesc* d = edit(m); if (!d) return 0; d->subsurfaceRadius = v; return commit(m);
}
int32_t aver_pbr_get_subsurface_color(aver_pbr_material m, float* out3) {
    const MaterialDesc* d = read(m);
    if (!d || !out3) return 0;
    std::memcpy(out3, d->subsurfaceColor, sizeof(d->subsurfaceColor));
    return 1;
}
int32_t aver_pbr_set_subsurface_color(aver_pbr_material m, float r, float g, float b) {
    MaterialDesc* d = edit(m);
    if (!d) return 0;
    d->subsurfaceColor[0] = r; d->subsurfaceColor[1] = g; d->subsurfaceColor[2] = b;
    return commit(m);
}
float aver_pbr_get_coat_weight(aver_pbr_material m) {
    const MaterialDesc* d = read(m); return d ? d->coatWeight : 0.0f;
}
int32_t aver_pbr_set_coat_weight(aver_pbr_material m, float v) {
    MaterialDesc* d = edit(m); if (!d) return 0; d->coatWeight = v; return commit(m);
}
float aver_pbr_get_coat_roughness(aver_pbr_material m) {
    const MaterialDesc* d = read(m); return d ? d->coatRoughness : 0.0f;
}
int32_t aver_pbr_set_coat_roughness(aver_pbr_material m, float v) {
    MaterialDesc* d = edit(m); if (!d) return 0; d->coatRoughness = v; return commit(m);
}
// Defaults to 0.04 rather than 0 on a bad handle: 0.04 is the field's own default (IOR 1.5),
// and a getter that returns 0 for "no material" would read as a coat with no reflectance.
float aver_pbr_get_coat_f0(aver_pbr_material m) {
    const MaterialDesc* d = read(m); return d ? d->coatF0 : 0.04f;
}
int32_t aver_pbr_set_coat_f0(aver_pbr_material m, float v) {
    MaterialDesc* d = edit(m); if (!d) return 0; d->coatF0 = v; return commit(m);
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
