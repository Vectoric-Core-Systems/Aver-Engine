#pragma once
// .ocinput -- named input actions and their default key/mouse bindings, replacing a hand-written
// Aver.Framework InputMappingContext subclass with data. Text, OC dialect, sized and shaped like
// .ocparticle: one dedicated record per concept scripting/csharp/Aver.Framework/EnhancedInput.cs
// already declares (InputAction, InputBinding, and the priority EnhancedInput.AddContext takes), no
// generic PARAM name=value bag -- the fields this format needs are exactly the fields that class
// already has, and nothing here is speculative beyond it.
//
// WHY THIS EXISTS. Today every mapping context -- which keys mean "Move", which mean "Fire" -- can
// only be a hand-written C# subclass of EnhancedInput.cs's own InputMappingContext (no such subclass
// exists anywhere in this tree yet; BindKey/BindAxis1D/BindAxis2D/BindMouseLook/BindMouseWheel are
// protected helpers waiting for one, EnhancedInput.cs:96-131): rebinding a key means recompiling and
// redeploying. A designer tuning defaults, or a future rebindable-controls menu, needs those bindings
// as DATA a project ships and an editor can list -- the same gap .ocmat closed for material
// parameters that used to be hardcoded shader constants.
//
// NO ENGINE DEPENDENCY, DELIBERATELY. Unlike .ocparticle (which parses straight into
// particles::ParticleEffect -- see modules/formats.particles/include/aver/formats/OcParticle.hpp's
// own comment on why), Aver.Framework is C#-only: there is no C++ InputAction/InputBinding type to
// parse into. So this format defines its own plain POD structs below, and a C# loader (not written
// as part of this change -- see the integration note on writeOcinput/OcInputData) is what turns them
// into real InputAction/InputMappingContext instances, the same division of labour
// scripting/csharp/Aver.Graph/OcGraphParser.cs already has with this module's C++ OcGraph reader for
// a different format. That keeps this header inside the BASE Aver.Formats target with zero new
// dependencies -- the same reasoning OcLand.hpp/OcBt.hpp/OcSound.hpp already state for their own
// "just a struct and a string table" formats (see modules/formats/CMakeLists.txt's own comments on
// each).
//
// KEY NAMES ARE OPAQUE STRINGS, NOT A VALIDATED ENUM -- the same choice OcGraph.hpp's
// OcGraphComponent::kind makes ("KIND IS AN OPAQUE STRING HERE, deliberately", OcGraph.hpp:111) and
// for the identical reason: the vocabulary (scripting/csharp/Aver.Framework/Input.cs's `Key` enum,
// whose own doc comment says "Matches the framework's AVER_FW_KEY_* enum", Input.cs:10-17) is owned
// by a different language on the other side of the C ABI, and hardcoding a second copy of its
// spelling list here would let the two silently disagree the day one of them adds a key the other
// does not know about. This layer only asks for a single non-empty token; a C# loader is what turns
// "W" into Key.W or reports one it does not recognise.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

// What shape of value an action carries. Mirrors Aver.Framework.InputValueType exactly (Digital,
// Axis1D, Axis2D -- EnhancedInput.cs:13-21) because this enum only exists to be written down and read
// back; the runtime meaning belongs entirely to the C# side that consumes a loaded scheme.
enum class OcInputValueType { Digital, Axis1D, Axis2D };

// Where one binding reads from. Mirrors Aver.Framework.InputSource (EnhancedInput.cs:24-34). Mouse
// BUTTONS are not a separate source here -- InputSource.Key's own doc comment says "A key or mouse
// button, held = 1", and Input.cs's Key enum carries MouseLeft/MouseRight/MouseMiddle right alongside
// the keyboard keys (Input.cs:16) -- so `BIND ... key MouseLeft` is how a mouse button is bound, the
// same as any keyboard key, and OcInputSource needs no separate value for it.
//
// GamepadButton/GamepadAxis were added beside the original four so a scheme can bind a pad the same
// way it binds a keyboard: `BIND ... gamepadbutton <name>` / `BIND ... gamepadaxis <name>`, where
// <name> is a member of Input.cs's own GamepadButton/GamepadAxis enum -- the identical "opaque
// string, validated by the C# loader, not here" treatment this header's own comment already states
// for a Key name, extended to the two enums Input.cs added beside Key for the gamepad ABI.
enum class OcInputSource { Key, MouseX, MouseY, MouseWheel, GamepadButton, GamepadAxis };

// One declared action: `ACTION <name> digital|axis1|axis2`. Mirrors InputAction::Name/ValueType
// (EnhancedInput.cs:37-79) and nothing else on that class -- Raw/Prev/IsHeld/WasPressed/WasReleased
// are per-frame RUNTIME state computed by EnhancedInput.Update, not something a binding scheme could
// state even in principle: a scheme says what an action IS, not what it currently reads.
//
// TWO ACTIONS SHARING A NAME IS NOT CHECKED HERE, on the same division of labour OcGraph.hpp draws
// for ENTRY's eventName ("checked by the C# runtime's Graph.Validate(), not by this reader",
// OcGraph.hpp:324): this layer accepts whatever is syntactically well-formed, and a loader turning
// this into real InputAction/EnhancedInput.Track calls is where a genuine collision would be caught
// (InputAction.Register calls EnhancedInput.Track, which already de-duplicates by reference identity,
// not by name -- so a name collision is exactly the kind of question the loader, not this format,
// has an opinion about).
struct OcInputAction {
    std::string name;
    OcInputValueType type = OcInputValueType::Digital;
};

// One binding, feeding exactly one declared action: `BIND <action> <source> [<key>] [scale <f32>]
// [component x|y|z]`. Mirrors the internal InputBinding struct (EnhancedInput.cs:82-93) field for
// field: `action` is Action (matched by name -- see parseOcinput's post-pass), and Source/Key/Scale/
// Component keep their exact meaning.
struct OcInputBinding {
    // Names the OcInputAction this feeds. A file with a BIND naming no declared ACTION is refused by
    // parseOcinput -- the same validation OcGraph.hpp's parseOcgraph doc comment states for a LINK to
    // a non-existent node ("e.g., invalid header, link to non-existent node"): a binding that cannot
    // be wired to anything is exactly as useless as a wire to nowhere.
    std::string action;
    OcInputSource source = OcInputSource::Key;
    // The Key/GamepadButton/GamepadAxis enum spelling (e.g. "W", "MouseLeft", "LeftShoulder",
    // "LeftTrigger") -- see the header comment above for why this is an unvalidated opaque token
    // rather than a checked enum. Meaningful, and required non-empty, for Key, GamepadButton and
    // GamepadAxis; empty and ignored for the three mouse-axis sources, matching EnhancedInput.cs's
    // own BindMouseLook/BindMouseWheel (EnhancedInput.cs:122-130), which pass a filler Key.A that
    // EnhancedInput.Update never reads because it branches on `source`, not `key`
    // (EnhancedInput.cs:189-196).
    std::string key;
    // BindKey's own default (EnhancedInput.cs:104). Negative values invert an axis -- see
    // BindAxis1D pairing +1/-1 on two keys sharing one action (EnhancedInput.cs:108-112), which is
    // exactly what the worked example in OcInput.cpp's header comment does for a Move action's
    // forward/back keys.
    f32 scale = 1.0f;
    // 0=X 1=Y 2=Z, matching EnhancedInput.Accumulate exactly (EnhancedInput.cs:208-216) -- including
    // the Z case, which BindKey's own parameter comment does not name ("0=X, 1=Y", EnhancedInput.cs:
    // 103) but Accumulate's `default:` branch still honours. This format allows every value BindKey's
    // callers have always been able to pass, not only the two its doc comment happens to mention.
    i32 component = 0;
};

// A complete binding scheme. One file replaces one hand-written InputMappingContext subclass.
struct OcInputData {
    int version = 1;
    // NAME, optional. A human authoring label ONLY -- unlike `contextName` below, nothing reads this
    // to register or look up the scheme at runtime. Matches OcParticle's own OcParticleExtras::name
    // and OcProject's NAME: free text, purely for a human or an editor panel.
    std::string name;

    std::vector<OcInputAction> actions;
    std::vector<OcInputBinding> bindings;

    // `CONTEXT <name> priority <i32>`, optional; an empty `contextName` means the record was absent --
    // the same empty-string-is-the-sentinel idiom OcProject.hpp's DRONE.GRAPH field documents at
    // length (OcProject.hpp:30-43). `name` above is this scheme's human label; `contextName` is the
    // separate identity EnhancedInput.AddContext's caller needs for InputMappingContext.Name
    // (EnhancedInput.cs:101), and `contextPriority` is the priority argument AddContext itself takes
    // (EnhancedInput.cs:150) -- moving a value that today lives at the call site into the data it
    // describes, the same gap this whole format closes for key bindings themselves.
    std::string contextName;
    i32 contextPriority = 0;
};

// Parses a scheme from memory. Returns false and sets *err on a missing/wrong-version header, a
// record with the wrong field count, an unrecognised enum word (ACTION type, BIND source, BIND
// component, or a malformed CONTEXT), a malformed number, or a BIND naming an ACTION this file never
// declares -- never a default/partial scheme that looks like it loaded. Matches
// modules/formats.particles/include/aver/formats/OcParticle.hpp's strictness contract exactly (that
// file's own header, "MALFORMED INPUT IS STRICTER..."): this is a new format with no hand-edited
// legacy history to be tolerant of, and a rebinding UI or a designer's typo deserves a named error,
// not a silently-empty control scheme. Record KINDS this format does not recognise at all are
// forward-compat: skipped during parse, preserved verbatim by writeOcinput.
bool parseOcinput(std::string_view text, OcInputData& out, std::string* err = nullptr);

// Loads a scheme from disk. False with *err set when the file is missing, unreadable, or malformed --
// see parseOcinput.
bool loadOcinput(const std::string& path, OcInputData& out, std::string* err = nullptr);

// Serialises a scheme to text. Pass the file's own previous text as `existing` to merge: ACTION and
// BIND lines are each regenerated as ONE whole block (in `out.actions`/`out.bindings` order) and
// placed at the position of the first original line of their own kind, matching
// modules/formats.particles/src/OcParticle.cpp's writeOcparticle strategy generalised from a
// single-line kind to a repeating one; NAME/CONTEXT are replaced in place the way OcProject's owned
// keys are (OcProject.cpp's isOwnedKey/write loop); everything else -- comments, blank lines, any
// record this format does not model -- is copied through untouched, at its original position. Pass
// an empty string_view (the default) to produce a fresh file. Round-trips through parseOcinput.
//
// INTEGRATION NOTE: OcInputData now DOES reach a real Aver.Framework.InputMappingContext --
// modules/framework/include/aver/framework/framework_abi.h's own INPUT SCHEME section
// (aver_fw_input_scheme_load and its count/index getters) exposes exactly this parser
// (aver::fmt::parseOcinput, via loadOcinput) across the C ABI, one file-static parsed scheme at a
// time, and scripting/csharp/Aver.Framework/InputScheme.cs is the C# loader that walks it into a
// live InputMappingContext (InputAction.Digital/Axis1D/Axis2D, then bindings by key name) and pushes
// it onto EnhancedInput. This header and OcInput.cpp still only make the data representable and
// round-trippable -- the ABI section is what makes it LIVE.
std::string writeOcinput(const OcInputData& d, std::string_view existing = "");

} // namespace aver::fmt
