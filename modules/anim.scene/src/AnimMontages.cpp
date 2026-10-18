// MontageSystem: the montage players of every entity. See AnimMontages.hpp.
#include "aver/anim/AnimMontages.hpp"

#include "aver/anim/AnimGraphAsset.hpp"
#include "aver/anim/AnimSystem.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/Components.hpp"

#include <algorithm>
#include <filesystem>

namespace aver::anim {

namespace {

f32 clamp01(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

u64 slotHashOf(const MontageAsset& a) {
    return fnv1a64(std::string_view(a.slot.empty() ? std::string_view(kDefaultSlot) : std::string_view(a.slot)));
}

std::string eventName(const std::string& label, const MontageEvent& ev) {
    switch (ev.kind) {
        case MontageEvent::Kind::Notify:         return ev.name;
        case MontageEvent::Kind::NotifyBegin:    return ev.name + "_Begin";
        case MontageEvent::Kind::NotifyEnd:      return ev.name + "_End";
        case MontageEvent::Kind::Started:        return label + "_Started";
        case MontageEvent::Kind::SectionStarted: return label + "_Section_" + ev.name;
        case MontageEvent::Kind::BlendOut:       return label + "_BlendOut";
        case MontageEvent::Kind::Interrupted:    return label + "_Interrupted";
        case MontageEvent::Kind::Ended:          return label + "_Ended";
    }
    return {};
}

i32 sectionByHash(const MontagePlayer& mp, u64 hash) {
    if (hash == 0) return -1;
    for (i32 i = 0; i < mp.sectionCount(); ++i)
        if (fnv1a64(std::string_view(mp.sectionName(i))) == hash) return i;
    return -1;
}

} // namespace

const MontageSystem::Loaded* MontageSystem::loadAsset(AnimSystem& anim, u64 id) {
    if (const auto it = assets_.find(id); it != assets_.end()) return it->second.get();

    // Cached even on failure, so a bad file is reported once and not per play.
    const std::string path = anim.assetPath(id);
    auto l = std::make_unique<Loaded>();
    std::string why;
    if (path.empty() || !loadMontage(path, l->asset, &why) || !l->asset.valid(&why)) {
        AVER_WARN("[Montage] could not load montage 0x{:016X}: {}", id, why.empty() ? std::string("no path") : why);
        assets_.emplace(id, nullptr);
        return nullptr;
    }
    l->label = l->asset.name.empty() ? std::filesystem::path(path).stem().string() : l->asset.name;
    const Loaded* raw = l.get();
    assets_.emplace(id, std::move(l));
    return raw;
}

MontageSystem::Player* MontageSystem::find(scene::Entity e, u64 montageId) {
    const auto it = players_.find(e);
    if (it == players_.end()) return nullptr;
    for (usize i = it->second.size(); i-- > 0;) {
        Player& p = it->second[i];
        if (p.mp.finished() || p.driven) continue;
        if (montageId == 0 || p.montageId == montageId) return &p;
    }
    return nullptr;
}

const MontageSystem::Player* MontageSystem::find(scene::Entity e, u64 montageId) const {
    return const_cast<MontageSystem*>(this)->find(e, montageId);
}

void MontageSystem::drain(scene::Entity e, Player& p, Pending& out) {
    std::vector<MontageEvent> evs;
    p.mp.drainEvents(evs);
    for (const MontageEvent& ev : evs) out.push_back({e, eventName(p.loaded->label, ev)});
}

void MontageSystem::deliver(Pending& events, const Sink& sink) {
    // Moved out first: a sink is arbitrary code (a graph) and may start or stop montages in response.
    Pending local;
    local.swap(events);
    if (!sink) return;
    for (const auto& [e, name] : local) sink(e, name.c_str());
}

bool MontageSystem::play(AnimSystem& anim, scene::World& w, scene::Entity e, u64 montageId, u64 slotOverride,
                         u64 sectionHash, f32 rate, f32 blendIn) {
    if (montageId == 0 || !w.valid(e) || w.destroyPending(e)) return false;
    const auto* sm = w.component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh);
    if (!sm || sm->skeleton == 0) return false;
    const Loaded* L = loadAsset(anim, montageId);
    if (!L) return false;
    const u64 slot = slotOverride ? slotOverride : slotHashOf(L->asset);

    auto startOn = [&](Player& p) {
        i32 sec = sectionByHash(p.mp, sectionHash);
        if (sectionHash != 0 && sec < 0)
            AVER_WARN("[Montage] '{}' has no section with hash 0x{:016X}; starting at the first", L->label, sectionHash);
        p.mp.start(sec, rate, blendIn);
    };

    std::vector<Player>& list = players_[e];
    // A running montage restarts in place and becomes the newest of its slot.
    for (usize i = 0; i < list.size(); ++i) {
        if (list[i].montageId != montageId || list[i].driven) continue;
        Player p = std::move(list[i]);
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
        p.slot = slot;
        startOn(p);
        list.push_back(std::move(p));
        if (!w.hasComponent(e, scene::kComponentAnimator)) w.addComponent(e, scene::kComponentAnimator);
        return true;
    }

    Player np;
    np.montageId = montageId;
    np.slot = slot;
    np.loaded = L;
    // [wave5/ik-retarget] A segment authored for another rig plays as its retargeted bake on this entity.
    const bool bound = np.mp.bind(&L->asset, [&anim, skel = sm->skeleton](const std::string& path) {
        return anim.clipFor(fnv1a64(std::string_view(path)), skel);
    });
    if (!bound) {
        AVER_WARN("[Montage] '{}' has no clip that loads", L->label);
        if (list.empty()) players_.erase(e);
        return false;
    }

    // The previous top of this slot fades out; past the cap the oldest is dropped outright.
    for (usize i = list.size(); i-- > 0;)
        if (list[i].slot == slot && !list[i].mp.finished() && !list[i].driven) { list[i].mp.stop(-1.0f); break; }
    usize inSlot = 0;
    for (const Player& p : list) inSlot += (p.slot == slot && !p.driven) ? 1u : 0u;
    while (inSlot >= kMaxPerSlot) {
        for (usize i = 0; i < list.size(); ++i) {
            if (list[i].slot != slot || list[i].driven) continue;
            list[i].mp.finish();
            drain(e, list[i], deferred_);
            list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
            --inSlot;
            break;
        }
    }
    startOn(np);
    list.push_back(std::move(np));
    if (!w.hasComponent(e, scene::kComponentAnimator)) w.addComponent(e, scene::kComponentAnimator);
    return true;
}

u32 MontageSystem::stop(scene::Entity e, u64 montageId, f32 blendOut) {
    const auto it = players_.find(e);
    if (it == players_.end()) return 0;
    u32 n = 0;
    for (Player& p : it->second) {
        if (p.mp.finished() || p.driven || (montageId != 0 && p.montageId != montageId)) continue;
        p.mp.stop(blendOut);
        ++n;
    }
    return n;
}

u32 MontageSystem::stopSlot(scene::Entity e, u64 slotHash, f32 blendOut) {
    const auto it = players_.find(e);
    if (it == players_.end()) return 0;
    u32 n = 0;
    for (Player& p : it->second) {
        if (p.mp.finished() || p.driven || p.slot != slotHash) continue;
        p.mp.stop(blendOut);
        ++n;
    }
    return n;
}

bool MontageSystem::jump(scene::Entity e, u64 montageId, u64 sectionHash) {
    Player* p = find(e, montageId);
    if (!p) return false;
    const i32 sec = sectionByHash(p->mp, sectionHash);
    return sec >= 0 && p->mp.jumpToSection(sec);
}

bool MontageSystem::setNextSection(scene::Entity e, u64 montageId, u64 fromHash, u64 toHash) {
    Player* p = find(e, montageId);
    if (!p) return false;
    const i32 from = sectionByHash(p->mp, fromHash);
    if (from < 0) return false;
    if (toHash == 0) return p->mp.setNextSection(from, -1);
    const i32 to = sectionByHash(p->mp, toHash);
    return to >= 0 && p->mp.setNextSection(from, to);
}

bool MontageSystem::setRate(scene::Entity e, u64 montageId, f32 rate) {
    Player* p = find(e, montageId);
    if (!p) return false;
    p->mp.setRate(rate);
    return true;
}

bool MontageSystem::isPlaying(scene::Entity e, u64 montageId) const { return find(e, montageId) != nullptr; }

bool MontageSystem::state(scene::Entity e, u64 montageId, f32 out[4], u64* sectionHash) const {
    const Player* p = find(e, montageId);
    if (!p) return false;
    if (out) {
        out[0] = p->mp.position();
        out[1] = p->mp.length();
        out[2] = p->mp.weight();
        out[3] = p->mp.rate();
    }
    if (sectionHash) *sectionHash = fnv1a64(std::string_view(p->mp.sectionName(p->mp.section())));
    return true;
}

void MontageSystem::stopAll() {
    for (auto& [e, list] : players_)
        for (Player& p : list) if (!p.driven) p.mp.finish();
    players_.clear();
    deferred_.clear();
}

void MontageSystem::clear(const Sink& sink) {
    // Everything ends while the assets (whose names the events need) are still alive; the sink runs last
    // and may start new montages, which are dropped below like the rest.
    Pending events;
    events.swap(deferred_);
    auto old = std::move(players_);
    players_.clear();
    for (auto& [e, list] : old)
        for (Player& p : list) {
            if (p.driven) continue;
            p.mp.finish();
            drain(e, p, events);
        }
    deliver(events, sink);
    players_.clear();
    deferred_.clear();
    old.clear();
    assets_.clear();
    clipAssets_.clear();
}

void MontageSystem::prune(const scene::World& w, const Sink& sink) {
    if (players_.empty()) return;
    Pending events;
    for (auto it = players_.begin(); it != players_.end();) {
        if (w.valid(it->first) && !w.destroyPending(it->first)) { ++it; continue; }
        // The entity is gone: no later tick will close a notify state, so close it now.
        for (Player& p : it->second) {
            if (p.driven) continue;
            p.mp.finish();
            drain(it->first, p, events);
        }
        it = players_.erase(it);
    }
    deliver(events, sink);
}

void MontageSystem::tick(AnimSystem& anim, scene::World& w, f32 dt, const Sink& sink) {
    Pending events;
    events.swap(deferred_);
    for (auto it = players_.begin(); it != players_.end();) {
        const scene::Entity e = it->first;
        const auto* a = w.component<scene::CAnimator>(e, scene::kComponentAnimator);
        const bool paused = a && (a->flags & scene::kAnimatorPaused);
        RootSpec spec;
        const bool rm = anim.rootSpec(w, e, kRootSrcMontage, spec);
        for (Player& p : it->second) {
            if (p.driven) {   // the sequencer owns the clock; a lapsed one is dropped without a word
                if (++p.stale > 2) p.dead = true;
                continue;
            }
            p.mp.setRootMotion(rm ? &spec : nullptr);
            p.delta = RootDelta{};
            if (!paused) {
                p.mp.advance(dt);
                p.delta = p.mp.consumeRootDelta();
            }
            drain(e, p, events);
        }
        it->second.erase(std::remove_if(it->second.begin(), it->second.end(),
                                        [](const Player& p) { return p.dead || p.mp.finished(); }),
                         it->second.end());
        if (it->second.empty()) it = players_.erase(it); else ++it;
    }
    deliver(events, sink);
}

// ---- [wave5/sequencer] driven players ----

const MontageSystem::Loaded* MontageSystem::loadClipAsset(AnimSystem& anim, u64 clipId) {
    if (const auto it = clipAssets_.find(clipId); it != clipAssets_.end()) return it->second.get();
    const fmt::OcAnimation* c = anim.clip(clipId);
    if (!c || !(c->duration > 0.0f)) {
        AVER_WARN("[Montage] clip 0x{:016X} cannot be driven (not loaded or no length)", clipId);
        clipAssets_.emplace(clipId, nullptr);
        return nullptr;
    }
    auto l = std::make_unique<Loaded>();
    l->asset.name = "Clip";
    l->asset.blendIn = l->asset.blendOut = 0.0f;
    l->asset.segments.push_back(MontageSegment{"@clip", 0.0f, -1.0f, 1.0f});
    l->label = "Clip";
    const Loaded* raw = l.get();
    clipAssets_.emplace(clipId, std::move(l));
    return raw;
}

bool MontageSystem::drive(AnimSystem& anim, scene::World& w, scene::Entity e, const Loaded* L, u64 assetId,
                          f32 position, f32 weight, u64 owner, bool clip) {
    if (!L) return false;
    std::vector<Player>& list = players_[e];
    Player* p = nullptr;
    for (Player& q : list)
        if (q.driven && q.owner == owner && q.montageId == assetId && !q.dead) { p = &q; break; }
    if (!p) {
        Player np;
        np.montageId = assetId;
        np.slot = slotHashOf(L->asset);
        np.loaded = L;
        np.driven = true;
        np.owner = owner;
        // clipFor, as play() does: a clip retargeted to this entity's skeleton plays its bake.
        const auto* skm = w.component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh);
        const u64 skel = skm ? skm->skeleton : 0;
        const bool bound = np.mp.bind(&L->asset, [&anim, assetId, clip, skel](const std::string& path) -> const fmt::OcAnimation* {
            return anim.clipFor(clip ? assetId : fnv1a64(std::string_view(path)), skel);
        });
        if (!bound) {
            AVER_WARN("[Montage] '{}' has no clip that loads", L->label);
            if (list.empty()) players_.erase(e);
            return false;
        }
        list.push_back(std::move(np));
        p = &list.back();
    }
    p->stale = 0;
    p->mp.drive(position, weight);
    if (!w.hasComponent(e, scene::kComponentAnimator)) w.addComponent(e, scene::kComponentAnimator);
    return true;
}

bool MontageSystem::driveMontage(AnimSystem& anim, scene::World& w, scene::Entity e, u64 montageId, f32 position,
                                 f32 weight, u64 owner) {
    if (montageId == 0 || !w.valid(e) || w.destroyPending(e)) return false;
    const auto* sm = w.component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh);
    if (!sm || sm->skeleton == 0) return false;
    return drive(anim, w, e, loadAsset(anim, montageId), montageId, position, weight, owner, false);
}

bool MontageSystem::driveClip(AnimSystem& anim, scene::World& w, scene::Entity e, u64 clipId, f32 clipTime,
                              f32 weight, u64 owner) {
    if (clipId == 0 || !w.valid(e) || w.destroyPending(e)) return false;
    const auto* sm = w.component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh);
    if (!sm || sm->skeleton == 0) return false;
    return drive(anim, w, e, loadClipAsset(anim, clipId), clipId, clipTime, weight, owner, true);
}

void MontageSystem::releaseDriven(scene::Entity e, u64 owner, u64 assetId) {
    const auto it = players_.find(e);
    if (it == players_.end()) return;
    std::vector<Player>& list = it->second;
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Player& p) {
                   return p.driven && p.owner == owner && (assetId == 0 || p.montageId == assetId);
               }), list.end());
    if (list.empty()) players_.erase(it);
}

u32 MontageSystem::drivenCount(scene::Entity e) const {
    const auto it = players_.find(e);
    if (it == players_.end()) return 0;
    u32 n = 0;
    for (const Player& p : it->second) n += p.driven ? 1u : 0u;
    return n;
}

void MontageSystem::slotHashes(scene::Entity e, std::vector<u64>& out) const {
    out.clear();
    const auto it = players_.find(e);
    if (it == players_.end()) return;
    for (const Player& p : it->second)
        if (std::find(out.begin(), out.end(), p.slot) == out.end()) out.push_back(p.slot);
}

bool MontageSystem::evaluateSlot(scene::Entity e, u64 slotHash, const fmt::OcSkeleton& skel, const Pose& base,
                                 Pose& out, f32& coverage) {
    const auto it = players_.find(e);
    if (it == players_.end()) return false;
    bool have = false;
    coverage = 0.0f;
    for (const Player& p : it->second) {
        if (p.slot != slotHash) continue;
        const f32 w = p.mp.weight();
        if (!(w > 0.0f)) continue;
        if (!have) {
            p.mp.evaluate(skel, base, out);
            coverage = w;
            have = true;
            continue;
        }
        p.mp.evaluate(skel, base, scratchPose_);
        Pose mixed;
        blendPose(out, scratchPose_, w, mixed);
        out.swap(mixed);
        coverage = 1.0f - (1.0f - coverage) * (1.0f - w);
    }
    coverage = clamp01(coverage);
    return have;
}

bool MontageSystem::rootDelta(scene::Entity e, u64 slotHash, RootDelta& out, f32& weight) const {
    const auto it = players_.find(e);
    if (it == players_.end()) return false;
    bool have = false;
    weight = 0.0f;
    for (const Player& p : it->second) {
        if (p.slot != slotHash || p.driven || !p.mp.hasRootMotion()) continue;
        const f32 w = p.mp.weight();
        if (!(w > 0.0f)) continue;
        if (!have) { out = p.delta; weight = w; have = true; continue; }
        out = lerpDelta(out, p.delta, w);
        weight = 1.0f - (1.0f - weight) * (1.0f - w);
    }
    weight = clamp01(weight);
    return have;
}

std::string MontageSystem::slotLabel(scene::Entity e, u64 slotHash) const {
    const auto it = players_.find(e);
    if (it != players_.end())
        for (const Player& p : it->second)
            if (p.slot == slotHash && p.loaded) return p.loaded->asset.slot;
    return {};
}

// ---- the framework relay's provider ----
// Op codes mirror AVER_FW_ANIM_OP_* in framework_anim_abi.h; this module does not link the framework.
namespace {
constexpr i32 kOpPlay = 1, kOpStop = 2, kOpStopSlot = 3, kOpJump = 4, kOpNext = 5, kOpRate = 6, kOpPlaying = 7,
              kOpState = 8;
}

i32 montageProvider(i32 op, i32 entity, i64 h0, i64 h1, i64 h2, f32 f0, f32 f1, f32* outF, i64* outI, void* user) {
    if (!user) return 0;
    AnimSystem& anim = *static_cast<AnimSystem*>(user);
    MontageSystem& ms = anim.montages();
    scene::World& w = scene::World::instance();
    const scene::Entity e = static_cast<scene::Entity>(entity);
    switch (op) {
        case kOpPlay:
            return ms.play(anim, w, e, static_cast<u64>(h0), static_cast<u64>(h1), static_cast<u64>(h2), f0, f1) ? 1 : 0;
        case kOpStop:      return static_cast<i32>(ms.stop(e, static_cast<u64>(h0), f0));
        case kOpStopSlot:  return static_cast<i32>(ms.stopSlot(e, static_cast<u64>(h0), f0));
        case kOpJump:      return ms.jump(e, static_cast<u64>(h0), static_cast<u64>(h1)) ? 1 : 0;
        case kOpNext:
            return ms.setNextSection(e, static_cast<u64>(h0), static_cast<u64>(h1), static_cast<u64>(h2)) ? 1 : 0;
        case kOpRate:      return ms.setRate(e, static_cast<u64>(h0), f0) ? 1 : 0;
        case kOpPlaying:   return ms.isPlaying(e, static_cast<u64>(h0)) ? 1 : 0;
        case kOpState: {
            f32 v[4] = {};
            u64 sec = 0;
            if (!ms.state(e, static_cast<u64>(h0), v, &sec)) return 0;
            if (outF) for (int i = 0; i < 4; ++i) outF[i] = v[i];
            if (outI) outI[0] = static_cast<i64>(sec);
            return 1;
        }
        default: return 0;
    }
}

} // namespace aver::anim
