// The fake host the level-instance runtime tests share: a library over in-memory levels, a ContentPort that counts
// acquires and releases, hooks that record, and a packed-rows sink. No GPU, no framework.
#pragma once
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcLevelInst.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/game/LevelInstances.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/scene_abi.h"
#include "aver/world/LevelTransform.hpp"

#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#endif

#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace aver;
using game::LevelInstances;

inline int g_failures = 0;

inline void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}
inline bool near(f32 a, f32 b, f32 eps = 1e-2f) { return std::fabs(a - b) <= eps; }

inline fmt::OcWorldData parse(const std::string& text) {
    fmt::OcWorldData d;
    std::string why;
    if (!fmt::parseOcworld(text, d, &why)) AVER_ERROR("test level did not parse: {}", why);
    return d;
}

struct Fixture {
    std::map<std::string, fmt::OcWorldData> levels;
    world::LevelAssetLibrary lib;
    LevelInstances li;
    std::map<u64, int> acquired, released;
    std::vector<scene::Entity> created, destroying, destroyedClasses;
    std::vector<std::string> spawnedClasses;
    struct SinkSet { fmt::OcInstanceData data; std::vector<std::string> mats; };
    std::map<u64, SinkSet> sink;
    int sinkSets = 0, sinkClears = 0;
    std::vector<std::tuple<scene::Entity, bool, u32>> residency;
    bool meshReady = true;
    int prefetches = 0;
    // port.buildMaterial: each acquire queues materialsPerMesh builds of materialMs each (a staged realise's work).
    int materialsPerMesh = 0, materialsWaiting = 0, materialsBuilt = 0;
    f64 materialMs = 0.0;
    std::string contentDir, projectDir;

    Fixture() {
        lib.setLoader([this](const std::string& path, fmt::OcWorldData& out, std::string* why) {
            const auto it = levels.find(path);
            if (it == levels.end()) { if (why) *why = "no test level " + path; return false; }
            out = it->second;
            return true;
        });
        li.init(lib, scene::World::instance());
        li.onResidency = [this](scene::Entity root, bool loaded, u32 count) { residency.emplace_back(root, loaded, count); };
    }
    ~Fixture() { li.end(); scene::World::instance().flush(); }

    game::ContentPort port() {
        game::ContentPort p;
        p.acquire = [this](u64 m) { ++acquired[m]; materialsWaiting += materialsPerMesh; return true; };
        p.buildMaterial = [this] {
            if (materialsWaiting == 0) return false;
            --materialsWaiting;
            ++materialsBuilt;
            const auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count() < materialMs) {}
            return true;
        };
        p.release = [this](u64 m) { ++released[m]; };
        p.prefetch = [this](u64) { ++prefetches; };
        p.ready = [this](u64) { return meshReady; };
        p.contentDir = contentDir;
        p.projectDir = projectDir;
        return p;
    }
    LevelInstances::Hooks hooks() {
        LevelInstances::Hooks h;
        h.created = [this](scene::Entity e, i32, const fmt::OcWorldPlacement*) { created.push_back(e); };
        h.destroying = [this](scene::Entity e) { destroying.push_back(e); };
        h.spawnClass = [this](const fmt::OcWorldPlacement& p, const Transform& w, scene::Entity) {
            spawnedClasses.push_back(p.className);
            return scene::World::instance().create(p.className, scene::kInvalidEntity, w);
        };
        h.destroyClass = [this](scene::Entity e) { destroyedClasses.push_back(e); scene::World::instance().destroy(e); };
        // The editor's capture, reduced to what the test needs: a mesh entity becomes a placement named after it.
        h.captureEntity = [](scene::Entity e, fmt::OcWorldData& into, i32 parentIdx) {
            scene::World& w = scene::World::instance();
            if (!w.hasComponent(e, scene::kComponentMeshRenderer)) return false;
            fmt::OcWorldPlacement p;
            p.asset = w.name(e);
            p.objectId = fnv1a64(std::string_view(p.asset));
            const Transform& t = w.localTransform(e);
            p.x = t.position.x; p.y = t.position.y; p.z = t.position.z;
            const Vec3 eul = world::eulerDegFromQuat(t.rotation);
            p.roll = eul.x; p.pitch = eul.y; p.yaw = eul.z;
            p.sx = t.scale.x; p.sy = t.scale.y; p.sz = t.scale.z;
            p.parent = parentIdx;
            into.placements.push_back(std::move(p));
            return true;
        };
        return h;
    }
    LevelInstances::PackedSink sinkFns() {
        LevelInstances::PackedSink s;
        s.set = [this](u64 owner, fmt::OcInstanceData d, std::vector<std::string> m) { sink[owner] = SinkSet{std::move(d), std::move(m)}; ++sinkSets; };
        s.clear = [this](u64 owner) { sink.erase(owner); ++sinkClears; };
        return s;
    }
    void begin(const fmt::OcWorldData& level, const world::PlacementStreamSettings* ss = nullptr,
               const world::InstantiateOptions& opt = {}) {
        li.begin(level, port(), opt, ss, hooks(), sinkFns());
    }
    int acquireTotal() const { int n = 0; for (const auto& [m, c] : acquired) n += c; return n; }
    int releaseTotal() const { int n = 0; for (const auto& [m, c] : released) n += c; return n; }
};

inline void flush() { scene::World::instance().flush(); }

inline std::vector<scene::Entity> descendants(scene::Entity root) {
    std::vector<scene::Entity> out;
    scene::World& w = scene::World::instance();
    std::vector<scene::Entity> stack{root};
    while (!stack.empty()) {
        const scene::Entity e = stack.back();
        stack.pop_back();
        for (scene::Entity c = w.firstChild(e); c != scene::kInvalidEntity; c = w.nextSibling(c)) {
            out.push_back(c);
            stack.push_back(c);
        }
    }
    return out;
}

inline Vec3 worldPos(scene::Entity e) {
    scene::World& w = scene::World::instance();
    return transformFromMatrix(w.worldMatrix(e)).position;
}

inline scene::CLevelInstanceLink linkOf(scene::Entity e) {
    scene::World& w = scene::World::instance();
    const u32 type = w.componentId(scene::kLevelInstanceLinkComponentName);
    const auto* l = w.component<scene::CLevelInstanceLink>(e, type);
    return l ? *l : scene::CLevelInstanceLink{};
}

inline i32 bodyCount() {
#if AVER_MODULE_PHYSICS
    return aver_phys_ready() ? aver_phys_body_count() : 0;
#else
    return 0;
#endif
}

inline fmt::OcPrefabOverride ovVec3(const std::string& path, const char* field, f32 x, f32 y, f32 z) {
    fmt::OcPrefabOverride o;
    o.op = fmt::OcOverrideOp::Set; o.path = path; o.component = fmt::kOcLevelInstPlacementComponent;
    o.value.name = field; o.value.kind = fmt::kOcPrefabKindVec3; o.value.f = {x, y, z};
    return o;
}
inline fmt::OcPrefabOverride ovStr(const std::string& path, const char* field, const char* s) {
    fmt::OcPrefabOverride o;
    o.op = fmt::OcOverrideOp::Set; o.path = path; o.component = fmt::kOcLevelInstPlacementComponent;
    o.value.name = field; o.value.kind = fmt::kOcPrefabKindString; o.value.s = s;
    return o;
}
inline fmt::OcPrefabOverride ovBool(const std::string& path, const char* field, bool v) {
    fmt::OcPrefabOverride o;
    o.op = fmt::OcOverrideOp::Set; o.path = path; o.component = fmt::kOcLevelInstPlacementComponent;
    o.value.name = field; o.value.kind = fmt::kOcPrefabKindBool; o.value.i = v ? 1 : 0;
    return o;
}
