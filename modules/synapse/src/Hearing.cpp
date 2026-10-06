#include "aver/synapse/Hearing.hpp"

#include <algorithm>
#include <cmath>

namespace aver::synapse {

f32 hearingLevel(const NoiseEvent& ev, const Vec3& ear, const HearingProfile& p, OcclusionFn occlusion,
                 void* occlusionUser, f32* outDistanceCm) {
    const f32 d = dist(ev.pos, ear);
    if (outDistanceCm) *outDistanceCm = d;
    if ((p.tagMask & (1u << (ev.tag & 31u))) == 0) return 0.0f;

    const f32 open = ev.loudnessCm * p.sensitivity;
    if (open <= 0.0f || d > open || d > p.maxRangeCm) return 0.0f;   // cheap rejects first

    f32 radius = open;
    if (occlusion) {
        const u32 hits = occlusion(occlusionUser, ev.pos, ear);
        for (u32 i = 0; i < hits && i < 8; ++i) radius *= p.occlusionFactor;
    }
    if (d >= radius) return 0.0f;
    return 1.0f - d / radius;
}

const HeardMemory& HearingMemory::remember(const Vec3& pos, f32 level, u32 tag, u32 source,
                                           f32 mergeRadiusCm) {
    for (HeardMemory& m : entries_) {
        const bool same = source != 0 ? (m.source == source && m.tag == tag)
                                      : (m.source == 0 && m.tag == tag && dist(m.pos, pos) <= mergeRadiusCm);
        if (!same) continue;
        m.pos = pos;
        m.level = level;
        m.ageSec = 0.0f;
        m.confidence = 1.0f;
        return m;
    }

    HeardMemory fresh;
    fresh.pos = pos;
    fresh.level = level;
    fresh.tag = tag;
    fresh.source = source;

    if (entries_.size() < capacity_) {
        entries_.push_back(fresh);
        return entries_.back();
    }
    // Full: replace the entry closest to being forgotten (oldest on a tie, first on a further tie).
    usize worst = 0;
    for (usize i = 1; i < entries_.size(); ++i) {
        const HeardMemory& a = entries_[i];
        const HeardMemory& b = entries_[worst];
        if (a.confidence < b.confidence || (a.confidence == b.confidence && a.ageSec > b.ageSec)) worst = i;
    }
    entries_[worst] = fresh;
    return entries_[worst];
}

void HearingMemory::tick(f32 dt, f32 memorySec, std::vector<HeardMemory>* forgotten) {
    for (usize i = 0; i < entries_.size();) {
        HeardMemory& m = entries_[i];
        m.ageSec += dt;
        m.confidence = memorySec > 0.0f ? std::max(0.0f, 1.0f - m.ageSec / memorySec) : 0.0f;
        if (m.confidence <= 0.0f) {
            if (forgotten) forgotten->push_back(m);
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

const HeardMemory* HearingMemory::best() const {
    const HeardMemory* best = nullptr;
    for (const HeardMemory& m : entries_) {
        if (!best || m.confidence > best->confidence ||
            (m.confidence == best->confidence &&
             (m.level > best->level || (m.level == best->level && m.ageSec < best->ageSec))))
            best = &m;
    }
    return best;
}

} // namespace aver::synapse
