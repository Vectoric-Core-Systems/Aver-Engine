#pragma once
#include "aver/core/Types.hpp"

#include <string_view>

namespace aver {

// Every object/asset carries a unique 64-bit Object ID (the "1 param" added across
// the format family). When a file doesn't declare one, it is derived stably from the
// asset's name via FNV-1a-64 — the same hash the .ocmap ID uses — so name-references
// and id-references are interchangeable.
using ObjectId = u64;
inline constexpr ObjectId kInvalidObjectId = 0;

ObjectId makeObjectId(std::string_view name);

enum class AssetType : u32 {
    Unknown = 0,
    Beam,     // .ocbeam  — breakable deformable skeletal/soft-body mesh
    Map,      // .ocmap   — world container (references + positions)
    Mesh,     // .ocmesh  — static mesh
    Texture,  // .octex
    Material, // .ocmat
    Skeletal, // .ocskel
    Anim,     // .ocanim
    Prefab,   // .ocprefab
    Aero,     // .ocaero
};

// Map a filename/extension to an AssetType (case-insensitive on the extension).
AssetType assetTypeFromPath(std::string_view path);
const char* assetTypeName(AssetType t);

} // namespace aver
