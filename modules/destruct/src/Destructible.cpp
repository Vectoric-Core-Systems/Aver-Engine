// Destructible: see aver/destruct/Destructible.hpp.
#include "aver/destruct/Destructible.hpp"

#include "aver/formats/Fracture.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_joints_abi.h"
#include "aver/physics/physics_shape_builder_abi.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace aver::destruct {
namespace {

struct V3 { f32 x = 0, y = 0, z = 0; };
V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 mul(V3 a, f32 s) { return {a.x * s, a.y * s, a.z * s}; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
f32 dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
f32 len(V3 a) { return std::sqrt(dot(a, a)); }
V3 get3(const f32* p) { return {p[0], p[1], p[2]}; }
void put3(V3 v, f32* p) { p[0] = v.x; p[1] = v.y; p[2] = v.z; }
V3 rotate(const f32 q[4], V3 v) {
    const V3 u = {q[0], q[1], q[2]};
    const V3 t = mul(cross(u, v), 2.0f);
    return add(add(v, mul(t, q[3])), cross(u, t));
}
f32 falloff(f32 d, f32 radius) { const f32 k = 1.0f - d / radius; return k > 0 ? k * k : 0.0f; }

}  // namespace

Destructible::Destructible(std::shared_ptr<const fmt::OcFracData> asset, const DestructibleDesc& desc) : asset_(std::move(asset)), desc_(desc) {
    if (!asset_ || !fmt::validateOcFrac(*asset_) || !aver_phys_ready()) return;
    const u32 n = static_cast<u32>(asset_->pieces.size());
    broken_.assign(asset_->links.size(), 0);
    strain_.assign(asset_->links.size(), 0.0f);
    state_.assign(n, static_cast<u8>(PieceState::Attached));
    shape_.assign(n, 0);
    groupOf_.assign(n, -1);
    joint_.assign(asset_->links.size(), 0);
    for (const fmt::OcFracLink& l : asset_->links)
        for (int k = 0; k < 3; ++k)
            linkCm_.push_back(0.5f * (asset_->pieces[l.a].centroid[k] + asset_->pieces[l.b].centroid[k]));

    u32 comps = 0;
    const std::vector<u32> comp = fmt::fracture::components(n, [&] {
        std::vector<fmt::fracture::Link> v;
        for (const fmt::OcFracLink& l : asset_->links) v.push_back({l.a, l.b, l.area});
        return v;
    }(), broken_, &comps);
    std::vector<u8> anchored(comps, 0);
    for (u32 i = 0; i < n; ++i) if (asset_->anchors[i]) anchored[comp[i]] = 1;

    if (desc_.mode == Mode::Compound) {
        for (u32 c = 0; c < comps; ++c) {
            std::vector<u32> pcs;
            for (u32 i = 0; i < n; ++i) if (comp[i] == c) pcs.push_back(i);
            if (makeGroup(pcs, anchored[c] != 0, false, desc_.originCm, desc_.rotation) < 0) return;
        }
    } else {
        for (u32 i = 0; i < n; ++i)
            if (makeGroup({i}, asset_->anchors[i] != 0, false, desc_.originCm, desc_.rotation) < 0) return;
        // a piece of an anchored component that is not itself an anchor still hangs on its joints
        const f32 axX[3] = {1, 0, 0}, axY[3] = {0, 1, 0};
        for (u32 li = 0; li < asset_->links.size(); ++li) {
            const fmt::OcFracLink& l = asset_->links[li];
            Group& ga = groups_[static_cast<size_t>(groupOf_[l.a])];
            Group& gb = groups_[static_cast<size_t>(groupOf_[l.b])];
            if (ga.anchored && gb.anchored) continue;
            f32 w[3];
            linkWorld(li, w);
            const i32 j = aver_phys_joint_fixed(ga.body, gb.body, w, axX, axY);
            if (!j) continue;
            const f32 force = l.threshold * desc_.toughness * desc_.jointForcePerThreshold;
            if (force > 0) aver_phys_joint_set_break(j, force, force * desc_.jointTorqueArmM);
            aver_phys_joint_set_exclude_collision(j, 1);
            joint_[li] = j;
        }
    }
    markStates();
    valid_ = true;
}

Destructible::~Destructible() {
    for (Group& g : groups_) if (g.body) aver_phys_remove_body(g.body);
    for (i32 j : joint_) if (j) aver_phys_joint_remove(j);
    for (i32 s : shape_) if (s) aver_phys_shape_release(s);
}

void Destructible::toWorld(const f32 local[3], f32 out[3]) const {
    put3(add(get3(desc_.originCm), rotate(desc_.rotation, mul(get3(local), desc_.unitScale))), out);
}

void Destructible::groupPose(const Group& g, f32 pos[3], f32 quat[4]) const {
    aver_phys_body_origin(g.body, pos);
    aver_phys_body_rotation(g.body, quat);
}

void Destructible::linkWorld(u32 link, f32 out[3]) const {
    const i32 gi = groupOf_[asset_->links[link].a];
    if (gi < 0) { toWorld(&linkCm_[link * 3], out); return; }
    f32 pos[3], q[4];
    groupPose(groups_[static_cast<size_t>(gi)], pos, q);
    put3(add(get3(pos), rotate(q, mul(get3(&linkCm_[link * 3]), desc_.unitScale))), out);
}

i32 Destructible::pieceShape(u32 piece) {
    if (shape_[piece]) return shape_[piece];
    const fmt::OcFracPiece& p = asset_->pieces[piece];
    std::vector<f32> pts(static_cast<size_t>(p.hullCount) * 3);
    for (u32 i = 0; i < p.hullCount * 3; ++i) pts[i] = asset_->hull[static_cast<size_t>(p.hullStart) * 3 + i] * desc_.unitScale;
    i32 s = aver_phys_shape_convex_hull(pts.data(), static_cast<i32>(p.hullCount), 0, nullptr, 0);
    if (!s) {   // a hull Jolt refuses (flat, or all points within tolerance): a box around the points keeps the piece solid
        f32 lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (size_t i = 0; i < pts.size(); i += 3)
            for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], pts[i + k]); hi[k] = std::max(hi[k], pts[i + k]); }
        const f32 half[3] = {std::max(0.05f, (hi[0] - lo[0]) * 0.5f), std::max(0.05f, (hi[1] - lo[1]) * 0.5f), std::max(0.05f, (hi[2] - lo[2]) * 0.5f)};
        const f32 mid[3] = {(hi[0] + lo[0]) * 0.5f, (hi[1] + lo[1]) * 0.5f, (hi[2] + lo[2]) * 0.5f};
        const i32 box = aver_phys_shape_box(half[0], half[1], half[2], 0, nullptr, 0);
        const f32 id[4] = {0, 0, 0, 1};
        if (box) { s = aver_phys_shape_rotated_translated(box, mid, id); aver_phys_shape_release(box); }
    }
    shape_[piece] = s;
    return s;
}

i32 Destructible::makeGroup(const std::vector<u32>& pieces, bool anchored, bool debris, const f32 origin[3], const f32 rot[4]) {
    i32 shape = 0, compound = 0;
    double volume = 0;
    for (u32 pc : pieces) volume += asset_->pieces[pc].volume;
    if (pieces.size() == 1) {
        shape = pieceShape(pieces[0]);
    } else {
        std::vector<i32> kids;
        for (u32 pc : pieces) { const i32 s = pieceShape(pc); if (s) kids.push_back(s); }
        if (kids.empty()) return -1;
        std::vector<f32> pos(kids.size() * 3, 0.0f), quat(kids.size() * 4, 0.0f);
        std::vector<i32> user(kids.size(), 0);
        for (size_t i = 0; i < kids.size(); ++i) quat[i * 4 + 3] = 1.0f;
        compound = aver_phys_shape_compound(AVER_PHYS_COMPOUND_STATIC, kids.data(), pos.data(), quat.data(), user.data(), static_cast<i32>(kids.size()));
        shape = compound;
    }
    if (!shape) return -1;
    const f32 scale = desc_.unitScale;
    const f32 mass = std::max(0.01f, static_cast<f32>(volume * scale * scale * scale * 1e-6 * desc_.densityKgM3));
    const i32 body = aver_phys_body_from_shape(shape, origin, rot, anchored ? AVER_PHYS_MOTION_STATIC : AVER_PHYS_MOTION_DYNAMIC, anchored ? 0.0f : mass, nullptr, 0);
    if (compound) aver_phys_shape_release(compound);
    if (!body) return -1;
    if (desc_.entity) aver_phys_set_entity(body, desc_.entity);

    i32 slot;
    if (!freeSlots_.empty()) { slot = freeSlots_.back(); freeSlots_.pop_back(); }
    else { slot = static_cast<i32>(groups_.size()); groups_.emplace_back(); }
    Group& g = groups_[static_cast<size_t>(slot)];
    g = Group{};
    g.body = body;
    g.pieces = pieces;
    g.anchored = anchored;
    g.debris = debris;
    g.serial = ++serial_;
    for (u32 pc : pieces) groupOf_[pc] = slot;
    return slot;
}

void Destructible::removeGroup(i32 index) {
    Group& g = groups_[static_cast<size_t>(index)];
    if (g.body) aver_phys_remove_body(g.body);
    for (u32 pc : g.pieces) if (groupOf_[pc] == index) groupOf_[pc] = -1;
    g = Group{};
    freeSlots_.push_back(index);
}

void Destructible::removePiece(u32 piece) {
    const i32 gi = groupOf_[piece];
    if (gi >= 0) {
        removeGroup(gi);   // callers remove whole free components, so a compound never loses just one of its pieces
    }
    state_[piece] = static_cast<u8>(PieceState::Gone);
    for (u32 li = 0; li < asset_->links.size(); ++li) {
        if (asset_->links[li].a != piece && asset_->links[li].b != piece) continue;
        broken_[li] = 1;
        if (joint_[li]) { aver_phys_joint_remove(joint_[li]); joint_[li] = 0; }
    }
}

// Pieces in a component that holds an anchor are Attached, the rest Free (Gone stay Gone).
void Destructible::markStates() {
    const u32 n = pieceCount();
    std::vector<fmt::fracture::Link> links;
    for (const fmt::OcFracLink& l : asset_->links) links.push_back({l.a, l.b, l.area});
    u32 comps = 0;
    const std::vector<u32> comp = fmt::fracture::components(n, links, broken_, &comps);
    std::vector<u8> anchored(comps, 0);
    for (u32 i = 0; i < n; ++i) if (asset_->anchors[i] && state_[i] != static_cast<u8>(PieceState::Gone)) anchored[comp[i]] = 1;
    for (u32 i = 0; i < n; ++i) {
        if (state_[i] == static_cast<u8>(PieceState::Gone)) continue;
        state_[i] = static_cast<u8>(anchored[comp[i]] ? PieceState::Attached : PieceState::Free);
    }
}

i32 Destructible::pieceBody(u32 piece) const {
    if (piece >= groupOf_.size() || groupOf_[piece] < 0) return 0;
    return groups_[static_cast<size_t>(groupOf_[piece])].body;
}

bool Destructible::piecePose(u32 piece, f32 outPosCm[3], f32 outQuat[4]) const {
    const i32 b = pieceBody(piece);
    if (!b) return false;
    aver_phys_body_origin(b, outPosCm);
    aver_phys_body_rotation(b, outQuat);
    return true;
}

u32 Destructible::bodyCount() const {
    u32 n = 0;
    for (const Group& g : groups_) if (g.body) ++n;
    return n;
}

u32 Destructible::debrisPieceCount() const {
    u32 n = 0;
    for (const Group& g : groups_) if (g.body && g.debris && !g.anchored) n += static_cast<u32>(g.pieces.size());
    return n;
}

u32 Destructible::componentCount() const {
    std::vector<fmt::fracture::Link> links;
    for (const fmt::OcFracLink& l : asset_->links) links.push_back({l.a, l.b, l.area});
    u32 comps = 0;
    const std::vector<u32> comp = fmt::fracture::components(pieceCount(), links, broken_, &comps);
    std::vector<u8> seen(comps, 0);
    u32 live = 0;
    for (u32 i = 0; i < pieceCount(); ++i)
        if (state_[i] != static_cast<u8>(PieceState::Gone) && !seen[comp[i]]) { seen[comp[i]] = 1; ++live; }
    return live;
}

std::vector<BreakEvent> Destructible::takeEvents() {
    std::vector<BreakEvent> out;
    out.swap(events_);
    return out;
}

void Destructible::clearBreakQueue() { aver_phys_joint_break_clear(); }

HitResult Destructible::applyHit(const HitEvent& e) {
    Strike s;
    s.p[0] = e.point.x; s.p[1] = e.point.y; s.p[2] = e.point.z;
    const V3 imp = {e.impulse.x, e.impulse.y, e.impulse.z};
    const f32 il = len(imp);
    s.magnitude = il > 0 ? il : e.damage * desc_.damageToImpulse;
    s.radius = e.radius > 0 ? e.radius : desc_.pointRadiusCm;
    if (il > 0 && e.radius <= 0) put3(mul(imp, 1.0f / il), s.dir);
    return strike(s);
}

HitResult Destructible::applyContact(const f32 pointCm[3], f32 impulse) {
    if (impulse < desc_.contactThreshold) return {};
    Strike s;
    for (int k = 0; k < 3; ++k) s.p[k] = pointCm[k];
    s.magnitude = impulse;
    s.radius = desc_.pointRadiusCm;
    return strike(s);
}

HitResult Destructible::strike(const Strike& s) {
    HitResult r;
    if (!valid_ || !(s.magnitude > 0) || !(s.radius > 0)) return r;
    const V3 p = get3(s.p);

    if (desc_.mode == Mode::Jointed) {   // push the pieces near it; the joints decide what gives way
        for (u32 i = 0; i < pieceCount(); ++i) {
            const i32 gi = groupOf_[i];
            if (gi < 0 || groups_[static_cast<size_t>(gi)].anchored) continue;
            f32 pos[3], q[4];
            groupPose(groups_[static_cast<size_t>(gi)], pos, q);
            const V3 c = add(get3(pos), rotate(q, mul(get3(asset_->pieces[i].centroid), desc_.unitScale)));
            const V3 d = sub(c, p);
            const f32 dist = len(d);
            if (dist >= s.radius) continue;
            V3 dir = get3(s.dir);
            if (len(dir) < 0.5f) dir = dist > 1e-4f ? mul(d, 1.0f / dist) : V3{0, 0, 1};
            const V3 imp = mul(dir, s.magnitude * falloff(dist, s.radius));
            aver_phys_body_add_impulse_at(groups_[static_cast<size_t>(gi)].body, imp.x, imp.y, imp.z, c.x, c.y, c.z);
            ++r.piecesFreed;
        }
        return r;
    }

    for (u32 li = 0; li < asset_->links.size(); ++li) {
        if (broken_[li]) continue;
        f32 w[3];
        linkWorld(li, w);
        const f32 d = len(sub(get3(w), p));
        if (d >= s.radius) continue;
        strain_[li] += s.magnitude * falloff(d, s.radius);
        if (strain_[li] >= asset_->links[li].threshold * desc_.toughness) {
            broken_[li] = 1;
            ++r.linksBroken;
            BreakEvent ev;
            ev.link = li;
            put3(get3(w), ev.pointCm);
            ev.strain = strain_[li];
            events_.push_back(ev);
        }
    }
    if (r.linksBroken) resplit(s, r);
    return r;
}

// Re-derives the components after links broke: a group that fell in several becomes several bodies, each carrying the parent's motion at its own
// centre of mass; the ones that just left the anchored mass get a share of the hit.
void Destructible::resplit(const Strike& s, HitResult& r) {
    const u32 n = pieceCount();
    std::vector<fmt::fracture::Link> links;
    for (const fmt::OcFracLink& l : asset_->links) links.push_back({l.a, l.b, l.area});
    u32 comps = 0;
    const std::vector<u32> comp = fmt::fracture::components(n, links, broken_, &comps);
    std::vector<u8> anchored(comps, 0);
    for (u32 i = 0; i < n; ++i) if (asset_->anchors[i] && state_[i] != static_cast<u8>(PieceState::Gone)) anchored[comp[i]] = 1;

    const size_t existing = groups_.size();
    for (size_t gi = 0; gi < existing; ++gi) {
        if (!groups_[gi].body) continue;
        std::map<u32, std::vector<u32>> byComp;
        for (u32 pc : groups_[gi].pieces) byComp[comp[pc]].push_back(pc);
        if (byComp.size() == 1) continue;

        const Group parent = groups_[gi];
        f32 origin[3], rot[4], com[3] = {0, 0, 0}, vel[3] = {0, 0, 0}, ang[3] = {0, 0, 0};
        groupPose(parent, origin, rot);
        if (!parent.anchored) {
            aver_phys_body_position(parent.body, com);
            aver_phys_body_velocity(parent.body, vel);
            aver_phys_body_angular_velocity(parent.body, ang);
        }
        removeGroup(static_cast<i32>(gi));
        for (auto& [c, pcs] : byComp) {
            const bool fixed = anchored[c] != 0;
            const i32 slot = makeGroup(pcs, fixed, !fixed, origin, rot);
            if (slot < 0) { for (u32 pc : pcs) removePiece(pc); continue; }   // no body: the pieces are gone, not orphaned
            ++r.bodiesCreated;
            const Group& g = groups_[static_cast<size_t>(slot)];
            if (fixed) continue;
            for (u32 pc : pcs) if (state_[pc] == static_cast<u8>(PieceState::Attached)) ++r.piecesFreed;
            if (parent.anchored) { aver_phys_body_set_velocity(g.body, 0, 0, 0); }
            else {
                f32 nc[3];
                aver_phys_body_position(g.body, nc);
                const V3 v = add(get3(vel), cross(get3(ang), sub(get3(nc), get3(com))));
                aver_phys_body_set_velocity(g.body, v.x, v.y, v.z);
                aver_phys_body_set_angular_velocity(g.body, ang[0], ang[1], ang[2]);
            }
            f32 nc[3];
            aver_phys_body_position(g.body, nc);
            const V3 d = sub(get3(nc), get3(s.p));
            const f32 dist = len(d);
            if (dist < s.radius) {
                V3 dir = get3(s.dir);
                if (len(dir) < 0.5f) dir = dist > 1e-4f ? mul(d, 1.0f / dist) : V3{0, 0, 1};
                const V3 imp = mul(dir, s.magnitude * desc_.releaseGain * falloff(dist, s.radius));
                aver_phys_body_add_impulse(g.body, imp.x, imp.y, imp.z);
            }
        }
    }
    markStates();
    enforceDebrisCap();
}

void Destructible::enforceDebrisCap() {
    for (;;) {
        u32 total = 0;
        i32 oldest = -1;
        for (size_t i = 0; i < groups_.size(); ++i) {
            const Group& g = groups_[i];
            if (!g.body || !g.debris || g.anchored) continue;
            total += static_cast<u32>(g.pieces.size());
            if (oldest < 0 || g.serial < groups_[static_cast<size_t>(oldest)].serial) oldest = static_cast<i32>(i);
        }
        if (oldest < 0 || total <= static_cast<u32>(std::max(0, desc_.maxDebrisPieces))) return;
        const std::vector<u32> pcs = groups_[static_cast<size_t>(oldest)].pieces;
        for (u32 pc : pcs) removePiece(pc);
    }
}

void Destructible::update(f32 dt) {
    if (!valid_) return;
    bool changed = false;
    if (desc_.mode == Mode::Jointed) {
        const i32 count = aver_phys_joint_break_count();
        for (i32 i = 0; i < count; ++i) {
            i32 j = 0;
            f32 force = 0, torque = 0;
            if (!aver_phys_joint_break_get(i, &j, &force, &torque)) continue;
            for (u32 li = 0; li < joint_.size(); ++li) {
                if (joint_[li] != j || broken_[li]) continue;
                f32 w[3];
                linkWorld(li, w);
                broken_[li] = 1;
                strain_[li] = force;
                BreakEvent ev;
                ev.link = li;
                put3(get3(w), ev.pointCm);
                ev.strain = force;
                events_.push_back(ev);
                aver_phys_joint_remove(joint_[li]);
                joint_[li] = 0;
                changed = true;
            }
        }
        if (changed) {
            markStates();
            for (Group& g : groups_)
                if (g.body && !g.anchored && state_[g.pieces[0]] == static_cast<u8>(PieceState::Free) && !g.debris) { g.debris = true; g.serial = ++serial_; }
            enforceDebrisCap();
        }
    }
    if (desc_.sleepDespawnS > 0) {
        std::vector<u32> doomed;
        for (Group& g : groups_) {
            if (!g.body || !g.debris || g.anchored) continue;
            if (aver_phys_body_is_active(g.body)) { g.idle = 0; continue; }
            g.idle += dt;
            if (g.idle >= desc_.sleepDespawnS) doomed.insert(doomed.end(), g.pieces.begin(), g.pieces.end());
        }
        for (u32 pc : doomed) removePiece(pc);
    }
}

}  // namespace aver::destruct
