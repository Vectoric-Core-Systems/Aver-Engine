#pragma once
// Blend-shape helpers shared by every importer and mesh tool (MORPH_TARGETS.md 2.2). The data model is
// OcMorphTarget in OcMesh.hpp; weights are named curves "morph.<target>" in an OcAnimation. Importers fill
// OcMeshData::morphTargets only through MorphTargetBuilder, so sparsifying, ordering and validation live
// in one place. Pure arithmetic: no device, no scene.
#include "aver/formats/OcAnim.hpp"
#include "aver/formats/OcMesh.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

// The curve name that drives mesh target `target`. Equals FBX_IMPORT's kFbxMorphCurvePrefix + name.
std::string morphCurveName(std::string_view target);

class MorphTargetBuilder {
public:
    explicit MorphTargetBuilder(u32 vertexCount);

    // Returns the target's index. A repeated or empty name is made unique ("name_2", "Target_<i>"), a name
    // over 128 bytes is cut. Targets past kOcMeshMaxMorphTargets are refused: the return is ~0u.
    u32 addTarget(std::string name, f32 defaultWeight = 0.0f);
    usize targetCount() const { return targets_.size(); }
    const std::string& targetName(u32 target) const { return targets_[target].name; }

    // One vertex's delta (engine space, cm). Called twice for the same vertex it accumulates. A vertex at or
    // past the count, or a bad target index, is ignored.
    void add(u32 target, u32 vertex, Vec3 dPos, const Vec3* dNrm = nullptr);

    // FBX: output vertex -> source control point. addForSource then expands one source vertex's delta to
    // every output vertex that came from it (a hard edge splits a control point into several vertices).
    void setSourceMap(std::vector<u32> outVertexToSource);
    void addForSource(u32 target, u32 sourceVertex, Vec3 dPos, const Vec3* dNrm = nullptr);

    // Drops entries whose position and normal deltas are both under `epsilonCm`, drops empty targets, sorts
    // by vertex, finalizes, and sets mesh.morphTargets (and the header flag). Returns how many targets were
    // dropped for being empty.
    usize finish(OcMeshData& mesh, f32 epsilonCm = 1e-4f);

private:
    struct Raw { u32 vertex; f32 dp[3]; f32 dn[3]; bool hasN; };
    struct Tgt { std::string name; f32 defaultWeight = 0.0f; std::vector<Raw> raw; };
    u32 vertexCount_ = 0;
    std::vector<Tgt> targets_;
    // CSR of source vertex -> output vertices.
    std::vector<u32> srcStart_, srcList_;
    bool hasSourceMap_ = false;
};

// Renumbers every target after the mesh's vertices were renumbered (simplifyMesh). oldToNew[v] == ~0u means
// the vertex is gone. Two old vertices landing on one new vertex keep the lower old vertex's delta. A target
// left with no entries stays (its name is still animated).
void remapMorphTargets(OcMeshData& mesh, const std::vector<u32>& oldToNew, u32 newCount);

// Merges src's targets into dst after src's vertices were appended to dst at dstVertexBase (mergeAll).
// Same-name targets union their entries, the others are added; entries of dst before the base are untouched.
void appendMorphTargets(OcMeshData& dst, const OcMeshData& src, u32 dstVertexBase);

// Appends the weight curve "morph.<target>" to `clip` and lifts clip.duration to its last key. Tangents are
// slopes (value per second) and are used only with OcInterp::CubicSpline; both or neither.
OcCurve& addMorphCurve(OcAnimation& clip, std::string_view target, OcInterp interp, const std::vector<f32>& times,
                       const std::vector<f32>& values, const std::vector<f32>* inTan = nullptr,
                       const std::vector<f32>* outTan = nullptr);

} // namespace aver::fmt
