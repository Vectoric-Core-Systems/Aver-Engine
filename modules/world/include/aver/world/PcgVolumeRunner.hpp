// Running a PCG graph over a streamed world: a program (graph + environment), per-cell partitioned generation, and a
// dirty-tracking cell store (docs/PCG.md).
//
// A cell is an XY square of `cellSizeCm` (the streaming chunk size when driving GeneratedChunkSource). Generating a cell
// evaluates the graph over the cell grown by the graph's margin, then keeps only the results whose position lies in the
// cell (half-open), so every point is owned by exactly one cell and neighbouring cells never duplicate or drop one.
#pragma once

#include "aver/world/PcgGraph.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace aver::world {

struct PcgProgram {
    std::shared_ptr<const fmt::OcGraphData> graph;
    i32 seed = 0;
    // Z range the graph samples in. XY is unbounded unless `bounded`.
    f32 zMin = -100000.0f, zMax = 100000.0f;
    bool bounded = false;
    PcgBox area;                                    // XY limits when bounded (z ignored)
    std::shared_ptr<const ILandscapeQuery> landscape;
    std::map<std::string, std::vector<Vec3>> splines;
    std::map<std::string, std::vector<Vec3>> actors;
    std::map<std::string, PcgBox> volumes;
    std::function<const fmt::OcGraphData*(const std::string&)> graphLoader;
    u64 environmentStamp = 0;                       // bump when landscape/splines/actors change
    f64 budgetMs = 0.0;                             // per cell; 0 = unlimited
};

// A landscape that only knows a height function (the host's existing heightSource); normals are differenced.
class PcgHeightFnLandscape final : public ILandscapeQuery {
public:
    explicit PcgHeightFnLandscape(std::function<bool(f32, f32, f32&)> h) : h_(std::move(h)) {}
    bool heightAt(f64 x, f64 y, f32& z) const override { return h_ && h_(static_cast<f32>(x), static_cast<f32>(y), z); }
private:
    std::function<bool(f32, f32, f32&)> h_;
};

// The generatorVersion recorded for graph-generated chunks: the high bits mark the PCG path (palette versions are small
// integers), the low bits are the graph hash, so editing a graph makes older saves detectable instead of silently mixed.
u32 pcgGeneratorVersion(const PcgProgram& p);

class PcgCellGenerator {
public:
    PcgCellGenerator(std::shared_ptr<const PcgProgram> program, i32 cellSizeCm);
    const PcgProgram& program() const { return *program_; }
    i32 cellSizeCm() const { return cell_; }
    f32 margin() const { return margin_; }
    PcgBox cellBox(i32 cx, i32 cy) const;

    // Everything the cell owns. Pure in (program, cx, cy).
    PcgResult generate(i32 cx, i32 cy, PcgCache* cache = nullptr) const;
    // The unpartitioned reference over an arbitrary box (tests, bake).
    PcgResult generateBox(const PcgBox& box, PcgCache* cache = nullptr) const;

private:
    PcgContext makeContext(const PcgBox& evalBox, bool partitioned, PcgCache* cache) const;
    std::shared_ptr<const PcgProgram> program_;
    i32 cell_;
    f32 margin_;
};

// Generated cells with staleness. Editing the graph makes every cell stale (each regenerates when next asked for);
// invalidateBox marks only the cells a changed region can reach.
class PcgCellStore {
public:
    PcgCellStore(std::shared_ptr<const PcgProgram> program, i32 cellSizeCm, bool useNodeCache = true);

    const PcgResult& get(i32 cx, i32 cy);
    bool isCurrent(i32 cx, i32 cy) const;
    // Replaces the program; cells generated under a different graph hash or environment stamp regenerate lazily.
    void setProgram(std::shared_ptr<const PcgProgram> program);
    // Marks cells whose region (grown by the margin) overlaps `box`. Returns how many were marked.
    u32 invalidateBox(const PcgBox& box);
    void clear() { cells_.clear(); }

    u32 generateCalls() const { return generateCalls_; }
    usize cellCount() const { return cells_.size(); }

private:
    struct Entry { PcgResult res; u64 graphHash = 0; u64 stamp = 0; bool valid = false; bool stale = false; };
    std::shared_ptr<const PcgProgram> program_;
    i32 cellSizeCm_;
    PcgCellGenerator gen_;
    PcgCache nodeCache_;
    bool useCache_;
    u64 graphHash_ = 0;
    std::map<std::pair<i32, i32>, Entry> cells_;
    u32 generateCalls_ = 0;
};

} // namespace aver::world
