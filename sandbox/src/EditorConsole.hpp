#pragma once
// Console command registry + live-variable registry for the editor's Console drawer tab. Mirrors
// GraphNodeDefs.hpp's shape (data table + push_back-per-line builder): adding a command or variable
// is one call in a builder function below, not a hunt through SandboxApp.cpp. Header-only for the
// same reason: SandboxApp.cpp is the only TU that includes this and may not touch CMakeLists.txt, so
// there is nowhere to register a second .cpp. Every function below is `inline`.
//
// Command table (help/frametime/get/set/vars) and variable table (voxi.*/post.*/rhi.*) share one
// file deliberately: get/set/vars are commands that exist only to walk the variable table.
//
// SCOPE: only reads/writes voxi::Renderer's process-wide settings, one rhi::IDevice's post-process
// settings, and a few already-public rhi::IDevice toggles outside PostSettings (rhi.depthPrepass so
// far) -- all already-public surfaces (Voxi.hpp, RHI.hpp). Nothing here adds a member to SandboxApp
// beyond what SandboxApp.cpp declares for the console panel itself: PRIVATE SandboxApp state with no
// accessor on voxi::Renderer/rhi::IDevice (occlusion culling, virtualized-geometry LOD selection)
// stays out -- see registerVoxiVars' comment.

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
// Case-insensitive equality (mirrors GraphNodeDefs.hpp's own ciEquals; not shared, to avoid a
// cross-file dependency for four lines). Used for both command names and variable names.
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
// Case-insensitive substring test: completion and the variable browser need "name or description
// CONTAINS what was typed", not equality -- a word from a description should still find the entry.
// O(n*m); callers are UI lists of a few dozen entries re-filtered per keystroke, not a hot loop.
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

// Tagged union of every scalar type a Settings/PostSettings field is. `Str` is a fifth case for
// read-only diagnostic text (voxi.status.*/voxi.device.*, e.g. Renderer::statusText) that no
// numeric type can hold; kept as a plain second field rather than folded into the union, since
// std::string is non-trivial and a hand-written destructor isn't worth it for the few entries using it.
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

// Formats a VarValue for get/set/vars output; reuses Renderer::qualityName rather than a new name table.
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

// Parsed as SIGNED first so a negative literal is reported as itself, not wrapped into a huge
// unsigned number -- the same trap SandboxApp.cpp's --rt-rays handling documents (a wrapped
// "4294967291 shadow rays clamped to 32" names the cast, not the typo that caused it).
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
// Accepts a case-insensitive name (off/low/medium/high/epic) or a bare 0-4, mirroring --gi and the
// manifest's giQuality.
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
// Setter closures, not two pre-seeded voxi::Settings structs: a dial-phase struct seeded when the
// batch is BUILT would carry stale tier fields once a tier-phase commit ran first, and setSettings
// detects a tier change by diffing incoming vs LIVE settings_ -- a stale tier field would read as a
// second tier change and re-derive, silently overwriting the dial values this exists to protect.
// Closures let commitBatch snapshot Renderer::get().settings() fresh at each phase's own commit --
// tier-phase from whatever was live before, dial-phase from whatever the tier-phase just derived.
struct ConsoleBatch {
    std::vector<std::function<void(void*)>> tierSetters;   // void* is a voxi::Settings*; see below
    std::vector<std::function<void(void*)>> dialSetters;
    rhi::PostSettings post;
    bool seededPost = false;
    bool touchedPost = false;
    // rhi.* vars are a single immediate IDevice call, not a struct field like `post` -- see
    // registerRhiVars' comment. Runs at commit, after Voxi and Post, so a mixed-namespace line
    // still applies all-or-nothing.
    std::vector<std::function<void(rhi::IDevice&)>> deviceSetters;
    rhi::IDevice* device = nullptr;
    // (dotted name, requested value), filled by `set` after each stage() call -- feeds the
    // post-commit diff report from one place regardless of source (Voxi, Post).
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
// DISCOVERABILITY: tooltips, grouping, and substring search over the table above -- a tool someone
// can learn FROM rather than one they must already know. Every function here reads ConsoleVar's
// existing fields (help, readOnly, read()) and adds none; the gap was never the data, only that
// nothing besides get/vars showed it. Kept beside the table for the same reason
// the table itself lives here, not in SandboxApp.cpp (see this file's header comment).
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

// Three honesty states a value can be in: voxi.giMode/voxi.denoiser read back what is ACTUALLY
// running (LiveTruth), voxi.layeredBsdf is NOT live until a reload (NotLive). Detected from the
// same help-text keywords a tooltip already shows ("ACTUALLY running" on voxi.giMode, "skips itself"
// on voxi.denoiser, "NOT live" on voxi.layeredBsdf) rather than a new per-entry field only three
// entries would ever set. "silently refus[ed/es]" is included pre-emptively so a future entry worded
// the same way is caught without touching this again.
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

// The valid range/enum a name's own description or validate() already documents -- not a second
// source of truth to keep in sync. Bool/Quality use their fixed vocabulary; others try validate()'s
// error message (an out-of-range probe is free, it stages nothing) then a help-text parenthetical
// containing "clamp" ("(engine clamps to [1,16])", "(no engine clamp)", etc). Empty when neither
// applies -- voxi.giMode/voxi.denoiser, where varHonesty already covers why "valid range" isn't the
// right question (any u32/bool is accepted; what the engine does with it is what matters).
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

// Group label for the browsable listing: first two dotted segments when there are 3+ (so
// voxi.status.*/voxi.device.* get their own heading instead of drowning in voxi.*'s ~20 dials),
// else the first segment. Generic on purpose -- a new voxi.foo.bar namespace needs no change here.
inline std::string varGroupKey(const std::string& dotted) {
    const std::size_t first = dotted.find('.');
    if (first == std::string::npos) return dotted;
    const std::size_t second = dotted.find('.', first + 1);
    return second == std::string::npos ? dotted.substr(0, first) : dotted.substr(0, second);
}
inline std::string varGroupLabel(const std::string& groupKey) { return groupKey + ".*"; }

// Substring match (not prefix) over name OR description, so "firefly" finds a dial even when it is
// not in the dotted name. Empty query matches everything (the unfiltered vars/browser state).
inline bool varMatchesQuery(const ConsoleVar& v, std::string_view query) {
    return detail::ciContains(v.name, query) || detail::ciContains(v.help, query);
}

// One tooltip body shared by the completion popup, browser and transcript hover, so the three
// cannot drift. Multi-line: what it is, its live value, its valid range, and any [live]/[reload] note.
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
// ReSTIR-GI poison debug view's live source of truth (same shape as consoleOcclusionForceWaitIdleSlot()
// below; registerOcclusionVars' comment has the full reasoning). VoxiRenderer::setGiPoisonView is private
// renderer state with no path through voxi::Renderer::Settings/setSettings, so it cannot be staged
// as an ordinary dial. Raw-slot idiom: a bool this header owns, written by `set voxi.giPoisonView`
// and reasserted onto voxiRenderer_ once a frame from SandboxApp.cpp's onUpdate -- the same
// "console sets the seed, a per-frame reassert makes it live" shape occlusion.debugForceWaitIdle uses.
inline bool& consoleGiPoisonViewSlot() { static bool v = false; return v; }

// voxi.debugResetHistoryEveryFrame's live source of truth (same raw-slot idiom as
// consoleGiPoisonViewSlot() above). Bisection aid, never saved: SandboxApp's onUpdate resets every
// history named here once a frame, so anything that still lags, smears or fades is not carried by
// that history. Bit 1 = ReSTIR GI reservoirs + GI visibility history, 2 = RT shadow/reflection/
// sky-occlusion history (also restarts NRD via beginShadowHistory, which resets it whenever
// rtHistValid_ is false), 4 = NRD alone.
inline u32& consoleResetHistoryEveryFrameSlot() { static u32 v = 0; return v; }

// NRD legacy-camera A/B switch's live source of truth (same raw-slot idiom as
// consoleGiPoisonViewSlot() above): VoxiRenderer::setNrdLegacyCamera is private renderer state (a
// toggle on VoxiRenderer::beginShadowHistory's camera-factorisation path) with no Settings path.
// Written by `set voxi.nrdLegacyCamera`, reasserted onto voxiRenderer_ each frame from onUpdate.
inline bool& consoleNrdLegacyCameraSlot() { static bool v = false; return v; }

// Lighting-contrast fix's legacy bitmask (contrast-fix plan F7; same raw-slot idiom as above). ONE
// u32 backs FIVE console variables below (voxi.legacyRestirSampleRing etc.), each flipping a single
// bit so one root cause can be A/B'd at a time. Seeded whole from --lighting-legacy for a --frames
// capture; reasserted each frame from onUpdate. Bit table: VoxiRenderer::setLightingLegacyBits.
inline u32& consoleLightingLegacySlot() { static u32 v = 0; return v; }

// Three engine-optimisation-plan measurement dials (M1-M4/W3/W12; same raw-slot idiom as above).
// setGiForceRebuild/setGiBoundedDispatch/setGiFreeAccumulator are public (unlike giPoisonView) but
// none round-trips through Settings/setSettings. Written by their `set voxi.gi*` command, reasserted
// each frame from onUpdate -- see each setter's own header comment (VoxiRenderer.hpp).
inline bool& consoleGiForceRebuildSlot()    { static bool v = false; return v; }
// Default TRUE (measured, not a dial nobody turned): this per-frame reassert is the editor's real
// default; VoxiRenderer.hpp's member initialiser (also true now, for hosts like the Runtime that
// never call the setter) is a separate default, not this one. Off: every GI rebuild clears, resolves
// and mip-filters the whole 512^3 grid to touch the ~1.3% it changed (30.64->30.25ms; census line and
// pixel-neutrality check alongside giBoundedDispatch_ in VoxiRenderer.hpp).
inline bool& consoleGiBoundedDispatchSlot() { static bool v = true; return v; }
// Default TRUE (gi-memory): same "this reassert is the editor's real default" note as
// consoleGiBoundedDispatchSlot() above -- VoxiRenderer.hpp's own member initialiser agrees now, but
// this is the value the editor actually runs with every frame. Off keeps the injection accumulator
// (2048 MiB at Epic's 512) allocated for the whole session regardless of how long GI sits idle.
inline bool& consoleGiFreeAccumulatorSlot() { static bool v = true; return v; }

// optimisation-wave-2's U1 path-debug view (2.10 I; same raw-slot idiom as consoleGiPoisonViewSlot()
// above): VoxiRenderer::setGiVisPathView is private, no Settings path. Paints F2's resolved path
// (traced/reconstructed/half-res/no-ray) over indirect diffuse, so a Half-mode run can be checked by
// eye for whether it actually reconstructs. Written by `set voxi.giVisPathView` or seeded from
// --gi-vis-path-view; reasserted each frame from onUpdate. Default OFF: the paint replaces indirect
// diffuse, so it must never happen just from opening the Console tab.
inline bool& consoleGiVisPathViewSlot() { static bool v = false; return v; }

// W6/M5's pricing switch (optimisation-wave-2 4(a); same raw-slot idiom, setBlendedGiCone public but
// no Settings path). false (default) shades a blended fragment from ReSTIR (W6, today's image); true
// forces the pre-existing cone-gather fallback so ReSTIR's share of the "blended replay" span
// (D3D12Device.cpp:5073) can be priced by difference. Written by `set voxi.blendedGiCone` or
// --blended-gi restir|cone; reasserted each frame from onUpdate.
inline bool& consoleBlendedGiConeSlot() { static bool v = false; return v; }

// Tier fields (msaa, globalIllumination, rayTracing, pathTracing, meshShaders) go through
// tierSetters; everything setSettings derives FROM a tier goes through dialSetters -- see
// ConsoleBatch's comment for why the split is closures, not two struct copies.
//
// DELIBERATELY ABSENT: occlusion culling and virtualized-geometry LOD selection
// (SandboxApp::occlusionCullEnabled_/lodSelectEnabled_/setLodSelect). Both are toggled live by the
// editor's Rendering panel, but as PRIVATE SandboxApp state with no accessor on voxi::Renderer or
// rhi::IDevice (this file's SCOPE). Reaching them would mean making them public on SandboxApp, or a
// per-frame consoleApp() slot mirroring setConsoleDevice() below -- both a SandboxApp.cpp change,
// out of scope here -- left out rather than routed around, so the gap stays visible.
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
        // Retuned ladder (QualityLadder.hpp): rtShadowRays 1/1/4/8, rtShadowDenoise 2/2/1/1,
        // rtRenderMode 0/1/1/1 at Low/Medium/High/Epic (rtPixelsPerRayTile flat at 1). Low rasterises
        // primary visibility by product decision (D3), not a hardware gap.
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
    // UE-style Overall Quality preset (D2, Scalability.hpp), reachable from the console like the
    // Rendering page's Overall row. Staged as a TIER setter (not dial), even though applyOverall also
    // writes the derived knobs directly: it must run in the FIRST commitBatch phase, or
    // `set voxi.scalability 4 voxi.rtShadowRays 2` would race the explicit rtShadowRays 2 the way a
    // mixed tier/knob setSettings call used to (SandboxApp.cpp's take(), N7). commitBatch's existing
    // fresh re-read before the dial phase (see its own comment) is what makes the example land on
    // rtShadowRays 2, not the ladder's 8, with no extra code needed.
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

    // ---- read-only: sits beside the tier fields (same Quality type) but is NOT one -- must not be
    // staged like one (Settings::layeredBsdf's own comment has the full reason). VoxiRenderer builds
    // its raster PSOs once at init from this value, so a `set`
    // here would update settings_ but change nothing on screen until a project reload -- the same
    // "lies about what is running" voxi.giMode's honesty exists to prevent, with no live value to
    // catch it. Read-only keeps get/vars truthful: this is next load's tier, not the live one.
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
    // comment has the full writeup). Voxi.cpp clamps any request above 2 down to 1; validate() still
    // refuses it up front so a bad `set` reports its OWN mistake, not the substituted number -- why
    // every voxi.* validate here exists. The engine does NOT clamp a RayTraced (2) request that RT
    // hardware/tier cannot honour -- that's resolve() (RenderSettingsResolver.hpp), which giMode and
    // denoiser read through but this var does not: it reads the RAW requested field, matching what
    // setSettings stores and how the UI control (buildRenderingSettings) shows the same distinction.
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
    // The switch between voxel-cone and RTXDI ReSTIR GI diffuse-bounce estimators (Settings::giMode's
    // comment has the full writeup). NOT a tierSetter despite reading like one -- setSettings never
    // derives it from a tier, and (since the settings-separation pass) no longer even range-checks
    // it, so it belongs with the dials setSettings leaves alone.
    // The read closure no longer reads the raw field (F-d, settings-separation pass): setSettings
    // used to hard-clamp giMode back to 0 when RayQuery/RT-tier/GI-tier could not honour it, which is
    // what let a raw read double as "is it actually running"; that clamp is gone, so this now reads
    // resolve(...).giMode.effective (RenderSettingsResolver.hpp) for the same honesty -- `set
    // voxi.giMode 1` on unsupported hardware stages/commits 1, and this reads back 0.
    t.push_back({"voxi.giMode", VarType::U32, false,
        "Which estimator answers the diffuse GI bounce: 0 = voxel cone gather (default), 1 = RTXDI ReSTIR GI. Needs RayQuery hardware, rayTracing != Off and globalIllumination != Off -- resolves back to 0 (the stored request is kept, untouched) when any is missing, and this always reads back what is ACTUALLY running, not merely what was last requested",
        []{ const Renderer& r = Renderer::get(); return vU32(voxi::resolve(r.settings(), r.deviceInfo()).giMode.effective); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->giMode = n; }); }});
    // optimisation-wave-2's U1: how much of F2 (candidate-hit sky) and F3 (reuse visibility) -- the
    // contrast fix's two per-pixel rays, cb4b48df -- giMode 1 pays for at this GI tier. A tier-derived
    // DIAL like voxi.giCones above, re-derived by `set voxi.globalIllumination <tier>` unless set in
    // the same line.
    // Reads the RAW field, not resolve()'s effective value (unlike voxi.giMode above):
    // Resolution::giRestirVisibility.effective deliberately EQUALS requested always (clamping to 0 on
    // a failed prerequisite would read "no ray" while no ReSTIR runs at all), so raw and effective
    // always agree here, unlike voxi.refractionMode's, and reading raw skips the extra resolve() call.
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
    // The two dials the fade bisection left standing -- everything else it tried was removed by its
    // own measurement: the moving-camera reservoir-age/history caps made the overshoot WORSE, the two
    // reuse-similarity tolerances matched their defaults, bias-correction and the Jacobian dial changed
    // nothing, and a wave-local boiling filter cost 13% of settled brightness without fixing the
    // overshoot. Full chain and numbers: Settings::giRestirMaxHistory (Voxi.hpp). Ordinary dialSetters
    // entries, real Settings fields like voxi.giRestirVisibility above, not console-only overrides.
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
    // NOT a Settings field -- see consoleGiPoisonViewSlot()'s comment for the raw-slot idiom. Seven
    // of the eight colours below are giMode 1 (ReSTIR) only (voxi_restir.hlsli's giRestirIndirect,
    // never called by giMode 0). The eighth, VIOLET (B1/F5), is NOT giMode-gated: painted in
    // voxi.hlsl's PSMainVoxi/PSRayDriven over the ray-traced specular indirect term's own
    // AVER_VOX_MAXRAD ceiling hit, which exists under either diffuse estimator. Precedence between
    // the two families: aver_IsGiRestirPoisonColour's comment (voxi.hlsl, above PSMainVoxi).
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
    // optimisation-wave-2's U1 path-debug view (2.10 I; see consoleGiVisPathViewSlot()'s comment for
    // the raw-slot idiom). Colour mapping is in the help string below. Suppressed while
    // voxi.giPoisonView is also on (it answers a different, higher-priority question -- corruption,
    // not which visibility path ran -- and paints first).
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
    // NOT a Settings field, same shape as voxi.giPoisonView above -- see consoleNrdLegacyCameraSlot()'s
    // comment. [live]: toggling resets NRD's history next frame (VoxiRenderer::setNrdLegacyCamera's
    // comment has the mechanism), enabling an A/B with no rebuild or reload.
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
    // ---- lighting-contrast fix's five legacy A/B switches (contrast-fix plan section 3), one bit
    // each of consoleLightingLegacySlot()'s u32 so one root cause can be isolated at a time. Same
    // raw-slot/deviceSetters shape as voxi.giPoisonView/nrdLegacyCamera above. Default OFF on all
    // five (the fixed behaviour); ALL FIVE true must reproduce HEAD's image exactly (checklist item 12).
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
    // W6/M5's own legacy A/B switch (optimisation-wave-2 4(b)) -- one more bit of the same
    // consoleLightingLegacySlot() word, composing with the five above and --lighting-legacy. Behaviour
    // and default are in the help string below; inert on Vulkan (VulkanDevice.cpp's blended=false).
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
    // ---- engine-optimisation-plan measurement dials (M1-M4/W3/W12), same raw-slot/deviceSetters
    // shape as voxi.giPoisonView above (see consoleGiForceRebuildSlot()'s comment for why). None
    // changes the rendered image, only what is measured or how work is scheduled -- but not all three
    // still default OFF: giBoundedDispatch and giFreeAccumulator (gi-memory) each proved safe enough
    // to ship on, so only giForceRebuild below keeps a measurement-only default. See each one's own
    // help string for its default and how to turn it off to measure against the alternative.
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
        "Frees the GI injection accumulator (roughly 2048 MiB at Epic's 512^3, this renderer's single "
        "largest idle GI allocation) after 240 consecutive quiet GI ticks -- ticks that needed no "
        "rebuild -- and recreates it the instant a change needs one again, which then runs one tick "
        "later than it otherwise would while the texture is recreated. Trades that one-tick latency "
        "and a recreation cost against holding the memory for the entire session regardless of how "
        "long the volume sits idle. Default ON; turn it off to measure against holding the accumulator "
        "for the whole session, or if a session that idles and resumes often finds the recreate cost "
        "not worth the memory back.",
        []{ return vBool(consoleGiFreeAccumulatorSlot()); },
        [](ConsoleBatch& b, VarValue v){
            const bool on = v.as.b;
            b.deviceSetters.push_back([on](rhi::IDevice&){ consoleGiFreeAccumulatorSlot() = on; });
        }});
    // W6/M5's pricing switch (optimisation-wave-2 4(a); see consoleBlendedGiConeSlot()'s comment for
    // the raw-slot idiom). Measurement only, like the three above -- default/behaviour details are in
    // the help string below.
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
    // DIAL, not a tier; reads back what is ACTUALLY running via voxi::resolve()'s denoiser field
    // (RenderSettingsResolver.hpp), the same honesty mechanism as voxi.giMode above --
    // denoiser was never clamped inside Settings itself (unlike giMode's old, now-removed clamp) --
    // reading raw and reading "is it actually running" only ever coincided because nothing could
    // refuse it that Settings' own honesty didn't already cover; RequiresNrd/RequiresRayTracingEnabled/
    // RequiresGlobalIllumination/NothingToDenoise can all now refuse it. NRD also refuses above MSAA 1
    // (D3D12 will not mix sample counts in one render-target set, so the G-buffer would be cleared and
    // never written) -- that reason is SOFT (RequiresMsaaOne): `set voxi.denoiser true` at 8x MSAA still
    // stages/commits true and reads back false; VoxiRenderer prints the matching WARN once.
    // Occlusion-aware fog (2026-09-24): in-scattered light scaled by GI-voxel sky visibility. OFF
    // restores the old unoccluded fog (the one that glowed blue inside covered arcades), for A/B.
    t.push_back({"voxi.fogOcclusion", VarType::Bool, false,
        "Fog in-scatter respects occlusion: enclosed air (arcades, rooms) stops glowing with sky light it cannot see. Built from the GI voxel volume, so it needs voxel GI on; off = the old unoccluded fog",
        []{ return vBool(Renderer::get().settings().fogOcclusion); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->fogOcclusion = on; }); }});
    t.push_back({"voxi.denoiser", VarType::Bool, false,
        "NVIDIA NRD over the ReSTIR indirect diffuse and the ray-traced sky occlusion. Allocates the thin G-buffer (velocity, view Z, normal/roughness -- nothing else in the engine wants it) and REQUIRES RT hardware, the RT tier not Off, something to denoise, D3D12+NRD and MSAA 1; above 1x sample count the pass skips itself and says so once at WARN, and this always reads back what is ACTUALLY running, not merely what was last requested",
        []{ const Renderer& r = Renderer::get(); return vBool(voxi::resolve(r.settings(), r.deviceInfo()).denoiser.effective != 0); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->denoiser = on; }); }});
    // ---- REBLUR_DIFFUSE history/prepass tuning -- LIVE: VoxiRenderer re-issues NRD's own
    // SetDenoiserSettings every frame (applyReblurTuning, VoxiRenderer.cpp), so a change takes effect
    // next frame with no reload, unlike voxi.layeredBsdf above. Only meaningful once voxi.denoiser is
    // on and REBLUR_DIFFUSE exists; harmless and kept otherwise. Ranges are NRD's own (NRDSettings.h).
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
    t.push_back({"voxi.reblurSunMovingFrameNum", VarType::U32, false,
        "REBLUR_DIFFUSE history depth while the sun moves (and one frame after), so a drag's bounce "
        "light does not lag the sun. 63 or at/above voxi.reblurMaxAccumulatedFrameNum turns it off. "
        "Default 4",
        []{ return vU32(Renderer::get().settings().reblurSunMovingFrameNum); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->reblurSunMovingFrameNum = n; }); }});
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
        // No longer opt-in only with no quality tier turning it on -- the ladder now derives it (D3,
        // settings-separation retune): Medium/High/Epic -> 1, Low -> 0 (rasteriser, product decision
        // not hardware gap), Off -> 0 (no acceleration structure to trace against). Full reasoning:
        // ladder::rtRenderMode.
        "0 = rasteriser finds the first surface, 1 = a primary ray per pixel does -- gives up hardware early-Z. Derived from the RT tier on a tier change (Off/Low 0, Medium/High/Epic 1) unless set in the same line",
        []{ return vU32(Renderer::get().settings().rtRenderMode); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->rtRenderMode = n; }); }});
    // Milestone 1's A/B switch over rtRenderMode's internal shape, placed beneath it (like
    // voxi.giRestirMaxHistory under voxi.giRestirVisibility) so the two are read together while
    // bisecting the staged split. Milestone 4's value 2 trades GI quality for speed rather than
    // staying a same-image comparison -- see Voxi.hpp's Settings::rayDrivenStages comment. Only
    // meaningful once voxi.rtRenderMode resolves to 1; see RenderSettingsResolver.hpp's
    // Resolution::rayDrivenStages for the Project Settings page's greyed reason.
    t.push_back({"voxi.rayDrivenStages", VarType::U32, false,
        "0 = single pass (one ray-driven draw; the baseline and fallback), 1 = staged: a "
        "visibility compute pass, lighting compute passes, then the same shading draw (same image "
        "as 0, measured 40-45% faster). 2 = staged + half-rate GI (DEFAULT): the ReSTIR GI stage "
        "traces half the pixels per frame in NRD's checkerboard and REBLUR reconstructs the rest -- "
        "a further ~1.4 ms, image within 0.4% of 1 still, slightly noisier in motion. 2 differs "
        "from 1 only while ReSTIR GI and the NRD denoiser are on (otherwise it behaves as 1, logged "
        "once). D3D12 only for 1 and 2; other backends run the single pass. Only applies when "
        "voxi.rtRenderMode resolves to 1. Out-of-range values clamp to 2.",
        []{ return vU32(Renderer::get().settings().rayDrivenStages); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->rayDrivenStages = n; }); },
        [](const VarValue& v, std::string& err) -> bool {
            if (v.as.u > 2) { err = "rayDrivenStages must be 0 (single pass), 1 (staged) or 2 (staged + half-rate GI)"; return false; }
            return true;
        }});
    t.push_back({"voxi.rayDrivenStageTiming", VarType::Bool, false,
        "Diagnostic: time each staged lighting pass (shadow, GI, sky occlusion, reflections) in its "
        "own GPU span, with a barrier after each, instead of the one shared 'Voxi RD lighting stages' "
        "span. The barriers stop the passes overlapping, so the sum reads a little higher than the "
        "shared span. No effect on the image; only while voxi.rayDrivenStages is 1 or 2.",
        []{ return vBool(Renderer::get().settings().rayDrivenStageTiming); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->rayDrivenStageTiming = on; }); }});
    t.push_back({"voxi.rayDrivenShadowTiles", VarType::Bool, false,
        "Splits the sun-shadow trace into a cheap per-8x8-tile probe pass plus the existing per-pixel "
        "pass, which skips its own ray wherever its tile's 3x3 neighbourhood agrees. Near-identical "
        "image, not a quality trade; falls back to the unsplit shadow pass if either pipeline fails to "
        "compile. Only while voxi.rayDrivenStages is 1 or 2.",
        []{ return vBool(Renderer::get().settings().rayDrivenShadowTiles); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->rayDrivenShadowTiles = on; }); }});
    t.push_back({"voxi.rayDrivenGiSplit", VarType::Bool, false,
        "Splits the ReSTIR GI candidate trace into its own compute pass (compacted to the traced half "
        "in checkerboard mode) feeding the resample/shade pass. Same image as voxi.rayDrivenStages == "
        "1 in every mode -- an occupancy/compaction saving, not a quality trade; falls back to the "
        "unsplit GI pass if either pipeline fails to compile. Only while voxi.rayDrivenStages is 1 or 2.",
        []{ return vBool(Renderer::get().settings().rayDrivenGiSplit); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->rayDrivenGiSplit = on; }); }});
    t.push_back({"voxi.rayDrivenReflSplit", VarType::Bool, false,
        "Splits CSRdRefl's own register-heavy ray from its bandwidth-heavy spatial history gather "
        "(rtReflectionSpatial) into two compute passes. Same image as the unsplit reflection stage, not "
        "a quality trade; falls back to the unsplit CSRdRefl if either pipeline fails to compile. Only "
        "while voxi.rayDrivenStages is 1 or 2.",
        []{ return vBool(Renderer::get().settings().rayDrivenReflSplit); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->rayDrivenReflSplit = on; }); }});
    t.push_back({"voxi.localLights", VarType::Bool, false,
        "Lamps: a material with lightIntensity > 0 makes each draw using it a sphere light (its "
        "bounds, tinted by its emissive colour), shadowed by one stochastic ray per pixel and "
        "accumulated over time like the sun's shadow, diffuse and specular. lightIntensity multiplies "
        "the light the material's own glow and size already, physically, cast -- 1 is that output, "
        "2 is twice it. At most 32 per frame, nearest-and-brightest first. No cost without lamps. Off "
        "forces the light count to 0 and frees the history. Any mode with ray tracing on D3D12 "
        "(staged, single pass, raster); translucent draws are lit unshadowed except where they sit on "
        "a staged-lit surface.",
        []{ return vBool(Renderer::get().settings().localLights); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->localLights = on; }); }});
    t.push_back({"voxi.rtSkipUnchangedTlas", VarType::Bool, false,
        "Skips the TLAS rebuild and instance/material table rewrite when nothing "
        "buildAccelerationStructures() reads from the draw list has changed since the last build -- "
        "MEASURED at 0.42 ms/frame on a static scene otherwise spent recomputing the identical answer. "
        "Same image always; a cached BLAS handle gone stale still forces a real rebuild regardless of "
        "this setting's own key match. A compute-skinned mesh present forces it too when "
        "voxi.rtRefitAccel is off; when that's on, this instead runs a lighter refit-only pass for it. "
        "With voxi.rtRefitAccel on, draws that move (Play's animated props, the pawn) are left out of "
        "the key and their new transforms are patched into the instance table and the TLAS refit "
        "(the mover patch lane), so a moving draw no longer forces the full per-draw rebuild; a draw "
        "starting or stopping moving, or any other change, still does. The Output Log's "
        "\"RT accel-structure gate\" line counts these as \"mover-patched\".",
        []{ return vBool(Renderer::get().settings().rtSkipUnchangedTlas); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->rtSkipUnchangedTlas = on; }); }});
    t.push_back({"voxi.rtRefitAccel", VarType::Bool, false,
        "Updates the TLAS and a compute-skinned mesh's BLAS in place (refit) instead of a full "
        "PREFER_FAST_TRACE build every time they move, including on a tick voxi.rtSkipUnchangedTlas "
        "would otherwise skip outright (a lighter refit-only pass instead). Falls back to a full "
        "rebuild periodically since refit quality drifts with the pose, and whenever the RHI can't "
        "refit in place. Off is today's behaviour exactly: full builds only, no ALLOW_UPDATE "
        "allocation, and a compute-skinned mesh forces the whole per-draw loop and a full TLAS build "
        "every frame. Whether a structure actually carries ALLOW_UPDATE is latched when it is created "
        "(project load), not reread every frame; toggling this live only changes whether a refit is "
        "attempted on structures already allocated updatable.",
        []{ return vBool(Renderer::get().settings().rtRefitAccel); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->rtRefitAccel = on; }); }});
    t.push_back({"voxi.rtSecondaryShadowOpaque", VarType::Bool, false,
        "Fires the sun-shadow ray from a secondary hit (reflection, ReSTIR GI candidate) as one "
        "RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH ray against the opaque-including-cutouts mask "
        "instead of rtShadow's full glass-tinting transmittance walk. Trade: glass/water stops "
        "casting a shadow for these two secondary rays. Primary shadows are untouched. ON by "
        "default: measured GI trace 3.88 -> 3.38 ms, reflection 3.14 -> 2.73 ms, image MAD 0.09. "
        "Staged ray-driven modes only; single-pass (voxi.rayDrivenStages 0) always has it on.",
        []{ return vBool(Renderer::get().settings().rtSecondaryShadowOpaque); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->rtSecondaryShadowOpaque = on; }); }});
    t.push_back({"voxi.rtSkyOcclusionHalfRate", VarType::Bool, false,
        "Skips rtSkyOcclusionTemporal's traced sample for a whole 8x8 tile on this frame's skip "
        "parity wherever that tile's reprojected history is valid, reusing the reprojection as the "
        "fresh estimate instead. Whole tiles skip together, not a per-pixel checkerboard. A pixel "
        "with no valid history always traces. ON by default: measured 0.72 -> 0.47 ms, still image "
        "MAD 0.40, no tile pattern in motion. Staged ray-driven modes only; single-pass "
        "(voxi.rayDrivenStages 0) always has it off.",
        []{ return vBool(Renderer::get().settings().rtSkyOcclusionHalfRate); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->rtSkyOcclusionHalfRate = on; }); }});
    t.push_back({"voxi.rtReflectionHalfRate", VarType::Bool, false,
        "Skips rtReflectionTemporalEx's traced reflection for a rough pixel (mirrors always retrace) "
        "on a skip-parity tile whose reflection history reprojects validly, reusing that reprojection "
        "as this frame's colour. ON by default: measured reflection trace 3.14 -> 2.17 ms, still "
        "image MAD 0.04, no tile pattern in motion. Staged ray-driven modes only; single-pass "
        "(voxi.rayDrivenStages 0) always has it off.",
        []{ return vBool(Renderer::get().settings().rtReflectionHalfRate); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->rtReflectionHalfRate = on; }); }});
    t.push_back({"voxi.rtGiHitShadowMap", VarType::Bool, false,
        "The sun visibility at a ReSTIR GI bounce hit comes from the GI-only shadow map instead of a "
        "shadow ray (the ray still fires where the map cannot answer). ON by default: measured whole "
        "frame 11.03 -> 10.41 ms (gallery) and -0.57 ms (court), image MAD 0.18 still / 0.34 moving "
        "(first measured at MAD 1.61, bounce light under column capitals; that no longer reproduces). "
        "Staged ray-driven modes only; single-pass (voxi.rayDrivenStages 0) always has it off.",
        []{ return vBool(Renderer::get().settings().rtGiHitShadowMap); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->rtGiHitShadowMap = on; }); }});
    t.push_back({"voxi.blendedReuseStagedLighting", VarType::Bool, false,
        "A translucent pixel over an opaque surface the staged ray-driven passes already lit this "
        "frame (a decal, e.g.) reuses their sun/GI/AO/reflection textures instead of re-tracing its "
        "own rays, gated per pixel on that surface's depth matching. A draw that reads the blended "
        "backdrop (glass, water) always keeps its own lighting regardless. ON by default.",
        []{ return vBool(Renderer::get().settings().blendedReuseStagedLighting); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->blendedReuseStagedLighting = on; }); }});
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
// voxi::Renderer -- but ConsoleVar::read is a zero-argument closure (no Engine& in scope, e.g. from
// handleVarGet/handleVarsList and commitBatch's post-commit diff). Rather than thread a device
// pointer through every call site, the console panel hands its (single, process-lifetime-stable once
// attached) device pointer to this file once a frame via setConsoleDevice() -- see drawConsole(Engine&)
// in SandboxApp.cpp, the only caller. A null device (no swapchain yet) makes every post.* read/stage
// a documented no-op, not a crash.
inline rhi::IDevice*& consoleDeviceSlot() { static rhi::IDevice* d = nullptr; return d; }
inline void setConsoleDevice(rhi::IDevice* d) { consoleDeviceSlot() = d; }
inline rhi::IDevice* consoleDevice() { return consoleDeviceSlot(); }

// setPostProcess does NO clamping (`post_ = p;` in both D3D12Device.cpp and VulkanCommon.hpp) --
// the opposite of Voxi's setSettings, which clamps unconditionally. So Post entries are the one place
// THIS TABLE clamps before calling setPostProcess, reporting the clamp through the same "requested vs
// now" diff commitBatch produces for Voxi (see ConsoleBatch's comment). Ranges below are inferred
// from RHI.hpp's field comments/defaults, not a documented contract -- PostSettings has none today.
// One entry per field rather than a `f32 PostSettings::*` table: a pointer-to-member table still
// needs one clamp range and help string per field, and autoExposure (bool, no clamp) does not fit
// the shape of the ten f32 fields around it -- naming each field once is the smaller amount of
// machinery for eleven entries.
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

    t.push_back({"post.exposure", VarType::F32, false, "Linear pre-tonemap radiance multiplier; with auto-exposure on, compensation on the adapted value (clamped >= 0)",
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
    t.push_back({"post.exposureSpeed", VarType::F32, false, "Eye adaptation toward a BRIGHTER view, in e-folds per second (clamped >= 0)",
        readField(&rhi::PostSettings::exposureSpeed), stageClamped(&rhi::PostSettings::exposureSpeed, 0.0f, 1e6f)});
    t.push_back({"post.exposureSpeedDark", VarType::F32, false, "Eye adaptation toward a DARKER view, in e-folds per second -- slower than post.exposureSpeed, as eyes are (clamped >= 0)",
        readField(&rhi::PostSettings::exposureSpeedDark), stageClamped(&rhi::PostSettings::exposureSpeedDark, 0.0f, 1e6f)});
    t.push_back({"post.exposureKey", VarType::F32, false, "Target brightness: the average luminance eye adaptation holds the view at (clamped >= 0)",
        readField(&rhi::PostSettings::exposureKey), stageClamped(&rhi::PostSettings::exposureKey, 0.0f, 1e6f)});
    t.push_back({"post.adaptationRealism", VarType::F32, false, "Krawczyk/Myszkowski/Seidel partial adaptation: 0 = every view settles at the same average brightness, 1 = bright scenes still look brighter than dark ones once adapted (clamped [0,1])",
        readField(&rhi::PostSettings::adaptationRealism), stageClamped(&rhi::PostSettings::adaptationRealism, 0.0f, 1.0f)});
    t.push_back({"post.nightVision", VarType::F32, false, "Scotopic (rod) night vision: below roughly 1 cd/m^2 colour drains and shifts blue-grey; 0 = off, 1 = full effect (clamped [0,1])",
        readField(&rhi::PostSettings::nightVision), stageClamped(&rhi::PostSettings::nightVision, 0.0f, 1.0f)});
    t.push_back({"post.meteringCenterWeight", VarType::F32, false, "How much more the centre of the view counts when metering exposure; 0 = the whole frame equally, 1 = centre weighted most heavily (clamped [0,1])",
        readField(&rhi::PostSettings::meteringCenterWeight), stageClamped(&rhi::PostSettings::meteringCenterWeight, 0.0f, 1.0f)});
    t.push_back({"post.histogramLowPercent", VarType::F32, false, "Fraction of the exposure histogram discarded at the dark end (clamped [0,1])",
        readField(&rhi::PostSettings::histogramLowPercent), stageClamped(&rhi::PostSettings::histogramLowPercent, 0.0f, 1.0f)});
    t.push_back({"post.histogramHighPercent", VarType::F32, false, "Fraction of the exposure histogram discarded at the bright end (clamped [0,1])",
        readField(&rhi::PostSettings::histogramHighPercent), stageClamped(&rhi::PostSettings::histogramHighPercent, 0.0f, 1.0f)});
    t.push_back({"post.localExposureShadows", VarType::F32, false, "Local exposure: fraction of a dark region's distance below middle grey that is lifted (0 = off, up to +4 stops; clamped [0,1])",
        readField(&rhi::PostSettings::localExposureShadows), stageClamped(&rhi::PostSettings::localExposureShadows, 0.0f, 1.0f)});
    t.push_back({"post.localExposureHighlights", VarType::F32, false, "Local exposure: fraction of a bright region's distance above middle grey that is pulled down (0 = off, at most -2 stops; clamped [0,1])",
        readField(&rhi::PostSettings::localExposureHighlights), stageClamped(&rhi::PostSettings::localExposureHighlights, 0.0f, 1.0f)});

    // Not an f32, so it cannot go through stageClamped above -- clamped by hand, same [0,2] shape.
    t.push_back({"post.tonemap", VarType::U32, false,
        "Which tone curve: 0 = per-channel Narkowicz/Hill (gentlest toe, keeps dim bounce light visible), 1 = ACES matrixed (the default: Unreal's filmic space, calibrated against a UE5 Lumen Sponza), 2 = ACES on luminance only so hue/saturation survive any exposure (clamped to [0,2])",
        []{ rhi::IDevice* d = consoleDevice(); return vU32(d ? d->postProcess().tonemap : 0u); },
        [](ConsoleBatch& b, VarValue v){
            if (!b.seededPost) { b.post = b.device ? b.device->postProcess() : rhi::PostSettings{}; b.seededPost = true; }
            b.post.tonemap = std::min(v.as.u, 2u);
            b.touchedPost = true;
        }});
    t.push_back({"post.maxRadiance", VarType::F32, false, "Ceiling applied to scene radiance immediately before the tonemap; 0 disables it (clamped >= 0)",
        readField(&rhi::PostSettings::maxRadiance), stageClamped(&rhi::PostSettings::maxRadiance, 0.0f, 1e6f)});
}

// Path tracer's matched-environment legacy switch (contrast-fix plan F6/F7; root cause R5), same
// raw-slot idiom as consoleGiPoisonViewSlot()/consoleNrdLegacyCameraSlot() above but DELIBERATELY
// outside their AVER_MODULE_VOXI guard: PtSceneView's registration/tier selection must keep working
// with that module off (see SandboxApp.cpp's ptSceneViewWantEnabled_ comment). Written by
// `set pt.legacyEnvironment` (registerRhiVars below) or seeded from --pt-legacy-env (for a --frames
// capture with no console); reasserted onto ptSceneView_ each frame from onUpdate via
// PtSceneView::setLegacyEnvironment.
inline bool& consolePtLegacyEnvSlot() { static bool v = false; return v; }

// Direct rhi::IDevice toggles OUTSIDE PostSettings entirely -- rhi.* not post.*, since these are raw
// per-device render state, not part of the post-processing chain. Staged through
// ConsoleBatch::deviceSetters (plain "call this on the device" closures), not a seed-then-commit
// struct like Post: there is no struct backing these to seed, each is a single immediate call, so
// batching only defers WHEN it runs (until commit, so `set` stays all-or-nothing), never merging
// several fields into one call the way Post's setPostProcess(one struct) does.
inline void registerRhiVars(std::vector<ConsoleVar>& t) {
    t.push_back({"rhi.depthPrepass", VarType::Bool, false,
        "Same-frame depth-only pass ahead of the opaque colour walk, so an occluded fragment never reaches PSMainVoxi's shadow lookup/cone trace/fog -- off by default (identical to every render before this existed), on with --depth-prepass or here",
        []{ rhi::IDevice* d = consoleDevice(); return vBool(d && d->depthPrepassEnabled()); },
        [](ConsoleBatch& b, VarValue v){
            const bool on = v.as.b;
            b.deviceSetters.push_back([on](rhi::IDevice& d){ d.setDepthPrepassEnabled(on); });
        }});
    // pt.*, not rhi.*: registered here only because this is the one variable table with NO module
    // guard (see consolePtLegacyEnvSlot()'s comment), not because it is per-device state like
    // rhi.depthPrepass. Behaviour is in the help string below; also true but not in it: every
    // INDIRECT miss off any lobe reads the SH sky and so does ReSTIR, but camera rays use skyColor
    // either way. (Since kSkyIrradianceCalibration 1 on 2026-09-24 the two skies share scale,
    // differing only by SH L2 smoothing; under the old 8 the reference sky was 8x dimmer.) See
    // PtSceneView::setLegacyEnvironment.
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
// setDebugForceWaitIdle). Every other occlusion setting is PRIVATE SandboxApp state with no accessor
// on voxi::Renderer/rhi::IDevice (this file's SCOPE) and stays out, occlusionCullEnabled_ included.
// This one differs: just a bool SandboxApp reasserts onto occluder_ each frame (onUpdate, same idiom
// as occlusionCullForceOff_) -- a raw bool slot this header owns, written by `set`/read by `get`, is
// enough, with no consoleApp()-style complete-type problem (SandboxApp is only forward-declared).
// Defaults TRUE, NOT the base interface's default-off (IOcclusionCuller's base setDebugForceWaitIdle
// is a false no-op for a hypothetical future implementer): a follow-up investigation found the no-wait
// path measurably worse (vs path-traced ground truth, and an A/B reproducing a "flat lighting"
// report) without finding which GI/lighting resource depends on the wait (OcclusionCuller.cpp's
// FOLLOW-UP comment above kInFlight). Pays the stall until fixed; set false or launch with
// --no-occlusion-waitidle (which also seeds this) for the faster, not-yet-proven-correct path.
inline bool& consoleOcclusionForceWaitIdleSlot() { static bool v = true; return v; }

// occlusion.showCulled -- the false-cull finder (white-panel investigation, plan 3B). An entity
// chooseRoute() (SceneSubmission.hpp) would frustum/occlusion-cull but that is NOT owner-hidden
// instead draws tinted magenta through the raster route, making what culling skips VISIBLE
// (ray-driven: same tint via inst.albedo). Magenta over a visible surface at a still camera means a
// false cull. DEFAULT FALSE, opposite of debugForceWaitIdle above: this changes the rendered image
// (feeds GI's voxelisation hash), never just from opening the tab. Cost when off: one bool test per
// culled entity, the same shape chooseRoute already pays for the frustum/occlusion booleans.
inline bool& consoleOcclusionShowCulledSlot() { static bool v = false; return v; }

// occlusion.cullUnderSuppression -- keeps the occlusion test running while a render feature
// (ray-driven Voxi, Path Tracing) paints the scene itself (sceneSuppressed()), when SandboxApp's F8
// gate would otherwise idle culling (little saved there, since a culled entity still submits for
// primary rays, so F8 skips paying HZB seed/reduce/test and a waitIdle for nothing). Without this,
// showCulled/F1-F4's route-parity checks have nothing to exercise once F8 lands. DEFAULT FALSE: never
// writes RENDER.OCCLUSIONCULL/project_.occlusionCull, only overrides F8's idle.
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

// Runs a staged batch through at most two voxi::Renderer::setSettings calls (tier phase, then dial
// phase -- see ConsoleBatch's comment for why each phase snapshots fresh, right here), at most one
// setPostProcess call, then every deviceSetters closure -- then diffs each field's REQUESTED value
// against read(): one path for all three sources (Voxi's clamp, Post's pre-clamp, or -- so far never,
// rhi.* fields being plain bools with nothing to clamp -- a future rhi.* device call declining the
// request).
inline SetOutcome commitBatch(ConsoleBatch& b) {
    SetOutcome out;
#if AVER_MODULE_VOXI
    if (!b.tierSetters.empty()) {
        voxi::Settings snap = voxi::Renderer::get().settings();
        for (auto& fn : b.tierSetters) fn(&snap);
        voxi::Renderer::get().setSettings(snap);
    }
    if (!b.dialSetters.empty()) {
        // RE-READ here, not a snapshot from when the batch was built, so a tier-phase derivation is
        // visible to the dial phase instead of clobbered by a stale copy. See ConsoleBatch's comment.
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

// `print` writes into the console's OWN scrollback, separate from the engine log by design (see
// SandboxApp.cpp's drawConsole) -- a handler wanting engine-wide visibility also calls AVER_INFO/WARN.
// Returns true on success; a failing handler MUST call print(Error, ...) itself before returning
// false, so the console never synthesizes a generic message for a failure it doesn't understand.
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

// Short welcome banner for the transcript's empty state (drawConsole seeds it whenever
// consoleLines_ is empty, incl. after Clear) -- see WHAT TO BUILD item 4, "an empty console worth
// reading". Distinct from `help`, which has the full command list.
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
    // Grammar and worked examples, appended below the command list -- the "help that teaches" half
    // of the console rework (WHAT TO BUILD item 4). Every example below names a variable genuinely
    // in allVars(), so copy-pasting one always runs.
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

// Walks IDevice::gpuTiming()'s flat, parent-indexed node list as an indented tree -- inclusive ms,
// exclusive ms (inclusive minus direct children, i.e. the pass's own time), percent of GPU total --
// then prints GPU total vs this instant's CPU frame time. No-data axes print as distinct sentences,
// not zeros that read as a measurement -- see GpuTimingReport's comment for why both axes exist.
inline bool handleFrameTime(SandboxApp&, Engine& e, const std::vector<std::string>&, const ConsolePrint& print) {
    if (!e.device()) { print(LogLevel::Error, "No render device is attached."); return false; }
    // One formatter, shared with the packaged game (aver/rhi/GpuTimingFormat.hpp). It used to live
    // here, in a header the game cannot include, which is why a shipped AverGame.exe could not show
    // timings it was already paying to collect -- moved beside the report it formats so the two
    // hosts cannot drift into printing different things from the same data.
    const rhi::GpuTimingReport r = e.device()->gpuTiming();
    const f64 gpuTotalMs = rhi::formatGpuTiming(
        r, [&](const std::string& line) { print(LogLevel::Info, line); });
    if (!r.supported || r.nodes.empty() || r.framesAccumulated == 0) return true;

    // GPU total is the sum of TOP-LEVEL marked spans only (collectGpuTiming's topLevelMs, which
    // excludes anything unmarked); CPU frame is THIS INSTANT's e.time().dt, not an average -- not a
    // matched pair (see the printed caveat). GPU total well below CPU frame says CPU-bound; close to
    // or above says GPU-bound.
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

// `vars` (no args): unchanged line-for-line -- WHAT TO BUILD says existing commands "should stay
// working exactly as they do" -- since a screenshot script's grep or muscle memory may depend on
// today's shape.
// `vars <text>` (WHAT TO BUILD item 3, the new half): substring filter over name-or-description,
// grouped by prefix (varGroupKey) with a [live]/[reload] tag -- "vars gi" groups every voxi.gi* dial
// under one heading instead of scattered among fifty other lines; "vars firefly" still finds a
// description match even when no NAME contains the word.
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

// `set name value [name value ...]`: all-or-nothing. Every pair is looked up, checked read-only,
// and parsed BEFORE anything is staged -- a bad token anywhere (unknown name, read-only, unparsable
// value) aborts the WHOLE line with one error and stages nothing, so a later typo cannot half-apply.
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

    // One line per field: commitBatch's clamp/derivation note if it produced one (the "say so, not
    // silently" case), else a plain confirmation it landed as asked.
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
    // history at a time, without a resize (VoxiRenderer::resetGiHistory's comment has the same-shape
    // flag-flip cure). Each raises a one-shot request on voxi::Renderer, consumed next frame by
    // SandboxApp.cpp beside voxiRenderer_.setSettings(vs). TRY resetgihistory FIRST (see each
    // command's help string for why, and Part 3(a) of this task's notes for the full order). The
    // voxel-cone GI volume needs none of these: voxelizePass clears before every injection, so it
    // cannot accumulate poison the way a ping-ponged history can.
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

// Cached like GraphNodeDefs.hpp's own catalog() at its line ~1069: built once, on first call, well
// after main() -- no static-init-order hazard even though `help`'s handler calls this to enumerate
// itself.
inline const std::vector<ConsoleCommandDesc>& consoleCatalog() {
    static const std::vector<ConsoleCommandDesc> table = detail::buildConsoleCatalog();
    return table;
}

// Case-insensitive lookup (findVar/graphNodeCatalog convention). Returns nullptr for an unknown
// name; drawConsole's Enter handler is the one place that synthesizes an "unknown command" line.
inline const ConsoleCommandDesc* findCommand(std::string_view name) {
    for (const ConsoleCommandDesc& c : consoleCatalog()) if (detail::ciEquals(c.name, name)) return &c;
    return nullptr;
}

// Substring search over name or help -- the command-table twin of varMatchesQuery, used by the live
// suggestion popup so typing what a command DOES ("frame" for frametime) finds it too.
inline bool commandMatchesQuery(const ConsoleCommandDesc& c, std::string_view query) {
    return detail::ciContains(c.name, query) || detail::ciContains(c.help, query);
}

} // namespace aver::editor
