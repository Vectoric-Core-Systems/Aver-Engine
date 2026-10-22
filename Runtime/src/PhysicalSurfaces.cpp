// PhysicalSurfaces: the project's physical-surface table -> the physics registry. See PhysicalSurfaces.hpp.
#include "aver/game/PhysicalSurfaces.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcSurf.hpp"

#include <filesystem>
#include <system_error>

#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_surface_abi.h"
#endif

namespace aver::game {

std::string physicalSurfacesPath(const std::string& contentDir) {
    if (contentDir.empty()) return std::string();
    return contentDir + AVER_PATH_SEP + "Physics" + AVER_PATH_SEP + "Surfaces.ocsurf";
}

u32 registerPhysicalSurfaces(const fmt::OcSurfData& data) {
#if AVER_MODULE_PHYSICS
    u32 accepted = 0;
    for (const fmt::OcSurfRecord& r : data.surfaces) {
        if (aver_phys_surface_register(r.name.c_str(), r.friction, r.restitution, r.tag) != 0) ++accepted;
        else AVER_WARN("[Surfaces] '{}' was refused by the physics registry (its id collides with another surface)", r.name);
    }
    return accepted;
#else
    (void)data;
    return 0;
#endif
}

u32 loadPhysicalSurfaces(const std::string& contentDir, std::string* err) {
    const std::string path = physicalSurfacesPath(contentDir);
    if (path.empty()) return 0;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return 0;   // a project without a table is the ordinary case

    fmt::OcSurfData data;
    std::string why;
    if (!fmt::loadOcSurf(path, data, &why)) {
        AVER_WARN("[Surfaces] could not load '{}': {}", path, why);
        if (err) *err = why;
        return 0;
    }
    const u32 n = registerPhysicalSurfaces(data);
    AVER_INFO("[Surfaces] {} physical surface(s) registered from {}", n, path);
    return n;
}

bool registerSurfaceIfAbsent(const std::string& name, f32 friction, f32 restitution) {
#if AVER_MODULE_PHYSICS
    if (name.empty()) return false;
    const i32 id = aver_phys_surface_id(name.c_str());
    // Registered means it has a NAME: a surface only interned by a shape built with its id is known with an empty one.
    uint8_t known[8] = {};
    if (id != 0 && aver_phys_surface_info(id, nullptr, nullptr, nullptr, known, static_cast<i32>(sizeof known)) == 1 && known[0] != 0)
        return false;
    return aver_phys_surface_register(name.c_str(), friction, restitution, 0) != 0;
#else
    (void)name; (void)friction; (void)restitution;
    return false;
#endif
}

} // namespace aver::game
