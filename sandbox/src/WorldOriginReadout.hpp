#pragma once
// Show > World Origin and `world.origin`: what the floating origin is doing, as plain data (LARGE_WORLD 2.7). The model is
// separate from the ImGui window (WorldOriginPanel.cpp) so a test reads exactly what the panel would draw.
#include "aver/game/OriginParticipants.hpp"

#if AVER_MODULE_SCENE

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace aver::editor {

struct WorldOriginReadout {
    bool enabled = false;
    const char* policy = "follow";
    i32 marginCm = 0;
    i64 regionX = 0, regionY = 0;           // O / one region
    f64 originKmX = 0, originKmY = 0;
    f64 camAbsKm[3] = {0, 0, 0};            // computed in f64 from the origin and the origin-relative camera
    f64 camLocalCm[3] = {0, 0, 0};
    f64 toTriggerCm = 0;                    // how far the camera is from the nearest shift trigger on X or Y; 0 or less = due
    bool nearTrigger = false;               // within 3 km of it: the panel shows the band
    u32 generation = 0, shifts = 0, deferrals = 0;
    const char* lastDeferReason = nullptr;
    f64 lastMs = 0;
    struct Part { const char* name; f64 ms; };
    std::vector<Part> parts;                // the last shift's commit time per participant, in commit order
};

inline const char* originPolicyName(world::OriginPolicy p) {
    switch (p) {
        case world::OriginPolicy::FollowCamera: return "follow";
        case world::OriginPolicy::Fixed:        return "fixed";
        case world::OriginPolicy::Manual:       return "manual";
    }
    return "?";
}

inline constexpr f64 kOriginBandCm = 300'000.0;   // the trigger band shows within 3 km of it

inline WorldOriginReadout makeWorldOriginReadout(const game::OriginParticipants& p, const Vec3& camLocalCm) {
    WorldOriginReadout r;
    const world::WorldOrigin& svc = p.service();
    const world::OriginCm o = svc.origin();
    r.enabled = svc.enabled();
    r.policy = originPolicyName(svc.settings().policy);
    r.marginCm = svc.settings().marginCm;
    r.regionX = o.x / world::kOriginStepCm;
    r.regionY = o.y / world::kOriginStepCm;
    r.originKmX = static_cast<f64>(o.x) / 100000.0;
    r.originKmY = static_cast<f64>(o.y) / 100000.0;
    const world::Vec3d abs = world::absOf(svc.absolute(camLocalCm));
    r.camAbsKm[0] = abs.x / 100000.0;
    r.camAbsKm[1] = abs.y / 100000.0;
    r.camAbsKm[2] = abs.z / 100000.0;
    r.camLocalCm[0] = camLocalCm.x; r.camLocalCm[1] = camLocalCm.y; r.camLocalCm[2] = camLocalCm.z;
    const f64 limit = static_cast<f64>(world::kRegionHalfCm) + static_cast<f64>(r.marginCm);
    const f64 far = std::fmax(std::fabs(static_cast<f64>(camLocalCm.x)), std::fabs(static_cast<f64>(camLocalCm.y)));
    r.toTriggerCm = limit - far;
    r.nearTrigger = r.enabled && r.toTriggerCm <= kOriginBandCm;
    const world::WorldOrigin::Stats& st = svc.stats();
    r.generation = svc.generation();
    r.shifts = st.shifts;
    r.deferrals = st.deferrals;
    r.lastDeferReason = st.lastDeferReason;
    r.lastMs = st.lastMs;
    for (usize i = 0; i < p.participantCount() && i < world::WorldOrigin::kMaxTimedParticipants; ++i)
        r.parts.push_back({p.participantName(i), st.lastParticipantMs[i]});
    return r;
}

// The same rows as text (the console's `world.origin`).
inline std::vector<std::string> worldOriginReadoutLines(const WorldOriginReadout& r) {
    char b[256];
    std::vector<std::string> out;
    std::snprintf(b, sizeof b, "floating origin %s, policy %s, margin %d cm", r.enabled ? "on" : "OFF", r.policy, r.marginCm);
    out.emplace_back(b);
    std::snprintf(b, sizeof b, "region (%lld, %lld)  origin (%.3f, %.3f) km  generation %u",
                  static_cast<long long>(r.regionX), static_cast<long long>(r.regionY), r.originKmX, r.originKmY, r.generation);
    out.emplace_back(b);
    std::snprintf(b, sizeof b, "camera absolute (%.4f, %.4f, %.4f) km   local (%.0f, %.0f, %.0f) cm",
                  r.camAbsKm[0], r.camAbsKm[1], r.camAbsKm[2], r.camLocalCm[0], r.camLocalCm[1], r.camLocalCm[2]);
    out.emplace_back(b);
    if (r.enabled) {
        std::snprintf(b, sizeof b, "next shift in %.2f km of travel", r.toTriggerCm / 100000.0);
        out.emplace_back(b);
    }
    std::snprintf(b, sizeof b, "shifts %u, deferrals %u%s%s, last shift %.3f ms", r.shifts, r.deferrals,
                  r.lastDeferReason ? ", last reason: " : "", r.lastDeferReason ? r.lastDeferReason : "", r.lastMs);
    out.emplace_back(b);
    return out;
}

} // namespace aver::editor

#endif // AVER_MODULE_SCENE
