#pragma once
// Compiles a `DOMAIN material` .ocgraph into the HLSL that replaces the stock averEvalMaterial.
//
// WHY THIS LIVES BESIDE PbrShaders.cpp AND NOT BESIDE THE .ocgraph READER. What it emits is not
// "some HLSL" -- it is text that must agree, name for name, with the AverAuthored/averBuildSurface
// contract declared a few hundred lines away in materialShaderPrelude(). Those two move together or
// the generated shader stops compiling, so they live in one module and one commit. The .ocgraph
// READER is a different concern entirely and stays where it is; this only borrows its data types.
//
// IT EMITS ONE FUNCTION FOR EVERY GRAPH IN THE PROCESS, dispatched by gMaterialGraphId, rather than
// one shader per material. That is a deliberate reading of what this engine actually is: every
// standard material today shares a single pipeline and differs only by a binding set and an 80-byte
// constant block (pbr::MaterialSystem::Entry), so per-material shaders would be the engine's first
// per-material pipeline variant -- a change to the hot draw path, its sorting and its lifetime
// rules, bought to render one cube. A uniform switch on a constant the draw path ALREADY carries
// costs nothing new: materials with no graph hold id 0, take the `default:` arm and shade exactly as
// they did. The trade is that all graphs share a compile, which is also what collapses the
// N-materials x M-consumer-shaders compile cost this tree has no cache for down to M.
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::fmt { struct OcGraphData; }

namespace aver::pbr {

// One graph, compiled.
struct MaterialGraphBody {
    bool ok = false;
    // When !ok: what is wrong and WHICH node it is wrong at, by the id the file gives it, so the
    // message can be read against the .ocgraph without opening the editor.
    std::string error;
    // Statements writing into an `AverAuthored a`, free to read `v` (AverVertex), `l` (AverLight)
    // and `uv` (float2). Indented ready to drop inside a switch arm; never empty when ok.
    std::string hlsl;
};

// Compiles one material graph. Fails, rather than emitting something plausible, when the graph
// names a node type this build has no emitter for -- a silently ignored node is a material that
// looks subtly wrong with nothing to explain it.
MaterialGraphBody compileMaterialGraph(const fmt::OcGraphData& g);

// One compiled graph's place in the process-wide dispatch.
struct MaterialGraphEntry {
    u32 id = 0;         // the gMaterialGraphId value. NEVER 0: that is reserved for "no graph".
    std::string name;   // emitted as a comment, so a dumped shader names the graph it came from
    std::string hlsl;   // MaterialGraphBody::hlsl
};

// The averEvalMaterial that replaces the stock one. Compile the prelude with AVER_MATERIAL_GRAPH
// defined and append this; see materialShaderPrelude()'s own comment for that contract.
//
// An EMPTY entry list still yields a valid function -- one that only ever takes the default arm.
// That is not a degenerate case to guard against but the honest answer for a project whose
// materials are all stock, and it keeps the caller from having to decide whether to append at all.
std::string materialGraphHlsl(const std::vector<MaterialGraphEntry>& entries);

} // namespace aver::pbr
