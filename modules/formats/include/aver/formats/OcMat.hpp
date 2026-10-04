#pragma once
// .ocmat — the MATERIAL format (FORMAT_SPECS.md §7), text, OC dialect. Parses straight into
// pbr::MaterialDesc; the optional GRAPH{} block is detected, reported and skipped. A separate
// GRAPHREF record names a compiled DOMAIN material .ocgraph by path (see OcMatExtras::graphRef).
#include "aver/pbr/Material.hpp"

#include <string>
#include <string_view>

namespace aver::fmt {

// What the file said that pbr::MaterialDesc has no field for. Kept for round-tripping and reporting.
struct OcMatExtras {
    std::string shader = "standard";   // standard | unlit | clearcoat | glass | decal | custom
    std::string cull   = "back";       // back | front | none
    bool additive = false;             // BLEND additive; the desc gets the nearest legal rule

    // PARENT {guid|path}. Recorded, never applied.
    u64         parentId = 0;
    std::string parentPath;

    // GRAPHREF <path>. The path to a DOMAIN material .ocgraph that shades this material, recorded
    // here VERBATIM and relative to the project's content root -- the same convention COMP mesh=
    // uses in .ocgraph (ActorScript.hpp's canonicalMeshPath), never with the content directory on
    // the front. Nothing in this format layer resolves the path, loads the graph, or checks that it
    // compiles: that is pbr::MaterialGraphRegistry::add's job, once some loader has both the graph
    // and this path in hand and can turn one into the runtime-assigned id that MaterialDesc::graphId
    // wants (see that field's own comment for why the id is never round-tripped through a file).
    std::string graphRef;

    bool hasGraph = false;
    u32 uvSet[pbr::kTextureSlotCount] = {};   // TEX ... uvN; only uv0 reaches a shader
};

// Parses a material from memory. Unknown records are skipped; false only for a missing or
// wrong-version OCMAT header.
bool parseOcmat(std::string_view text, pbr::MaterialDesc& out,
                OcMatExtras* extras = nullptr, std::string* err = nullptr);

// Loads a material from disk. The name defaults to the file's stem when the file states none.
bool loadOcmat(const std::string& path, pbr::MaterialDesc& out,
               OcMatExtras* extras = nullptr, std::string* err = nullptr);

// Serialises a material to the text form. Round-trips through parseOcmat.
std::string writeOcmat(const pbr::MaterialDesc& d, const OcMatExtras* extras = nullptr);

// Writes a material to disk, creating parent directories.
bool saveOcmat(const std::string& path, const pbr::MaterialDesc& d,
               const OcMatExtras* extras = nullptr, std::string* err = nullptr);

// The colour-space word §7 writes for a texture slot: sRGB for base colour and emissive, linear for
// the rest.
const char* ocmatColorSpace(pbr::TextureSlot s);

} // namespace aver::fmt
