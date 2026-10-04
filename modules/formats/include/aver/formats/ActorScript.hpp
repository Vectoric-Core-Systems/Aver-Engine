#pragma once
// Reading and rewriting the C# an actor is written in: the model rows of a `.Designer.cs` generated
// region, and the values a hand-written actor class declares. Never writes outside what it read.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

// One `b.Place(...)` row: a mesh placed in an actor, and where its text lives.
struct ActorModel {
    u64 objectId = 0;       // the match key; also stamped onto the spawned child entity

    std::string property;   // the C# identifier the row assigns to
    std::string meshPath;   // as written in the source
    std::string material;

    f32 pos[3]   = {0.0f, 0.0f, 0.0f};   // centimetres
    f32 rot[3]   = {0.0f, 0.0f, 0.0f};   // degrees, (yaw, pitch, roll)
    f32 scale[3] = {1.0f, 1.0f, 1.0f};

    usize begin = 0, end = 0;   // byte range of the whole statement in the source
};

// How reading a generated region ended.
enum class ActorParseStatus {
    Ok,
    NoRegion,
    UnknownSchema,
    Malformed,      // in the region but outside the locked grammar; retry through Roslyn
};

// Which implementation produced a result.
enum class ActorParserBackend { Builtin, Roslyn };

// The result of reading a `.Designer.cs` generated region.
struct ActorScript {
    ActorParseStatus status = ActorParseStatus::NoRegion;
    ActorParserBackend backend = ActorParserBackend::Builtin;
    std::string error;                  // set for UnknownSchema / Malformed
    std::vector<ActorModel> models;
    usize regionBegin = 0, regionEnd = 0;   // the span strictly between the marker lines
};

// What an actor is, which decides whether a 3D view means anything for it.
enum class ActorKind {
    Unknown,
    Actor,
    Pawn,
    Character,
    PlayerController,
    GameMode,
    GameInstance,
};

// Name of an actor kind, for display.
const char* actorKindName(ActorKind k);
// Whether a 3D preview is meaningful. False for the kinds that have no transform.
bool actorKindHasViewport(ActorKind k);

// Where a value lives in the source, so an edit can go back to the bytes it came from.
struct ActorValueSpan {
    usize begin = 0, end = 0;
    bool valid() const { return end > begin; }
};

// One actor class as declared in hand-written C#, with a span for every editable value.
struct ActorClassInfo {
    std::string className;      // from [AverClass("AN_Thing")] / [AverGameMode(...)]
    std::string typeName;       // the C# class the attribute is on
    std::string baseType;       // the C# base it derives, verbatim
    ActorKind   kind = ActorKind::Unknown;

    f32 capsuleHeight = 0.0f, capsuleRadius = 0.0f, eyeHeight = 0.0f;   // centimetres; 0 = not stated
    ActorValueSpan capsuleHeightSpan{}, capsuleRadiusSpan{}, eyeHeightSpan{};

    bool hasMesh = false;
    std::string meshPath, material;
    ActorValueSpan meshPathSpan{}, materialSpan{};

    bool hasCamera = false;
    f32  cameraFovDeg = 0.0f, cameraNearCm = 0.0f, cameraFarCm = 0.0f;
    ActorValueSpan cameraSpan[3]{};
    bool hasPointLight = false;
    f32  lightIntensityLux = 0.0f, lightRangeCm = 0.0f;
    ActorValueSpan lightSpan[2]{};

    bool anything() const { return hasMesh || hasCamera || hasPointLight; }
    bool hasViewport() const { return actorKindHasViewport(kind); }
    // Something to draw: geometry, or a character's capsule.
    bool drawable() const { return hasMesh || kind == ActorKind::Character; }
};

// Every actor a file declares, in source order. Each entry carries only what is declared between its
// own attribute and the next.
std::vector<ActorClassInfo> parseActorClasses(std::string_view csText);

// The first actor a file declares, or an empty one.
ActorClassInfo parseActorClass(std::string_view csText);

// Writes an edited class's values back over the spans they were read from. Returns false with `err`
// set, and `out` untouched, when a span no longer matches what was read there.
bool rewriteActorClass(std::string_view csText, const ActorClassInfo& edited,
                       std::string& out, std::string* err = nullptr);

// The same, for every class in one file at once. NOT equivalent to calling rewriteActorClass in a
// loop and chaining the output: every span here is checked and applied against the SAME original
// `csText` in one pass, ordered so an earlier edit can never invalidate a later one's span. Chaining
// instead -- feeding class N's rewrite the output of class N-1's -- leaves class N's spans pointing
// at the offsets they had in the ORIGINAL text, which are wrong as soon as an earlier class's edit
// changes the text's length. Use this whenever more than one class in a file may have been edited.
bool rewriteActorClasses(std::string_view csText, const std::vector<ActorClassInfo>& edited,
                         std::string& out, std::string* err = nullptr);

// Reads the generated region. Never modifies anything.
ActorScript parseActorScript(std::string_view csText);

// Rewrites the pos/rot/scale of the models in `edits`, matched by object id. Models the file does not
// contain are ignored. Returns false with `err` set, and `out` untouched, on a parse failure.
bool rewriteActorScript(std::string_view csText, const std::vector<ActorModel>& edits,
                        std::string& out, std::string* err = nullptr);

// The canonical form of a mesh path as the registry keys it: content-relative, forward slashes, no
// `Content/` prefix.
std::string canonicalMeshPath(std::string_view path);

} // namespace aver::fmt
