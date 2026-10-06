#include "aver/synapse/SynapseTactics.hpp"

#include "aver/core/Assert.hpp"
#include "aver/synapse/Nav.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace aver::synapse {
namespace {

// An agent counts as "in" its cover point when this close to it.
constexpr f32 kInCoverReachCm = 80.0f;

V2 worldXY(scene::World& w, scene::Entity e, f32* z = nullptr) {
    const Mat4& m = w.worldMatrix(e);
    if (z) *z = m.m[3][2];
    return {m.m[3][0], m.m[3][1]};
}

// Local +X in the ground plane: the way a marker faces.
V2 worldForwardXY(scene::World& w, scene::Entity e) {
    const Mat4& m = w.worldMatrix(e);
    return norm2(V2{m.m[0][0], m.m[0][1]}, V2{1.0f, 0.0f});
}

} // namespace

TacticsSystem::TacticsSystem() = default;

u32 TacticsSystem::registerComponents(scene::World& world) {
    {
        auto b = world.registerComponent<CSynapseCoverMarker>("CSynapseCoverMarker");
        b.field("height", scene::FieldKind::I32, static_cast<u16>(offsetof(CSynapseCoverMarker, height)))
            .field("arcHalfAngleDeg", scene::FieldKind::F32,
                   static_cast<u16>(offsetof(CSynapseCoverMarker, arcHalfAngleDeg)))
            .field("enabled", scene::FieldKind::I32, static_cast<u16>(offsetof(CSynapseCoverMarker, enabled)));
        AVER_ASSERTM(b.verify(sizeof(CSynapseCoverMarker)), "CSynapseCoverMarker");
        markerType_ = b.typeId();
    }
    {
        auto b = world.registerComponent<CSynapseSquad>("CSynapseSquad");
        const auto f = [&](const char* n, scene::FieldKind k, usize off, bool ro) {
            b.field(n, k, static_cast<u16>(off), 0, ro);
        };
        f("squadId", scene::FieldKind::I32, offsetof(CSynapseSquad, squadId), false);
        f("spacingCm", scene::FieldKind::F32, offsetof(CSynapseSquad, spacingCm), false);
        f("role", scene::FieldKind::I32, offsetof(CSynapseSquad, role), true);
        f("hasSlot", scene::FieldKind::I32, offsetof(CSynapseSquad, hasSlot), true);
        f("slotXCm", scene::FieldKind::F32, offsetof(CSynapseSquad, slotXCm), true);
        f("slotYCm", scene::FieldKind::F32, offsetof(CSynapseSquad, slotYCm), true);
        f("slotZCm", scene::FieldKind::F32, offsetof(CSynapseSquad, slotZCm), true);
        f("coverId", scene::FieldKind::I32, offsetof(CSynapseSquad, coverId), true);
        AVER_ASSERTM(b.verify(sizeof(CSynapseSquad)), "CSynapseSquad");
        squadType_ = b.typeId();
    }
    return markerType_;
}

CSynapseCoverMarker* TacticsSystem::attachMarker(scene::World& world, scene::Entity e) {
    if (markerType_ == 0) return nullptr;
    if (world.hasComponent(e, markerType_)) return world.component<CSynapseCoverMarker>(e, markerType_);
    auto* m = static_cast<CSynapseCoverMarker*>(world.addComponent(e, markerType_));
    if (m) *m = CSynapseCoverMarker{};
    return m;
}

CSynapseSquad* TacticsSystem::attachSquad(scene::World& world, scene::Entity e) {
    if (squadType_ == 0) return nullptr;
    if (world.hasComponent(e, squadType_)) return world.component<CSynapseSquad>(e, squadType_);
    auto* s = static_cast<CSynapseSquad*>(world.addComponent(e, squadType_));
    if (s) *s = CSynapseSquad{};
    return s;
}

f32 TacticsSystem::gridPathCost(void* user, V2 from, V2 to) {
    const auto* self = static_cast<const TacticsSystem*>(user);
    if (!self->nav_) return dist2(from, to);
    PathRequest req;
    req.startXCm = from.x; req.startYCm = from.y;
    req.goalXCm = to.x;    req.goalYCm = to.y;
    req.maxExpansions = 1500;
    const PathResult r = findPath(*self->nav_, req);
    if (r.status != PathStatus::Found) return -1.0f;
    f32 len = 0.0f;
    for (usize i = 0; i + 1 < r.points.size(); ++i)
        len += dist2(V2{r.points[i].x, r.points[i].y}, V2{r.points[i + 1].x, r.points[i + 1].y});
    return r.points.size() < 2 ? dist2(from, to) : len;
}

bool TacticsSystem::findCover(scene::World& world, scene::Entity seeker, const Vec3& threat,
                              const CoverSearch& s, CoverResult& out) {
    CoverQuery q;
    q.from = worldXY(world, seeker);
    q.threat = {threat.x, threat.y};
    q.maxSeekCm = s.maxSeekCm;
    q.minThreatDistCm = s.minThreatDistCm;
    q.maxCloserCm = s.maxCloserCm;
    q.requireHigh = s.requireHigh;
    q.seeker = seeker;
    q.nav = nav_;
    q.blocked = blocked_;
    q.blockedUser = blockedUser_;
    if (pathCost_) { q.pathCost = pathCost_; q.pathUser = pathUser_; }
    else if (useGridPath_ && nav_) { q.pathCost = &TacticsSystem::gridPathCost; q.pathUser = this; }
    return covers_.claim(q, reservations_, s.reserveSec, out);
}

void TacticsSystem::releaseCover(scene::Entity seeker) { reservations_.releaseAll(seeker); }

bool TacticsSystem::isCovered(scene::World& world, scene::Entity seeker, const Vec3& threat) const {
    const u32 id = reservations_.coverOf(seeker);
    const CoverPoint* c = id ? covers_.find(id) : nullptr;
    if (!c) return false;
    if (dist2(worldXY(world, seeker), c->pos) > kInCoverReachCm) return false;
    return covers_.protectsFrom(*c, V2{threat.x, threat.y}, nav_, blocked_, blockedUser_);
}

void TacticsSystem::setSquadTarget(i32 squadId, const Vec3& target) { squadTargets_[squadId] = target; }
void TacticsSystem::clearSquadTarget(i32 squadId) { squadTargets_.erase(squadId); }

void TacticsSystem::syncMarkers(scene::World& world) {
    scratch_.clear();
    if (scene::ComponentPool* pool = markerType_ ? world.pool(markerType_) : nullptr) {
        for (usize i = 0; i < pool->size(); ++i) {
            const scene::Entity e = pool->entityAt(i);
            const auto* m = static_cast<const CSynapseCoverMarker*>(pool->dataAt(i));
            if (!m || world.destroyPending(e)) continue;
            scratch_.push_back(e);

            const V2 pos = worldXY(world, e);
            const V2 dir = worldForwardXY(world, e);
            const auto mapped = markerCover_.find(e);
            if (mapped == markerCover_.end()) {
                const u32 id = covers_.addAuthored(pos, dir, m->height ? CoverHeight::High : CoverHeight::Low,
                                                   m->arcHalfAngleDeg, e);
                markerCover_[e] = id;
                covers_.setEnabled(id, m->enabled != 0);
            } else if (CoverPoint* c = covers_.findMutable(mapped->second)) {
                c->pos = pos;
                c->dir = dir;
                c->height = m->height ? CoverHeight::High : CoverHeight::Low;
                c->arcHalfAngleDeg = m->arcHalfAngleDeg;
                c->enabled = m->enabled != 0;
            }
        }
    }
    std::sort(scratch_.begin(), scratch_.end());

    for (auto it = markerCover_.begin(); it != markerCover_.end();) {
        if (std::binary_search(scratch_.begin(), scratch_.end(), it->first)) { ++it; continue; }
        const u32 id = it->second;
        reservations_.release(id, reservations_.ownerOf(id));
        covers_.remove(id);
        it = markerCover_.erase(it);
    }
}

void TacticsSystem::tickSquads(scene::World& world) {
    scene::ComponentPool* pool = squadType_ ? world.pool(squadType_) : nullptr;
    if (!pool) return;

    std::map<i32, std::vector<scene::Entity>> groups;
    for (usize i = 0; i < pool->size(); ++i) {
        const scene::Entity e = pool->entityAt(i);
        const auto* s = static_cast<const CSynapseSquad*>(pool->dataAt(i));
        if (s && !world.destroyPending(e)) groups[s->squadId].push_back(e);
    }

    std::vector<SquadMemberIn> in;
    std::vector<SquadRole> roles;
    for (auto& [id, members] : groups) {
        std::sort(members.begin(), members.end());
        const auto target = squadTargets_.find(id);
        if (target == squadTargets_.end()) {
            for (const scene::Entity e : members) {
                auto* s = world.component<CSynapseSquad>(e, squadType_);
                s->role = 0;
                s->hasSlot = 0;
                s->coverId = static_cast<i32>(reservations_.coverOf(e));
            }
            continue;
        }

        const V2 tgt{target->second.x, target->second.y};
        in.clear();
        V2 centre{};
        for (const scene::Entity e : members) {
            const V2 p = worldXY(world, e);
            in.push_back({e, p});
            centre += p;
        }
        centre = centre * (1.0f / static_cast<f32>(members.size()));
        roles.assign(members.size(), SquadRole::None);
        squadAssignRoles(in.data(), static_cast<u32>(in.size()), tgt, roles.data());

        V2 anchor = centre;
        for (usize i = 0; i < members.size(); ++i)
            if (roles[i] == SquadRole::Anchor) anchor = in[i].pos;

        u32 support = 0;
        for (usize i = 0; i < members.size(); ++i) {
            auto* s = world.component<CSynapseSquad>(members[i], squadType_);
            f32 z = 0.0f;
            worldXY(world, members[i], &z);
            V2 slot = in[i].pos;
            switch (roles[i]) {
                case SquadRole::FlankLeft:
                case SquadRole::FlankRight:
                    slot = squadFlankPosition(centre, tgt, roles[i], squadParams_.flankRadiusCm,
                                              squadParams_.flankAngleDeg);
                    break;
                case SquadRole::Support:
                    slot = squadSupportPosition(anchor, tgt, squadParams_.supportBehindCm, support++,
                                                s->spacingCm);
                    break;
                default: break;   // the anchor holds where it is
            }
            s->role = static_cast<i32>(roles[i]);
            s->hasSlot = 1;
            s->slotXCm = slot.x;
            s->slotYCm = slot.y;
            s->slotZCm = z;
            s->coverId = static_cast<i32>(reservations_.coverOf(members[i]));
        }
    }
}

V2 TacticsSystem::spacingPush(scene::World& world, scene::Entity e) const {
    if (!squadType_) return {};
    const auto* me = world.component<CSynapseSquad>(e, squadType_);
    scene::ComponentPool* pool = world.pool(squadType_);
    if (!me || !pool) return {};

    std::vector<V2> others;
    for (usize i = 0; i < pool->size(); ++i) {
        const scene::Entity o = pool->entityAt(i);
        const auto* s = static_cast<const CSynapseSquad*>(pool->dataAt(i));
        if (o == e || !s || s->squadId != me->squadId) continue;
        others.push_back(worldXY(world, o));
    }
    return squadSpacingPush(worldXY(world, e), others.data(), static_cast<u32>(others.size()), me->spacingCm);
}

void TacticsSystem::tick(scene::World& world, const fmt::OcNavData* nav, f32 dt) {
    nav_ = nav && nav->valid() ? nav : nullptr;

    if (autoGen_ && nav_ != lastNav_) {
        if (nav_) covers_.generateFromNav(*nav_, genParams_);
        else      covers_.clearGenerated();
        lastNav_ = nav_;
    }

    syncMarkers(world);

    // Drop claims whose owner died or whose point no longer exists.
    reservations_.owners(owners_);
    std::sort(owners_.begin(), owners_.end());
    owners_.erase(std::unique(owners_.begin(), owners_.end()), owners_.end());
    for (const u32 owner : owners_) {
        if (!world.valid(owner) || world.destroyPending(owner)) { reservations_.releaseAll(owner); continue; }
        const u32 cover = reservations_.coverOf(owner);
        if (cover && !covers_.find(cover)) reservations_.releaseAll(owner);
    }
    reservations_.tick(dt);

    tickSquads(world);
}

} // namespace aver::synapse
