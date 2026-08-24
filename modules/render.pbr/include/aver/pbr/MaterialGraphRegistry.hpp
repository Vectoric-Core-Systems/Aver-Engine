#pragma once
// Every material graph in the process, and the single averEvalMaterial they compile to.
//
// WHY THIS IS PROCESS-WIDE RATHER THAN OWNED BY A RENDERER. The generated function is baked into
// every pipeline that calls averEvalMaterial -- today PSMainVoxi, PSVoxel and PSClusterMain, three
// shaders built by two different subsystems. If each owned its own set of graphs they would compile
// different functions from the same files and a material would shade differently depending on which
// path drew it. There is exactly one right answer to "what does graph 7 do", so there is one place
// that answers it. materialShaderPrelude() is process-wide for the same reason and this sits beside
// it deliberately.
//
// IDS ARE STABLE FOR THE PROCESS AND START AT 1. A material's id is written into its constant block
// (MaterialConstants::graphId), which may already have been packed and uploaded, so re-registering
// the same path -- a hot reload, a second material naming the same graph -- MUST return the id it
// returned before. 0 is reserved and means "no graph": it is what every material authored before
// this feature holds, and what the generated switch's `default:` arm answers.
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::fmt { struct OcGraphData; }

namespace aver::pbr {

class MaterialGraphRegistry {
public:
    // Registers the graph at `key` (its content path, which is its identity) and returns the id to
    // store in a material's constants.
    //
    // RETURNS 0 AND LOGS WHEN THE GRAPH DOES NOT COMPILE, rather than failing the material outright.
    // A material whose graph is broken falls back to the stock path, which is a surface an author
    // can still see and place while they fix the graph; refusing to load it would take the whole
    // object out of the scene and make a shading mistake look like a missing asset.
    u32 add(const std::string& key, const std::string& displayName, const fmt::OcGraphData& g);

    // The id `key` holds, or 0 if it has never compiled.
    u32 idOf(const std::string& key) const;

    // The generated averEvalMaterial, or EMPTY when no graph has ever compiled.
    //
    // Empty is the signal for a renderer not to define AVER_MATERIAL_GRAPH at all, so a project with
    // no material graphs -- which is every project that exists today -- compiles the stock prelude
    // untouched and cannot be affected by any of this.
    const std::string& hlsl() const { return hlsl_; }

    // Bumped whenever hlsl() changes. A renderer records the revision its pipelines were built
    // against and rebuilds when this moves; see MaterialGraphRegistry's use in VoxiRenderer.
    u64 revision() const { return revision_; }

    // How many graphs are registered and compiled.
    usize count() const { return entries_.size(); }

    // Forgets everything. For a project close, and for tests, which must not inherit each other's
    // ids through a process-wide table.
    void clear();

private:
    struct Entry {
        std::string key;    // the content path
        std::string name;   // for the comment in the emitted shader
        std::string body;   // the compiled statements
        u32 id = 0;
    };
    void rebuild();

    std::vector<Entry> entries_;
    std::string hlsl_;
    u64 revision_ = 0;
    u32 nextId_ = 1;
};

// The one registry. A free accessor rather than a constructor argument threaded through four
// subsystems, matching materialShaderPrelude()'s own shape: both answer a question about the
// process's shaders, and neither has a second implementation to select between.
MaterialGraphRegistry& materialGraphs();

} // namespace aver::pbr
