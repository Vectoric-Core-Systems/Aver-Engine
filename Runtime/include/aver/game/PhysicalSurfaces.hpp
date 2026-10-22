// PhysicalSurfaces: the project's physical-surface table, loaded into the physics module's registry.
//
// A surface is a NAME hashed to a 32-bit id (aver::surfaceIdFromName); Content/Physics/Surfaces.ocsurf names the surfaces a
// project uses and the friction, restitution and tag of each (modules/formats OcSurf.hpp). Both hosts call
// loadPhysicalSurfaces when a project opens, BEFORE any level builds a shape, so a record's numbers are in the registry when
// a material slot, a mesh palette or a landscape layer first names the surface. Layers register their own surface with
// registerSurfaceIfAbsent, which never overwrites a name the table already holds: an authored table is authoritative.
//
// Without the physics module compiled in (AVER_MODULE_PHYSICS) every call here is a no-op returning 0 or false.
#pragma once
#include "aver/core/Types.hpp"

#include <string>

namespace aver::fmt { struct OcSurfData; }

namespace aver::game {

// The table's location under a project's content directory.
inline constexpr const char* kPhysicalSurfacesRelPath = "Physics/Surfaces.ocsurf";
std::string physicalSurfacesPath(const std::string& contentDir);

// Registers every record of `data` (upsert: the table's numbers win over anything registered before). Returns how many the
// registry accepted; a record it refuses (a hash collision with a different registered name) is logged and skipped.
u32 registerPhysicalSurfaces(const fmt::OcSurfData& data);

// Loads <contentDir>/Physics/Surfaces.ocsurf and registers it. A missing file is not an error: 0 is returned and nothing is
// logged. A file that does not parse logs the reason (and sets `err` when given) and registers nothing. Returns the number
// of surfaces registered.
u32 loadPhysicalSurfaces(const std::string& contentDir, std::string* err = nullptr);

// Registers `name` with the numbers a landscape layer states ONLY when no surface of that name is registered yet (find-or-add),
// so the authored table, loaded first, keeps its values. True when this call registered it.
bool registerSurfaceIfAbsent(const std::string& name, f32 friction, f32 restitution);

} // namespace aver::game
