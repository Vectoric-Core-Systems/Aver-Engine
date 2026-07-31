#pragma once
// Asset identity: the 64-bit ObjectId every object carries, and the asset type behind a filename.
#include "aver/core/Types.hpp"

#include <string_view>

namespace aver {

// A unique 64-bit id for an object or asset. Derived from the name via FNV-1a-64 when a file
// declares none, so name-references and id-references are interchangeable.
using ObjectId = u64;
inline constexpr ObjectId kInvalidObjectId = 0;

// Derives a stable ObjectId from an asset name.
ObjectId makeObjectId(std::string_view name);

// The kinds of asset, one per file extension.
enum class AssetType : u32 {
    Unknown = 0,
    Beam,     // .ocbeam
    Map,      // .ocmap
    Mesh,     // .ocmesh
    Texture,  // .octex
    Material, // .ocmat
    Skeletal, // .ocskel
    Anim,     // .ocanim
    Prefab,   // .ocprefab
    Aero,     // .ocaero
};

// Maps a filename to an AssetType. Case-insensitive on the extension.
AssetType assetTypeFromPath(std::string_view path);
// Returns the display name of an AssetType.
const char* assetTypeName(AssetType t);

} // namespace aver
