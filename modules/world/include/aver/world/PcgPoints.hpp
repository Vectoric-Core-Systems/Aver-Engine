// Point data for the PCG graph evaluator (docs/PCG.md): a struct-of-arrays point set with named
// attributes, the Aver.Pcg hash port, and a spatial hash grid.
//
// NO SCENE, NO RHI, NO GPU. Pure Core types, so the editor, the chunk streamer and a headless test
// all run the same code. Units are centimetres, +Z up, the engine's left-handed basis.
#pragma once

#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace aver::world {

// ---- Aver.Pcg.Rand, ported bit for bit (scripting/fsharp/Aver.Pcg/Pcg.fs) --------------------------------------------------
// Stateless splitmix32. tests/world/PcgPointsTest pins these against values generated once from the F# module.
u32 pcgHash(u32 x);
u32 pcgHash2(i32 seed, i32 i);
u32 pcgHash3(i32 seed, i32 x, i32 y, i32 z);
f32 pcgFloat01(u32 h);   // top 24 bits / 2^24, exact in f32
f32 pcgRange(i32 seed, i32 i, f32 lo, f32 hi);
u32 pcgHashString(std::string_view s);   // a stable id -> salt (FNV-1a-64 folded to 32 bits)

// One point, array-of-structs, for building and for tests.
struct PcgPoint {
    Vec3 pos{};
    Quat rot = Quat::identity();
    Vec3 scale{1, 1, 1};
    f32 density = 1.0f;
    i32 seed = 0;
    Vec3 color{1, 1, 1};
    f32 steepness = 0.0f;       // degrees from +Z of the surface the point sits on
    Vec3 extent{0, 0, 0};       // local half-extents in cm; 0 = unspecified
};

// SoA point set. Every column has size() entries; named attributes are extra columns.
struct PcgPoints {
    std::vector<Vec3> pos;
    std::vector<Quat> rot;
    std::vector<Vec3> scale;
    std::vector<f32> density;
    std::vector<i32> seed;
    std::vector<Vec3> color;
    std::vector<f32> steepness;
    std::vector<Vec3> extent;

    std::map<std::string, std::vector<f32>> fAttr;
    std::map<std::string, std::vector<i32>> iAttr;
    std::map<std::string, std::vector<Vec3>> vAttr;

    usize size() const { return pos.size(); }
    bool empty() const { return pos.empty(); }
    void reserve(usize n);
    void clear();

    PcgPoint get(usize i) const;
    void push(const PcgPoint& p);
    // Copies point `i` of `src` including its named attributes; columns missing on either side are zero-filled.
    void appendFrom(const PcgPoints& src, usize i);
    void appendAll(const PcgPoints& src);
    PcgPoints select(const std::vector<u32>& indices) const;
    // The same schema, no points.
    PcgPoints emptyLike() const;

    // Attribute access by name. The core columns answer to "density", "steepness" and "seed"; anything else is a named column.
    bool hasScalar(std::string_view name) const;
    f32 scalar(std::string_view name, usize i, f32 fallback = 0.0f) const;
    void setScalar(std::string_view name, usize i, f32 v);   // creates a named f32 column on first use

    // FNV-1a-64 over every column's bytes, name-sorted. The evaluator's cache key for "the same input".
    u64 contentHash() const;
};

// Uniform 3D hash grid over point positions. Query order is deterministic: cells ascending in (z, y, x), points in
// insertion order inside a cell.
class PcgSpatialGrid {
public:
    explicit PcgSpatialGrid(f32 cellCm = 100.0f) : cell_(cellCm > 0.0f ? cellCm : 100.0f) {}
    void insert(const Vec3& p, u32 index);
    void build(const std::vector<Vec3>& positions);

    // Calls f(index) for every inserted point whose cell is within `radius` of `p` (a superset of the points
    // within radius; the caller does the exact distance test).
    template <class F>
    void forEachNear(const Vec3& p, f32 radius, F&& f) const {
        i32 lo[3], hi[3];
        range(p, radius, lo, hi);
        for (i32 z = lo[2]; z <= hi[2]; ++z)
            for (i32 y = lo[1]; y <= hi[1]; ++y)
                for (i32 x = lo[0]; x <= hi[0]; ++x) {
                    const auto it = cells_.find(key(x, y, z));
                    if (it == cells_.end()) continue;
                    for (u32 idx : it->second) f(idx);
                }
    }

private:
    static u64 key(i32 x, i32 y, i32 z);
    void range(const Vec3& p, f32 r, i32* lo, i32* hi) const;
    f32 cell_;
    std::map<u64, std::vector<u32>> cells_;
};

} // namespace aver::world
