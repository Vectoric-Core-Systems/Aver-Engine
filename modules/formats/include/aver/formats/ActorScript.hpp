#pragma once
// The actor designer's half of a `.Designer.cs`: reading the model rows out, and writing dragged
// coordinates back in.
//
// This is what an actor editor's viewport DRAWS and what its gizmo SAVES.
//
// TWO BACKENDS, and which one runs is a choice rather than a compromise:
//
//   Builtin -- the scanner below. No dependency, always present, and sufficient BY CONSTRUCTION:
//     docs/DESIGNER_REWRITE.md locks the placement statement to one exact token sequence and makes
//     the three coordinate tuples the only rewritable payload, precisely so that reading and
//     rewriting a designer-owned region needs no compiler. A designer-owned region is a region whose
//     shape the designer chose.
//
//   Roslyn -- the `averdesign` tool, if it is staged. It parses real C#, so it survives everything
//     the locked grammar forbids: a named argument moved, an argument omitted, a coordinate written
//     as an expression rather than a literal, a `#if` around a placement. The moment somebody hand
//     edits inside the region -- which the header of every generated file tells them not to do, and
//     which people will do -- the scanner declines and this does not.
//
// The scanner is tried first because it needs nothing and answers instantly. Roslyn is the fallback
// for a file it declines, and the escalation is automatic: `Malformed` is exactly the signal that
// the text has left the grammar the scanner was built for.
//
// So a machine with no .NET tooling still opens every conforming actor, and a machine with it opens
// the rest too. Neither is a degraded mode; they cover different files.
//
// WHAT IT WILL NOT TOUCH, and the rules are the file format's, not this header's:
//   - anything outside the two `// <aver-generated region="models" schema="1">` marker lines
//   - the marker lines themselves, the usings, the namespace, the class header
//   - the hand-written half of the actor (Car.cs), which the editor never even reads
//   - any field of a placement other than pos / rot / scale
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

// One `b.Place(...)` row.
struct ActorModel {
    // THE MATCH KEY. A placement is identified by this and never by its position in the file, so a
    // reordered region, an inserted row or a renamed property all still find the right line -- and
    // the same id is stamped onto the spawned child entity, which is how a gizmo in the viewport
    // gets back to the text that put it there.
    u64 objectId = 0;

    std::string property;   // the C# identifier the row assigns to
    std::string meshPath;   // as written in the source
    std::string material;

    f32 pos[3]   = {0.0f, 0.0f, 0.0f};   // centimetres
    f32 rot[3]   = {0.0f, 0.0f, 0.0f};   // degrees, (yaw, pitch, roll)
    f32 scale[3] = {1.0f, 1.0f, 1.0f};

    // Byte range of the whole statement within the source, so a rewriter never has to find it twice.
    usize begin = 0, end = 0;
};

enum class ActorParseStatus {
    Ok,
    NoRegion,        // no open marker: the file has no editor territory (not an error, per the spec)
    UnknownSchema,   // a schema this build does not know; the file is left alone and reported
    // A region that is there but has left the locked grammar. THE ESCALATION SIGNAL: this is the
    // status a caller with `averdesign` available should retry through Roslyn, because it means the
    // text is C# the scanner was not built for rather than C# that is wrong.
    Malformed,
};

// Which implementation produced a result. Recorded rather than inferred, so a log line or a panel
// can say which one answered -- when the two ever disagree, knowing which ran is the whole debugging
// story.
enum class ActorParserBackend { Builtin, Roslyn };

struct ActorScript {
    ActorParseStatus status = ActorParseStatus::NoRegion;
    ActorParserBackend backend = ActorParserBackend::Builtin;
    std::string error;                  // set for UnknownSchema / Malformed
    std::vector<ActorModel> models;
    usize regionBegin = 0, regionEnd = 0;   // the span strictly between the marker lines
};

// What an actor declares ABOUT ITSELF, in `Configure(ClassBuilder b)`.
//
// The generated region is the MULTI-PART path: an actor assembled from several placed meshes. Most
// actors in most games are not that. They are one mesh, declared with `b.Mesh(...)` in the class's
// own Configure, and they have no designer file at all -- every actor in the SkyForge template is
// this shape. A preview that only understood placements would show an empty view for all of them,
// which is not a general-purpose actor editor.
//
// So this reads the hand-written half. READS. docs/DESIGNER_REWRITE.md says the editor never writes
// a byte outside the generated region and that is unchanged and unchangeable; displaying what a
// class declares is not writing it, and refusing to look would mean refusing to preview the common
// case in order to honour a rule about a different operation.
struct ActorClassInfo {
    std::string className;      // from [AverClass("BP_Thing")] / [AverGameMode(...)]
    std::string typeName;       // the C# class the attribute is on

    bool hasMesh = false;
    std::string meshPath, material;

    // Declared but not drawn as geometry -- see ActorPreview. They are surfaced so the panel can say
    // an actor HAS a camera or a light, because an actor that is only a light previews as nothing
    // and "nothing" and "broken" look identical.
    bool hasCamera = false;
    f32  cameraFovDeg = 0.0f, cameraNearCm = 0.0f, cameraFarCm = 0.0f;
    bool hasPointLight = false;
    f32  lightIntensityLux = 0.0f, lightRangeCm = 0.0f;

    bool anything() const { return hasMesh || hasCamera || hasPointLight; }
};

// Every actor a file declares, in the order they appear.
//
// A VECTOR, not one, and that is a correctness fix rather than a generalisation for its own sake: a
// real project puts several actors in one file. SkyForge's FpsGameMode.cs declares a GameMode, a
// PlayerController, a Block, a Crate and a GameInstance; its Gun.cs declares a GunPart and a Gun.
// Returning the first found made the editor name the wrong class -- it reported Gun.cs as 'BP_GunPart'
// -- and would have previewed the wrong mesh with nothing anywhere to say so.
//
// Each entry carries only what is declared between its own attribute and the next, so two actors in
// one file cannot borrow each other's mesh.
std::vector<ActorClassInfo> parseActorClasses(std::string_view csText);

// The first actor a file declares, or an empty one. A convenience for the common single-actor file.
ActorClassInfo parseActorClass(std::string_view csText);

// Read the generated region. Never modifies anything.
ActorScript parseActorScript(std::string_view csText);

// Rewrite the pos/rot/scale of the models in `edits`, matched BY OBJECT ID, leaving every other byte
// of the file alone. Models the file does not contain are ignored rather than appended: adding a
// placement is a different operation with different rules (it has to mint an id and declare a
// property), and doing it silently from a coordinate save would be surprising.
//
// Returns false with `err` set when the file cannot be parsed, in which case `out` is untouched. A
// rewriter that half-succeeds on a file it did not understand is worse than one that declines.
bool rewriteActorScript(std::string_view csText, const std::vector<ActorModel>& edits,
                        std::string& out, std::string* err = nullptr);

// The canonical form of a mesh path as the engine's registry keys it: content-relative, forward
// slashes, no `Content/` prefix.
//
// It exists because the two halves disagree, and the disagreement is silent. The engine registers
// meshes under `fnv1a64("Meshes/cube.ocmesh")` while the one sample in the tree writes
// `"Content/Meshes/CarBody.ocmesh"` into its Place call -- a different hash, so the placement
// resolves to an id nothing holds and the child draws NOTHING, with the renderer's own comment
// noting that an unresolved mesh id draws nothing rather than garbage. Normalising here means a
// designer file written either way still finds its mesh.
std::string canonicalMeshPath(std::string_view path);

} // namespace aver::fmt
