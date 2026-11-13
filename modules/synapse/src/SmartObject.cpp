#include "aver/synapse/SmartObject.hpp"

#include <algorithm>

namespace aver::synapse {
namespace {
constexpr u32 kIndexBits = 12;
constexpr u32 kIndexMask = (1u << kIndexBits) - 1;
constexpr u32 kGenMask = (1u << (31 - kIndexBits)) - 1;   // handles cross the ABI as positive int32
} // namespace

const SoDef* SmartObjectRegistry::defOf(u32 object) const {
    const auto it = objects_.find(object);
    return it == objects_.end() ? nullptr : it->second.def;
}

std::vector<u32> SmartObjectRegistry::objects() const {
    std::vector<u32> r;
    r.reserve(objects_.size());
    for (const auto& [id, o] : objects_) r.push_back(id);
    return r;
}

SmartObjectRegistry::ClaimRec* SmartObjectRegistry::rec(u32 handle) {
    const u32 idx = handle & kIndexMask;
    if (handle == 0 || idx >= claims_.size()) return nullptr;
    ClaimRec& r = claims_[idx];
    return (r.active && r.gen == (handle >> kIndexBits)) ? &r : nullptr;
}

const SmartObjectRegistry::ClaimRec* SmartObjectRegistry::rec(u32 handle) const {
    return const_cast<SmartObjectRegistry*>(this)->rec(handle);
}

SoSlotState SmartObjectRegistry::visibleState(const Obj& o, const Slot& s) const {
    if (s.claim != 0) return s.state;
    if (!o.enabled || s.disabled) return SoSlotState::Disabled;
    return s.state;
}

bool SmartObjectRegistry::releaseRec(u32 handle, SoReleaseReason reason, bool cooldown, SoRelease* out) {
    ClaimRec* r = rec(handle);
    if (!r) return false;
    const auto oit = objects_.find(r->object);
    SoRelease rel;
    rel.claim = handle;
    rel.slot = SoSlotRef{r->object, r->slot};
    rel.user = r->user;
    rel.reason = reason;
    rel.wasUsed = r->state == SoSlotState::Occupied;
    if (oit != objects_.end()) {
        Obj& o = oit->second;
        rel.def = o.def && r->slot < o.def->slots.size() ? &o.def->slots[r->slot] : nullptr;
        if (r->slot < o.slots.size()) {
            Slot& s = o.slots[r->slot];
            s.claim = 0;
            if (cooldown && rel.def && rel.def->cooldownSec > 0) {
                s.state = SoSlotState::Cooldown;
                s.timer = rel.def->cooldownSec;
            } else {
                s.state = SoSlotState::Free;
                s.timer = 0;
            }
        }
    }
    const u32 idx = handle & kIndexMask;
    r->active = false;
    r->gen = (r->gen + 1) & kGenMask;
    if (r->gen == 0) r->gen = 1;
    freeClaims_.push_back(idx);
    --liveClaims_;
    if (out) *out = rel;
    return true;
}

void SmartObjectRegistry::setObject(u32 object, const SoDef* def, bool enabled) {
    if (!def) return;
    const auto it = objects_.find(object);
    if (it == objects_.end()) {
        Obj o;
        o.def = def;
        o.enabled = enabled;
        o.slots.resize(def->slots.size());
        objects_.emplace(object, std::move(o));
        return;
    }
    Obj& o = it->second;
    o.enabled = enabled;
    if (o.def == def) return;

    // A different definition: carry state across by slot name, release what has no counterpart.
    std::vector<Slot> next(def->slots.size());
    std::vector<i32> remap(o.slots.size(), -1);
    for (usize i = 0; i < o.slots.size() && o.def && i < o.def->slots.size(); ++i) {
        for (usize j = 0; j < def->slots.size(); ++j) {
            if (def->slots[j].name == o.def->slots[i].name) {
                next[j] = o.slots[i];
                remap[i] = static_cast<i32>(j);
                break;
            }
        }
    }
    for (ClaimRec& c : claims_) {
        if (!c.active || c.object != object) continue;
        const i32 m = c.slot < remap.size() ? remap[c.slot] : -1;
        if (m >= 0) {
            c.slot = static_cast<u32>(m);
            continue;
        }
        SoRelease rel;
        const u32 handle = (c.gen << kIndexBits) | static_cast<u32>(&c - claims_.data());
        const SoDef* old = o.def;
        releaseRec(handle, SoReleaseReason::ObjectGone, false, &rel);
        if (old && rel.slot.slot < old->slots.size()) rel.def = &old->slots[rel.slot.slot];
        pending_.push_back(rel);
    }
    o.def = def;
    o.slots = std::move(next);
}

void SmartObjectRegistry::setWorld(u32 object, u32 slot, const SoWorldSlot& w) {
    const auto it = objects_.find(object);
    if (it != objects_.end() && slot < it->second.slots.size()) it->second.slots[slot].world = w;
}

void SmartObjectRegistry::removeObject(u32 object, std::vector<SoRelease>& released) {
    const auto it = objects_.find(object);
    if (it == objects_.end()) return;
    for (usize i = 0; i < claims_.size(); ++i) {
        ClaimRec& c = claims_[i];
        if (!c.active || c.object != object) continue;
        SoRelease rel;
        releaseRec((c.gen << kIndexBits) | static_cast<u32>(i), SoReleaseReason::ObjectGone, false, &rel);
        released.push_back(rel);
    }
    objects_.erase(object);
}

u32 SmartObjectRegistry::slotCount(u32 object) const {
    const auto it = objects_.find(object);
    return it == objects_.end() ? 0 : static_cast<u32>(it->second.slots.size());
}

bool SmartObjectRegistry::slotInfo(u32 object, u32 slot, SoSlotInfo& out) const {
    const auto it = objects_.find(object);
    if (it == objects_.end() || slot >= it->second.slots.size()) return false;
    const Obj& o = it->second;
    const Slot& s = o.slots[slot];
    out = SoSlotInfo{};
    out.state = visibleState(o, s);
    out.claim = s.claim;
    if (const ClaimRec* r = rec(s.claim)) out.user = r->user;
    out.timer = s.timer;
    out.world = s.world;
    out.def = o.def && slot < o.def->slots.size() ? &o.def->slots[slot] : nullptr;
    return true;
}

bool SmartObjectRegistry::setSlotEnabled(u32 object, u32 slot, bool enabled) {
    const auto it = objects_.find(object);
    if (it == objects_.end() || slot >= it->second.slots.size()) return false;
    it->second.slots[slot].disabled = !enabled;
    return true;
}

u32 SmartObjectRegistry::find(const SoQuery& q, SoSlotRef* out, f32* distOut, u32 cap) const {
    struct Hit { f32 d; u32 object, slot; };
    std::vector<Hit> hits;
    for (const auto& [id, o] : objects_) {
        if (!o.def) continue;
        for (u32 i = 0; i < o.slots.size() && i < o.def->slots.size(); ++i) {
            const Slot& s = o.slots[i];
            const SoSlotDef& d = o.def->slots[i];
            const SoSlotState st = visibleState(o, s);
            if (st == SoSlotState::Disabled) continue;
            if (!q.includeClaimed && st != SoSlotState::Free) continue;
            const f32 dist = (s.world.pos - q.origin).size();
            if (dist > q.radius) continue;
            if (!q.slotTags.v.empty()) {
                TagList have = o.def->tags;
                for (const std::string& t : d.tags.v) have.v.push_back(t);
                if (q.matchAll ? !have.hasAll(q.slotTags) : !have.hasAny(q.slotTags)) continue;
            }
            if (!q.userTags.hasAll(d.requireAll) || !q.userTags.hasNone(d.blockedBy)) continue;
            hits.push_back({dist, id, i});
        }
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.d != b.d) return a.d < b.d;
        if (a.object != b.object) return a.object < b.object;
        return a.slot < b.slot;
    });
    const u32 n = std::min<u32>({static_cast<u32>(hits.size()), cap, q.max});
    for (u32 i = 0; i < n; ++i) {
        if (out) out[i] = SoSlotRef{hits[i].object, hits[i].slot};
        if (distOut) distOut[i] = hits[i].d;
    }
    return n;
}

u32 SmartObjectRegistry::claim(SoSlotRef slot, u32 user, const TagList& userTags, f32 ttlOverride, SoRefusal* why) {
    const auto refuse = [&](SoRefusal r) {
        if (why) *why = r;
        return 0u;
    };
    if (why) *why = SoRefusal::None;
    if (user == 0) return refuse(SoRefusal::NoUser);
    const auto it = objects_.find(slot.object);
    if (it == objects_.end() || slot.slot >= it->second.slots.size() || !it->second.def || slot.slot >= it->second.def->slots.size())
        return refuse(SoRefusal::NoSuchSlot);
    Obj& o = it->second;
    Slot& s = o.slots[slot.slot];
    const SoSlotDef& d = o.def->slots[slot.slot];
    if (!o.enabled || s.disabled) return refuse(SoRefusal::Disabled);
    if (s.state != SoSlotState::Free) return refuse(SoRefusal::NotFree);
    if (!userTags.hasAll(d.requireAll) || !userTags.hasNone(d.blockedBy)) return refuse(SoRefusal::Tags);
    u32 held = 0;
    for (const ClaimRec& c : claims_) held += (c.active && c.user == user) ? 1 : 0;
    if (held >= kSoMaxClaimsPerUser) return refuse(SoRefusal::UserLimit);

    u32 idx;
    if (!freeClaims_.empty()) {
        idx = freeClaims_.back();
        freeClaims_.pop_back();
    } else {
        if (claims_.size() > kIndexMask) return refuse(SoRefusal::NotFree);
        idx = static_cast<u32>(claims_.size());
        claims_.emplace_back();
    }
    ClaimRec& c = claims_[idx];
    c.active = true;
    c.object = slot.object;
    c.slot = slot.slot;
    c.user = user;
    c.state = SoSlotState::Claimed;
    c.age = 0;
    c.ttl = ttlOverride < 0 ? d.claimTtlSec : ttlOverride;
    ++liveClaims_;
    const u32 handle = (c.gen << kIndexBits) | idx;
    s.state = SoSlotState::Claimed;
    s.claim = handle;
    s.timer = 0;
    return handle;
}

bool SmartObjectRegistry::use(u32 claim) {
    ClaimRec* r = rec(claim);
    if (!r || r->state != SoSlotState::Claimed) return false;
    r->state = SoSlotState::Occupied;
    r->age = 0;
    const auto it = objects_.find(r->object);
    if (it != objects_.end() && r->slot < it->second.slots.size()) it->second.slots[r->slot].state = SoSlotState::Occupied;
    return true;
}

bool SmartObjectRegistry::release(u32 claim, SoReleaseReason reason, SoRelease* out) {
    return releaseRec(claim, reason, reason != SoReleaseReason::ObjectGone, out);
}

bool SmartObjectRegistry::claimInfo(u32 claim, SoClaimInfo& out) const {
    const ClaimRec* r = rec(claim);
    if (!r) return false;
    out = SoClaimInfo{};
    out.handle = claim;
    out.slot = SoSlotRef{r->object, r->slot};
    out.user = r->user;
    out.state = r->state;
    out.age = r->age;
    const auto it = objects_.find(r->object);
    if (it != objects_.end() && r->slot < it->second.slots.size()) {
        out.world = it->second.slots[r->slot].world;
        out.def = it->second.def && r->slot < it->second.def->slots.size() ? &it->second.def->slots[r->slot] : nullptr;
    }
    return true;
}

u32 SmartObjectRegistry::claimsOf(u32 user, u32* out, u32 cap) const {
    u32 n = 0;
    for (usize i = 0; i < claims_.size(); ++i) {
        const ClaimRec& c = claims_[i];
        if (!c.active || c.user != user) continue;
        if (out && n < cap) out[n] = (c.gen << kIndexBits) | static_cast<u32>(i);
        ++n;
    }
    return out ? std::min(n, cap) : n;
}

void SmartObjectRegistry::releaseUser(u32 user, std::vector<SoRelease>& out) {
    for (usize i = 0; i < claims_.size(); ++i) {
        const ClaimRec& c = claims_[i];
        if (!c.active || c.user != user) continue;
        SoRelease rel;
        releaseRec((c.gen << kIndexBits) | static_cast<u32>(i), SoReleaseReason::UserGone, true, &rel);
        out.push_back(rel);
    }
}

void SmartObjectRegistry::tick(f32 dt, std::vector<SoRelease>& released) {
    for (SoRelease& p : pending_) released.push_back(p);
    pending_.clear();
    // Cooldowns first: a slot released by the time-out below starts its cooldown full, from the next tick.
    for (auto& [id, o] : objects_) {
        for (Slot& s : o.slots) {
            if (s.state != SoSlotState::Cooldown) continue;
            s.timer -= dt;
            if (s.timer <= 0) {
                s.timer = 0;
                s.state = SoSlotState::Free;
            }
        }
    }
    for (usize i = 0; i < claims_.size(); ++i) {
        ClaimRec& c = claims_[i];
        if (!c.active) continue;
        c.age += dt;
        if (c.state == SoSlotState::Claimed && c.ttl > 0 && c.age >= c.ttl) {
            SoRelease rel;
            releaseRec((c.gen << kIndexBits) | static_cast<u32>(i), SoReleaseReason::Expired, true, &rel);
            released.push_back(rel);
        }
    }
}

} // namespace aver::synapse
