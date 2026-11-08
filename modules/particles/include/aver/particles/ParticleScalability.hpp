// Scalability tiers for an authored effect: how much of it plays at Low, Medium, High and Epic (the engine's quality ladder).
// Pure data and arithmetic, no RHI and no scene: ParticleEffectLibrary applies the active tier when an effect is registered, and
// the editor previews any tier with the same function.
//
// An effect carries a default per tier (`ParticleSystemDef::scalability`) and each emitter may override a tier
// (`ParticleEffect::scalability`, where only a `set` tier counts). A tier with nothing authored plays the effect as written.
#pragma once
#include "aver/particles/ParticleTypes.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>
#include <vector>

namespace aver::particles {

inline const char* tierWord(ScalabilityTier t) {
    switch (t) {
    case ScalabilityTier::Low:    return "low";
    case ScalabilityTier::Medium: return "medium";
    case ScalabilityTier::High:   return "high";
    case ScalabilityTier::Epic:   default: return "epic";
    }
}
// "low" / "medium" / "high" / "epic", ASCII case-insensitive. False for anything else.
inline bool parseTierWord(std::string_view w, ScalabilityTier& out) {
    const auto eq = [&](const char* s) {
        usize i = 0;
        for (; i < w.size() && s[i]; ++i) if (std::tolower(static_cast<unsigned char>(w[i])) != s[i]) return false;
        return i == w.size() && s[i] == '\0';
    };
    if (eq("low")) { out = ScalabilityTier::Low; return true; }
    if (eq("medium")) { out = ScalabilityTier::Medium; return true; }
    if (eq("high")) { out = ScalabilityTier::High; return true; }
    if (eq("epic")) { out = ScalabilityTier::Epic; return true; }
    return false;
}

// The particle tier the engine's Overall Quality rung asks for (the render ladder numbers its rungs 1 Low .. 4 Epic; 0 is Custom). False for
// anything but a rung: the tier is then left as it is (a project key, or Epic).
inline bool tierForQualityRung(u32 rung, ScalabilityTier& out) {
    if (rung < 1u || rung > 4u) return false;
    out = static_cast<ScalabilityTier>(rung - 1u);
    return true;
}

inline bool tierIsIdentity(const TierScale& s) { return s.enabled && s.rateScale == 1.0f && s.maxScale == 1.0f; }
inline bool anyTierSet(const Scalability& s) {
    for (const TierScale& t : s.tier) if (t.set) return true;
    return false;
}
inline bool hasScalability(const ParticleSystemDef& d) {
    if (anyTierSet(d.scalability)) return true;
    for (const ParticleEffect& e : d.emitters) if (anyTierSet(e.scalability)) return true;
    return false;
}

// What applies to one emitter at a tier: its own override if it has one, else the effect's default, else the effect as written.
inline TierScale effectiveTierScale(const ParticleSystemDef& d, usize emitter, ScalabilityTier tier) {
    const usize t = static_cast<usize>(tier);
    if (t >= kScalabilityTiers) return TierScale{};
    if (emitter < d.emitters.size() && d.emitters[emitter].scalability.tier[t].set) return d.emitters[emitter].scalability.tier[t];
    if (d.scalability.tier[t].set) return d.scalability.tier[t];
    return TierScale{};
}

// The effect as it plays at `tier`: an emitter that is off at the tier spawns nothing (rate, burst and spawn-at off, and no event
// from another emitter feeds it); otherwise the spawn rate and burst count scale by rateScale, and the particle cap (and a linked
// ribbon's ring) by maxScale, never below one particle. Returns the effect unchanged when nothing is authored.
inline ParticleSystemDef scaledForTier(const ParticleSystemDef& d, ScalabilityTier tier) {
    ParticleSystemDef out = d;
    if (!hasScalability(d)) return out;
    std::vector<u8> off(d.emitters.size(), 0);
    for (usize i = 0; i < d.emitters.size(); ++i) {
        const TierScale s = effectiveTierScale(d, i, tier);
        ParticleEffect& e = out.emitters[i];
        if (!s.enabled) {
            off[i] = 1;
            e.emissionRate = 0.0f;
            e.burstCount = 0;
            e.spawnAtTarget = false;
            e.maxParticles = 1;
            continue;
        }
        e.emissionRate = d.emitters[i].emissionRate * s.rateScale;
        e.burstCount = static_cast<u32>(std::lround(static_cast<f64>(d.emitters[i].burstCount) * s.rateScale));
        e.maxParticles = std::max(1u, static_cast<u32>(std::lround(static_cast<f64>(d.emitters[i].maxParticles) * s.maxScale)));
        if (e.linked) e.linkedCapacity = static_cast<u16>(std::clamp<long>(std::lround(static_cast<f64>(d.emitters[i].linkedCapacity) * s.maxScale), 2l, static_cast<long>(kMaxLinkedCapacity)));
    }
    for (ParticleEffect& e : out.emitters)
        e.events.erase(std::remove_if(e.events.begin(), e.events.end(), [&](const EventSpawn& ev) { return ev.emitter < off.size() && off[ev.emitter]; }), e.events.end());
    return out;
}

} // namespace aver::particles
