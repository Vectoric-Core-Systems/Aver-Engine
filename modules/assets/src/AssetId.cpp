// Object ids and the extension-to-AssetType mapping.
#include "aver/assets/AssetId.hpp"
#include "aver/core/Hash.hpp"

#include <string>

namespace aver {

// Derives a stable ObjectId from an asset name.
ObjectId makeObjectId(std::string_view name) {
    return fnv1a64(name);
}

// Returns the lowercased extension of a path, without the dot. Empty if there is none.
static std::string extLower(std::string_view path) {
    const usize dot = path.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    std::string e(path.substr(dot + 1));
    for (char& c : e) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return e;
}

// Maps a filename to its AssetType. Unknown when the extension is not recognised.
AssetType assetTypeFromPath(std::string_view path) {
    const std::string e = extLower(path);
    if (e == "ocbeam") return AssetType::Beam;
    if (e == "ocmap")  return AssetType::Map;
    if (e == "ocmesh") return AssetType::Mesh;
    if (e == "octex")  return AssetType::Texture;
    if (e == "png" || e == "jpg" || e == "jpeg" || e == "tga" || e == "bmp") return AssetType::Texture;
    if (e == "ocmat")  return AssetType::Material;
    if (e == "ocskel") return AssetType::Skeletal;
    if (e == "ocanim") return AssetType::Anim;
    if (e == "ocprefab") return AssetType::Prefab;
    if (e == "ocaero") return AssetType::Aero;
    if (e == "ocparticle") return AssetType::Particle;
    return AssetType::Unknown;
}

} // namespace aver
