// Fixtures for VehicleSystemTest: a recording audio port, recording sinks, and a Bench that stands up a physics world with a
// floor, the scene, the component and a system whose seams (specs, actions, sinks) are all injected. Nothing is drawn, no
// device, no window, no host: the whole system is decidable headless.
#pragma once

#include "aver/core/Hash.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_character_abi.h"
#include "aver/physics/physics_vehicle_abi.h"
#include "aver/physics/physics_vehicle_ext_abi.h"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"
#include "aver/vehicle/OcVehicle.hpp"
#include "aver/vehicle/VehiclePhysicsSystem.hpp"
#include "aver/vehicle/VehiclePresets.hpp"

#include <map>
#include <string>
#include <vector>

namespace vt {

using namespace aver;
using namespace aver::vehicle;

// ---- a recording audio port --------------------------------------------------------------------------------

struct FakeAudio : AudioPort {
    struct Voice { i32 sound = 0; Vec3 pos; f32 volume = 0, pitch = 1; bool looping = false, live = true; u32 sets = 0; };
    std::vector<Voice> voices;                       // handle - 1
    std::vector<std::string> paths;                  // sound handle - 1
    u32 oneShots = 0;

    i32 loadSound(const std::string& path) override {
        if (path.rfind("missing", 0) == 0) return 0;
        for (usize i = 0; i < paths.size(); ++i) if (paths[i] == path) return static_cast<i32>(i) + 1;
        paths.push_back(path);
        return static_cast<i32>(paths.size());
    }
    i32 play(i32 sound, const Vec3& position, f32 volume, f32 pitch, bool looping) override {
        Voice v;
        v.sound = sound; v.pos = position; v.volume = volume; v.pitch = pitch; v.looping = looping;
        voices.push_back(v);
        if (!looping) ++oneShots;
        return static_cast<i32>(voices.size());
    }
    void set(i32 voice, const Vec3& position, f32 volume, f32 pitch) override {
        if (voice < 1 || static_cast<usize>(voice) > voices.size()) return;
        Voice& v = voices[static_cast<usize>(voice) - 1];
        v.pos = position; v.volume = volume; v.pitch = pitch; ++v.sets;
    }
    void stop(i32 voice) override {
        if (voice >= 1 && static_cast<usize>(voice) <= voices.size()) voices[static_cast<usize>(voice) - 1].live = false;
    }
    u32 liveLooping() const {
        u32 n = 0;
        for (const Voice& v : voices) n += (v.live && v.looping) ? 1u : 0u;
        return n;
    }
};

// ---- recording sinks ------------------------------------------------------------------------------------------

struct EffectCall { u64 id; Vec3 pos, dir; u32 count; };
struct CameraCall { scene::Entity vehicle, driver; CameraEvent ev; VehicleCameraInfo info; };

struct Sinks {
    std::vector<EffectCall> effects;
    std::vector<CrashInfo> crashes;
    std::vector<CameraCall> cameras;
    std::vector<std::pair<scene::Entity, std::string>> notifies;
    struct RigCall { scene::Entity e; u64 hash; u32 count; };
    std::vector<RigCall> rig;

    u32 notifyCount(const char* name) const {
        u32 n = 0;
        for (const auto& p : notifies) n += p.second == name ? 1u : 0u;
        return n;
    }
    u32 effectTotal(u64 id) const {
        u32 n = 0;
        for (const EffectCall& c : effects) n += c.id == id ? c.count : 0u;
        return n;
    }
};

inline void effectThunk(u64 id, const Vec3& p, const Vec3& d, u32 n, void* u) { static_cast<Sinks*>(u)->effects.push_back(EffectCall{id, p, d, n}); }
inline void crashThunk(const CrashInfo& info, void* u) { static_cast<Sinks*>(u)->crashes.push_back(info); }
inline void cameraThunk(scene::Entity v, scene::Entity d, CameraEvent ev, const VehicleCameraInfo& info, void* u) {
    static_cast<Sinks*>(u)->cameras.push_back(CameraCall{v, d, ev, info});
}
inline void notifyThunk(scene::Entity e, const char* name, void* u) { static_cast<Sinks*>(u)->notifies.emplace_back(e, name); }
inline void rigThunk(scene::Entity e, u64 hash, const f32*, u32 n, void* u) { static_cast<Sinks*>(u)->rig.push_back(Sinks::RigCall{e, hash, n}); }

// ---- the bench ----------------------------------------------------------------------------------------------------

constexpr f32 kDt = 1.0f / 60.0f;

struct Bench {
    scene::World& w = scene::World::instance();
    VehiclePhysicsSystem sys;
    FakeAudio audio;
    Sinks sinks;
    std::map<u64, VehicleSpec> specs;
    std::map<std::string, ActionState> actions;
    std::vector<scene::Entity> made;
    i32 floorBody = 0;

    explicit Bench(bool withFloor = true) {
        VehiclePhysicsSystem::registerComponents(w);
        sys.install(w);
        sys.setActionSource([this](const char* name, ActionState& out) {
            const auto it = actions.find(name);
            if (it == actions.end()) return false;
            out = it->second;
            out.found = true;
            return true;
        });
        sys.setSpecSource([this](u64 id) -> const VehicleSpec* {
            const auto it = specs.find(id);
            return it == specs.end() ? nullptr : &it->second;
        });
        sys.setAudioPort(&audio);
        sys.setEffectSink(&effectThunk, &sinks);
        sys.setCrashSink(&crashThunk, &sinks);
        sys.setCameraSink(&cameraThunk, &sinks);
        sys.setNotifySink(&notifyThunk, &sinks);
        sys.setRigInputSink(&rigThunk, &sinks);
        aver_phys_init();
        if (withFloor) floorBody = aver_phys_add_static_box(0.0f, 0.0f, -50.0f, 20000.0f, 20000.0f, 50.0f);
    }

    ~Bench() {
        sys.uninstall();
        for (scene::Entity e : made) if (w.valid(e)) w.destroy(e);
        w.flush();
        aver_phys_shutdown();
    }

    // Registers a spec under the content path, so a CVehicle can name it by id.
    u64 addSpec(const std::string& path, const VehicleSpec& spec) {
        const u64 id = fnv1a64(std::string_view(path));
        specs[id] = spec;
        return id;
    }

    static void setBounds(scene::World& w, scene::Entity e, f32 lx, f32 ly, f32 lz) {
        auto* mr = static_cast<scene::CMeshRenderer*>(w.addComponent(e, scene::kComponentMeshRenderer));
        mr->flags = scene::kMeshRendererVisible;
        mr->aabbMin[0] = -0.5f * lx; mr->aabbMin[1] = -0.5f * ly; mr->aabbMin[2] = 0.0f;
        mr->aabbMax[0] = 0.5f * lx;  mr->aabbMax[1] = 0.5f * ly;  mr->aabbMax[2] = lz;
    }

    scene::Entity entityAt(const char* name, const Vec3& pos) {
        const scene::Entity e = w.create(name);
        Transform xf;
        xf.position = pos;
        w.setLocalTransform(e, xf);
        made.push_back(e);
        return e;
    }

    // A car-shaped entity with a mesh and a Vehicle component of `preset` (0 car .. 5 tank), sized like the default.
    scene::Entity presetCar(const char* name, const Vec3& pos, u32 preset = 0) {
        const scene::Entity e = entityAt(name, pos);
        setBounds(w, e, 440.0f, 180.0f, 140.0f);
        VehiclePhysicsSystem::attach(w, e, 0, preset);
        return e;
    }

    // An entity with no mesh whose Vehicle names the spec registered as `path`.
    scene::Entity specCar(const char* name, const Vec3& pos, const std::string& path, const VehicleSpec& spec) {
        const u64 id = addSpec(path, spec);
        const scene::Entity e = entityAt(name, pos);
        VehiclePhysicsSystem::attach(w, e, id, 0);
        return e;
    }

    CVehicle* comp(scene::Entity e) { return w.component<CVehicle>(e, VehiclePhysicsSystem::typeId()); }

    void frame(f32 dt = kDt) {
        sys.prePhysics(w, dt);
        aver_phys_step(dt);
        sys.postPhysics(w, dt);
        w.flush();
    }
    void frames(int n, f32 dt = kDt) { for (int i = 0; i < n; ++i) frame(dt); }

    Vec3 worldPos(scene::Entity e) { const Mat4& m = w.worldMatrix(e); return Vec3(m.m[3][0], m.m[3][1], m.m[3][2]); }
};

// The sedan with a different diet of records, for the tests that want a specific shape.
inline VehicleSpec sedanSpec() {
    const VehicleSpec* s = builtinSpec("sedan");
    return s ? *s : VehicleSpec{};
}

}  // namespace vt
