#include "aver/assets/AssetId.hpp"
#include "aver/core/Hash.hpp"

#include <string>

namespace aver {

ObjectId makeObjectId(std::string_view name) {
    return fnv1a64(name);
}

static std::string extLower(std::string_view path) {
    const usize dot = path.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    std::string e(path.substr(dot + 1));
    for (char& c : e) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return e;
}

AssetType assetTypeFromPath(std::string_view path) {
    const std::string e = extLower(path);
    if (e == "ocbeam") return AssetType::Beam;
    if (e == "ocmap")  return AssetType::Map;
    if (e == "ocmesh") return AssetType::Mesh;
    if (e == "octex")  return AssetType::Texture;
    // Source images are textures too. Until an asset cooker exists to bake .octex, a material's
    // maps ARE .png/.tga files, and the loader has to be able to say so.
    if (e == "png" || e == "jpg" || e == "jpeg" || e == "tga" || e == "bmp") return AssetType::Texture;
    if (e == "ocmat")  return AssetType::Material;
    if (e == "ocskel") return AssetType::Skeletal;
    if (e == "ocanim") return AssetType::Anim;
    if (e == "ocprefab") return AssetType::Prefab;
    if (e == "ocaero") return AssetType::Aero;
    return AssetType::Unknown;
}

const char* assetTypeName(AssetType t) {
    switch (t) {
        case AssetType::Beam:     return "Beam";
        case AssetType::Map:      return "Map";
        case AssetType::Mesh:     return "Mesh";
        case AssetType::Texture:  return "Texture";
        case AssetType::Material: return "Material";
        case AssetType::Skeletal: return "Skeletal";
        case AssetType::Anim:     return "Anim";
        case AssetType::Prefab:   return "Prefab";
        case AssetType::Aero:     return "Aero";
        case AssetType::Unknown:  return "Unknown";
    }
    return "Unknown";
}

} // namespace aver
