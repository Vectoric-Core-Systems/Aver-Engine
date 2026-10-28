// Post-process volumes: weights, the per-field blend and the LUT texture cache. See GamePostVolumes.hpp.
#include "aver/game/GamePostVolumes.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/CubeLut.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace aver::game {

namespace {

// Exact at both ends, so full weight gives precisely the volume's value and none gives precisely the base's.
f32 lerpF(f32 a, f32 b, f32 t) { return t >= 1.0f ? b : t <= 0.0f ? a : a + (b - a) * t; }

// Exposure-like multipliers blend in stops, so half way between 1 and 4 is 2, not 2.5.
f32 lerpLog(f32 a, f32 b, f32 t) {
    if (a <= 0.0f || b <= 0.0f || t >= 1.0f || t <= 0.0f) return lerpF(a, b, t);
    return std::exp2(lerpF(std::log2(a), std::log2(b), t));
}

// The volume's value for `key` (index c of the key's values), clamped to the key's slider range.
f32 ovF(const fmt::OcPostValue& ov, const fmt::OcPostKeyInfo& ki, int c = 0) {
    return std::clamp(static_cast<f32>(ov.v[c]), ki.lo, ki.hi);
}

} // namespace

f32 postVolumeWeight(const fmt::OcPostVolume& v, const Vec3& p) {
    const f32 base = std::clamp(static_cast<f32>(v.weight), 0.0f, 1.0f);
    if (v.infinite) return base;
    const f64 q[3] = {p.x, p.y, p.z};
    f64 d2 = 0.0;
    for (int a = 0; a < 3; ++a) {
        const f64 lo = std::min(v.bounds[a], v.bounds[a + 3]), hi = std::max(v.bounds[a], v.bounds[a + 3]);
        const f64 out = q[a] < lo ? lo - q[a] : (q[a] > hi ? q[a] - hi : 0.0);
        d2 += out * out;
    }
    if (d2 <= 0.0) return base;
    if (v.blendCm <= 0.0) return 0.0f;
    const f32 t = 1.0f - static_cast<f32>(std::sqrt(d2) / v.blendCm);
    if (t <= 0.0f) return 0.0f;
    return base * t * t * (3.0f - 2.0f * t);
}

void applyPostVolumeFields(rhi::PostSettings& s, const fmt::OcPostVolume& v, f32 w) {
    if (w <= 0.0f) return;
    const bool flip = w >= 0.5f;
    for (const fmt::OcPostValue& ov : v.values) {
        const fmt::OcPostKeyInfo* ki = fmt::ocPostKeyInfo(ov.key);
        if (!ki) continue;
        const std::string& k = ov.key;
        if      (k == "exposure")        s.exposure = lerpLog(s.exposure, ovF(ov, *ki), w);
        else if (k == "autoexposure")    { if (flip) s.autoExposure = ov.v[0] >= 0.5; }
        else if (k == "exposuremin")     s.exposureMin = lerpLog(s.exposureMin, ovF(ov, *ki), w);
        else if (k == "exposuremax")     s.exposureMax = lerpLog(s.exposureMax, ovF(ov, *ki), w);
        else if (k == "exposurekey")     s.exposureKey = lerpF(s.exposureKey, ovF(ov, *ki), w);
        else if (k == "nightvision")     s.nightVision = lerpF(s.nightVision, ovF(ov, *ki), w);
        else if (k == "bloom")           s.bloomIntensity = lerpF(s.bloomIntensity, ovF(ov, *ki), w);
        else if (k == "bloomthreshold")  s.bloomThreshold = lerpF(s.bloomThreshold, ovF(ov, *ki), w);
        else if (k == "bloomknee")       s.bloomKnee = lerpF(s.bloomKnee, ovF(ov, *ki), w);
        else if (k == "tonemap")         { if (flip) s.tonemap = static_cast<u32>(std::lround(ovF(ov, *ki))); }
        else if (k == "tonemapwhite")    s.tonemapWhite = lerpF(s.tonemapWhite, ovF(ov, *ki), w);
        else if (k == "adjbrightness")   s.adjustBrightness = lerpF(s.adjustBrightness, ovF(ov, *ki), w);
        else if (k == "adjcontrast")     s.adjustContrast = lerpF(s.adjustContrast, ovF(ov, *ki), w);
        else if (k == "adjsaturation")   s.adjustSaturation = lerpF(s.adjustSaturation, ovF(ov, *ki), w);
        else if (k == "maxradiance")     s.maxRadiance = lerpF(s.maxRadiance, ovF(ov, *ki), w);
        else if (k == "localshadows")    s.localExposureShadows = lerpF(s.localExposureShadows, ovF(ov, *ki), w);
        else if (k == "localhighlights") s.localExposureHighlights = lerpF(s.localExposureHighlights, ovF(ov, *ki), w);
        else if (k == "vignette")        s.vignette = lerpF(s.vignette, ovF(ov, *ki), w);
        else if (k == "grain")           s.grain = lerpF(s.grain, ovF(ov, *ki), w);
        else if (k == "chromatic")       s.chromaticAberration = lerpF(s.chromaticAberration, ovF(ov, *ki), w);
        else if (k == "lensflare")       s.lensFlare = lerpF(s.lensFlare, ovF(ov, *ki), w);
        else if (k == "motionblur")      s.motionBlur = lerpF(s.motionBlur, ovF(ov, *ki), w);
        else if (k == "dof")             { if (flip) s.dofEnabled = ov.v[0] >= 0.5; }
        else if (k == "doffocus")        s.dofFocusCm = lerpLog(s.dofFocusCm, ovF(ov, *ki), w);
        else if (k == "dofstop")         s.dofFStop = lerpF(s.dofFStop, ovF(ov, *ki), w);
        else if (k == "dofmm")           s.dofFocalMm = lerpF(s.dofFocalMm, ovF(ov, *ki), w);
        else if (k == "saturation")      s.saturation = lerpF(s.saturation, ovF(ov, *ki), w);
        else if (k == "contrast")        s.contrast = lerpF(s.contrast, ovF(ov, *ki), w);
        else if (k == "lift")   for (int c = 0; c < 3; ++c) s.lift[c]  = lerpF(s.lift[c],  ovF(ov, *ki, c), w);
        else if (k == "gamma")  for (int c = 0; c < 3; ++c) s.gamma[c] = lerpF(s.gamma[c], ovF(ov, *ki, c), w);
        else if (k == "gain")   for (int c = 0; c < 3; ++c) s.gain[c]  = lerpF(s.gain[c],  ovF(ov, *ki, c), w);
        else if (k == "wbk")             s.whiteBalanceK = lerpF(s.whiteBalanceK, ovF(ov, *ki), w);
        else if (k == "wbtint")          s.whiteBalanceTint = lerpF(s.whiteBalanceTint, ovF(ov, *ki), w);
        // lutintensity: the LUT stage of blendPostVolumes owns it.
    }
}

bool readPostField(const rhi::PostSettings& s, std::string_view k, f64 out[3]) {
    out[0] = out[1] = out[2] = 0.0;
    if      (k == "exposure")        out[0] = s.exposure;
    else if (k == "autoexposure")    out[0] = s.autoExposure ? 1.0 : 0.0;
    else if (k == "exposuremin")     out[0] = s.exposureMin;
    else if (k == "exposuremax")     out[0] = s.exposureMax;
    else if (k == "exposurekey")     out[0] = s.exposureKey;
    else if (k == "nightvision")     out[0] = s.nightVision;
    else if (k == "bloom")           out[0] = s.bloomIntensity;
    else if (k == "bloomthreshold")  out[0] = s.bloomThreshold;
    else if (k == "bloomknee")       out[0] = s.bloomKnee;
    else if (k == "tonemap")         out[0] = s.tonemap;
    else if (k == "tonemapwhite")    out[0] = s.tonemapWhite;
    else if (k == "adjbrightness")   out[0] = s.adjustBrightness;
    else if (k == "adjcontrast")     out[0] = s.adjustContrast;
    else if (k == "adjsaturation")   out[0] = s.adjustSaturation;
    else if (k == "maxradiance")     out[0] = s.maxRadiance;
    else if (k == "localshadows")    out[0] = s.localExposureShadows;
    else if (k == "localhighlights") out[0] = s.localExposureHighlights;
    else if (k == "vignette")        out[0] = s.vignette;
    else if (k == "grain")           out[0] = s.grain;
    else if (k == "chromatic")       out[0] = s.chromaticAberration;
    else if (k == "lensflare")       out[0] = s.lensFlare;
    else if (k == "motionblur")      out[0] = s.motionBlur;
    else if (k == "dof")             out[0] = s.dofEnabled ? 1.0 : 0.0;
    else if (k == "doffocus")        out[0] = s.dofFocusCm;
    else if (k == "dofstop")         out[0] = s.dofFStop;
    else if (k == "dofmm")           out[0] = s.dofFocalMm;
    else if (k == "saturation")      out[0] = s.saturation;
    else if (k == "contrast")        out[0] = s.contrast;
    else if (k == "lift")   for (int c = 0; c < 3; ++c) out[c] = s.lift[c];
    else if (k == "gamma")  for (int c = 0; c < 3; ++c) out[c] = s.gamma[c];
    else if (k == "gain")   for (int c = 0; c < 3; ++c) out[c] = s.gain[c];
    else if (k == "wbk")             out[0] = s.whiteBalanceK;
    else if (k == "wbtint")          out[0] = s.whiteBalanceTint;
    else if (k == "lutintensity")    out[0] = s.gradingLutIntensity;
    else return false;
    return true;
}

rhi::PostSettings blendPostVolumes(const std::vector<fmt::OcPostVolume>& volumes, const rhi::PostSettings& base,
                                   const Vec3& eye, const PostLutResolver& luts) {
    struct Active { const fmt::OcPostVolume* v; f32 w; };
    std::vector<Active> act;
    for (const fmt::OcPostVolume& v : volumes) {
        const f32 w = postVolumeWeight(v, eye);
        if (w > 0.0f) act.push_back({&v, w});
    }
    if (act.empty()) return base;
    std::stable_sort(act.begin(), act.end(), [](const Active& a, const Active& b) { return a.v->priority < b.v->priority; });

    rhi::PostSettings s = base;
    for (const Active& a : act) applyPostVolumeFields(s, *a.v, a.w);

    // The LUT: highest weight wins (a later, higher-priority volume wins a tie).
    if (luts) {
        const Active* best = nullptr;
        rhi::TextureHandle bestTex = 0;
        for (const Active& a : act) {
            if (a.v->lut.empty()) continue;
            const rhi::TextureHandle t = luts(a.v->lut);
            if (!t) continue;
            if (!best || a.w >= best->w) { best = &a; bestTex = t; }
        }
        if (best) {
            const fmt::OcPostValue* li = best->v->find("lutintensity");
            const f32 target = li ? std::clamp(static_cast<f32>(li->v[0]), 0.0f, 1.0f) : 1.0f;
            if (base.gradingLut == 0 || base.gradingLut == bestTex) {
                const f32 from = base.gradingLut == bestTex ? base.gradingLutIntensity : 0.0f;
                s.gradingLut = bestTex;
                s.gradingLutIntensity = lerpF(from, target, best->w);
            } else if (best->w >= 0.5f) {
                s.gradingLut = bestTex;
                s.gradingLutIntensity = target;
            }
        }
    }
    return s;
}

rhi::TextureHandle PostLutCache::get(rhi::IDevice& dev, const std::string& contentDir, const std::string& path) {
    if (path.empty()) return 0;
    if (const auto it = textures_.find(path); it != textures_.end()) return it->second;
    rhi::TextureHandle tex = 0;
    rhi::IResourceFactory* rf = dev.resources();
    if (!rf) {
        AVER_WARN("[PostVolume] no resource factory: grading LUT '{}' ignored", path);
    } else {
        fmt::CubeLut lut;
        std::string err;
        const std::string full = (std::filesystem::path(contentDir) / path).string();
        if (!fmt::loadCubeLut(full, lut, &err)) {
            AVER_WARN("[PostVolume] grading LUT '{}': {}", path, err);
        } else {
            const void* mip0 = lut.rgba16f.data();
            rhi::TextureDesc d;
            d.dim = rhi::TextureDim::Tex3D;
            d.width = d.height = d.depth = lut.size;
            d.mips = 1;
            d.format = rhi::Format::RGBA16F;
            d.bind = rhi::ResourceBind::ShaderResource;
            d.initialState = rhi::ResourceState::ShaderResource;
            d.debugName = "post grading LUT";
            d.initialData = &mip0;
            d.initialDataCount = 1;
            tex = rf->createTexture(d);
            if (tex) AVER_INFO("[PostVolume] grading LUT '{}': {}^3", path, lut.size);
            else     AVER_WARN("[PostVolume] grading LUT '{}': the device could not create a {}^3 texture", path, lut.size);
        }
    }
    textures_[path] = tex;
    return tex;
}

void PostLutCache::preload(rhi::IDevice& dev, const std::string& contentDir, const std::vector<fmt::OcPostVolume>& volumes) {
    for (const fmt::OcPostVolume& v : volumes) get(dev, contentDir, v.lut);
}

PostLutResolver PostLutCache::resolver(rhi::IDevice& dev, const std::string& contentDir) {
    return [this, &dev, contentDir](const std::string& path) { return get(dev, contentDir, path); };
}

void PostLutCache::forget(rhi::IDevice* dev, const std::string& path) {
    const auto it = textures_.find(path);
    if (it == textures_.end()) return;
    if (it->second && dev)
        if (rhi::IResourceFactory* rf = dev->resources()) rf->destroyTexture(it->second);
    textures_.erase(it);
}

void PostLutCache::clear(rhi::IDevice* dev) {
    if (dev)
        if (rhi::IResourceFactory* rf = dev->resources())
            for (const auto& [path, tex] : textures_)
                if (tex) rf->destroyTexture(tex);
    textures_.clear();
}

} // namespace aver::game
