#include "aver/particles/ParticleLighting.hpp"

#include "aver/rhi/RHI.hpp"

#include <algorithm>
#include <cmath>

namespace aver::particles {

f32 particleLightE1m(f32 candela) { return candela / rhi::kLuminanceToCdm2; }

u32 selectParticleLights(const std::vector<scene::WorldLight>& lights, const Vec3& centre, f32 radiusCm,
                         ParticleLight out[kParticleLightCount]) {
    struct Scored { f32 score; usize index; };
    std::vector<Scored> scored;
    scored.reserve(lights.size());
    for (usize i = 0; i < lights.size(); ++i) {
        const scene::WorldLight& l = lights[i];
        if (l.kind == scene::kLightDirectional || !(l.intensityCd > 0.0f)) continue;
        const Vec3 d = Vec3{l.pos[0], l.pos[1], l.pos[2]} - centre;
        const f32 nearest = std::max(0.0f, d.size() - std::max(0.0f, radiusCm));
        if (l.rangeCm > 0.0f && nearest > l.rangeCm) continue;
        const f32 dm = std::max(nearest * 0.01f, 0.1f);
        const f32 lum = 0.2126f * l.colour[0] + 0.7152f * l.colour[1] + 0.0722f * l.colour[2];
        scored.push_back({particleLightE1m(l.intensityCd) * lum / (dm * dm), i});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) { return a.score > b.score; });

    for (u32 k = 0; k < kParticleLightCount; ++k) out[k] = ParticleLight{};
    u32 n = 0;
    for (const Scored& s : scored) {
        if (n >= kParticleLightCount) break;
        const scene::WorldLight& l = lights[s.index];
        ParticleLight& o = out[n++];
        const f32 e1m = particleLightE1m(l.intensityCd);
        o.posRange[0] = l.pos[0]; o.posRange[1] = l.pos[1]; o.posRange[2] = l.pos[2]; o.posRange[3] = l.rangeCm;
        o.radiance[0] = l.colour[0] * e1m; o.radiance[1] = l.colour[1] * e1m; o.radiance[2] = l.colour[2] * e1m;
        o.radiance[3] = l.sourceRadiusCm;
        o.axisKind[0] = l.axis[0]; o.axisKind[1] = l.axis[1]; o.axisKind[2] = l.axis[2];
        o.axisKind[3] = l.kind == scene::kLightSpot ? 2.0f : (l.kind == scene::kLightRect ? 3.0f : 1.0f);
        o.cone[0] = l.innerCos;
        o.cone[1] = l.outerCos;
    }
    return n;
}

} // namespace aver::particles
