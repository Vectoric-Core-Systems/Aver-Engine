#include "aver/prefab/PrefabAbiHost.hpp"

#include "aver/world/LevelTransform.hpp"

namespace aver::prefab {
namespace {

PrefabSystem& sysOf(void* user) { return *static_cast<PrefabSystem*>(user); }

int32_t abiSpawn(void* user, const char* path, int32_t parent, const float* xyz, const float* ypr, float scale) {
    PrefabSystem& s = sysOf(user);
    Transform xf;
    if (xyz) xf.position = Vec3{xyz[0], xyz[1], xyz[2]};
    if (ypr) xf.rotation = world::quatFromEulerDeg(Vec3{ypr[2], ypr[1], ypr[0]});   // roll, pitch, yaw
    const f32 k = scale == 0.0f ? 1.0f : scale;
    xf.scale = Vec3{k, k, k};
    const scene::Entity p = static_cast<scene::Entity>(parent);
    return static_cast<int32_t>(s.instantiate(path, xf, s.world().valid(p) ? p : scene::kInvalidEntity));
}

int32_t abiDestroy(void* user, int32_t root) {
    return sysOf(user).destroyInstance(static_cast<scene::Entity>(root)) ? 1 : 0;
}

int32_t abiRootOf(void* user, int32_t e) {
    return static_cast<int32_t>(sysOf(user).instanceRoot(static_cast<scene::Entity>(e)));
}

int32_t abiFind(void* user, int32_t root, const char* path) {
    return static_cast<int32_t>(sysOf(user).entityAtPath(static_cast<scene::Entity>(root), path ? path : ""));
}

int32_t abiRevert(void* user, int32_t root) {
    return sysOf(user).revertAll(static_cast<scene::Entity>(root)) ? 1 : 0;
}

int32_t abiOverrideCount(void* user, int32_t root) {
    std::vector<fmt::OcPrefabOverride> ov;
    return sysOf(user).computeOverrides(static_cast<scene::Entity>(root), ov) ? static_cast<int32_t>(ov.size()) : 0;
}

} // namespace

AverPrefabHost makeAbiHost(PrefabSystem& sys) {
    AverPrefabHost h{};
    h.user = &sys;
    h.spawn = &abiSpawn;
    h.destroy = &abiDestroy;
    h.root_of = &abiRootOf;
    h.find = &abiFind;
    h.revert = &abiRevert;
    h.override_count = &abiOverrideCount;
    return h;
}

} // namespace aver::prefab
