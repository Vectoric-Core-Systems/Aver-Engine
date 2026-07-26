#pragma once
// .ocmat — the MATERIAL format (FORMAT_SPECS.md §7), text, OC dialect.
//
// This parses straight into pbr::MaterialDesc. There is deliberately no intermediate OcMatData
// struct: Material.hpp names its fields after the spec's PARAM keys one-for-one precisely so the
// loader is a rename-free mapping, and a duplicate struct here would reintroduce the translation
// table that comment exists to prevent.
//
// WHY THIS IS ITS OWN TARGET (Aver.Formats.Material): it needs both the text scanners in
// Aver.Formats and the MaterialDesc in Aver.Render.PBR. Aver.Render.PBR is a Core-only shared DLL --
// the scripting layer P/Invokes it -- so it cannot link Aver.Formats, and making Aver.Formats link
// the PBR DLL would push a renderer-family binary onto every headless tool that only wanted to read
// a map. A third target is the only arrangement where neither of those is true. Same split, same
// reason, as Aver.Render.PBR / Aver.Render.PBR.Materials.
//
// Implemented: OCMAT, NAME, SHADER, BLEND, CULL, FLAGS, PARAM, TEX, PARENT. NOT implemented: the
// optional GRAPH{} block, which needs a material compiler that does not exist -- it is detected,
// reported through OcMatExtras::hasGraph and skipped, so a graph material still loads with its
// factors rather than failing.
#include "aver/pbr/Material.hpp"

#include <string>
#include <string_view>

namespace aver::fmt {

// What the file said that MaterialDesc has no field for. Kept separate rather than bolted onto
// MaterialDesc because none of it reaches a shader today: putting it in the desc would mean the GPU
// pack function has to ignore fields, which is how a field ends up silently ignored forever.
// Round-tripping needs it, and the editor needs it to say "this material asks for something the
// renderer does not do yet" instead of pretending the file was fully understood.
struct OcMatExtras {
    std::string shader = "standard";   // standard | unlit | clearcoat | glass | decal | custom
    std::string cull   = "back";       // back | front | none
    // BLEND additive. Material.hpp is explicit that additive is an authoring MODE and not an alpha
    // rule, so it cannot be an AlphaMode; it is recorded here and the desc gets the nearest legal
    // rule (Blend) so the file still loads.
    bool additive = false;

    // PARENT {guid|path} — the material-instance mechanism in §7. Recorded, never applied: applying
    // it means resolving another asset and layering only the params this file listed, which needs
    // per-field "was this authored" tracking the desc does not carry. Reported so nothing pretends
    // the parent's values were inherited.
    u64         parentId = 0;
    std::string parentPath;

    bool hasGraph = false;
    // TEX ... uvN. Only uv0 reaches a shader (the vertex format carries one set), so a file asking
    // for uv1 is recorded and ignored rather than silently sampling the wrong coordinates.
    u32 uvSet[pbr::kTextureSlotCount] = {};
};

// Parse from memory. Unknown records are SKIPPED, not failed — the same forward-compatibility rule
// every other .oc* reader follows. false only for a missing or wrong-version OCMAT header.
bool parseOcmat(std::string_view text, pbr::MaterialDesc& out,
                OcMatExtras* extras = nullptr, std::string* err = nullptr);

// Load from disk. The material's name defaults to the file's stem when the file states none, so a
// material is never nameless in the editor.
bool loadOcmat(const std::string& path, pbr::MaterialDesc& out,
               OcMatExtras* extras = nullptr, std::string* err = nullptr);

// Serialise to the text form. Round-trips through parseOcmat.
std::string writeOcmat(const pbr::MaterialDesc& d, const OcMatExtras* extras = nullptr);

// Write to disk, creating parent directories.
bool saveOcmat(const std::string& path, const pbr::MaterialDesc& d,
               const OcMatExtras* extras = nullptr, std::string* err = nullptr);

// The colour-space word §7 writes for a slot. It is a property of the SLOT, not a free choice: the
// shading model reads base colour and emissive as sRGB-encoded colour and the rest as linear data,
// so this is what the writer emits and what the reader checks an incoming file against.
const char* ocmatColorSpace(pbr::TextureSlot s);

} // namespace aver::fmt
