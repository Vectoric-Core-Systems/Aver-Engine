#pragma once
// THE console command registry and live-variable registry for the editor's Console drawer tab.
// Mirrors GraphNodeDefs.hpp's own shape (plain data table + a push_back-per-line builder), because
// that file is the precedent this one is asked to follow: adding a command or a variable is one
// call in one of the builder functions below, not a hunt through SandboxApp.cpp's drawing code.
//
// Header-only, like GraphNodeDefs.hpp, and for the same reason plus one more: SandboxApp.cpp is the
// only translation unit that includes this, and the build task this file was written for is not
// permitted to touch CMakeLists.txt, so there is nowhere to register a second .cpp even if one were
// wanted. Every function below is `inline`.
//
// TWO REGISTRIES, ONE FILE, DELIBERATELY NOT SPLIT: the command table (`help`, `frametime`, `get`,
// `set`, `vars`) and the variable table (`voxi.*`, `post.*`, `rhi.*`) are two different shapes of
// data, but `get`/`set`/`vars` are themselves commands that exist only to walk the variable table, so
// the two are one feature wearing two tables, not two features that happen to share a file.
//
// SCOPE: this table only reads/writes aver::voxi::Renderer's process-wide settings, one
// rhi::IDevice's post-process settings, and (rhi.* -- one entry so far, depthPrepass) a handful of
// other already-public rhi::IDevice toggles that live OUTSIDE PostSettings -- all already-public
// surfaces (Voxi.hpp, RHI.hpp). Nothing here adds a member to SandboxApp beyond what SandboxApp.cpp
// declares for the console panel itself: settings this file cannot reach because they are PRIVATE
// SandboxApp state with no accessor threaded through voxi::Renderer or rhi::IDevice (occlusion
// culling, virtualized-geometry LOD selection -- see registerVoxiVars' own comment on why those two
// are not here) stay out of this table until such an accessor exists, rather than growing a second
// way for this file to reach into SandboxApp.

#include "aver/core/Types.hpp"
#include "aver/core/Log.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/GpuTimingFormat.hpp"
#include "aver/runtime/Engine.hpp"
#if AVER_MODULE_VOXI
#include "aver/voxi/Voxi.hpp"
// Scalability.hpp pulls in QualityLadder.hpp and RenderSettingsResolver.hpp along the way -- one
// include gives this file voxi::resolve() (the effective-value reads below), voxi::ladder::* (the RT
// tier var's derived-knob help text) and voxi::applyOverall/overallFromSettings (voxi.scalability).
#include "aver/voxi/Scalability.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aver { class SandboxApp; }

namespace aver::editor {

namespace detail {
// Case-insensitive equality, identical in spirit to GraphNodeDefs.hpp's own ciEquals -- both this
// table's command names and its variable names are looked up without regard to case, and neither
// file wants to depend on the other for four lines.
inline bool ciEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}
// Case-insensitive SUBSTRING test, the other half of ciEquals: completion and the variable browser
// both need "does this name or description CONTAIN what was typed", not just "is it equal to it" --
// see this file's own note on the console rework (someone who remembers a word from a description,
// not a name, should still find the entry). O(n*m); every caller is a UI list of a few dozen entries
// re-filtered a keystroke at a time, not a hot loop.
inline bool ciContains(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}
} // namespace detail

// =================================================================================================
// PART B: the live-variable registry (voxi.*, post.*, rhi.*)
// =================================================================================================

// A tagged union covering every scalar type a Settings/PostSettings field actually is. `Str` is a
// fifth case beyond the four the design called for -- added because voxi.status.* and voxi.device.*
// (read-only diagnostic fields such as Renderer::statusText, which is prose, not a number) are part
// of "what to expose" in the design and cannot be represented by U32/F32/Bool/Quality at all. It is
// a plain second field, not folded into the union, because std::string is non-trivial and giving the
// union a hand-written destructor discriminated on `type` is not worth it for the handful of entries
// that ever use it.
enum class VarType : u8 { U32, F32, Bool, Quality, Str };

struct VarValue {
    VarType type = VarType::U32;
    union { u32 u = 0; f32 f; bool b; } as;
    std::string str;   // meaningful only when type == VarType::Str
};

inline VarValue vU32(u32 v)      { VarValue r; r.type = VarType::U32;     r.as.u = v; return r; }
inline VarValue vF32(f32 v)      { VarValue r; r.type = VarType::F32;     r.as.f = v; return r; }
inline VarValue vBool(bool v)    { VarValue r; r.type = VarType::Bool;    r.as.b = v; return r; }
inline VarValue vQualityRaw(u32 v){ VarValue r; r.type = VarType::Quality; r.as.u = v; return r; }
inline VarValue vStr(std::string s) { VarValue r; r.type = VarType::Str; r.str = std::move(s); return r; }

inline bool valuesEqual(const VarValue& a, const VarValue& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case VarType::U32:
        case VarType::Quality: return a.as.u == b.as.u;
        case VarType::F32:     return a.as.f == b.as.f;
        case VarType::Bool:    return a.as.b == b.as.b;
        case VarType::Str:     return a.str == b.str;
    }
    return true;
}

#if AVER_MODULE_VOXI
inline std::string qualityDisplayName(u32 raw) { return voxi::Renderer::qualityName(static_cast<voxi::Quality>(raw)); }
#else
inline std::string qualityDisplayName(u32 raw) { return std::to_string(raw); }
#endif

// Formats a VarValue for `get`/`set`/`vars` output. Reuses Renderer::qualityName rather than
// reinventing a name table, exactly as the design calls for.
inline std::string formatValue(const VarValue& v) {
    switch (v.type) {
        case VarType::U32:  return std::to_string(v.as.u);
        case VarType::F32: { char buf[32]; std::snprintf(buf, sizeof buf, "%g", v.as.f); return buf; }
        case VarType::Bool: return v.as.b ? "true" : "false";
        case VarType::Quality: return qualityDisplayName(v.as.u);
        case VarType::Str:  return v.str;
    }
    return "?";
}

// ---- string -> VarValue parsing, per type ------------------------------------------------------

// U32: parsed as SIGNED first and a negative literal is reported as itself, never cast-and-wrapped
// into a huge unsigned number. This is the exact trap SandboxApp.cpp's --rt-rays handling already
// documents ("the warning... read '4294967291 shadow rays clamped to 32', which describes the cast
// and not the typo that caused it") -- the console must not reintroduce it for `set`.
inline bool parseU32(std::string_view tok, u32& out, std::string& err) {
    const std::string s(tok);
    char* end = nullptr;
    errno = 0;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (s.empty() || end != s.c_str() + s.size()) { err = "'" + s + "' is not a whole number"; return false; }
    if (errno == ERANGE) { err = "'" + s + "' is too large"; return false; }
    if (v < 0) { err = s + " is negative; a count cannot be negative"; return false; }
    out = static_cast<u32>(v);
    return true;
}
inline bool parseF32(std::string_view tok, f32& out, std::string& err) {
    const std::string s(tok);
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    if (s.empty() || end != s.c_str() + s.size()) { err = "'" + s + "' is not a number"; return false; }
    out = v;
    return true;
}
inline bool parseBool(std::string_view tok, bool& out, std::string& err) {
    std::string s(tok);
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "true" || s == "1" || s == "on")  { out = true;  return true; }
    if (s == "false" || s == "0" || s == "off") { out = false; return true; }
    err = "'" + std::string(tok) + "' is not a bool (true/false, 1/0, on/off)";
    return false;
}
// Accepts either spelling: a case-insensitive name (off/low/medium/high/epic) or a bare 0-4, mirroring
// how --gi already accepts a raw int and a project manifest stores giQuality as one.
inline bool parseQuality(std::string_view tok, u32& out, std::string& err) {
    std::string s(tok);
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static constexpr std::pair<const char*, u32> kNames[] = {
        {"off", 0}, {"low", 1}, {"medium", 2}, {"high", 3}, {"epic", 4},
    };
    for (const auto& [name, q] : kNames) if (s == name) { out = q; return true; }
    u32 n = 0; std::string ignored;
    if (parseU32(tok, n, ignored) && n <= 4) { out = n; return true; }
    err = "'" + std::string(tok) + "' is not a quality (off/low/medium/high/epic, or 0-4)";
    return false;
}

// ---- the batch a `set` line stages into, and commits at most twice ------------------------------
//
// WHY THIS IS A LIST OF SETTER CLOSURES, NOT TWO PRE-SEEDED voxi::Settings STRUCTS (the shape the
// design sketched). Seeding a "dial phase" Settings copy when the batch is BUILT and only calling
// setSettings on it LATER, after a tier-phase commit may already have run, would hand that second
// call a copy whose tier fields are the OLD tier -- and setSettings decides "did the tier change"
// by comparing the incoming struct's tier fields against LIVE settings_ at the time of the call. A
// stale tier field in the dial-phase copy would read as a second tier change and re-trigger
// derivation, silently overwriting the very dial values this call exists to protect. Closures let
// commitBatch build EACH phase's snapshot from Renderer::get().settings() at the moment that phase
// actually commits -- tier-phase from whatever was live before this line ran, dial-phase from
// whatever the tier-phase call (if any) just derived -- which is what section 2 of the design
// actually specifies in its numbered steps, just not in the struct sketch above them.
struct ConsoleBatch {
    std::vector<std::function<void(void*)>> tierSetters;   // void* is a voxi::Settings*; see below
    std::vector<std::function<void(void*)>> dialSetters;
    rhi::PostSettings post;
    bool seededPost = false;
    bool touchedPost = false;
    // rhi.* vars that are a single immediate IDevice call rather than a field in a struct like `post`
    // above -- see registerRhiVars' own comment for why there is no seed-then-commit struct for these.
    // Run at commit, after Voxi and Post, so a line mixing namespaces still applies all-or-nothing.
    std::vector<std::function<void(rhi::IDevice&)>> deviceSetters;
    rhi::IDevice* device = nullptr;
    // (dotted name, requested value) pairs, filled by the `set` handler right after each stage()
    // call -- used only for the post-commit diff report, so every field's "requested" line is
    // driven by one piece of code regardless of which source (Voxi, Post) it came from.
    std::vector<std::pair<std::string, VarValue>> requested;
};

struct SetOutcome { std::vector<std::string> notes; };   // "<name>: requested X, now Y" per field that moved

// One entry in the variable table.
struct ConsoleVar {
    std::string name;                 // dotted: "voxi.giCones", "post.bloomIntensity"
    VarType type;
    bool readOnly = false;
    std::string help;
    std::function<VarValue()> read;                                     // live value, for get/list
    std::function<void(ConsoleBatch&, VarValue)> stage;                  // null for read-only vars
    std::function<bool(const VarValue&, std::string&)> validate;        // optional extra legality check
};

inline const std::vector<ConsoleVar>& allVars();
inline const ConsoleVar* findVar(std::string_view dotted) {
    for (const ConsoleVar& v : allVars()) if (detail::ciEquals(v.name, dotted)) return &v;
    return nullptr;
}

// =================================================================================================
// DISCOVERABILITY: tooltips, grouping, and substring search over the table above. Added for the
// console rework ("make a tool someone can learn FROM rather than one they must already know") --
// every function here reads ConsoleVar's existing fields, it adds none, because the design brief for
// this table already produced the raw material (a rich one-line `help` per entry, a `readOnly` flag,
// a live `read()`) and the gap was never the data, only that nothing besides `get`/`vars` ever showed
// it. Kept in Part B, beside the table these read, rather than in SandboxApp.cpp: the same reason the
// table itself lives here and not there (see this file's own header comment).
// =================================================================================================

inline const char* varTypeName(VarType t) {
    switch (t) {
        case VarType::U32:     return "integer";
        case VarType::F32:     return "float";
        case VarType::Bool:    return "bool";
        case VarType::Quality: return "quality tier";
        case VarType::Str:     return "text (read-only)";
    }
    return "?";
}

// Three honesty states a variable's VALUE can be in, named for the ones the table's own comments
// already call out at length: voxi.giMode and voxi.denoiser read back what is ACTUALLY running
// (LiveTruth), voxi.layeredBsdf is NOT live until a reload (NotLive) -- rather than a generic "has
// caveats" flag every entry would need to opt into.
// Detected from the SAME help text a tooltip already shows in full, by the words those two entries'
// help strings already use ("ACTUALLY running" on voxi.giMode, "skips itself" on voxi.denoiser, "NOT
// live" on voxi.layeredBsdf) -- a second field on ConsoleVar that only three entries would ever set is
// more machinery than re-reading the sentence that already has to exist for the tooltip anyway.
// "silently refus[ed/es]" is included pre-emptively (this table's own comments already reach for that
// exact phrase in prose) so a future entry worded the same way is caught without touching this again.
enum class Honesty : u8 { Normal, LiveTruth, NotLive };
inline Honesty varHonesty(const ConsoleVar& v) {
    if (v.help.find("NOT live") != std::string::npos) return Honesty::NotLive;
    if (v.help.find("ACTUALLY running") != std::string::npos ||
        v.help.find("skips itself") != std::string::npos ||
        v.help.find("silently refus") != std::string::npos)
        return Honesty::LiveTruth;
    return Honesty::Normal;
}
// Short tag for a value shown next to a var: transcript lines, the browser, and `vars <filter>` all
// use the identical two words so the meaning does not drift between surfaces.
inline const char* varHonestyTag(Honesty h) {
    switch (h) {
        case Honesty::LiveTruth: return "[live]";
        case Honesty::NotLive:   return "[reload]";
        default:                 return "";
    }
}

// The valid range or enum a name's OWN description or validate() already documents, not a second
// source of truth someone has to remember to update alongside the help string. Bool/Quality have a
// fixed vocabulary (parseBool/parseQuality already accept exactly this); everything else either has a
// `validate` closure whose error message already spells the legal set out in words (voxi.msaa,
// voxi.refractionMode -- calling it with a value no real field would accept is free, validate stages
// nothing), or a help string ending in a parenthetical containing the word "clamp" (registerVoxiVars
// and registerPostVars put one on nearly every dial: "(engine clamps to [1,16])", "(no engine
// clamp)", "(clamped >= 0)"). Returns empty when none of those apply -- voxi.giMode and voxi.denoiser
// are the two that matters most: varHonesty above already flags both with a fuller explanation of why
// "valid range" is not even the right question for them (any u32/bool is accepted; what the engine
// then does with it is the part worth reading).
inline std::string varRangeHint(const ConsoleVar& v) {
    switch (v.type) {
        case VarType::Bool:    return "true/false, 1/0, on/off";
        case VarType::Quality: return "off, low, medium, high, epic (or 0-4)";
        case VarType::Str:     return "";
        default: break;
    }
    if (v.validate) {
        const VarValue probe = v.type == VarType::F32 ? vF32(-1e30f) : vU32(0xFFFFFFFFu);
        std::string err;
        if (!v.validate(probe, err) && !err.empty()) return err;
    }
    std::string lower = v.help;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::size_t word = lower.find("clamp");
    if (word == std::string::npos) return "";
    const std::size_t open = v.help.rfind('(', word);
    const std::size_t close = v.help.find(')', word);
    if (open == std::string::npos || close == std::string::npos) return "";
    return v.help.substr(open + 1, close - open - 1);
}

// Group label for the browsable listing: the first TWO dotted segments when there are three or more
// ("voxi.status.*" and "voxi.device.*" are each a coherent little table of their own -- see
// registerVoxiVars' own comment -- so they get their own heading instead of drowning in "voxi.*"'s
// twenty-odd dials), otherwise just the first segment. Generic on purpose: a future voxi.foo.bar
// namespace needs no change here, unlike a hand-maintained list of group names would.
inline std::string varGroupKey(const std::string& dotted) {
    const std::size_t first = dotted.find('.');
    if (first == std::string::npos) return dotted;
    const std::size_t second = dotted.find('.', first + 1);
    return second == std::string::npos ? dotted.substr(0, first) : dotted.substr(0, second);
}
inline std::string varGroupLabel(const std::string& groupKey) { return groupKey + ".*"; }

// Does this variable answer a search for `query`? Matches the NAME or the DESCRIPTION, substring not
// prefix -- see WHAT TO BUILD item 2 in the task this exists for: someone who remembers "firefly" but
// not the exact dotted name of whatever dial it lives on should still find it if the word is in the
// help text. An empty query matches everything (the unfiltered `vars`/browser state).
inline bool varMatchesQuery(const ConsoleVar& v, std::string_view query) {
    return detail::ciContains(v.name, query) || detail::ciContains(v.help, query);
}

// The one tooltip body every surface shares -- the completion popup, the browser, and the transcript
// hover all call this SAME function, so the three cannot drift into showing different information for
// the same name. Multi-line: what it is, its live value right now, its valid range if known, and the
// live-truth/reload note if it is one of the entries this table's own comments single out for it.
inline std::string varTooltipText(const ConsoleVar& v) {
    std::string s = v.name;
    s += "  (";
    s += varTypeName(v.type);
    if (v.readOnly) s += ", read-only";
    s += ")\n\n";
    s += v.help.empty() ? std::string("(no description)") : v.help;
    s += "\n\nCurrent value: ";
    s += formatValue(v.read());
    const std::string range = varRangeHint(v);
    if (!range.empty()) { s += "\nValid: "; s += range; }
    const Honesty h = varHonesty(v);
    if (h == Honesty::LiveTruth)
        s += "\n\n[live] This reads back what the engine is ACTUALLY doing right now, which can differ "
             "from the last value you asked for -- see the description above for why.";
    else if (h == Honesty::NotLive)
        s += "\n\n[reload] Not live: this needs a project reload before a change here has any effect.";
    return s;
}

#if AVER_MODULE_VOXI
// The ReSTIR-GI poison debug view's live source of truth -- SAME SHAPE as
// consoleOcclusionForceWaitIdleSlot() below (registerOcclusionVars' own comment has the full
// reasoning): VoxiRenderer::setGiPoisonView is private renderer state with no path through
// voxi::Renderer::Settings/setSettings, so this table cannot stage it as an ordinary dial the way
// voxi.denoiser etc. are staged. A raw bool slot this header owns, written by `set voxi.giPoisonView`
// and reasserted onto the live voxiRenderer_ once a frame from SandboxApp.cpp's onUpdate (right
// beside voxiRenderer_.setDebugView(giDebugView_)), is the same "console sets the seed, a per-frame
// reassert makes it live" idiom occlusion.debugForceWaitIdle already uses.
inline bool& consoleGiPoisonViewSlot() { static bool v = false; return v; }

// voxi.debugResetHistoryEveryFrame's live source of truth -- the same raw-slot idiom as
// consoleGiPoisonViewSlot() directly above. A BISECTION AID for a temporal artifact, never saved:
// SandboxApp.cpp's onUpdate resets every history named here once a frame, quietly, beside the
// one-shot reset* commands. Anything that still lags, smears or fades with a history reset every
// frame is not carried by that history. Bit 1 = ReSTIR GI reservoirs + GI visibility history,
// 2 = RT shadow/reflection/sky-occlusion history (this also restarts NRD, which beginShadowHistory
// resets whenever rtHistValid_ is false), 4 = NRD alone.
inline u32& consoleResetHistoryEveryFrameSlot() { static u32 v = 0; return v; }

// The NRD legacy-camera A/B switch's live source of truth -- the SAME raw-slot idiom as
// consoleGiPoisonViewSlot() directly above and for the identical reason: VoxiRenderer::
// setNrdLegacyCamera is private renderer state (a toggle on an internal camera-factorisation path,
// VoxiRenderer::beginShadowHistory) with no path through voxi::Renderer::Settings/setSettings, so
// this table cannot stage it as an ordinary dial. Written by `set voxi.nrdLegacyCamera` and
// reasserted onto the live voxiRenderer_ once a frame from SandboxApp.cpp's onUpdate, right beside
// the voxiRenderer_.setGiPoisonView(consoleGiPoisonViewSlot()) call this mirrors.
inline bool& consoleNrdLegacyCameraSlot() { static bool v = false; return v; }

// The lighting-contrast fix's legacy bitmask (contrast-fix plan, F7) -- the SAME raw-slot idiom as
// consoleGiPoisonViewSlot()/consoleNrdLegacyCameraSlot() directly above and for the identical reason:
// VoxiRenderer::setLightingLegacyBits is private renderer state with no path through
// voxi::Renderer::Settings/setSettings. ONE u32 here backs FIVE console variables below
// (voxi.legacyRestirSampleRing etc.), each one flipping a single bit rather than the whole word, so
// the user can A/B one root cause at a time. Seeded whole from --lighting-legacy for a --frames
// capture, which has no console to flip five bits by hand; reasserted onto the live voxiRenderer_
// once a frame from SandboxApp.cpp's onUpdate, right beside the
// voxiRenderer_.setNrdLegacyCamera(consoleNrdLegacyCameraSlot()) call this mirrors. See
// VoxiRenderer::setLightingLegacyBits' own header comment for the bit table.
inline u32& consoleLightingLegacySlot() { static u32 v = 0; return v; }

// The three engine-optimisation-plan measurement dials (M1-M4/W3/W12) -- SAME SHAPE as
// consoleGiPoisonViewSlot()/consoleNrdLegacyCameraSlot()/consoleLightingLegacySlot() directly above,
// and for the identical reason: VoxiRenderer::setGiForceRebuild/setGiBoundedDispatch/
// setGiFreeAccumulator are public methods on the renderer (unlike giPoisonView, which is private), but
// none of the three round-trips through voxi::Renderer::Settings/setSettings -- so this table still
// cannot stage them as ordinary dials, only as raw slots reasserted once a frame. Written by their
// matching `set voxi.gi*` command and reasserted onto the live voxiRenderer_ from SandboxApp.cpp's
// onUpdate, right beside the voxi.giPoisonView/nrdLegacyCamera reasserts these three mirror -- see
// each setter's own header comment (VoxiRenderer.hpp) for what changing it actually does.
inline bool& consoleGiForceRebuildSlot()    { static bool v = false; return v; }
// TRUE, because it was measured rather than left as a dial nobody turned. SandboxApp reasserts this
// slot into VoxiRenderer every frame, so THIS is the editor's real default and the member
// initialiser in VoxiRenderer.hpp (also true now, for hosts like the Runtime that never call the
// setter) cannot be it. With it off, every GI rebuild cleared, resolved and mip-filtered the whole
// 512^3 grid to touch 1.3% of it. See VoxiRenderer.hpp's own comment beside giBoundedDispatch_ for
// the census line, the 30.64 -> 30.25ms measurement and the pixel-neutrality check.
inline bool& consoleGiBoundedDispatchSlot() { static bool v = true; return v; }
inline bool& consoleGiFreeAccumulatorSlot() { static bool v = false; return v; }

// optimisation-wave-2's U1 path-debug view (2.10 I) -- the SAME raw-slot idiom as
// consoleGiPoisonViewSlot() above and for the identical reason: VoxiRenderer::setGiVisPathView is
// private renderer state with no path through voxi::Renderer::Settings/setSettings. Paints F2's
// resolved path (traced/reconstructed/half-res/no-ray) directly over indirect diffuse, so a by-hand
// verification run can see whether U1's Half mode is actually reconstructing or quietly falling back
// to Full everywhere (2.10 I's own comment on the poison view outranking this one when both are on).
// Written by `set voxi.giVisPathView` or seeded from --gi-vis-path-view for a --frames capture with
// no console, and reasserted onto the live voxiRenderer_ once a frame from SandboxApp.cpp's onUpdate,
// right beside the voxiRenderer_.setGiPoisonView(consoleGiPoisonViewSlot()) call this mirrors. Default
// OFF: painting replaces indirect diffuse, so it must never happen just from opening the Console tab.
inline bool& consoleGiVisPathViewSlot() { static bool v = false; return v; }

// W6/M5's pricing switch (section 4(a) of the optimisation-wave-2 plan) -- SAME raw-slot idiom as
// consoleGiVisPathViewSlot() directly above: VoxiRenderer::setBlendedGiCone is a public method (like
// setGiForceRebuild etc.) but, like every renderer setting in this file's raw-slot family, does not
// round-trip through voxi::Renderer::Settings/setSettings. false (the default) keeps a blended
// (alpha-blend or transmissive) fragment shading from ReSTIR, the fixed W6 behaviour and today's
// image; true forces the pre-existing cone-gather fallback on every blended fragment instead, so the
// "blended replay" span this wave's own M5 measurement reads (D3D12Device.cpp:5073) can be priced with
// and without ReSTIR's share of it. Written by `set voxi.blendedGiCone` or seeded from --blended-gi
// restir|cone, and reasserted onto the live voxiRenderer_ once a frame from SandboxApp.cpp's onUpdate,
// right beside the voxiRenderer_.setGiVisPathView(consoleGiVisPathViewSlot()) call this mirrors.
inline bool& consoleBlendedGiConeSlot() { static bool v = false; return v; }

// Tier fields (msaa, globalIllumination, rayTracing, pathTracing, meshShaders) go through
// tierSetters; everything setSettings derives FROM a tier goes through dialSetters. See
// ConsoleBatch's own comment for why the split is closures rather than two struct copies.
//
// TWO RENDER SETTINGS THAT LOOK LIKE THEY BELONG HERE AND ARE DELIBERATELY ABSENT: occlusion culling
// (SandboxApp::occlusionCullEnabled_) and virtualized-geometry LOD selection
// (SandboxApp::lodSelectEnabled_, SandboxApp::setLodSelect). Both are toggled live already -- the
// editor's own Rendering-settings panel flips them every frame -- but as PRIVATE SandboxApp state
// with no accessor on voxi::Renderer or rhi::IDevice, which is the only surface this table is allowed
// to reach (see this file's own SCOPE comment). Reaching them would mean either making them public on
// SandboxApp or adding a per-frame consoleApp() slot mirroring setConsoleDevice() below -- both real
// options, but both a change to SandboxApp.cpp, which is out of scope for this file. Left out rather
// than routed around, so the gap is visible instead of quietly worked around with a parallel path.
inline void registerVoxiVars(std::vector<ConsoleVar>& t) {
    using voxi::Renderer;
    using voxi::Settings;
    using voxi::Quality;
    using voxi::Msaa;

    // ---- tier fields -----------------------------------------------------------------------
    t.push_back({"voxi.msaa", VarType::U32, false,
        "MSAA sample count (1/2/4/8); the device's supported mask still clamps the request",
        []{ return vU32(static_cast<u32>(Renderer::get().settings().msaa)); },
        [](ConsoleBatch& b, VarValue v){
            const u32 samples = v.as.u;
            b.tierSetters.push_back([samples](void* sp){ static_cast<Settings*>(sp)->msaa = static_cast<Msaa>(samples); });
        },
        [](const VarValue& v, std::string& err) -> bool {
            const u32 s = v.as.u;
            if (s != 1 && s != 2 && s != 4 && s != 8) {
                err = "MSAA must be 1, 2, 4, or 8 (device support is checked on commit, not here)";
                return false;
            }
            return true;
        }});
    t.push_back({"voxi.globalIllumination", VarType::Quality, false,
        "GI quality tier; changing it derives voxelResolution/giCones/giUpdateInterval unless they are set in the same line",
        []{ return vQualityRaw(static_cast<u32>(Renderer::get().settings().globalIllumination)); },
        [](ConsoleBatch& b, VarValue v){ const u32 q=v.as.u; b.tierSetters.push_back([q](void* sp){ static_cast<Settings*>(sp)->globalIllumination = static_cast<Quality>(q); }); }});
    t.push_back({"voxi.rayTracing", VarType::Quality, false,
        // THE RETUNED LADDER (QualityLadder.hpp): rtShadowRays 1/1/4/8, rtShadowDenoise 2/2/1/1,
        // rtRenderMode 0/1/1/1 at Low/Medium/High/Epic (rtPixelsPerRayTile stays flat at 1 everywhere).
        // Low rasterises primary visibility by explicit product decision (D3) -- 0 is not a hardware
        // gap the way Off's 0 is.
        "Ray-traced sun shadow quality tier; changing it derives rtShadowRays (1/1/4/8), "
        "rtPixelsPerRayTile (1 flat), rtShadowDenoise (2/2/1/1) and rtRenderMode (0/1/1/1, Low "
        "rasterises by product decision) at Low/Medium/High/Epic unless set in the same line",
        []{ return vQualityRaw(static_cast<u32>(Renderer::get().settings().rayTracing)); },
        [](ConsoleBatch& b, VarValue v){ const u32 q=v.as.u; b.tierSetters.push_back([q](void* sp){ static_cast<Settings*>(sp)->rayTracing = static_cast<Quality>(q); }); }});
    t.push_back({"voxi.pathTracing", VarType::Quality, false,
        "Path tracing tier; changing it derives ptBounces unless set in the same line",
        []{ return vQualityRaw(static_cast<u32>(Renderer::get().settings().pathTracing)); },
        [](ConsoleBatch& b, VarValue v){ const u32 q=v.as.u; b.tierSetters.push_back([q](void* sp){ static_cast<Settings*>(sp)->pathTracing = static_cast<Quality>(q); }); }});
    t.push_back({"voxi.meshShaders", VarType::Bool, false,
        "Mesh-shader submission path on/off",
        []{ return vBool(Renderer::get().settings().meshShaders); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.tierSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->meshShaders = on; }); }});
    // THE UE-STYLE OVERALL QUALITY PRESET (D2, Scalability.hpp), reachable from the console the same
    // way the Rendering settings page's own Overall row is. STAGED AS A TIER SETTER, not a dial one,
    // even though applyOverall ALSO writes every one of the three groups' derived knobs directly (not
    // relying on setSettings' own change-gated derivation at all) -- it belongs with the tier setters
    // because it must run in the FIRST commitBatch phase: `set voxi.scalability 4 voxi.rtShadowRays 2`
    // needs the Overall preset applied and committed BEFORE the dial phase re-reads live settings and
    // stages the explicit rtShadowRays 2 on top of them, or the two would race the same way a UI edit
    // mixing tier and knob flags in one setSettings call used to (see SandboxApp.cpp's take(), N7).
    // commitBatch already re-reads voxi::Renderer::get().settings() FRESH right before the dial phase
    // (this file's own commitBatch comment) -- that existing re-read is what makes the example above
    // end at rtShadowRays 2, not the ladder's 8, with no change needed here.
    t.push_back({"voxi.scalability", VarType::U32, false,
        "UE-style Overall Quality preset: 1 (Low) .. 4 (Epic) moves Global Illumination, Ray Tracing "
        "and Path Tracing to the same rung at once (Path Tracing always goes to Off -- a locked "
        "decision, see Scalability.hpp), writing every derived knob to the ladder's value for that "
        "rung. An unavailable group (no RT hardware, say) is left completely untouched. 0 (Custom) "
        "reads back when the settings do not agree with any single rung -- it is derived, not settable",
        []{
            const Renderer& r = Renderer::get();
            return vU32(static_cast<u32>(voxi::overallFromSettings(r.settings(), r.deviceInfo())));
        },
        [](ConsoleBatch& b, VarValue v){
            const u32 q = v.as.u;
            b.tierSetters.push_back([q](void* sp){
                voxi::applyOverall(*static_cast<Settings*>(sp), static_cast<voxi::OverallQuality>(q),
                                    Renderer::get().deviceInfo());
            });
        },
        [](const VarValue& v, std::string& err) -> bool {
            if (v.as.u < 1 || v.as.u > 4) {
                err = "voxi.scalability must be 1 (Low), 2 (Medium), 3 (High) or 4 (Epic) -- 0 (Custom) "
                      "is read-only, derived from whatever the settings already are, never something "
                      "you can set";
                return false;
            }
            return true;
        }});

    // ---- read-only: sits beside the tier fields in Settings (same Quality type, same ladder shape)
    // but is NOT one, and must not be staged like one. Settings::layeredBsdf's own comment says why:
    // VoxiRenderer builds its raster PSOs once at init from whatever this held at that moment, so a
    // `set` here would update settings_ and change NOTHING on screen until a project reload rebuilds
    // the pipelines -- exactly the "lies about what is running" failure voxi.giMode's own honesty
    // requirement (see below) exists to prevent, just with no live value to read back that would
    // reveal the lie. Read-only is what keeps `get`/`vars` truthful about it instead: this is the
    // tier that will apply on next load, not one that is live now.
    t.push_back({"voxi.layeredBsdf", VarType::Quality, true,
        "Layered BSDF (clear coat) tier over the base BRDF -- NOT live: pipelines are built from this once at project load, so changing it here takes a reload to have any effect (read-only for that reason)",
        []{ return vQualityRaw(static_cast<u32>(Renderer::get().settings().layeredBsdf)); }, nullptr});

    // ---- dial fields, each derived from a tier above unless set explicitly -----------------
    t.push_back({"voxi.voxelResolution", VarType::U32, false,
        "Cubic voxel grid edge (engine clamps to [32,512])",
        []{ return vU32(Renderer::get().settings().voxelResolution); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->voxelResolution = n; }); }});
    t.push_back({"voxi.giCones", VarType::U32, false,
        "Diffuse gather cone count -- the one GI setting that actually costs anything (engine clamps to [1,16])",
        []{ return vU32(Renderer::get().settings().giCones); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->giCones = n; }); }});
    t.push_back({"voxi.giSkyOcclusionRays", VarType::U32, false,
        "Sky-visibility rays the ambient term traces per pixel; 0 estimates it from the cone gather instead, which is optimistic in enclosed geometry (renderer clamps to VoxiRenderer::kMaxShadowRays=32 at use, and forces 0 whenever ray tracing is not active this frame regardless of what is set here)",
        []{ return vU32(Renderer::get().settings().giSkyOcclusionRays); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->giSkyOcclusionRays = n; }); }});
    t.push_back({"voxi.giSkyOcclusionTile", VarType::U32, false,
        "Coherence tile edge those sky-occlusion rays share one ray direction across, in pixels; 1 is a fresh direction per pixel (engine clamps to [1,16])",
        []{ return vU32(Renderer::get().settings().giSkyOcclusionTile); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->giSkyOcclusionTile = n; }); }});
    t.push_back({"voxi.giIntensity", VarType::F32, false,
        "Indirect bounce multiplier (engine clamps to [0,8])",
        []{ return vF32(Renderer::get().settings().giIntensity); },
        [](ConsoleBatch& b, VarValue v){ const f32 n=v.as.f; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->giIntensity = n; }); }});
    t.push_back({"voxi.causticStrength", VarType::F32, false,
        "How strongly light focused by a water surface brightens what is beneath it; 0 switches the term off entirely -- a look, not a quality rung, so it is not on any ladder (no engine clamp)",
        []{ return vF32(Renderer::get().settings().causticStrength); },
        [](ConsoleBatch& b, VarValue v){ const f32 n=v.as.f; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->causticStrength = n; }); }});
    t.push_back({"voxi.giMaxDistance", VarType::F32, false,
        "Cone-trace range, in centimetres (engine clamps to [1,100000])",
        []{ return vF32(Renderer::get().settings().giMaxDistance); },
        [](ConsoleBatch& b, VarValue v){ const f32 n=v.as.f; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->giMaxDistance = n; }); }});
    t.push_back({"voxi.giRadianceCeiling", VarType::F32, false,
        "Caps GI radiance before the tonemap (mirrored to the shader as AVER_VOX_MAXRAD); 16 is the "
        "default. The tonemap is already flat white by about x = 4-5, so a value pinned at the "
        "ceiling paints solid white -- lowering this can remove a white patch caused by that, but it "
        "also dims any legitimately bright bounce near the same number, and there is no way to tell "
        "the two apart from this dial alone. Compare by hand; voxi.giPoisonView's red/green/violet "
        "paint exactly which pixels are hitting this ceiling (engine clamps to [0.1,256])",
        []{ return vF32(Renderer::get().settings().giRadianceCeiling); },
        [](ConsoleBatch& b, VarValue v){ const f32 n=v.as.f; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->giRadianceCeiling = n; }); }});
    // ---- refraction: how a translucent surface bends what is behind it (Settings::refractionMode's
    // own comment has the full three-mode writeup). THERE NOW IS AN ENGINE-SIDE RANGE CLAMP (Voxi.cpp,
    // between the REBLUR clamps and the resolve() call: any value above 2 clamps down to 1) -- this
    // validate() still refuses out-of-range values up front so a bad `set` reports its OWN mistake
    // rather than the different number the engine silently substituted, the same reason every other
    // voxi.* validate in this file exists. What the engine does NOT do is clamp a request of 2
    // (RayTraced) back down when RT hardware or the RT tier cannot honour it -- that is answered by
    // resolve() (RenderSettingsResolver.hpp), the same effective-value mechanism voxi.giMode and
    // voxi.denoiser below already read through: this var still reads the RAW requested field, not the
    // effective one, since refraction's own UI control (buildRenderingSettings) shows the same
    // distinction its own way and this table stays consistent with what setSettings actually stores.
    t.push_back({"voxi.refractionMode", VarType::U32, false,
        "How a translucent surface bends what is behind it: 0 = off (straight sample), 1 = screen-space offset (nearly free, the Medium/Low rung), 2 = ray-traced hit point (costs a ray, the High/Epic rung; resolves back to 1 without RT hardware or with the RT tier Off)",
        []{ return vU32(Renderer::get().settings().refractionMode); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->refractionMode = n; }); },
        [](const VarValue& v, std::string& err) -> bool {
            if (v.as.u > 2) { err = "refractionMode must be 0 (off), 1 (screen-space) or 2 (ray-traced) -- values above 2 are clamped to 1 by the engine, but this refuses them up front so the message names your own mistake, not the substitute"; return false; }
            return true;
        }});
    t.push_back({"voxi.refractionStrength", VarType::F32, false,
        "Multiplies the refraction offset; 1.0 is the physically correct bend for the material's own IOR, below trades correctness for calm, above exaggerates (no engine clamp)",
        []{ return vF32(Renderer::get().settings().refractionStrength); },
        [](ConsoleBatch& b, VarValue v){ const f32 n=v.as.f; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->refractionStrength = n; }); }});
    t.push_back({"voxi.refractionEdgeFade", VarType::F32, false,
        "How far from the screen edge the refraction offset is faded out, as a fraction of the smaller dimension; 0 disables the fade and lets the screen-space artefact show (no engine clamp)",
        []{ return vF32(Renderer::get().settings().refractionEdgeFade); },
        [](ConsoleBatch& b, VarValue v){ const f32 n=v.as.f; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->refractionEdgeFade = n; }); }});
    t.push_back({"voxi.rtShadowRays", VarType::U32, false,
        "Occlusion rays per pixel when it traces this frame (engine clamps to [1,32])",
        []{ return vU32(Renderer::get().settings().rtShadowRays); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->rtShadowRays = n; }); }});
    t.push_back({"voxi.rtPixelsPerRayTile", VarType::U32, false,
        "Shadow-ray amortisation tile edge, rounded to a power of two by the renderer (engine clamps to [1,16])",
        []{ return vU32(Renderer::get().settings().rtPixelsPerRayTile); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->rtPixelsPerRayTile = n; }); }});
    t.push_back({"voxi.giUpdateInterval", VarType::U32, false,
        "Frames between GI volume revoxelisations (engine clamps to [1,8])",
        []{ return vU32(Renderer::get().settings().giUpdateInterval); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->giUpdateInterval = n; }); }});
    // THE HEADLINE ADDITION THIS FILE EXISTS FOR: the switch between the voxel-cone and RTXDI ReSTIR
    // GI diffuse-bounce estimators (Settings::giMode's own comment has the full writeup). NOT staged
    // as a tierSetter even though it reads like one -- setSettings never derives it from a tier, it
    // only range-checks it (not even that, since the settings-separation pass, see below), so it
    // belongs with the other dials that setSettings leaves alone unless a caller touches them directly.
    //
    // THE READ CLOSURE NO LONGER READS THE RAW STORED FIELD (F-d, the settings-separation pass):
    // Renderer::setSettings used to hard-clamp giMode back to 0 whenever RayQuery hardware, the RT
    // tier or the GI tier could not honour it, which is what let a plain `Renderer::get().settings().
    // giMode` read double as "is it actually running" -- that clamp is gone, so a raw read here would
    // now show 1 forever once requested, even on a device that refused it. voxi::resolve()
    // (RenderSettingsResolver.hpp) is the one place that question is still answered, so this reads
    // resolve(...).giMode.effective instead -- the SAME honesty guarantee as before (`set voxi.giMode 1`
    // on a device with no RayQuery hardware, or with rayTracing/globalIllumination Off, stages 1,
    // commits it, and this read reports back 0, wired into commitBatch's "requested vs now" diff same
    // as always), just computed a different way now that the raw field cannot be trusted to carry it.
    t.push_back({"voxi.giMode", VarType::U32, false,
        "Which estimator answers the diffuse GI bounce: 0 = voxel cone gather (default), 1 = RTXDI ReSTIR GI. Needs RayQuery hardware, rayTracing != Off and globalIllumination != Off -- resolves back to 0 (the stored request is kept, untouched) when any is missing, and this always reads back what is ACTUALLY running, not merely what was last requested",
        []{ const Renderer& r = Renderer::get(); return vU32(voxi::resolve(r.settings(), r.deviceInfo()).giMode.effective); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->giMode = n; }); }});
    // optimisation-wave-2's U1: how much of F2 (candidate-hit sky) and F3 (reuse visibility) -- the
    // contrast fix's two per-pixel rays, cb4b48df -- giMode 1 pays for at this GI tier. A GI-group
    // tier-derived DIAL, like voxi.giCones/voxi.voxelResolution above, so it belongs with the other
    // dials `set voxi.globalIllumination <tier>` re-derives unless set in the same line.
    //
    // READS THE RAW STORED FIELD, not resolve()'s effective value, unlike voxi.giMode just above --
    // Resolution::giRestirVisibility.effective deliberately EQUALS requested always
    // (RenderSettingsResolver.hpp's own comment on that field explains why: clamping to 0 on a failed
    // prerequisite would read "No ray (over-bright)" while no ReSTIR runs at all), so reading the raw
    // field here reports the identical number resolve() would, without the extra resolve() call --
    // the same "read the raw field, since raw and effective always agree for this one" shape
    // voxi.refractionMode's own comment documents for the reason its own request/effective can diverge
    // and why this dial's cannot.
    t.push_back({"voxi.giRestirVisibility", VarType::U32, false,
        "0 no ray (pre-fix, over-bright), 1 reconstructed (no ray), 2 half resolution, 3 full. Only "
        "applies when voxi.giMode resolves to 1. voxi.legacyRestirHitSky / "
        "voxi.legacyRestirReuseVisibility, when on, force 'no ray' for their own ray regardless of "
        "this.",
        []{ return vU32(Renderer::get().settings().giRestirVisibility); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->giRestirVisibility = n; }); },
        [](const VarValue& v, std::string& err) -> bool {
            if (v.as.u > 3) { err = "giRestirVisibility must be 0 (no ray), 1 (reconstructed), 2 (half resolution) or 3 (full) -- values above 3 are clamped to 3 by the engine, but this refuses them up front so the message names your own mistake, not the substitute"; return false; }
            return true;
        }});
    // THE TWO DIALS THE FADE BISECTION LEFT STANDING. Everything else it added is gone again,
    // each removed by its own measurement rather than by taste: the moving-camera reservoir-age cap
    // and the moving-camera history cap both made the overshoot WORSE, the two reuse-similarity
    // tolerances measured identical to their defaults, the bias-correction mode changed nothing,
    // the Jacobian dial changed nothing, and a wave-local boiling filter cost 13% of the settled
    // image's brightness while leaving the overshoot. See Settings::giRestirMaxHistory (Voxi.hpp)
    // for the full chain and the numbers behind it. Ordinary dialSetters entries, same shape as
    // voxi.giRestirVisibility above -- real Settings fields, not console-only overrides.
    t.push_back({"voxi.giRestirMaxHistory", VarType::U32, false,
        "stparams.maxHistoryLength: how much weight a previous-frame ReSTIR GI reservoir may carry "
        "into the combine. DEFAULT 0, which is the camera-motion fade fix -- 1 was the old value and "
        "overshoots ~8% for ~25 frames after the camera stops, 8 overshoots ~104%. 0 measured no "
        "worse at rest or in motion (same settled brightness, same grain and flicker), because NRD "
        "is what actually smooths this. Set 1 to get the old behaviour back. Engine clamps to [0,31].",
        []{ return vU32(Renderer::get().settings().giRestirMaxHistory); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->giRestirMaxHistory = n; }); },
        [](const VarValue& v, std::string& err) -> bool {
            if (v.as.u > 31) { err = "giRestirMaxHistory must be 0..31 -- five packed bits is all it has"; return false; }
            return true;
        }});
    t.push_back({"voxi.giRestirSpatialSamples", VarType::U32, false,
        "Overrides the ReSTIR GI spatial-reuse tap count (stparams.numSamples) the moving-camera "
        "motion discount would otherwise compute: 15 = auto (leave the discount alone, today's "
        "image), 0 = no spatial reuse at all, 1..8 pin the count regardless of camera motion. Only "
        "applies when voxi.giMode resolves to 1 (engine clamps to [0,15]).",
        []{ return vU32(Renderer::get().settings().giRestirSpatialSamples); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->giRestirSpatialSamples = n; }); },
        [](const VarValue& v, std::string& err) -> bool {
            if (v.as.u > 15) { err = "giRestirSpatialSamples must be 0 (no spatial reuse) through 15 (auto) -- four packed bits is all it has"; return false; }
            return true;
        }});
    // NOT a Settings field -- see consoleGiPoisonViewSlot()'s own comment for why this is the raw-slot
    // idiom rather than an ordinary dialSetters entry. SEVEN of the eight colours below are giMode 1
    // (ReSTIR) only: those guards live in voxi_restir.hlsli's giRestirIndirect, which giMode 0 never
    // calls. The eighth, VIOLET, is NOT giMode-gated (B1/F5, added by the build/shader-safety review):
    // it is painted directly in voxi.hlsl's PSMainVoxi/PSRayDriven over the ray-traced specular
    // indirect term's own AVER_VOX_MAXRAD ceiling hit, which exists under either diffuse estimator --
    // see aver_IsGiRestirPoisonColour's own comment (voxi.hlsl, just above PSMainVoxi) for the
    // precedence between the two families.
    t.push_back({"voxi.giPoisonView", VarType::Bool, false,
        "ReSTIR-GI poison debug view: paints an unmistakable colour over any pixel where one of "
        "voxi_restir.hlsli's guards fired THIS frame -- magenta = the store-time reservoir guard (the "
        "one that matters most), cyan = a candidate-radiance clamp, yellow = the target-pdf guard, "
        "orange = the pre-existing final-estimate guard, blue = the NRD-readback guard (these five are "
        "all non-finite/NaN corruption); red = the raw estimate hit the voxi.giRadianceCeiling clamp "
        "while still finite, green = the NRD-denoised readback hit the same ceiling -- these last two "
        "are WHERE a white patch's cause lives, not a bug: a non-finite guard above always outranks "
        "them at the same pixel. These seven are giMode 1 (ReSTIR) only. An EIGHTH colour, violet, is "
        "NOT giMode-gated: it marks the ray-traced specular indirect term's own ceiling hit "
        "(voxi.hlsl's PSMainVoxi/PSRayDriven), which exists under either diffuse estimator. All "
        "colours zero (the scene renders normally) means no guard is firing.",
        []{ return vBool(consoleGiPoisonViewSlot()); },
        [](ConsoleBatch& b, VarValue v){
            const bool on = v.as.b;
            b.deviceSetters.push_back([on](rhi::IDevice&){ consoleGiPoisonViewSlot() = on; });
        }});
    // See consoleResetHistoryEveryFrameSlot()'s own comment for the raw-slot idiom and the bit table.
    t.push_back({"voxi.debugResetHistoryEveryFrame", VarType::U32, false,
        "Bisection aid, never saved: resets the chosen temporal histories EVERY frame, without log "
        "lines. 1 = ReSTIR GI reservoirs + GI visibility history, 2 = RT shadow/reflection/"
        "sky-occlusion history (also restarts NRD), 4 = NRD only; add bits to combine, 0 = off. A "
        "fade or smear that still happens with a history reset every frame is not carried by that "
        "history. Expect a noisier image while it is on.",
        []{ return vU32(consoleResetHistoryEveryFrameSlot()); },
        [](ConsoleBatch& b, VarValue v){
            const u32 mask = v.as.u;
            b.deviceSetters.push_back([mask](rhi::IDevice&){ consoleResetHistoryEveryFrameSlot() = mask; });
        },
        [](const VarValue& v, std::string& err) -> bool {
            if (v.as.u > 7) { err = "debugResetHistoryEveryFrame is a mask of 1, 2 and 4 -- 0 to 7"; return false; }
            return true;
        }});
    // optimisation-wave-2's U1 path-debug view (2.10 I) -- see consoleGiVisPathViewSlot()'s own
    // comment above for the raw-slot idiom and why. Paints F2's resolved path (yellow = no ray,
    // green = reconstructed, blue = half-res reconstruction, red = half-res fallback trace, white =
    // full trace) directly over indirect diffuse, so a Half-mode run can be checked by eye for
    // whether it is actually reconstructing anywhere instead of quietly tracing every pixel as Full.
    // Suppressed while voxi.giPoisonView is also on (that view answers a different, higher-priority
    // question -- corruption, not which visibility path ran -- and paints first).
    t.push_back({"voxi.giVisPathView", VarType::Bool, false,
        "U1 path-debug view: paints F2's resolved visibility path over indirect diffuse -- yellow = "
        "no ray (mode 0 or a legacy bit), green = reconstructed (voxel cone), blue = half-resolution "
        "reconstruction, red = half-resolution fallback (traced because no valid reconstruction was "
        "available this pixel), white = full trace. Suppressed while voxi.giPoisonView is also on, "
        "which paints first. Off by default: the paint replaces indirect diffuse, so it must never "
        "happen just from opening the Console tab.",
        []{ return vBool(consoleGiVisPathViewSlot()); },
        [](ConsoleBatch& b, VarValue v){
            const bool on = v.as.b;
            b.deviceSetters.push_back([on](rhi::IDevice&){ consoleGiVisPathViewSlot() = on; });
        }});
    // NOT a Settings field, SAME SHAPE as voxi.giPoisonView directly above and for the identical
    // reason -- see consoleNrdLegacyCameraSlot()'s own comment. [live]: toggling this resets NRD's
    // history on the very next frame (VoxiRenderer::setNrdLegacyCamera's own comment has the
    // mechanism), which is what lets an A/B comparison be made by hand without a rebuild or a
    // project reload.
    t.push_back({"voxi.nrdLegacyCamera", VarType::Bool, false,
        "A/B switch for the NRD camera-contract fix in VoxiRenderer::beginShadowHistory (see its own "
        "comment on the NRD block for the mechanism this reinstates). Default OFF is the FIXED "
        "behaviour: NRD is handed this frame's camera, freshly read and factorised into a real "
        "worldToView/viewToClip pair. ON REINSTATES A KNOWN-WRONG ENCODING FOR COMPARISON ONLY: "
        "identity worldToView and last frame's combined viewProj standing in for viewToClip -- the "
        "exact pre-fix behaviour, never a setting to leave on. Toggling either way resets NRD's "
        "denoiser history on the next frame, so the two encodings are never blended into one image.",
        []{ return vBool(consoleNrdLegacyCameraSlot()); },
        [](ConsoleBatch& b, VarValue v){
            const bool on = v.as.b;
            b.deviceSetters.push_back([on](rhi::IDevice&){ consoleNrdLegacyCameraSlot() = on; });
        }});
    // ---- the lighting-contrast fix's five legacy A/B switches (contrast-fix plan, section 3's A/B
    // table) -- one bit each of consoleLightingLegacySlot()'s own u32, so the user can isolate ONE
    // root cause at a time instead of typing a bitmask by hand. SAME raw-slot/deviceSetters shape as
    // voxi.giPoisonView and voxi.nrdLegacyCamera directly above; only the read/stage pair differ,
    // since these five share one underlying word instead of each owning a whole bool. Default
    // OFF/false on every one of them: the FIXED behaviour is what ships, and ALL FIVE true must
    // reproduce HEAD's image exactly (review checklist item 12) -- never a setting to leave on.
    auto legacyBitRead = [](u32 bit) {
        return [bit]{ return vBool((consoleLightingLegacySlot() & bit) != 0u); };
    };
    auto legacyBitStage = [](u32 bit) {
        return [bit](ConsoleBatch& b, VarValue v){
            const bool on = v.as.b;
            b.deviceSetters.push_back([bit, on](rhi::IDevice&){
                if (on) consoleLightingLegacySlot() |= bit; else consoleLightingLegacySlot() &= ~bit;
            });
        };
    };
    t.push_back({"voxi.legacyRestirSampleRing", VarType::Bool, false,
        "ON reinstates the pre-fix behaviour for comparison only: the ReSTIR candidate ray and the "
        "sky-occlusion ray both sample a fixed 45-degree ring instead of a cosine-weighted hemisphere "
        "(root cause R0 of the contrast-fix plan; gAmbientParams.z bit 1). Default OFF samples the "
        "cosine hemisphere and resets GI/NRD/AO history on either transition.",
        legacyBitRead(1u), legacyBitStage(1u)});
    t.push_back({"voxi.legacySkyDoubleCount", VarType::Bool, false,
        "ON reinstates the pre-fix behaviour for comparison only: giMode 1's receiver counts its own "
        "sky twice, once through the traced ReSTIR estimate and again through the ambient term (root "
        "cause R1; gAmbientParams.z bit 2). Default OFF counts it once, through the traced estimate.",
        legacyBitRead(2u), legacyBitStage(2u)});
    t.push_back({"voxi.legacyRestirHitSky", VarType::Bool, false,
        "ON reinstates the pre-fix behaviour for comparison only: the ReSTIR candidate hit's own "
        "second-bounce sky is added with no visibility test at all (root cause R2; gAmbientParams.z "
        "bit 4). Default OFF traces one visibility ray for it and resets GI/NRD history on either "
        "transition.",
        legacyBitRead(4u), legacyBitStage(4u)});
    t.push_back({"voxi.legacyRestirReuseVisibility", VarType::Bool, false,
        "ON reinstates the pre-fix behaviour for comparison only: a spatio-temporally reused ReSTIR "
        "sample shades with no visibility test between the receiver and the reused sample's position "
        "(root cause R3; gAmbientParams.z bit 8). Default OFF traces that visibility ray and resets "
        "GI/NRD history on either transition.",
        legacyBitRead(8u), legacyBitStage(8u)});
    t.push_back({"voxi.legacyConeWeights", VarType::Bool, false,
        "ON reinstates the pre-fix behaviour for comparison only: the cone gather's directions are "
        "cosine-distributed AND cosine-weighted a second time, an effective cos^2 distribution "
        "instead of cos (root cause R6; gAmbientParams.z bit 16). Default OFF weights each cone once. "
        "Affects giMode 0, the non-RT fallback, and the cluster and particle passes.",
        legacyBitRead(16u), legacyBitStage(16u)});
    // W6/M5's OWN legacy A/B switch (section 4(b) of the optimisation-wave-2 plan) -- one bit more of
    // the SAME consoleLightingLegacySlot() word the five above share, so it composes with them (and
    // with --lighting-legacy) exactly the same way: one root cause isolated at a time. ON reinstates
    // the pre-W6 behaviour for comparison only -- a blended (alpha-blend or transmissive) fragment's
    // per-pixel reservoir store, surface history and NRD readback go back to writing/reading
    // unconditionally, so the opaque surface behind glass or water loses temporal reuse to whichever
    // pane covered it last, on D3D12 (Vulkan never marks a draw blended -- see VulkanDevice.cpp's own
    // blended=false comment -- so this bit is inert there, like the others in this table already are
    // off their own backend's gaps). Default OFF/false: the FIXED behaviour (W6) is what ships.
    t.push_back({"voxi.legacyBlendedHistoryWrite", VarType::Bool, false,
        "ON reinstates the pre-fix behaviour for comparison only: a blended (alpha-blend or "
        "transmissive) fragment's per-pixel GI/NRD history writes and readbacks stop being suppressed, "
        "so the opaque surface behind glass or water is overwritten by whichever pane covered it last "
        "(root cause W6; gAmbientParams.z bit 32; D3D12 only -- inert on Vulkan, which never marks a "
        "draw blended). Default OFF keeps W6's fix; VoxiRenderer::setLightingLegacyBits is the piece "
        "that resets GI/NRD/RT history on this bit's transition, the same as the five bits above it.",
        legacyBitRead(32u), legacyBitStage(32u)});
    t.push_back({"voxi.legacyNrdReadback", VarType::Bool, false,
        "ON reinstates the pre-fix read of NRD's denoised GI for comparison only: at THIS frame's "
        "pixel, although NRD filtered LAST frame's -- a one-frame displacement in motion, seen as GI "
        "leaking along edges. Default OFF reprojects the read to where the surface was last frame "
        "(gAmbientParams.z bit 64). A still frame is identical either way.",
        legacyBitRead(64u), legacyBitStage(64u)});
    // ---- the engine-optimisation-plan measurement dials (M1-M4/W3/W12) -- SAME raw-slot/
    // deviceSetters shape as voxi.giPoisonView above (see consoleGiForceRebuildSlot()'s own comment
    // for why these three, unlike the tier/dial fields, cannot be staged as ordinary Settings dials).
    // Default OFF on all three: none changes the rendered image when off, only what the renderer
    // measures or how it schedules work getting there.
    t.push_back({"voxi.giForceRebuild", VarType::Bool, false,
        "Measurement only: forces every GI tick to rebuild from scratch, the way the very first tick "
        "after a level load always does. The GI derived-data cache (VoxiRenderer::setGiCacheDir) is "
        "neither read nor written while this is on, so a baked volume on disk is left untouched but "
        "ignored -- this isolates the cost of a full rebuild from the cost of everything the cache and "
        "the update-interval gate normally do to avoid one. Default OFF.",
        []{ return vBool(consoleGiForceRebuildSlot()); },
        [](ConsoleBatch& b, VarValue v){
            const bool on = v.as.b;
            b.deviceSetters.push_back([on](rhi::IDevice&){ consoleGiForceRebuildSlot() = on; });
        }});
    t.push_back({"voxi.giBoundedDispatch", VarType::Bool, false,
        "The GI volume's clear/resolve/mip compute passes dispatch only over the voxel box the "
        "injected draws can actually touch this rebuild, instead of the whole grid. Falls back to "
        "the full grid on the first build, a moved or resized volume, an unbounded draw, or a cache "
        "restore -- see GiDispatchBounds.hpp's own comment for the box math. Produces IDENTICAL "
        "output to the full-grid path: verified at 99.3% bit-identical, 0.0024 mean absolute "
        "difference, the residual being this renderer's own GI temporal noise. MEASURED on PTTest "
        "NewSponza, ray-driven, 400 moving frames: the census line goes from covering 100% of the "
        "512^3 grid to 1.3%, and GPU total 30.64 -> 30.25ms. Default ON; turn it off to measure "
        "against the full-grid path.",
        []{ return vBool(consoleGiBoundedDispatchSlot()); },
        [](ConsoleBatch& b, VarValue v){
            const bool on = v.as.b;
            b.deviceSetters.push_back([on](rhi::IDevice&){ consoleGiBoundedDispatchSlot() = on; });
        }});
    t.push_back({"voxi.giFreeAccumulator", VarType::Bool, false,
        "Measurement only: frees the GI injection accumulator (roughly 2 GB at 512^3) after 240 "
        "consecutive quiet GI ticks -- ticks that needed no rebuild -- and recreates it the instant a "
        "change needs one again, which then runs one tick later than it otherwise would while the "
        "texture is recreated. Trades that one-tick latency and a recreation cost against holding the "
        "memory for the entire session regardless of how long the volume sits idle. Default OFF.",
        []{ return vBool(consoleGiFreeAccumulatorSlot()); },
        [](ConsoleBatch& b, VarValue v){
            const bool on = v.as.b;
            b.deviceSetters.push_back([on](rhi::IDevice&){ consoleGiFreeAccumulatorSlot() = on; });
        }});
    // W6/M5's pricing switch (section 4(a) of the optimisation-wave-2 plan) -- see
    // consoleBlendedGiConeSlot()'s own comment above for the raw-slot idiom and why. Measurement only,
    // like the three above: neither value changes the rendered image on its own, only which term a
    // blended (alpha-blend or transmissive) fragment shades its indirect diffuse from. Default false
    // keeps W6's fix -- ReSTIR shades every fragment, blended ones included, the way the D3 decision
    // ships it. true drops blended fragments back to the pre-W6 cone gather so the ReSTIR share of
    // the "blended replay" span (D3D12Device.cpp:5073) can be read by difference.
    t.push_back({"voxi.blendedGiCone", VarType::Bool, false,
        "Measurement only (W6/M5): false (default) shades a blended fragment's indirect diffuse from "
        "ReSTIR like any opaque fragment, the fixed W6 behaviour. true forces the pre-existing "
        "voxel-cone gather on blended fragments instead, so ReSTIR's own share of the 'blended "
        "replay' span can be read by taking 'frametime' with this true, then false.",
        []{ return vBool(consoleBlendedGiConeSlot()); },
        [](ConsoleBatch& b, VarValue v){
            const bool on = v.as.b;
            b.deviceSetters.push_back([on](rhi::IDevice&){ consoleBlendedGiConeSlot() = on; });
        }});
    // DIAL, NOT A TIER, and it reads back what is ACTUALLY running rather than what was asked for --
    // the same honesty voxi.giMode above documents at length, and through the SAME mechanism now:
    // voxi::resolve()'s denoiser field, not a raw Settings read. denoiser was never clamped inside
    // Settings itself (unlike giMode's old, now-removed clamp), but "read the raw field" and "read
    // whether it is actually running" only ever coincided because nothing could refuse it that
    // Settings' own honesty didn't already cover -- RequiresNrd/RequiresRayTracingEnabled/
    // RequiresGlobalIllumination/NothingToDenoise below all can now (RenderSettingsResolver.hpp). NRD
    // also refuses to run at any MSAA above 1 (D3D12 will not mix sample counts in one render-target
    // set, so the G-buffer would be cleared and never written) -- that ONE reason is SOFT
    // (RequiresMsaaOne): resolve() still reports it as not-effective here, so `set voxi.denoiser true`
    // at 8x MSAA stages true, commits true, and this read reports back false, with nothing more needed
    // than `set voxi.msaa 1` alongside it -- the WARN from VoxiRenderer is the other half of that same
    // story, printed once from the render side.
    // Occlusion-aware fog (2026-09-24): the fog's in-scattered light scaled by how much sky the air
    // along the view ray can see, from a world-space volume built off the GI voxels. OFF restores the
    // old unoccluded fog exactly, for A/B -- the one that glowed blue inside covered arcades.
    t.push_back({"voxi.fogOcclusion", VarType::Bool, false,
        "Fog in-scatter respects occlusion: enclosed air (arcades, rooms) stops glowing with sky light it cannot see. Built from the GI voxel volume, so it needs voxel GI on; off = the old unoccluded fog",
        []{ return vBool(Renderer::get().settings().fogOcclusion); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->fogOcclusion = on; }); }});
    t.push_back({"voxi.denoiser", VarType::Bool, false,
        "NVIDIA NRD over the ReSTIR indirect diffuse and the ray-traced sky occlusion. Allocates the thin G-buffer (velocity, view Z, normal/roughness -- nothing else in the engine wants it) and REQUIRES RT hardware, the RT tier not Off, something to denoise, D3D12+NRD and MSAA 1; above 1x sample count the pass skips itself and says so once at WARN, and this always reads back what is ACTUALLY running, not merely what was last requested",
        []{ const Renderer& r = Renderer::get(); return vBool(voxi::resolve(r.settings(), r.deviceInfo()).denoiser.effective != 0); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->denoiser = on; }); }});
    // ---- REBLUR_DIFFUSE history/prepass tuning -- LIVE: VoxiRenderer re-issues NRD's own
    // SetDenoiserSettings every frame (see applyReblurTuning, VoxiRenderer.cpp), so a change here
    // takes effect on the NEXT frame with no project reload and no NRD instance recreation -- unlike
    // voxi.layeredBsdf above, this is NOT a [reload] entry. Only meaningful once voxi.denoiser is on
    // and REBLUR_DIFFUSE has actually been created (giMode 1 or the sky-occlusion rays wanting it);
    // harmless and silently kept for later otherwise, same as any other dial set before its feature is
    // active. Ranges are NRD's own (third_party/nrd/Include/NRDSettings.h's ReblurSettings).
    t.push_back({"voxi.reblurDiffusePrepassBlurRadius", VarType::F32, false,
        "REBLUR_DIFFUSE pre-accumulation spatial blur radius, in pixels; 0 skips the pre-pass dispatch "
        "entirely. NRD's own default and this engine's is 30 (engine clamps to [0,100] -- NRD's header "
        "states only the 0 lower bound, 100 is a defensive ceiling this engine adds)",
        []{ return vF32(Renderer::get().settings().reblurDiffusePrepassBlurRadius); },
        [](ConsoleBatch& b, VarValue v){ const f32 n=v.as.f; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->reblurDiffusePrepassBlurRadius = n; }); }});
    t.push_back({"voxi.reblurMaxAccumulatedFrameNum", VarType::U32, false,
        "REBLUR_DIFFUSE main history depth, in frames -- latency/noise trade, not a dispatch toggle. "
        "NRD's own default and this engine's is 30 (engine clamps to [0,63], NRD's own "
        "REBLUR_MAX_HISTORY_FRAME_NUM)",
        []{ return vU32(Renderer::get().settings().reblurMaxAccumulatedFrameNum); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->reblurMaxAccumulatedFrameNum = n; }); }});
    t.push_back({"voxi.reblurMaxStabilizedFrameNum", VarType::U32, false,
        "REBLUR_DIFFUSE temporal-stabilization history depth, in frames; 0 skips the stabilization "
        "dispatch entirely, and a value at or above voxi.reblurMaxAccumulatedFrameNum gets REDUCED to "
        "it by NRD ITSELF (its own header documents this), not by this engine -- today's defaults (63 "
        "here, 30 there) are exactly such a pair, unchanged. NRD's own default and this engine's is 63 "
        "(engine clamps to [0,63], NRD's own REBLUR_MAX_HISTORY_FRAME_NUM)",
        []{ return vU32(Renderer::get().settings().reblurMaxStabilizedFrameNum); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->reblurMaxStabilizedFrameNum = n; }); }});
    // The residual-noise dials -- see render.nrd::Denoiser::ReblurTuning for each one's NRD guidance.
    // Same LIVE shape as the three above; NRD's own defaults.
    t.push_back({"voxi.reblurAntiFirefly", VarType::Bool, false,
        "REBLUR_DIFFUSE anti-firefly (NRD's enableAntiFirefly). NRD default ON",
        []{ return vBool(Renderer::get().settings().reblurAntiFirefly); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->reblurAntiFirefly = on; }); }});
    t.push_back({"voxi.reblurFireflySuppressorScale", VarType::F32, false,
        "REBLUR_DIFFUSE temporal firefly suppressor: each new value is clamped to (this + 38/(history "
        "length+1)) x the pixel's own history. NRD documents [1,3], default 2",
        []{ return vF32(Renderer::get().settings().reblurFireflySuppressorScale); },
        [](ConsoleBatch& b, VarValue v){ const f32 n=v.as.f; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->reblurFireflySuppressorScale = n; }); }});
    t.push_back({"voxi.reblurAntilagSigmaScale", VarType::F32, false,
        "REBLUR_DIFFUSE antilag: luminance delta is discounted by local variance times this; LARGER "
        "quietens antilag (NRD has no off switch). NRD default 2 (its old default was 4)",
        []{ return vF32(Renderer::get().settings().reblurAntilagSigmaScale); },
        [](ConsoleBatch& b, VarValue v){ const f32 n=v.as.f; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->reblurAntilagSigmaScale = n; }); }});
    t.push_back({"voxi.reblurAntilagSensitivity", VarType::F32, false,
        "REBLUR_DIFFUSE antilag sensitivity; SMALLER is more sensitive. NRD default 3",
        []{ return vF32(Renderer::get().settings().reblurAntilagSensitivity); },
        [](ConsoleBatch& b, VarValue v){ const f32 n=v.as.f; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->reblurAntilagSensitivity = n; }); }});
    t.push_back({"voxi.reblurMinHitDistanceWeight", VarType::F32, false,
        "REBLUR_DIFFUSE spatial hit-distance weight floor, (0,0.2]; NRD recommends smaller for "
        "RTXDI/ReSTIR signals. NRD default 0.1",
        []{ return vF32(Renderer::get().settings().reblurMinHitDistanceWeight); },
        [](ConsoleBatch& b, VarValue v){ const f32 n=v.as.f; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->reblurMinHitDistanceWeight = n; }); }});
    t.push_back({"voxi.reblurFastHistoryClampSigma", VarType::F32, false,
        "REBLUR_DIFFUSE colour-box scale clamping main history to fast history, [1,3]; NRD: 1.5 "
        "works well even for dirty signals. NRD default 2",
        []{ return vF32(Renderer::get().settings().reblurFastHistoryClampSigma); },
        [](ConsoleBatch& b, VarValue v){ const f32 n=v.as.f; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->reblurFastHistoryClampSigma = n; }); }});
    t.push_back({"voxi.reblurMaxFastAccumulatedFrameNum", VarType::U32, false,
        "REBLUR_DIFFUSE fast-history depth in frames, at most the main depth (equal disables fast "
        "history). NRD default 6",
        []{ return vU32(Renderer::get().settings().reblurMaxFastAccumulatedFrameNum); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->reblurMaxFastAccumulatedFrameNum = n; }); }});
    t.push_back({"voxi.reblurHistoryFixFrameNum", VarType::U32, false,
        "REBLUR_DIFFUSE frames reconstructed spatially after a history reset, below the fast depth. "
        "NRD default 3",
        []{ return vU32(Renderer::get().settings().reblurHistoryFixFrameNum); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->reblurHistoryFixFrameNum = n; }); }});
    t.push_back({"voxi.reblurMinBlurRadius", VarType::F32, false,
        "REBLUR_DIFFUSE spatial radius once converged, in pixels. NRD default 1",
        []{ return vF32(Renderer::get().settings().reblurMinBlurRadius); },
        [](ConsoleBatch& b, VarValue v){ const f32 n=v.as.f; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->reblurMinBlurRadius = n; }); }});
    t.push_back({"voxi.nrdCameraMatchesInputs", VarType::Bool, false,
        "ON tells NRD its inputs were rendered with last frame's camera, which they were -- it runs "
        "before this frame's scene pass. Default OFF (this frame's camera, the shipped wiring): the "
        "consistent pairing measured no visible gain and ~3% more motion grain on NRD's GI. Flipping "
        "it resets NRD's history",
        []{ return vBool(Renderer::get().settings().nrdCameraMatchesInputs); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->nrdCameraMatchesInputs = on; }); }});
    t.push_back({"voxi.reblurMaxBlurRadius", VarType::F32, false,
        "REBLUR_DIFFUSE spatial radius on a fresh history, in pixels. Engine default 10 (NRD's is 30): "
        "measured ~15% less grain just after camera motion, no still-frame change",
        []{ return vF32(Renderer::get().settings().reblurMaxBlurRadius); },
        [](ConsoleBatch& b, VarValue v){ const f32 n=v.as.f; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->reblurMaxBlurRadius = n; }); }});
    t.push_back({"voxi.rtShadowDenoise", VarType::U32, false,
        "Spatial denoise radius for the ray-traced sun shadow, in pixels (engine clamps to [0,3])",
        []{ return vU32(Renderer::get().settings().rtShadowDenoise); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->rtShadowDenoise = n; }); }});
    t.push_back({"voxi.rtRenderMode", VarType::U32, false,
        // NO LONGER "opt-in only, no quality tier turns this on" -- the ladder does now (D3, the
        // settings-separation retune): Medium/High/Epic derive this to 1, Low derives it to 0
        // (rasteriser, by explicit product decision, not a hardware gap) and Off to 0 (no acceleration
        // structure to trace against). ladder::rtRenderMode (QualityLadder.hpp) has the full reasoning.
        "0 = rasteriser finds the first surface, 1 = a primary ray per pixel does -- gives up hardware early-Z. Derived from the RT tier on a tier change (Off/Low 0, Medium/High/Epic 1) unless set in the same line",
        []{ return vU32(Renderer::get().settings().rtRenderMode); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->rtRenderMode = n; }); }});
    // Milestone 1's A/B switch over rtRenderMode's OWN internal shape, directly beneath it for the
    // same reason voxi.giRestirMaxHistory sits under voxi.giRestirVisibility: the two are read
    // together while bisecting the staged split against the single pass it replaces. Milestone 4
    // adds value 2, a staged variant that deliberately trades GI quality for speed rather than
    // staying a same-image comparison -- see Voxi.hpp's own comment on Settings::rayDrivenStages
    // for the full reasoning. Only meaningful once voxi.rtRenderMode itself resolves to 1 (primary
    // rays); see RenderSettingsResolver.hpp's Resolution::rayDrivenStages for the Project Settings
    // page's own greyed reason.
    t.push_back({"voxi.rayDrivenStages", VarType::U32, false,
        "0 = single pass (default, today's one ray-driven draw -- the comparison baseline), 1 = "
        "staged: a visibility compute pass, a shadow compute pass, then the same shading draw "
        "(same image as 0). 2 = staged + half-rate GI: the same staged path, but the ReSTIR GI "
        "stage traces only half the pixels per frame in NRD's checkerboard pattern and REBLUR "
        "reconstructs the rest -- a deliberate GI quality/latency-for-speed trade, distinct from 1 "
        "only while ReSTIR GI is the diffuse estimator and the NRD denoiser is on (otherwise 2 "
        "behaves as 1, logged once). D3D12 only for 1 and 2; falls back to "
        "single pass and logs once when anything staged needs is missing. Only applies when "
        "voxi.rtRenderMode resolves to 1. Engine clamps to [0,2].",
        []{ return vU32(Renderer::get().settings().rayDrivenStages); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->rayDrivenStages = n; }); },
        [](const VarValue& v, std::string& err) -> bool {
            if (v.as.u > 2) { err = "rayDrivenStages must be 0 (single pass), 1 (staged) or 2 (staged + half-rate GI)"; return false; }
            return true;
        }});
    t.push_back({"voxi.ptBounces", VarType::U32, false,
        "Path-tracing bounce budget; 1 means no extra bounces (ray tracing, not path tracing) (engine clamps to [1,8])",
        []{ return vU32(Renderer::get().settings().ptBounces); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->ptBounces = n; }); }});

    // ---- read-only: why a tier is stuck, and what the device actually reports --------------
    t.push_back({"voxi.status.msaa", VarType::Str, true, "Why MSAA is or is not available",
        []{ return vStr(Renderer::get().statusText(voxi::Feature::Msaa)); }, nullptr});
    t.push_back({"voxi.status.gi", VarType::Str, true, "Why Global Illumination is or is not available",
        []{ return vStr(Renderer::get().statusText(voxi::Feature::GlobalIllumination)); }, nullptr});
    t.push_back({"voxi.status.rt", VarType::Str, true, "Why Ray Tracing is or is not available",
        []{ return vStr(Renderer::get().statusText(voxi::Feature::RayTracing)); }, nullptr});
    t.push_back({"voxi.status.pt", VarType::Str, true, "Why Path Tracing is or is not available",
        []{ return vStr(Renderer::get().statusText(voxi::Feature::PathTracing)); }, nullptr});
    t.push_back({"voxi.status.meshShaders", VarType::Str, true, "Why Mesh Shaders is or is not available",
        []{ return vStr(Renderer::get().statusText(voxi::Feature::MeshShaders)); }, nullptr});

    t.push_back({"voxi.device.msaaMask", VarType::U32, true, "Bitmask of MSAA sample counts the device supports",
        []{ return vU32(Renderer::get().deviceInfo().msaaMask); }, nullptr});
    t.push_back({"voxi.device.maxMsaaSamples", VarType::U32, true, "Highest MSAA sample count the device supports",
        []{ return vU32(Renderer::get().deviceInfo().maxMsaaSamples); }, nullptr});
    t.push_back({"voxi.device.rayTracingTier", VarType::U32, true, "0 none, 10 = DXR 1.0, 11 = DXR 1.1",
        []{ return vU32(Renderer::get().deviceInfo().rayTracingTier); }, nullptr});
    t.push_back({"voxi.device.computeShaders", VarType::Bool, true, "Whether the device has compute support",
        []{ return vBool(Renderer::get().deviceInfo().computeShaders); }, nullptr});
    t.push_back({"voxi.device.typedUavLoads", VarType::Bool, true, "Whether the device supports typed UAV loads",
        []{ return vBool(Renderer::get().deviceInfo().typedUavLoads); }, nullptr});
    t.push_back({"voxi.device.conservativeRaster", VarType::Bool, true, "Whether the device supports conservative rasterisation",
        []{ return vBool(Renderer::get().deviceInfo().conservativeRaster); }, nullptr});
    t.push_back({"voxi.device.shaderModel", VarType::U32, true, "60 = SM 6.0, 65 = SM 6.5, etc.",
        []{ return vU32(Renderer::get().deviceInfo().shaderModel); }, nullptr});
    t.push_back({"voxi.device.meshShaderTier", VarType::U32, true, "0 = none, 1 = Tier 1",
        []{ return vU32(Renderer::get().deviceInfo().meshShaderTier); }, nullptr});
    t.push_back({"voxi.device.dxcAvailable", VarType::Bool, true, "Whether the DXIL compiler is present",
        []{ return vBool(Renderer::get().deviceInfo().dxcAvailable); }, nullptr});
}
#endif // AVER_MODULE_VOXI

// Post is per-DEVICE state (IDevice::postProcess/setPostProcess), not a process-wide singleton like
// voxi::Renderer -- but ConsoleVar::read is a zero-argument closure (called with no Engine& in scope
// from handleVarGet/handleVarsList, and from commitBatch's post-commit diff, which has only the
// ConsoleBatch). Rather than thread a device pointer through every one of those call sites, the
// console panel hands its (single, process-lifetime-stable once attached) device pointer to this file
// once a frame via setConsoleDevice() -- see drawConsole(Engine&) in SandboxApp.cpp, which is the only
// caller. A null device (no swapchain yet) makes every post.* read/stage a documented no-op, not a
// crash.
inline rhi::IDevice*& consoleDeviceSlot() { static rhi::IDevice* d = nullptr; return d; }
inline void setConsoleDevice(rhi::IDevice* d) { consoleDeviceSlot() = d; }
inline rhi::IDevice* consoleDevice() { return consoleDeviceSlot(); }

// setPostProcess DOES NO CLAMPING AT ALL (`post_ = p;` in both D3D12Device.cpp and VulkanCommon.hpp)
// -- the opposite asymmetry from Voxi's setSettings, which clamps unconditionally. So Post entries
// are the one place THIS TABLE clamps before ever calling setPostProcess, and reports the clamp
// through the same "requested vs now" diff commitBatch already produces for Voxi -- see ConsoleBatch's
// own comment. Ranges below are inferred from the field comments and defaults in RHI.hpp, not from any
// documented contract; PostSettings has none today.
//
// One entry per field, spelled out rather than driven by a `f32 PostSettings::*` table: a
// pointer-to-member table would still need one clamp range and one help string per field anyway, and
// autoExposure (bool, no clamp) does not fit the same shape as the ten f32 fields around it -- naming
// each field once is the smaller amount of machinery for eleven entries.
inline void registerPostVars(std::vector<ConsoleVar>& t) {
    auto stageClamped = [](f32 rhi::PostSettings::* mem, f32 lo, f32 hi) {
        return [mem, lo, hi](ConsoleBatch& b, VarValue v) {
            if (!b.seededPost) { b.post = b.device ? b.device->postProcess() : rhi::PostSettings{}; b.seededPost = true; }
            b.post.*mem = std::clamp(v.as.f, lo, hi);
            b.touchedPost = true;
        };
    };
    auto readField = [](f32 rhi::PostSettings::* mem) {
        return [mem]{ rhi::IDevice* d = consoleDevice(); return vF32(d ? d->postProcess().*mem : 0.0f); };
    };

    t.push_back({"post.exposure", VarType::F32, false, "Linear pre-tonemap radiance multiplier (clamped >= 0)",
        readField(&rhi::PostSettings::exposure), stageClamped(&rhi::PostSettings::exposure, 0.0f, 1e6f)});
    t.push_back({"post.bloomIntensity", VarType::F32, false, "Bloom contribution; 0 records no bloom pass at all (clamped >= 0)",
        readField(&rhi::PostSettings::bloomIntensity), stageClamped(&rhi::PostSettings::bloomIntensity, 0.0f, 1e6f)});
    t.push_back({"post.bloomThreshold", VarType::F32, false, "Luminance above which a pixel contributes to bloom (clamped >= 0)",
        readField(&rhi::PostSettings::bloomThreshold), stageClamped(&rhi::PostSettings::bloomThreshold, 0.0f, 1e6f)});
    t.push_back({"post.bloomKnee", VarType::F32, false, "Soft-knee width below the bloom threshold (clamped >= 0)",
        readField(&rhi::PostSettings::bloomKnee), stageClamped(&rhi::PostSettings::bloomKnee, 0.0f, 1e6f)});
    t.push_back({"post.autoExposure", VarType::Bool, false, "Eye adaptation from a luminance histogram, on/off",
        []{ rhi::IDevice* d = consoleDevice(); return vBool(d && d->postProcess().autoExposure); },
        [](ConsoleBatch& b, VarValue v){
            if (!b.seededPost) { b.post = b.device ? b.device->postProcess() : rhi::PostSettings{}; b.seededPost = true; }
            b.post.autoExposure = v.as.b; b.touchedPost = true;
        }});
    t.push_back({"post.exposureMin", VarType::F32, false, "Lower clamp on the auto-exposure multiplier (clamped >= 0)",
        readField(&rhi::PostSettings::exposureMin), stageClamped(&rhi::PostSettings::exposureMin, 0.0f, 1e6f)});
    t.push_back({"post.exposureMax", VarType::F32, false, "Upper clamp on the auto-exposure multiplier (clamped >= 0)",
        readField(&rhi::PostSettings::exposureMax), stageClamped(&rhi::PostSettings::exposureMax, 0.0f, 1e6f)});
    t.push_back({"post.exposureSpeed", VarType::F32, false, "Auto-exposure adaptation rate, in e-folds per second (clamped >= 0)",
        readField(&rhi::PostSettings::exposureSpeed), stageClamped(&rhi::PostSettings::exposureSpeed, 0.0f, 1e6f)});
    t.push_back({"post.exposureKey", VarType::F32, false, "Middle grey the average luminance is driven towards (clamped >= 0)",
        readField(&rhi::PostSettings::exposureKey), stageClamped(&rhi::PostSettings::exposureKey, 0.0f, 1e6f)});
    t.push_back({"post.histogramLowPercent", VarType::F32, false, "Fraction of the exposure histogram discarded at the dark end (clamped [0,1])",
        readField(&rhi::PostSettings::histogramLowPercent), stageClamped(&rhi::PostSettings::histogramLowPercent, 0.0f, 1.0f)});
    t.push_back({"post.histogramHighPercent", VarType::F32, false, "Fraction of the exposure histogram discarded at the bright end (clamped [0,1])",
        readField(&rhi::PostSettings::histogramHighPercent), stageClamped(&rhi::PostSettings::histogramHighPercent, 0.0f, 1.0f)});

    // Not an f32, so it cannot go through stageClamped above -- clamped by hand, same [0,2] shape.
    t.push_back({"post.tonemap", VarType::U32, false,
        "Which tone curve: 0 = per-channel Narkowicz/Hill (the default: gentlest toe, keeps dim bounce light visible), 1 = ACES matrixed, 2 = ACES on luminance only so hue/saturation survive any exposure (clamped to [0,2])",
        []{ rhi::IDevice* d = consoleDevice(); return vU32(d ? d->postProcess().tonemap : 0u); },
        [](ConsoleBatch& b, VarValue v){
            if (!b.seededPost) { b.post = b.device ? b.device->postProcess() : rhi::PostSettings{}; b.seededPost = true; }
            b.post.tonemap = std::min(v.as.u, 2u);
            b.touchedPost = true;
        }});
    t.push_back({"post.maxRadiance", VarType::F32, false, "Ceiling applied to scene radiance immediately before the tonemap; 0 disables it (clamped >= 0)",
        readField(&rhi::PostSettings::maxRadiance), stageClamped(&rhi::PostSettings::maxRadiance, 0.0f, 1e6f)});
}

// The path tracer's matched-environment legacy switch (contrast-fix plan F6/F7; root cause R5) --
// the SAME raw-slot idiom as consoleGiPoisonViewSlot()/consoleNrdLegacyCameraSlot() above, but
// DELIBERATELY NOT declared inside their AVER_MODULE_VOXI guard: aver::pt::PtSceneView's own
// registration and tier selection have to keep working with AVER_MODULE_VOXI off (see
// SandboxApp.cpp's own comment on ptSceneViewWantEnabled_ for why), and a slot that variable and
// SandboxApp.cpp's per-frame reassert both need cannot live inside a guard the path tracer does not
// share. Written by `set pt.legacyEnvironment` (registerRhiVars just below, the one variable table
// this file builds unconditionally) or seeded from --pt-legacy-env for a --frames capture with no
// console, and reasserted onto the live ptSceneView_ once a frame from SandboxApp.cpp's onUpdate via
// PtSceneView::setLegacyEnvironment.
inline bool& consolePtLegacyEnvSlot() { static bool v = false; return v; }

// Direct rhi::IDevice toggles that live OUTSIDE PostSettings entirely -- rhi.* rather than post.*,
// because they are not part of the camera post-processing chain PostSettings describes, they are raw
// per-device render state. Staged through ConsoleBatch::deviceSetters (a plain list of "call this on
// the device" closures) rather than through a seed-then-commit struct the way Post is: there is no
// struct backing these to seed, each one is a single immediate IDevice call, so batching only needs
// to defer WHEN it runs (until commit, so `set` stays all-or-nothing), not merge several fields into
// one call the way Post's setPostProcess(one struct) does.
inline void registerRhiVars(std::vector<ConsoleVar>& t) {
    t.push_back({"rhi.depthPrepass", VarType::Bool, false,
        "Same-frame depth-only pass ahead of the opaque colour walk, so an occluded fragment never reaches PSMainVoxi's shadow lookup/cone trace/fog -- off by default (identical to every render before this existed), on with --depth-prepass or here",
        []{ rhi::IDevice* d = consoleDevice(); return vBool(d && d->depthPrepassEnabled()); },
        [](ConsoleBatch& b, VarValue v){
            const bool on = v.as.b;
            b.deviceSetters.push_back([on](rhi::IDevice& d){ d.setDepthPrepassEnabled(on); });
        }});
    // pt.*, not rhi.*: registered here only because this is the one variable table this file builds
    // with NO module guard (see consolePtLegacyEnvSlot()'s own comment for why that matters for the
    // path tracer specifically) -- not because this is per-device render state the way
    // rhi.depthPrepass above is. ON reinstates root cause R5 for comparison only: every path-tracer
    // miss returns the reference sky (skyColor/averSkyPhysical) instead of the SAME SH sky the raster
    // ambient term and ReSTIR use (PtFrame gPtTrace.z). Default OFF: every INDIRECT miss, off any
    // lobe, reads that SH sky; camera rays use skyColor either way. (With kSkyIrradianceCalibration 1
    // since 2026-09-24 the two skies share a scale and differ only by the SH's L2 smoothing; under
    // the old 8 the reference sky was 8x dimmer.) See PtSceneView::setLegacyEnvironment.
    t.push_back({"pt.legacyEnvironment", VarType::Bool, false,
        "ON reinstates the pre-fix behaviour for comparison only: the path tracer's diffuse-bounce "
        "miss returns the unmatched reference sky instead of the same calibrated sky the raster "
        "ambient term uses (root cause R5; PtFrame gPtTrace.z). Default OFF matches the raster's "
        "diffuse sky lobe for lobe. Toggling either way re-arms the path tracer's accumulation.",
        []{ return vBool(consolePtLegacyEnvSlot()); },
        [](ConsoleBatch& b, VarValue v){
            const bool on = v.as.b;
            b.deviceSetters.push_back([on](rhi::IDevice&){ consolePtLegacyEnvSlot() = on; });
        }});
}

#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
// occlusion.* -- ONE entry, for OcclusionCullerImpl::debugForceWaitIdle_ (Occlusion.hpp's
// setDebugForceWaitIdle). Every OTHER occlusion setting is exactly the case this file's own comment
// above registerVoxiVars calls out and deliberately leaves out ("occlusion culling... PRIVATE
// SandboxApp state with no accessor on voxi::Renderer or rhi::IDevice, which is the only surface this
// table is allowed to reach") -- that reasoning still holds for occlusionCullEnabled_ itself, which
// stays out. This one is different in exactly the way that comment's own "real options" describe: it
// needs no call into SandboxApp or IOcclusionCuller at all, only a bool SandboxApp itself reasserts
// onto the live occluder_ every frame (onUpdate, the same idiom occlusionCullForceOff_ already uses to
// survive a manifest reload) -- so a raw bool slot this header owns, written by `set`/read by `get`
// and read back by SandboxApp once a frame, is enough, with none of the complete-type problem a
// consoleApp() mirroring setConsoleDevice() would have (SandboxApp is only forward-declared up top).
// Defaults to TRUE -- NOT the base interface's own default-off contract (IOcclusionCuller::
// setDebugForceWaitIdle's base is a false no-op for a hypothetical future implementer). A follow-up
// investigation found the no-wait path measurably worse (against a path-traced ground truth, and in
// a same-shader controlled A/B reproducing a "flat lighting" report) without finding which GI/
// lighting resource actually depends on the wait -- see OcclusionCuller.cpp's FOLLOW-UP comment
// above its kInFlight member. This pays the stall by default until that dependency is found and
// fixed narrowly; set false here (or launch with --no-occlusion-waitidle) to opt back into the
// faster, not-yet-proven-correct path. SandboxApp seeds this from occlusionDebugForceWaitIdleArg_
// (--occlusion-waitidle / --no-occlusion-waitidle) before the first read.
inline bool& consoleOcclusionForceWaitIdleSlot() { static bool v = true; return v; }

// occlusion.showCulled -- THE FALSE-CULL FINDER, added for the white-panel investigation's by-hand
// verification instrument (see the plan's section 3B). Every entity aver/game/SceneSubmission.hpp's
// chooseRoute() would otherwise route around drawMesh() -- frustum- or occlusion-culled, but NOT
// owner-hidden, exactly chooseRoute's own debug case -- is instead sent through the ordinary raster
// route with its colour tinted magenta, so what culling is skipping becomes VISIBLE instead of merely
// absent. Ray-driven shows the identical tint through inst.albedo, because the direct route shares
// deliver()'s look with the raster one by construction (that sharing is F1's whole point). At a still
// camera with this on, any magenta over a surface you can plainly see is a false cull.
// DEFAULT FALSE -- the OPPOSITE of debugForceWaitIdle's default above, and deliberately so: that one
// only changes a wait's timing, while this one changes the rendered image (a tinted draw is a
// different colour going into GI's voxelisation hash, so GI re-voxelises while this is on), which
// must never happen by accident just from opening the Console tab. Cost when off: one bool test per
// culled entity, the same shape chooseRoute already pays for the frustum/occlusion booleans.
inline bool& consoleOcclusionShowCulledSlot() { static bool v = false; return v; }

// occlusion.cullUnderSuppression -- keeps the occlusion test running even while a render feature
// (ray-driven Voxi, Path Tracing) has claimed the frame and is painting the scene itself
// (e.device()->sceneSuppressed()), which is exactly when SandboxApp's F8 gate would otherwise idle
// culling entirely: in ray-driven mode culling legitimately saves almost no work, because every
// culled entity still has to be submitted for primary rays, so F8 turns the test off there by
// default rather than pay HZB seed/reduce/test and a waitIdle for nothing. Without this override,
// showCulled above (and F1-F4's route-parity guarantee generally) would have nothing left to exercise
// in the one render mode PTTest actually runs in once F8 lands -- this is the only way to force the
// occlusion test to keep producing verdicts under suppression so those verdicts can be inspected.
// DEFAULT FALSE: RENDER.OCCLUSIONCULL and project_.occlusionCull are never written by this slot or by
// its reader -- it overrides only the idle F8 introduces, never the manifest's own on/off switch, so
// turning F8's idle back off stays an explicit, named choice made from the Console.
inline bool& consoleOcclusionCullUnderSuppressionSlot() { static bool v = false; return v; }

inline void registerOcclusionVars(std::vector<ConsoleVar>& t) {
    t.push_back({"occlusion.debugForceWaitIdle", VarType::Bool, false,
        "A-B SWITCH, DEFAULT TRUE: forces OcclusionCuller::testBatch() to end with the unconditional "
        "res.waitIdle() the buffer-rotation fix made unnecessary for correctness of THAT fix, but "
        "which a later investigation found something else in GI/lighting still depends on (root "
        "cause not yet found -- see Occlusion.hpp's setDebugForceWaitIdle comment). Flip off to try "
        "the faster, not-yet-proven-correct no-wait path live. Same effect as launching with "
        "--occlusion-waitidle / --no-occlusion-waitidle.",
        []{ return vBool(consoleOcclusionForceWaitIdleSlot()); },
        [](ConsoleBatch& b, VarValue v){
            const bool on = v.as.b;
            b.deviceSetters.push_back([on](rhi::IDevice&){ consoleOcclusionForceWaitIdleSlot() = on; });
        }});

    t.push_back({"occlusion.showCulled", VarType::Bool, false,
        "debug view; tinted draws change GI's colour hash, so GI re-voxelises while on. Every "
        "frustum- or occlusion-culled entity that is NOT owner-hidden draws through the raster route "
        "with its colour tinted magenta instead of being skipped -- ray-driven shows the same tint "
        "through inst.albedo, since both routes share one delivered look. Magenta over a surface you "
        "can plainly see, at a still camera, means a false cull. Off by default because it changes "
        "the rendered image, not only which work runs.",
        []{ return vBool(consoleOcclusionShowCulledSlot()); },
        [](ConsoleBatch& b, VarValue v){
            const bool on = v.as.b;
            b.deviceSetters.push_back([on](rhi::IDevice&){ consoleOcclusionShowCulledSlot() = on; });
        }});

    t.push_back({"occlusion.cullUnderSuppression", VarType::Bool, false,
        "Runs the occlusion test even while a render feature (ray-driven Voxi, Path Tracing) is "
        "painting the scene and would otherwise idle it -- the only way to exercise F1-F4's route "
        "parity in ray-driven mode once that idle is in effect. Overrides only the idle; never writes "
        "RENDER.OCCLUSIONCULL or project_.occlusionCull, so the manifest's own on/off switch is "
        "untouched either way.",
        []{ return vBool(consoleOcclusionCullUnderSuppressionSlot()); },
        [](ConsoleBatch& b, VarValue v){
            const bool on = v.as.b;
            b.deviceSetters.push_back([on](rhi::IDevice&){ consoleOcclusionCullUnderSuppressionSlot() = on; });
        }});
}
#endif

inline std::vector<ConsoleVar> buildVarTable() {
    std::vector<ConsoleVar> t;
#if AVER_MODULE_VOXI
    registerVoxiVars(t);
#endif
    registerPostVars(t);
    registerRhiVars(t);
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
    registerOcclusionVars(t);
#endif
    return t;
}
// Cached exactly like GraphNodeDefs.hpp's own catalog(): built once, on first call.
inline const std::vector<ConsoleVar>& allVars() {
    static const std::vector<ConsoleVar> table = buildVarTable();
    return table;
}

// Runs a staged batch through AT MOST TWO voxi::Renderer::setSettings calls (tier phase, then dial
// phase -- see ConsoleBatch's own comment for why each phase's snapshot is taken fresh, right here,
// rather than pre-built when the batch was staged), at most one IDevice::setPostProcess call, and
// then every staged rhi.* deviceSetters closure, then diffs every staged field's REQUESTED value
// against what actually landed, using each field's own `read()` -- one diff path for all three
// sources, since a field can move because the engine clamped it (Voxi), because this table clamped
// it before ever calling setPostProcess (Post), or -- so far, never, rhi.* fields being plain bools
// with nothing to clamp -- because a future rhi.* entry's own device call declines the request.
inline SetOutcome commitBatch(ConsoleBatch& b) {
    SetOutcome out;
#if AVER_MODULE_VOXI
    if (!b.tierSetters.empty()) {
        voxi::Settings snap = voxi::Renderer::get().settings();
        for (auto& fn : b.tierSetters) fn(&snap);
        voxi::Renderer::get().setSettings(snap);
    }
    if (!b.dialSetters.empty()) {
        // RE-READ here, not a snapshot taken when the batch was built: if the tier phase above just
        // ran, this is what makes its derivation visible to the dial phase instead of being clobbered
        // by a stale pre-tier-change copy. See ConsoleBatch's own comment for the full reasoning.
        voxi::Settings snap = voxi::Renderer::get().settings();
        for (auto& fn : b.dialSetters) fn(&snap);
        voxi::Renderer::get().setSettings(snap);
    }
#endif
    if (b.touchedPost && b.device) b.device->setPostProcess(b.post);
    if (b.device) for (auto& fn : b.deviceSetters) fn(*b.device);

    for (const auto& [name, requested] : b.requested) {
        const ConsoleVar* v = findVar(name);
        if (!v) continue;   // cannot happen (the name came from a successful findVar in `set`), but not fatal
        const VarValue actual = v->read();
        if (!valuesEqual(requested, actual))
            out.notes.push_back(name + ": requested " + formatValue(requested) + ", now " + formatValue(actual));
    }
    return out;
}

// =================================================================================================
// PART A: the console command registry (help, frametime, get, set, vars)
// =================================================================================================

// `print` writes into the console's OWN scrollback, a channel deliberately separate from the engine
// log (see SandboxApp.cpp's drawConsole for why) -- a handler that also wants engine-wide visibility
// calls AVER_INFO/AVER_WARN itself in addition. Returns true on success; a handler that fails MUST
// call print(LogLevel::Error, ...) itself before returning false, so the console never has to
// synthesize a generic error message for a specific failure it does not understand.
using ConsolePrint = std::function<void(LogLevel, std::string)>;
using ConsoleHandler = std::function<bool(SandboxApp& app, Engine& e,
                                           const std::vector<std::string>& args,
                                           const ConsolePrint& print)>;

struct ConsoleCommandDesc {
    std::string name;   // typed verbatim; looked up case-insensitively
    std::string help;   // one line, shown by `help`
    ConsoleHandler handler;
};

inline const std::vector<ConsoleCommandDesc>& consoleCatalog();

// A short, DISTINCT-from-`help` welcome banner for the transcript's OWN empty state (drawConsole in
// SandboxApp.cpp seeds these in whenever consoleLines_ is empty, including right after Clear) -- see
// WHAT TO BUILD item 4, "an empty console worth reading". Kept short on purpose: `help` below has the
// full command list and grammar, this only orients someone who has never typed anything here.
inline std::vector<std::string> consoleWelcomeLines() {
    return {
        "This is a live REPL over the engine's own render/post settings, not a log viewer.",
        "Every name it knows has a one-line description -- hover it anywhere it appears (as you",
        "type below, in a listing, or in this transcript) to see it, plus the type, the current",
        "value, and the valid range where one is known.",
        "",
        "Type 'help' for the full command list, grammar and worked examples -- or just start typing",
        "a name below and watch the suggestions.",
    };
}

// ---- handlers -----------------------------------------------------------------------------------

inline bool handleHelp(SandboxApp&, Engine&, const std::vector<std::string>&, const ConsolePrint& print) {
    const std::vector<ConsoleCommandDesc>& cat = consoleCatalog();
    std::size_t w = 0;
    for (const ConsoleCommandDesc& c : cat) w = std::max(w, c.name.size());
    for (const ConsoleCommandDesc& c : cat) {
        std::string line = c.name;
        line.resize(w, ' ');
        print(LogLevel::Info, line + "  -- " + c.help);
    }
    // GRAMMAR AND WORKED EXAMPLES, appended below the command list rather than replacing it -- the
    // "help that teaches" half of the console rework (WHAT TO BUILD item 4). Every example below
    // names a variable that is genuinely in allVars() right now, so copy-pasting one always runs
    // rather than teaching a name this table used to have.
    print(LogLevel::Info, "");
    print(LogLevel::Info, "Names are dotted (voxi.giCones, post.exposure, rhi.depthPrepass) and case-insensitive.");
    print(LogLevel::Info, "Values: a whole number, a decimal, a bool (true/false, 1/0, on/off), or a");
    print(LogLevel::Info, "quality name (off/low/medium/high/epic, or 0-4).");
    print(LogLevel::Info, "Examples:");
    print(LogLevel::Info, "  get voxi.giCones                 read one variable");
    print(LogLevel::Info, "  set voxi.giCones 6                write one variable");
    print(LogLevel::Info, "  set voxi.msaa 4 post.tonemap 2    write several at once, all-or-nothing");
    print(LogLevel::Info, "  vars gi                           list every variable whose name or");
    print(LogLevel::Info, "                                    description mentions 'gi', grouped");
    print(LogLevel::Info, "A [live] tag on a value means it reads back what the engine is ACTUALLY doing,");
    print(LogLevel::Info, "which the request behind it may not have gotten (see that variable's own");
    print(LogLevel::Info, "description). A [reload] tag means a project reload is needed before a change");
    print(LogLevel::Info, "takes effect. Both are explained in full in that name's own tooltip.");
    print(LogLevel::Info, "Not everything the editor can toggle is here yet: occlusion culling and");
    print(LogLevel::Info, "virtualized-geometry LOD selection are deliberately absent (see this file's");
    print(LogLevel::Info, "own EditorConsole.hpp comment on registerVoxiVars for why).");
    return true;
}

// The headline command. Walks IDevice::gpuTiming()'s flat, parent-indexed node list as an indented
// tree -- inclusive ms (what the node's own report), exclusive ms (inclusive minus the sum of direct
// children, i.e. the pass's own time), and percent of the GPU total -- then prints the GPU total
// against this instant's CPU frame time so GPU-bound vs CPU-bound is visible at a glance. Both
// no-data axes are reported as DIFFERENT SENTENCES, not as zeros that could be mistaken for a
// measurement -- see GpuTimingReport's own comment for why the two axes exist.
inline bool handleFrameTime(SandboxApp&, Engine& e, const std::vector<std::string>&, const ConsolePrint& print) {
    if (!e.device()) { print(LogLevel::Error, "No render device is attached."); return false; }
    // ONE FORMATTER, shared with the packaged game (aver/rhi/GpuTimingFormat.hpp). It used to live
    // here, in a header the game cannot include -- which is the whole reason a shipped AverGame.exe
    // could not show timings it was already paying to collect. Moving it beside the report it
    // formats means the two hosts cannot drift into printing different things from the same data.
    const rhi::GpuTimingReport r = e.device()->gpuTiming();
    const f64 gpuTotalMs = rhi::formatGpuTiming(
        r, [&](const std::string& line) { print(LogLevel::Info, line); });
    if (!r.supported || r.nodes.empty() || r.framesAccumulated == 0) return true;

    // GPU total here is the sum of TOP-LEVEL marked spans only (mirrors collectGpuTiming's own
    // topLevelMs, which excludes anything unmarked); CPU frame is THIS INSTANT's e.time().dt, not an
    // average. The two are deliberately not claimed to be a matched pair -- see the caveat printed
    // alongside them -- but a GPU total that tracks well below the CPU frame time says CPU-bound, and
    // one that tracks close to or above it says GPU-bound, which is the whole point of printing both.
    char tot[256];
    std::snprintf(tot, sizeof tot,
                  "GPU total (marked passes): %.2fms  |  CPU frame (this instant): %.2fms -- a rough "
                  "GPU-bound/CPU-bound signal, not a matched pair (different sources, different moments).",
                  gpuTotalMs, static_cast<f64>(e.time().dt) * 1000.0);
    print(LogLevel::Info, tot);
    return true;
}

inline bool handleVarGet(SandboxApp&, Engine&, const std::vector<std::string>& args, const ConsolePrint& print) {
    if (args.empty()) { print(LogLevel::Error, "Usage: get <name>"); return false; }
    const ConsoleVar* v = findVar(args[0]);
    if (!v) { print(LogLevel::Error, "Unknown variable '" + args[0] + "'. Type 'vars' for a list."); return false; }
    std::string line = v->name + " = " + formatValue(v->read());
    if (v->readOnly) line += "  (read-only)";
    const char* tag = varHonestyTag(varHonesty(*v));
    if (*tag) line += std::string("  ") + tag;
    if (!v->help.empty()) line += "  -- " + v->help;
    print(LogLevel::Info, line);
    return true;
}

// `vars` (no args): EXACTLY the original output, unchanged line-for-line -- WHAT TO BUILD says the
// existing command "should stay working exactly as they do", and this is the form anything already
// relying on today's shape (a screenshot script's grep, a person's muscle memory) depends on.
// `vars <text>` (WHAT TO BUILD item 3, the new half): a substring FILTER over name-or-description,
// grouped by prefix (see varGroupKey's own comment) with a [live]/[reload] honesty tag inline -- "vars
// gi" finds every voxi.gi* dial under one heading instead of scattered among fifty other lines, and
// "vars firefly" (a word no variable's NAME contains) still finds anything whose DESCRIPTION does.
inline bool handleVarsList(SandboxApp&, Engine&, const std::vector<std::string>& args, const ConsolePrint& print) {
    if (args.empty()) {
        for (const ConsoleVar& v : allVars()) {
            std::string line = v.name + " = " + formatValue(v.read());
            if (v.readOnly) line += "  (read-only)";
            if (!v.help.empty()) line += "  -- " + v.help;
            print(LogLevel::Info, line);
        }
        return true;
    }
    std::string query;
    for (std::size_t i = 0; i < args.size(); ++i) { if (i) query += ' '; query += args[i]; }
    std::string lastGroup;
    std::size_t shown = 0;
    for (const ConsoleVar& v : allVars()) {
        if (!varMatchesQuery(v, query)) continue;
        const std::string group = varGroupKey(v.name);
        if (group != lastGroup) { print(LogLevel::Info, "-- " + varGroupLabel(group) + " --"); lastGroup = group; }
        std::string line = "  " + v.name + " = " + formatValue(v.read());
        if (v.readOnly) line += "  (read-only)";
        const char* tag = varHonestyTag(varHonesty(v));
        if (*tag) line += std::string("  ") + tag;
        if (!v.help.empty()) line += "  -- " + v.help;
        print(LogLevel::Info, line);
        ++shown;
    }
    if (shown == 0) print(LogLevel::Warn, "No variable's name or description contains '" + query + "'.");
    return true;
}

// `set name value [name value ...]`: all-or-nothing. Every pair is looked up, checked for
// read-only, and parsed to its declared type BEFORE anything is staged -- a bad token anywhere in
// the line (an unknown name, a read-only name, or a value that does not parse) aborts the WHOLE
// line with one specific error and stages nothing, so a typo in the third pair can never half-apply
// the first two.
inline bool handleVarSet(SandboxApp&, Engine& e, const std::vector<std::string>& args, const ConsolePrint& print) {
    if (args.size() < 2 || args.size() % 2 != 0) {
        print(LogLevel::Error, "Usage: set <name> <value> [<name> <value> ...]");
        return false;
    }

    struct Staged { const ConsoleVar* var; VarValue value; };
    std::vector<Staged> staged;
    staged.reserve(args.size() / 2);

    for (std::size_t i = 0; i + 1 < args.size(); i += 2) {
        const std::string& name = args[i];
        const std::string& raw  = args[i + 1];
        const ConsoleVar* v = findVar(name);
        if (!v) {
            print(LogLevel::Error, "Unknown variable '" + name + "'. Type 'vars' for a list. Nothing was set.");
            return false;
        }
        if (v->readOnly) {
            print(LogLevel::Error, "'" + v->name + "' is read-only: reflects the device, not a request. Nothing was set.");
            return false;
        }
        VarValue val;
        std::string err;
        bool ok = false;
        switch (v->type) {
            case VarType::U32:     { u32 n = 0;  ok = parseU32(raw, n, err);     val = vU32(n);      break; }
            case VarType::F32:     { f32 f = 0;  ok = parseF32(raw, f, err);     val = vF32(f);      break; }
            case VarType::Bool:    { bool b = false; ok = parseBool(raw, b, err); val = vBool(b);    break; }
            case VarType::Quality: { u32 q = 0;  ok = parseQuality(raw, q, err); val = vQualityRaw(q); break; }
            case VarType::Str:     ok = false; err = "'" + v->name + "' has no settable representation"; break;
        }
        if (!ok) {
            print(LogLevel::Error, "'" + raw + "' for " + v->name + ": " + err + ". Nothing was set.");
            return false;
        }
        if (v->validate && !v->validate(val, err)) {
            print(LogLevel::Error, v->name + ": " + err + ". Nothing was set.");
            return false;
        }
        staged.push_back({v, val});
    }

    ConsoleBatch batch;
    batch.device = e.device();
    for (const Staged& s : staged) {
        s.var->stage(batch, s.value);
        batch.requested.push_back({s.var->name, s.value});
    }
    const SetOutcome outcome = commitBatch(batch);

    // One line per field: the clamp/derivation note if commitBatch produced one for it (the "say so,
    // not silently" case), otherwise a plain confirmation that it landed exactly as asked.
    for (const Staged& s : staged) {
        const std::string prefix = s.var->name + ":";
        bool noted = false;
        for (const std::string& note : outcome.notes) {
            if (note.rfind(prefix, 0) == 0) { print(LogLevel::Warn, note); noted = true; break; }
        }
        if (!noted) {
            std::string line = s.var->name + " = " + formatValue(s.var->read());
            const char* tag = varHonestyTag(varHonesty(*s.var));
            if (*tag) line += std::string("  ") + tag;
            print(LogLevel::Info, line);
        }
    }
    return true;
}

namespace detail {
inline std::vector<ConsoleCommandDesc> buildConsoleCatalog() {
    std::vector<ConsoleCommandDesc> t;
    t.push_back({"help", "List every console command, the grammar, and worked examples", &handleHelp});
    t.push_back({"frametime", "Print the current per-pass GPU frame-time breakdown", &handleFrameTime});
    t.push_back({"get", "get <name> -- print an engine variable's current value", &handleVarGet});
    t.push_back({"set", "set <name> <value> [<name> <value> ...] -- set one or more engine variables", &handleVarSet});
    t.push_back({"vars", "vars [text] -- list every variable, or filter by name/description and group by prefix", &handleVarsList});
#if AVER_MODULE_VOXI
    // ---- per-history reset commands: bisect a burned-in ReSTIR-GI/RT/NRD artifact by hand, one
    // history at a time, without a resize (a resize's own "cure" is a same-shape flag flip -- see
    // VoxiRenderer::resetGiHistory's own comment). Each raises a one-shot request on voxi::Renderer
    // (Voxi.hpp), consumed next frame by SandboxApp.cpp right beside voxiRenderer_.setSettings(vs)
    // and forwarded to the matching VoxiRenderer::reset*History() method. TRY resetgihistory FIRST --
    // see each command's own help string for why, and Part 3(a) of this task's own notes for the
    // full order. The voxel-cone GI volume needs none of these: voxelizePass clears before every
    // injection (VoxiRenderer.cpp), so it cannot accumulate poison across rebuilds the way a
    // ping-ponged history can.
    t.push_back({"resetgihistory",
        "Invalidate the ReSTIR-GI reservoir + surface history (voxi.giMode 1 only) on the next frame "
        "-- the same thing a viewport resize does to this one resource, without resizing anything. "
        "TRY THIS FIRST if a GI blotch/burn-in appears: it is the resource this task's own "
        "investigation found the concrete NaN-storage gap in.",
        [](SandboxApp&, Engine&, const std::vector<std::string>&, const ConsolePrint& print) -> bool {
            voxi::Renderer::get().requestGiHistoryReset();
            print(LogLevel::Info, "Requested: GI ReSTIR reservoir history reset. Watch the Output "
                                   "Log for '[Voxi] GI ReSTIR reservoir history reset' next frame.");
            return true;
        }});
    t.push_back({"resetrthistory",
        "Invalidate RT shadow + reflection + sky-occlusion history on the next frame (all three share "
        "one validity flag today) -- the same thing a resize or an RT-tier toggle does. Try this if "
        "resetgihistory alone did not clear the artifact.",
        [](SandboxApp&, Engine&, const std::vector<std::string>&, const ConsolePrint& print) -> bool {
            voxi::Renderer::get().requestRtHistoryReset();
            print(LogLevel::Info, "Requested: RT shadow/reflection/AO history reset. Watch the "
                                   "Output Log for '[Voxi] RT temporal history reset' next frame.");
            return true;
        }});
    t.push_back({"resetaohistory",
        "Invalidate AO/sky-occlusion history -- CURRENTLY IDENTICAL to resetrthistory (no independent "
        "validity flag exists yet; see that command's own log line for why).",
        [](SandboxApp&, Engine&, const std::vector<std::string>&, const ConsolePrint& print) -> bool {
            voxi::Renderer::get().requestAoHistoryReset();
            print(LogLevel::Info, "Requested: AO history reset (also resets RT shadow/reflection -- "
                                   "shared flag, see help resetrthistory).");
            return true;
        }});
    t.push_back({"resetnrdhistory",
        "Force NVIDIA NRD/REBLUR to throw away its own internal temporal history on the next frame, "
        "without a resize -- uses NRD's own resetHistory contract (NrdRecorder::forceHistoryReset). "
        "Try this if neither resetgihistory nor resetrthistory cleared the artifact.",
        [](SandboxApp&, Engine&, const std::vector<std::string>&, const ConsolePrint& print) -> bool {
            voxi::Renderer::get().requestNrdHistoryReset();
            print(LogLevel::Info, "Requested: NRD history reset. Watch the Output Log for "
                                   "'[NRD] history reset' next frame.");
            return true;
        }});
    t.push_back({"resetallhistory",
        "Run every reset* command above in one call: GI reservoir, RT shadow/reflection/AO, and NRD. "
        "Do NOT run this FIRST during a bisection -- it clears everything at once and tells you "
        "nothing about which buffer was actually poisoned; try the individual commands one at a time "
        "first. The voxel-cone GI volume needs none of these -- it clears itself every rebuild.",
        [](SandboxApp&, Engine&, const std::vector<std::string>&, const ConsolePrint& print) -> bool {
            voxi::Renderer::get().requestGiHistoryReset();
            voxi::Renderer::get().requestRtHistoryReset();
            voxi::Renderer::get().requestNrdHistoryReset();
            print(LogLevel::Info, "Requested: GI + RT/shadow/reflection/AO + NRD history reset, all "
                                   "next frame. Watch the Output Log for three separate lines.");
            return true;
        }});
#endif
    return t;
}
} // namespace detail

// Cached exactly like GraphNodeDefs.hpp's own catalog() at its line ~1069: built once, on first
// call, which happens well after main() so there is no static-init-order hazard even though
// `help`'s own handler calls this same function to enumerate every entry including itself.
inline const std::vector<ConsoleCommandDesc>& consoleCatalog() {
    static const std::vector<ConsoleCommandDesc> table = detail::buildConsoleCatalog();
    return table;
}

// Case-insensitive lookup, same convention as findVar/graphNodeCatalog's own lookup. Returns
// nullptr for a name this catalog does not know; the caller (drawConsole's Enter handler) is the
// ONE place that synthesizes an "unknown command" line, because by definition no handler exists yet
// to say it more specifically.
inline const ConsoleCommandDesc* findCommand(std::string_view name) {
    for (const ConsoleCommandDesc& c : consoleCatalog()) if (detail::ciEquals(c.name, name)) return &c;
    return nullptr;
}

// Substring search over a command, name or one-line help -- the command-table twin of varMatchesQuery
// above, used by the live suggestion popup (SandboxApp.cpp) so typing a fragment of what a command
// DOES ("frame" for frametime's "frame-time breakdown") finds it same as typing its name would.
inline bool commandMatchesQuery(const ConsoleCommandDesc& c, std::string_view query) {
    return detail::ciContains(c.name, query) || detail::ciContains(c.help, query);
}

} // namespace aver::editor
