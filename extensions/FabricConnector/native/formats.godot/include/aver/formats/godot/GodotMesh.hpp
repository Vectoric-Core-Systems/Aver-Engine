#pragma once
// Godot 4's mesh storage as plain arrays (G4 R7): one ArrayMesh surface dictionary ("_surfaces" of a .tres / .tscn sub-resource) decoded from the
// packed vertex / attribute / skin / index byte arrays, and GridMap's packed cell table with its 24 orthogonal bases. Pure data: Godot space
// (metres, +Y up, right-handed), no engine type. Written from how Godot 4.4 stores and reads a surface (servers/rendering_server.cpp,
// mesh_surface_make_offsets_from_format and _get_array_from_surface; read for behaviour, nothing copied). Contract: docs/plans/fabric/G4_RENDER.md R7.
#include "aver/core/Types.hpp"
#include "aver/formats/godot/GodotVariant.hpp"

#include <string>
#include <vector>

namespace aver::fmt::godot {

// RenderingServer.ArrayFormat bits.
inline constexpr u64 kGdArrayVertex = 1ull << 0, kGdArrayNormal = 1ull << 1, kGdArrayTangent = 1ull << 2, kGdArrayColor = 1ull << 3,
                     kGdArrayUv = 1ull << 4, kGdArrayUv2 = 1ull << 5, kGdArrayBones = 1ull << 10, kGdArrayWeights = 1ull << 11,
                     kGdArrayIndex = 1ull << 12;
inline constexpr int kGdArrayCustomShift[4] = {13, 16, 19, 22};
inline constexpr u64 kGdFlag2dVertices = 1ull << 25, kGdFlag8BoneWeights = 1ull << 27, kGdFlagEmptyVertexArray = 1ull << 28,
                     kGdFlagCompressAttributes = 1ull << 29;
inline constexpr int kGdFormatVersionShift = 35;

struct GodotSurface {
    int primitive = 3;                 // Mesh.PrimitiveType: 3 triangles, 4 triangle strip (decoded as triangles)
    u64 format = 0;
    int formatVersion = 0;
    u32 vertexCount = 0;
    std::vector<f32> pos;              // 3 per vertex
    std::vector<f32> nrm;              // 3 per vertex, or empty
    std::vector<f32> tan;              // 4 per vertex (xyz, handedness), or empty
    std::vector<f32> col;              // 4 per vertex, 0..1, or empty
    std::vector<f32> uv;               // 2 per vertex, or empty
    std::vector<f32> uv2;              // 2 per vertex, or empty
    std::vector<u32> indices;          // triangle list; a surface with no index array gets 0..n-1
    int boneCount = 0;                 // influences per vertex stored (4 or 8); 0 = no skin
    std::vector<u16> bones;            // boneCount per vertex
    std::vector<f32> weights;          // boneCount per vertex
    int customFormat[4] = {-1, -1, -1, -1};   // ArrayCustomFormat of CUSTOM0..3, -1 = absent (not decoded)
    f32 aabb[6] = {0, 0, 0, 0, 0, 0};
    bool compressed = false;
    bool hasLods = false, hasBlendShapes = false;
};

// Decodes one surface dictionary. False with *why when the data is not a triangle surface or does not match its format; things that are present and not
// decoded (custom data, LODs, blend shapes) are said in `notes` and the surface is still good.
bool decodeArrayMeshSurface(const GodotVariant& dict, GodotSurface& out, std::string* why, std::vector<std::string>* notes = nullptr);

// Godot's octahedral normal coding (Vector3::octahedron_encode / decode), here for the tests and the decoder: components 0..1.
void gdOctEncode(const f32 n[3], f32 out[2]);
void gdOctDecode(const f32 in[2], f32 out[3]);

// ---- GridMap ----

struct GodotGridCell {
    int x = 0, y = 0, z = 0;
    int item = 0;
    int orientation = 0;   // index into the 24 orthogonal bases
    int layer = 0;
};

// GridMap's "data" dictionary: "cells" is a PackedInt32Array, three ints per cell. The first two are the 64-bit key (x in bits 0..15, y in 16..31, z in
// 32..47, signed) and the third is the cell value (item in bits 0..15, orientation in 16..20, layer in 21..28). A cell whose item is 0xFFFF is empty and skipped.
bool decodeGridCells(const GodotVariant& data, std::vector<GodotGridCell>& out, std::string* why);
// The packing, for the tests and the writers.
void encodeGridCell(const GodotGridCell& c, i32 out3[3]);

// GridMap's 24 orthogonal bases (Basis(xx, xy, xz, yx, yy, yz, zx, zy, zz), rows as Godot lists them): out is those nine numbers. An index outside 0..23 answers the identity, as Godot does.
void gdGridBasis(int orientationIndex, double out9[9]);

}  // namespace aver::fmt::godot
