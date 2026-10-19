// Shared by the soft-body system tests: procedural sheet meshes, in-memory meshes and .ocsoft assets, and a rig
// that owns a physics world plus a SoftBodySystem and steps them the way a host frame does. No GPU, no device.
#pragma once

#include "aver/core/EffectSpawn.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_layers_abi.h"
#include "aver/physics/physics_softbody_abi.h"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"
#include "aver/softbody/SoftBodySystem.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sbfix {

using namespace aver;
namespace sb = aver::softbody;

enum class Plane { XZ, YZ, XY };

// A cols x rows sheet in MESH-LOCAL space. Vertical planes hang from z = 0 (row 0 is the top) down to
// -(rows-1) * spacing; XY is horizontal at z = 0. `skinned` binds every vertex to joint 0.
inline fmt::OcMeshData sheetMesh(u32 cols, u32 rows, f32 spacing, Plane plane, bool skinned = false) {
    fmt::OcMeshData m;
    for (u32 r = 0; r < rows; ++r)
        for (u32 c = 0; c < cols; ++c) {
            const f32 u = static_cast<f32>(c) * spacing, v = static_cast<f32>(r) * spacing;
            Vec3 p, n;
            switch (plane) {
                case Plane::XZ: p = {u, 0, -v}; n = {0, -1, 0}; break;
                case Plane::YZ: p = {0, u, -v}; n = {-1, 0, 0}; break;
                default:        p = {u, v, 0};  n = {0, 0, 1};  break;
            }
            m.positions.insert(m.positions.end(), {p.x, p.y, p.z});
            m.normals.insert(m.normals.end(), {n.x, n.y, n.z});
            m.uvs.insert(m.uvs.end(), {static_cast<f32>(c) / static_cast<f32>(cols - 1), static_cast<f32>(r) / static_cast<f32>(rows - 1)});
            if (skinned) {
                m.joints.insert(m.joints.end(), {0, 0, 0, 0});
                m.weights.insert(m.weights.end(), {1.0f, 0.0f, 0.0f, 0.0f});
            }
        }
    for (u32 r = 0; r + 1 < rows; ++r)
        for (u32 c = 0; c + 1 < cols; ++c) {
            const u32 a = r * cols + c, b = a + 1, d = a + cols, e = d + 1;
            for (const u32 i : {a, b, e, a, e, d}) m.indices.push_back(i);
        }
    m.computeBounds();
    return m;
}

// A .ocsoft in memory: the preset, with the top row pinned by an `above z` rule (mesh-local z >= -1).
inline sb::SoftAsset pinnedTopAsset(sb::SoftKind kind, sb::SoftPreset preset) {
    sb::SoftAsset a = sb::makeSoftAsset(kind, preset);
    sb::PaintRule r;
    r.type = sb::PaintRule::Type::Above;
    r.axis = 2;
    r.valueCm = -1.0f;
    a.paint.rules.push_back(r);
    return a;
}

struct Stores {
    std::map<u64, fmt::OcMeshData> meshes;
    std::map<u64, sb::SoftAsset> assets;
    static const fmt::OcMeshData* mesh(u64 id, void* user) {
        auto& s = *static_cast<Stores*>(user);
        const auto it = s.meshes.find(id);
        return it == s.meshes.end() ? nullptr : &it->second;
    }
    static const sb::SoftAsset* asset(u64 id, void* user) {
        auto& s = *static_cast<Stores*>(user);
        const auto it = s.assets.find(id);
        return it == s.assets.end() ? nullptr : &it->second;
    }
};

// A palette the test moves by hand: one joint.
struct PaletteSource {
    std::vector<Mat4> mats{Mat4::identity()};
    static const Mat4* get(scene::Entity, u32& count, void* user) {
        auto& p = *static_cast<PaletteSource*>(user);
        count = static_cast<u32>(p.mats.size());
        return p.mats.data();
    }
};

struct Rig {
    scene::World& w;
    sb::SoftBodySystem sys;
    Stores stores;
    PaletteSource palette;
    std::vector<scene::Entity> made;
    std::vector<sb::SoftEvent> events;

    Rig() : w(scene::World::instance()) {
        aver_phys_shutdown();
        aver_phys_reset_layer_collisions();
        aver_phys_init();
        aver_phys_sb_set_wind(0, 0, 0, 0, 0, 0);
        sb::SoftBodySystem::registerComponents(w);
        sys.install(w);
        sys.setMeshSource(&Stores::mesh, &stores);
        sys.setAssetSource(&Stores::asset, &stores);
    }

    ~Rig() {
        sys.clear();
        for (const scene::Entity e : made)
            if (w.valid(e)) w.destroy(e);
        w.flush();
        sys.uninstall();
        aver_phys_shutdown();
    }

    // An entity drawing mesh `meshId` at `pos` (rotation identity).
    scene::Entity entity(const char* name, u64 meshId, Vec3 pos) {
        const scene::Entity e = w.create(name);
        made.push_back(e);
        auto* mr = static_cast<scene::CMeshRenderer*>(w.addComponent(e, scene::kComponentMeshRenderer));
        mr->mesh = meshId;
        mr->flags = scene::kMeshRendererVisible;
        w.setLocalPosition(e, pos);
        w.flush();
        return e;
    }

    // One host frame: physics steps, the world flushes, the system ticks.
    void frame(f32 dt = 1.0f / 60.0f) {
        aver_phys_step(dt);
        w.flush();
        sys.tick(w, dt);
        sb::SoftEvent buf[64];
        for (u32 n; (n = sys.drainEvents(buf, 64)) > 0;) events.insert(events.end(), buf, buf + n);
    }
    void run(f32 seconds) {
        for (f32 t = 0.0f; t < seconds; t += 1.0f / 60.0f) frame();
    }

    // The cage particles of an entity (empty when it has none).
    const std::vector<sb::Particle>& particles(scene::Entity e) const {
        static const std::vector<sb::Particle> none;
        const sb::Cage* c = sys.cageOf(e);
        return c ? c->particles : none;
    }
    u32 kindCount(sb::SoftEventKind k) const {
        u32 n = 0;
        for (const sb::SoftEvent& ev : events) n += ev.kind == k ? 1u : 0u;
        return n;
    }
    // Index of the first event of a kind, or the size when none.
    usize firstOf(sb::SoftEventKind k) const {
        for (usize i = 0; i < events.size(); ++i)
            if (events[i].kind == k) return i;
        return events.size();
    }
};


// ---- wave 10 helpers ----------------------------------------------------------------------------------------

// A static floor with its top at z = 0.
inline void addFloor() { aver_phys_add_static_box(0.0f, 0.0f, -10.0f, 5000.0f, 5000.0f, 10.0f); }

inline u32 brokenCount(const sb::Cage* c) { return c ? sb::brokenBeamCount(*c) : 0u; }

struct Views {
    const sb::PieceView* v = nullptr;
    u32 n = 0;
};
inline Views viewsOf(const Rig& r) {
    Views out;
    out.v = r.sys.pieces(out.n);
    return out;
}

inline Vec3 meanOf(const sb::PieceView& v) {
    Vec3 sum{0, 0, 0};
    for (u32 i = 0; i < v.vertexCount; ++i) sum += v.world[i];
    return v.vertexCount ? sum / static_cast<f32>(v.vertexCount) : Vec3{0, 0, 0};
}

// The debris views (every view whose entity is not `root` and is not a cap), and their mean position.
inline std::vector<const sb::PieceView*> debrisOf(const Rig& r, scene::Entity root) {
    std::vector<const sb::PieceView*> out;
    const Views vs = viewsOf(r);
    for (u32 i = 0; i < vs.n; ++i)
        if (vs.v[i].entity != root && vs.v[i].layer == 0) out.push_back(&vs.v[i]);
    return out;
}

inline Vec3 meanOfDebris(const Rig& r, scene::Entity root) {
    Vec3 sum{0, 0, 0};
    u32 n = 0;
    for (const sb::PieceView* v : debrisOf(r, root)) { sum += meanOf(*v); ++n; }
    return n ? sum / static_cast<f32>(n) : Vec3{0, 0, 0};
}

inline bool visible(const Rig& r, scene::Entity e) {
    const auto* mr = r.w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
    return mr && (mr->flags & scene::kMeshRendererVisible) != 0;
}

// Stands in for Aver.Render.SoftBody's draw side: builds at most `budget` new residents a frame, in view order, and sets the
// visible bit on the entity of a view that asks to be revealed once its resident exists.
struct SceneMimic {
    u32 budget = 8;
    std::set<std::pair<scene::Entity, u32>> built;
    u32 builtLastFrame = 0;
    u32 maxBuiltInAFrame = 0;

    void update(Rig& r) {
        builtLastFrame = 0;
        const Views vs = viewsOf(r);
        for (u32 i = 0; i < vs.n; ++i) {
            const sb::PieceView& v = vs.v[i];
            const auto key = std::make_pair(v.entity, v.piece);
            if (!built.count(key)) {
                if (builtLastFrame >= budget) continue;
                built.insert(key);
                ++builtLastFrame;
            }
            if (v.revealOnReady) {
                if (auto* mr = r.w.component<scene::CMeshRenderer>(v.entity, scene::kComponentMeshRenderer)) mr->flags |= scene::kMeshRendererVisible;
            }
        }
        maxBuiltInAFrame = std::max(maxBuiltInAFrame, builtLastFrame);
    }
    bool hasResident(scene::Entity e) const {
        for (const auto& k : built)
            if (k.first == e) return true;
        return false;
    }
};

// Records what the effect seams are handed.
struct SpawnRecord { u64 id; Vec3 pos, dir; u32 count; };
struct FxRecorder {
    std::vector<SpawnRecord> spawns;
    std::vector<sb::FxRequest> sounds;
    static u32 spawn(u64 id, const Vec3& pos, const Vec3& dir, u32 count, void* user) {
        static_cast<FxRecorder*>(user)->spawns.push_back({id, pos, dir, count});
        return count;
    }
    static void sound(const sb::FxRequest& r, void* user) { static_cast<FxRecorder*>(user)->sounds.push_back(r); }
    void install(sb::SoftBodySystem& sys) {
        setEffectSpawner(&FxRecorder::spawn, this);
        sys.setSoundSink(&FxRecorder::sound, this);
    }
    void uninstall(sb::SoftBodySystem& sys) {
        setEffectSpawner(nullptr, nullptr);
        sys.setSoundSink(nullptr, nullptr);
    }
    void clear() { spawns.clear(); sounds.clear(); }
};

// A glass pane asset with effects bound on the tear, detach and dust slots (and impact when asked).
inline sb::SoftAsset glassWithEffects(bool impact = false) {
    sb::SoftAsset a = pinnedTopAsset(sb::SoftKind::Destructible, sb::SoftPreset::Glass);
    sb::FxSlotParams& tear = a.effects.slot[static_cast<u32>(sb::FxSlot::Tear)];
    tear.particle = "FX/Shards.ocparticle";
    tear.sounds = {"Audio/Break1.wav", "Audio/Break2.wav"};
    tear.count = 12.0f;
    tear.cooldownS = 0.0f;
    sb::FxSlotParams& detach = a.effects.slot[static_cast<u32>(sb::FxSlot::Detach)];
    detach.particle = "FX/Chunk.ocparticle";
    detach.sounds = {"Audio/Tick.wav"};
    detach.count = 3.0f;
    detach.cooldownS = 0.0f;
    sb::FxSlotParams& dust = a.effects.slot[static_cast<u32>(sb::FxSlot::Dust)];
    dust.particle = "FX/Dust.ocparticle";
    dust.countPerParticle = 1.0f;
    dust.cooldownS = 0.0f;
    if (impact) {
        sb::FxSlotParams& imp = a.effects.slot[static_cast<u32>(sb::FxSlot::Impact)];
        imp.particle = "FX/Spark.ocparticle";
        imp.sounds = {"Audio/Clang.wav"};
        imp.minSpeedCmS = 100.0f;
        imp.count = 2.0f;
        imp.cooldownS = 0.0f;
    }
    return a;
}

}  // namespace sbfix
