#pragma once
// Godot's PrimitiveMesh classes as Aver meshes, and the polygon helpers CollisionPolygon3D needs.
#include "FabricInternal.hpp"

namespace aver::fabric {

// Looks one property up by name; null when the file did not write it (the Godot default then applies).
using PropGetter = std::function<const Var*(const char*)>;

// Builds the mesh in Godot space (metres, +Y up) with Godot's defaults for anything the getter does not return.
// False with *why for a type this does not generate.
struct SoupMesh {
    std::vector<float> pos, nrm, uv;
    std::vector<unsigned> idx;
};
bool buildPrimitiveMesh(const std::string& type, const PropGetter& get, SoupMesh& out, std::string* why);

// The soup through the engine axis map (aver/formats/AxisMap.hpp), winding reversed, one material slot.
fmt::OcMeshData toOcMesh(const SoupMesh& soup, const std::string& materialSlot);

// A simple polygon in the XY plane. isConvex ignores winding; triangulate returns index triples (ear clipping).
bool polygonIsConvex(const std::vector<std::pair<double, double>>& p);
std::vector<unsigned> polygonTriangulate(const std::vector<std::pair<double, double>>& p);

}  // namespace aver::fabric
