#include "aver/assets/TexturePlanner.hpp"

#include <algorithm>
#include <cmath>

namespace aver::assets {

namespace {
constexpr u32 kNoWant = ~0u;
constexpr u32 kMaxFitBias = kMaxTextureMips;
}

// ---- TexturePlanner ----------------------------------------------------------------------------------

u32 TexturePlanner::add(const PlannerTextureDesc& d, u64 frame) {
    u32 id;
    if (!free_.empty()) { id = free_.back(); free_.pop_back(); }
    else { id = static_cast<u32>(recs_.size()); recs_.emplace_back(); }
    Rec& r = recs_[id];
    const u32 gen = r.generation;   // survives reuse, so a result for the previous occupant stays stale
    r = Rec{};
    r.generation = gen + 1;
    r.live = true;
    r.d = d;
    if (r.d.fullMips == 0) r.d.fullMips = 1;
    if (r.d.firstMip >= r.d.fullMips) r.d.firstMip = r.d.fullMips - 1;
    r.allowed = allowedTopMask(r.d.fullW, r.d.fullH, r.d.fullMips);
    r.tail = firstMipWithin(r.d.fullW, r.d.fullH, r.d.fullMips, r.allowed, s_.tailDim);
    const u32 init = firstMipWithin(r.d.fullW, r.d.fullH, r.d.fullMips, r.allowed, s_.initialDim);
    r.initial = init < r.tail ? init : r.tail;
    r.state = d.pinned ? PlannerState::Pinned : PlannerState::Idle;
    r.target = r.d.firstMip;
    r.lastSeen = frame;
    ++live_;
    return id;
}

void TexturePlanner::remove(u32 id) {
    if (id >= recs_.size() || !recs_[id].live) return;
    recs_[id].live = false;
    ++recs_[id].generation;
    free_.push_back(id);
    --live_;
}

void TexturePlanner::onSeen(u32 id, f32 footprint, u64 frame) {
    if (id >= recs_.size() || !recs_[id].live) return;
    Rec& r = recs_[id];
    const u32 want = snap(r, wantedFirstMip(footprint, r.d.fullW, r.d.fullH, r.d.fullMips, s_.biasMips, r.allowed));
    // A texture seen again in the same frame (several draws) keeps the finer want.
    if (r.recentCount > 0) {
        const u32 last = (r.recentPos + 15) & 15;
        if (r.recentFrame[last] == frame) { if (want < r.recentWant[last]) r.recentWant[last] = want; r.lastSeen = frame; return; }
    }
    r.recentWant[r.recentPos] = want;
    r.recentFrame[r.recentPos] = frame;
    r.recentPos = (r.recentPos + 1) & 15;
    if (r.recentCount < 16) ++r.recentCount;
    r.lastSeen = frame;
}

// The finest want over the last seenWindow frames, or kNoWant when the texture was not seen in them.
u32 TexturePlanner::seenWant(const Rec& r, u64 frame) const {
    const u32 window = s_.seenWindow < 1 ? 1 : (s_.seenWindow > 16 ? 16 : s_.seenWindow);
    u32 best = kNoWant;
    for (u32 i = 0; i < r.recentCount; ++i) {
        if (r.recentFrame[i] + window <= frame) continue;
        if (r.recentWant[i] < best) best = r.recentWant[i];
    }
    return best;
}

std::vector<PlannerRequest> TexturePlanner::plan(u64 frame, u64 pool) {
    struct Active { u32 id; u32 want; };
    std::vector<Active> active;
    std::vector<u32> cold;
    u64 fixed = 0, resident = 0;
    u32 loading = 0;

    for (u32 id = 0; id < recs_.size(); ++id) {
        Rec& r = recs_[id];
        if (!r.live) continue;
        resident += cost(r, r.d.firstMip);
        if (r.state == PlannerState::Pinned) { r.target = r.d.firstMip; fixed += cost(r, r.d.firstMip); continue; }
        if (r.state == PlannerState::Loading) {
            ++loading;
            fixed += cost(r, r.target < r.d.firstMip ? r.target : r.d.firstMip);   // the larger of the two windows
            continue;
        }
        if (!feedback_) { active.push_back({id, 0}); continue; }
        const u32 w = seenWant(r, frame);
        if (w != kNoWant) { active.push_back({id, w}); continue; }
        if (frame - r.lastSeen < s_.holdFrames) { r.target = r.d.firstMip; fixed += cost(r, r.d.firstMip); continue; }
        cold.push_back(id);
        fixed += cost(r, r.d.firstMip > r.initial ? r.d.firstMip : r.initial);   // baseline: no finer than initial
    }

    // The smallest global coarsening that makes the active textures fit what the fixed ones leave.
    const auto activeCost = [&](u32 b) {
        u64 t = 0;
        for (const Active& a : active) {
            const Rec& r = recs_[a.id];
            t += cost(r, snap(r, a.want + b));
        }
        return t;
    };
    u32 bias = 0;
    while (bias < kMaxFitBias && fixed + activeCost(bias) > pool) ++bias;
    u64 planned = fixed + activeCost(bias);

    for (const Active& a : active) {
        Rec& r = recs_[a.id];
        r.target = snap(r, a.want + bias);
    }

    // Spend what is left on unseen textures that are still resident finer than their baseline, most recently seen first.
    std::sort(cold.begin(), cold.end(), [&](u32 a, u32 b) { return recs_[a].lastSeen > recs_[b].lastSeen; });
    for (const u32 id : cold) {
        Rec& r = recs_[id];
        const u32 base = r.d.firstMip > r.initial ? r.d.firstMip : r.initial;
        r.target = base;
        if (base > r.d.firstMip) {   // resident finer than the baseline
            const u64 extra = cost(r, r.d.firstMip) - cost(r, base);
            if (planned + extra <= pool) { r.target = r.d.firstMip; planned += extra; }
        }
    }

    const bool overPool = resident > pool;

    // Hysteresis: upgrades at once, downgrades after the lower target has persisted (pressure overrides).
    for (u32 id = 0; id < recs_.size(); ++id) {
        Rec& r = recs_[id];
        if (!r.live || r.state != PlannerState::Idle) continue;
        if (r.target > r.d.firstMip) {
            if (r.downSince == 0) r.downSince = frame ? frame : 1;
            if (!overPool && frame - r.downSince < s_.downgradeFrames) r.target = r.d.firstMip;
        } else {
            r.downSince = 0;
        }
    }

    std::vector<PlannerRequest> trims, ups;
    for (u32 id = 0; id < recs_.size(); ++id) {
        Rec& r = recs_[id];
        if (!r.live || r.state != PlannerState::Idle || r.target == r.d.firstMip) continue;
        if (r.retryAt > frame) { r.target = r.d.firstMip; continue; }
        PlannerRequest q;
        q.id = id;
        q.fromFirst = r.d.firstMip;
        q.toFirst = r.target;
        if (q.trim()) trims.push_back(q);
        else {
            q.uploadBytes = windowBytes(r.d.fullW, r.d.fullH, r.target, r.d.firstMip);
            ups.push_back(q);
        }
    }
    std::sort(trims.begin(), trims.end(), [&](const PlannerRequest& a, const PlannerRequest& b) {
        // Largest saving first.
        return (cost(recs_[a.id], a.fromFirst) - cost(recs_[a.id], a.toFirst)) >
               (cost(recs_[b.id], b.fromFirst) - cost(recs_[b.id], b.toFirst));
    });
    std::sort(ups.begin(), ups.end(), [&](const PlannerRequest& a, const PlannerRequest& b) {
        const u32 da = a.fromFirst - a.toFirst, db = b.fromFirst - b.toFirst;
        if (da != db) return da > db;
        return recs_[a.id].lastSeen > recs_[b.id].lastSeen;
    });

    std::vector<PlannerRequest> out;
    u32 swaps = 0;
    u64 running = resident;
    for (const PlannerRequest& q : trims) {
        if (!overPool && swaps >= s_.maxSwapsPerFrame) break;
        running -= cost(recs_[q.id], q.fromFirst) - cost(recs_[q.id], q.toFirst);
        out.push_back(q);
        ++swaps;
    }
    u64 spent = 0;
    for (const PlannerRequest& q : ups) {
        if (swaps >= s_.maxSwapsPerFrame) break;
        const u64 grow = cost(recs_[q.id], q.toFirst) - cost(recs_[q.id], q.fromFirst);
        if (running + grow > pool) continue;   // residency never climbs past the pool
        if (spent != 0 && spent + q.uploadBytes > s_.uploadBytesPerFrame) continue;
        running += grow;
        spent += q.uploadBytes;
        out.push_back(q);
        ++swaps;
    }

    for (PlannerRequest& q : out) {
        Rec& r = recs_[q.id];
        r.state = PlannerState::Loading;
        r.generation++;
        q.generation = r.generation;
        ++loading;
    }
    stats_.residentBytes = resident;
    stats_.targetBytes = planned;
    stats_.poolBytes = pool;
    stats_.fitBias = bias;
    stats_.overPool = overPool;
    stats_.loading = loading;
    return out;
}

bool TexturePlanner::complete(u32 id, u32 generation, u32 newFirstMip, u64 frame) {
    (void)frame;
    if (id >= recs_.size() || !recs_[id].live) return false;
    Rec& r = recs_[id];
    if (r.generation != generation || r.state != PlannerState::Loading) return false;
    r.d.firstMip = newFirstMip < r.d.fullMips ? newFirstMip : r.d.fullMips - 1;
    r.target = r.d.firstMip;
    r.state = PlannerState::Idle;
    r.downSince = 0;
    return true;
}

void TexturePlanner::fail(u32 id, u32 generation, bool permanent, u64 frame) {
    if (id >= recs_.size() || !recs_[id].live) return;
    Rec& r = recs_[id];
    if (r.generation != generation || r.state != PlannerState::Loading) return;
    r.target = r.d.firstMip;
    r.state = permanent ? PlannerState::Pinned : PlannerState::Idle;
    r.retryAt = frame + s_.retryFrames;
}

void TexturePlanner::pin(u32 id) {
    if (id >= recs_.size() || !recs_[id].live) return;
    Rec& r = recs_[id];
    r.state = PlannerState::Pinned;
    r.target = r.d.firstMip;
    ++r.generation;   // an in-flight result is now stale
}

u32 TexturePlanner::firstMip(u32 id) const { return id < recs_.size() && recs_[id].live ? recs_[id].d.firstMip : 0; }
u32 TexturePlanner::targetMip(u32 id) const { return id < recs_.size() && recs_[id].live ? recs_[id].target : 0; }
u32 TexturePlanner::tailFirst(u32 id) const { return id < recs_.size() && recs_[id].live ? recs_[id].tail : 0; }
u32 TexturePlanner::initialFirst(u32 id) const { return id < recs_.size() && recs_[id].live ? recs_[id].initial : 0; }
PlannerState TexturePlanner::state(u32 id) const { return id < recs_.size() && recs_[id].live ? recs_[id].state : PlannerState::Pinned; }
u64 TexturePlanner::residentBytes(u32 id) const {
    if (id >= recs_.size() || !recs_[id].live) return 0;
    return cost(recs_[id], recs_[id].d.firstMip);
}

// ---- PoolController -----------------------------------------------------------------------------------

u64 PoolController::update(bool vramSupported, u64 localBudget, u64 localUsage, u64 residentTextureBytes, u64 tailSumBytes, f64 dt) {
    u64 base;
    if (s_.poolBytes) base = s_.poolBytes;
    else if (!vramSupported || localBudget == 0) base = s_.unsupportedBytes;
    else {
        const u64 nonTexture = localUsage > residentTextureBytes ? localUsage - residentTextureBytes : 0;
        const u64 room = localBudget > nonTexture + s_.reserveBytes ? localBudget - nonTexture - s_.reserveBytes : 0;
        u64 want = static_cast<u64>(static_cast<f64>(s_.poolFraction) * static_cast<f64>(localBudget));
        if (want < s_.minAutoBytes) want = s_.minAutoBytes;
        base = want < room ? want : room;
    }
    if (vramSupported && localBudget > 0 && dt > 0.0) {
        const f64 ratio = static_cast<f64>(localUsage) / static_cast<f64>(localBudget);
        if (ratio > static_cast<f64>(s_.highWater)) scale_ *= std::pow(static_cast<f64>(s_.shrinkPerSecond), dt);
        else if (ratio < static_cast<f64>(s_.lowWater)) scale_ = std::min(1.0, scale_ * std::pow(static_cast<f64>(s_.growPerSecond), dt));
        if (scale_ < 1e-3) scale_ = 1e-3;
    }
    u64 pool = static_cast<u64>(static_cast<f64>(base) * scale_);
    const u64 floor = tailSumBytes + s_.floorExtraBytes;
    return pool < floor ? floor : pool;
}

// ---- UploadRing ---------------------------------------------------------------------------------------

u64 UploadRing::reserve(u64 bytes) {
    u64 size = ((bytes + align_ - 1) / align_) * align_;
    if (size == 0) size = align_;
    u64 prevEnd = 0;
    for (usize i = 0; i <= blocks_.size(); ++i) {
        const u64 gapEnd = i < blocks_.size() ? blocks_[i].offset : cap_;
        if (gapEnd >= prevEnd && gapEnd - prevEnd >= size) {
            blocks_.insert(blocks_.begin() + static_cast<isize>(i), Block{prevEnd, size, false, 0});
            return prevEnd;
        }
        if (i < blocks_.size()) prevEnd = blocks_[i].offset + blocks_[i].size;
    }
    return kNone;
}

bool UploadRing::submit(u64 offset, u64 frame) {
    for (Block& b : blocks_)
        if (b.offset == offset && !b.submitted) { b.submitted = true; b.frame = frame; return true; }
    return false;
}

bool UploadRing::cancel(u64 offset) {
    for (usize i = 0; i < blocks_.size(); ++i)
        if (blocks_[i].offset == offset && !blocks_[i].submitted) { blocks_.erase(blocks_.begin() + static_cast<isize>(i)); return true; }
    return false;
}

void UploadRing::retire(u64 currentFrame) {
    blocks_.erase(std::remove_if(blocks_.begin(), blocks_.end(),
                                 [&](const Block& b) { return b.submitted && b.frame + delay_ <= currentFrame; }),
                  blocks_.end());
}

u64 UploadRing::inUse() const {
    u64 t = 0;
    for (const Block& b : blocks_) t += b.size;
    return t;
}

} // namespace aver::assets
