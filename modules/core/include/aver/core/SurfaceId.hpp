#pragma once
// A physical surface's id: the 32-bit FNV-1a hash of its UTF-8 name, never 0. 0 is "no surface", for an empty name.
//
// The same function is Aver.Physics.Surfaces.IdOf in C# (Aver.Framework.Surface.IdOf calls it) and
// aver_phys_surface_id in the physics module, so a landscape layer, a material slot, a raycast hit and a
// contact all answer in one id space. Golden values are pinned in tests/physics/src/SurfaceTest.cpp.
#include "aver/core/Types.hpp"

#include <string_view>

namespace aver {

// FNV-1a 32 over the bytes of `name` (the offset basis and prime are the standard 32-bit ones). A zero hash
// becomes 1 so that a real name never collides with "none". Case-sensitive.
inline i32 surfaceIdFromName(std::string_view name) {
    if (name.empty()) return 0;
    u32 h = 2166136261u;
    for (const char c : name) {
        h ^= static_cast<u8>(c);
        h *= 16777619u;
    }
    if (h == 0) h = 1;
    return static_cast<i32>(h);
}

} // namespace aver
