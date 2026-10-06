#pragma once
// Console command + live-variable registry for editor Console tab. Mirrors GraphNodeDefs.hpp.
// Header-only: SandboxApp.cpp is the only TU including this. Every function is inline.
// Command and variable tables share one file: get/set/vars walk the variable table.
// SCOPE: reads/writes voxi::Renderer settings, one rhi::IDevice post/toggles (already-public).

#include "aver/core/Types.hpp"
#include "aver/core/Log.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/GpuTimingFormat.hpp"
#include "aver/runtime/Engine.hpp"
#if AVER_MODULE_VOXI
#include "aver/voxi/Voxi.hpp"
// Scalability.hpp gives voxi::resolve(), ladder::*, applyOverall/overallFromSettings.
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
// Case-insensitive equality. Used for command and variable names.
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

// Parsed as SIGNED to report negative literals as themselves, not as wrapped huge unsigned numbers.
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
// Setter closures capture values: tier-phase reads live settings before dial-phase, avoiding stale
// derived fields. Each phase re-reads the live renderer settings at commit time.
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
// DISCOVERABILITY: tooltips, grouping, and substring search over the table above.
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

// Three honesty states: LiveTruth (reads what engine is actually running), NotLive (needs reload),
// Normal (reads requested value). Detected from help-text keywords: "ACTUALLY running", "skips itself", "NOT live".
enum class Honesty : u8 { Normal, LiveTruth, NotLive };
inline Honesty varHonesty(const ConsoleVar& v) {
    if (v.help.find("NOT live") != std::string::npos) return Honesty::NotLive;
    if (v.help.find("ACTUALLY running") != std::string::npos ||
        v.help.find("skips itself") != std::string::npos ||
        v.help.find("silently refus") != std::string::npos)
        return Honesty::LiveTruth;
    return Honesty::Normal;
}
// Short tag for a value shown next to a var: transcript lines, the browser, and `vars <filter>`.
inline const char* varHonestyTag(Honesty h) {
    switch (h) {
        case Honesty::LiveTruth: return "[live]";
        case Honesty::NotLive:   return "[reload]";
        default:                 return "";
    }
}

// Valid range/enum documented in help or validate() -- not a second source of truth.
// Bool/Quality use their fixed vocabulary; others extract from validate() error or help-text parenthetical.
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

// Group label: first two dotted segments when 3+, else the first.
inline std::string varGroupKey(const std::string& dotted) {
    const std::size_t first = dotted.find('.');
    if (first == std::string::npos) return dotted;
    const std::size_t second = dotted.find('.', first + 1);
    return second == std::string::npos ? dotted.substr(0, first) : dotted.substr(0, second);
}
inline std::string varGroupLabel(const std::string& groupKey) { return groupKey + ".*"; }

// Substring match (not prefix) over name OR description.
inline bool varMatchesQuery(const ConsoleVar& v, std::string_view query) {
    return detail::ciContains(v.name, query) || detail::ciContains(v.help, query);
}

// One tooltip body shared by completion, browser and transcript hover.
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
// Raw-slot idiom: a bool/u32 this header owns, written by console `set` and reasserted each frame
// from SandboxApp's onUpdate. Used for VoxiRenderer's private debug views and measurement toggles.
inline bool& consoleGiPoisonViewSlot() { static bool v = false; return v; }

// Bisection aid: resets chosen temporal histories every frame. Bit 1 = ReSTIR GI + visibility,
// 2 = RT shadow/reflection/sky-occlusion, 4 = denoiser only.
inline u32& consoleResetHistoryEveryFrameSlot() { static u32 v = 0; return v; }

// One u32 backs FIVE legacy A/B switches below, each flipping a single bit.
inline u32& consoleLightingLegacySlot() { static u32 v = 0; return v; }

// Engine-optimisation measurement dials. None changes the rendered image, only measurement/scheduling.
inline bool& consoleGiForceRebuildSlot()    { static bool v = false; return v; }
// Default TRUE: rebuilds always dispatch bounded (1.3% of grid instead of 100%, measured 30.64->30.25ms on PTTest).
inline bool& consoleGiBoundedDispatchSlot() { static bool v = true; return v; }
// Default TRUE: frees accumulator after 60 quiet ticks (~2048 MiB at Epic's 512^3).
inline bool& consoleGiFreeAccumulatorSlot() { static bool v = true; return v; }

// Path-debug view: shows F2's resolved visibility path over indirect diffuse.
inline bool& consoleGiVisPathViewSlot() { static bool v = false; return v; }

// Blended GI measurement switch.
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
    // UE-style Overall Quality preset, reachable from the console like the Rendering page's Overall row.
    // Staged as a TIER setter: must run in the FIRST commitBatch phase, so explicit dials in the same
    // line take precedence over ladder-derived values.
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

    // ---- read-only: NOT a tier (Settings::layeredBsdf has the full reason). VoxiRenderer builds
    // its raster PSOs once at init from this value, so a `set` here updates settings_ but changes
    // nothing on screen until a project reload. Read-only keeps get/vars truthful.
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
    // Refraction: how a translucent surface bends what is behind it. See Settings::refractionMode.
    // Voxi.cpp clamps any request above 2 down to 1; validate() refuses it up front so a bad `set`
    // reports its own mistake, not the substituted number.
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
    // Switch between voxel-cone and ReSTIR GI diffuse-bounce estimators. NOT a tierSetter:
    // setSettings never derives it from a tier. Reads resolve().giMode.effective for honesty.
    t.push_back({"voxi.giMode", VarType::U32, false,
        "Which estimator answers the diffuse GI bounce: 0 = voxel cone gather (default), 1 = ReSTIR GI. Needs RayQuery hardware, rayTracing != Off and globalIllumination != Off -- resolves back to 0 (the stored request is kept, untouched) when any is missing, and this always reads back what is ACTUALLY running, not merely what was last requested",
        []{ const Renderer& r = Renderer::get(); return vU32(voxi::resolve(r.settings(), r.deviceInfo()).giMode.effective); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->giMode = n; }); }});
    // How much ReSTIR GI pays for at this GI tier. A tier-derived DIAL, re-derived when the GI tier changes.
    t.push_back({"voxi.giRestirVisibility", VarType::U32, false,
        "0 no ray (pre-fix, over-bright), 1 reconstructed (no ray), 2 half resolution, 3 full, 4 cached (NeuRaC; staged D3D12 only, else acts as half). Only "
        "applies when voxi.giMode resolves to 1. voxi.legacyRestirHitSky / "
        "voxi.legacyRestirReuseVisibility, when on, force 'no ray' for their own ray regardless of "
        "this.",
        []{ return vU32(Renderer::get().settings().giRestirVisibility); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->giRestirVisibility = n; }); },
        [](const VarValue& v, std::string& err) -> bool {
            if (v.as.u > 4) { err = "giRestirVisibility must be 0 (no ray), 1 (reconstructed), 2 (half resolution), 3 (full) or 4 (cached) -- values above 4 are clamped to 3 by the engine, but this refuses them up front so the message names your own mistake, not the substitute"; return false; }
            return true;
        }});
    // ReSTIR GI history weight: camera-motion fade fix. Ordinary dial, a real Settings field.
    t.push_back({"voxi.giRestirMaxHistory", VarType::U32, false,
        "stparams.maxHistoryLength: how much weight a previous-frame ReSTIR GI reservoir may carry "
        "into the combine. DEFAULT 0, which is the camera-motion fade fix -- 1 was the old value and "
        "overshoots ~8% for ~25 frames after the camera stops, 8 overshoots ~104%. 0 measured no "
        "worse at rest or in motion (same settled brightness, same grain and flicker), because the "
        "denoiser is what actually smooths this. Set 1 to get the old behaviour back. Engine clamps to [0,31].",
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
    // NOT a Settings field -- see consoleGiPoisonViewSlot() comment for the raw-slot idiom.
    // Paints colours over pixels where guards fired. Seven colours are giMode 1 (ReSTIR) only,
    // one (VIOLET) is NOT giMode-gated: painted over the ray-traced specular indirect ceiling hit.
    t.push_back({"voxi.giPoisonView", VarType::Bool, false,
        "ReSTIR-GI poison debug view: paints an unmistakable colour over any pixel where one of "
        "voxi_restir.hlsli's guards fired THIS frame -- magenta = the store-time reservoir guard (the "
        "one that matters most), cyan = a candidate-radiance clamp, yellow = the target-pdf guard, "
        "orange = the pre-existing final-estimate guard, blue = the denoiser-readback guard (these five are "
        "all non-finite/NaN corruption); red = the raw estimate hit the voxi.giRadianceCeiling clamp "
        "while still finite, green = the denoised readback hit the same ceiling -- these last two "
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
    t.push_back({"voxi.debugResetHistoryEveryFrame", VarType::U32, false,
        "Bisection aid, never saved: resets the chosen temporal histories EVERY frame, without log "
        "lines. 1 = ReSTIR GI reservoirs + GI visibility history, 2 = RT shadow/reflection/"
        "sky-occlusion history (also restarts the denoiser), 4 = denoiser only; add bits to combine, 0 = off. A "
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
    // Path-debug view: shows F2's resolved visibility path. Suppressed while voxi.giPoisonView is on.
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
    // Five legacy A/B switches, one bit each of consoleLightingLegacySlot(). Default OFF (fixed behaviour).
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
        "cosine hemisphere and resets GI/denoiser/AO history on either transition.",
        legacyBitRead(1u), legacyBitStage(1u)});
    t.push_back({"voxi.legacySkyDoubleCount", VarType::Bool, false,
        "ON reinstates the pre-fix behaviour for comparison only: giMode 1's receiver counts its own "
        "sky twice, once through the traced ReSTIR estimate and again through the ambient term (root "
        "cause R1; gAmbientParams.z bit 2). Default OFF counts it once, through the traced estimate.",
        legacyBitRead(2u), legacyBitStage(2u)});
    t.push_back({"voxi.legacyRestirHitSky", VarType::Bool, false,
        "ON reinstates the pre-fix behaviour for comparison only: the ReSTIR candidate hit's own "
        "second-bounce sky is added with no visibility test at all (root cause R2; gAmbientParams.z "
        "bit 4). Default OFF traces one visibility ray for it and resets GI/denoiser history on either "
        "transition.",
        legacyBitRead(4u), legacyBitStage(4u)});
    t.push_back({"voxi.legacyRestirReuseVisibility", VarType::Bool, false,
        "ON reinstates the pre-fix behaviour for comparison only: a spatio-temporally reused ReSTIR "
        "sample shades with no visibility test between the receiver and the reused sample's position "
        "(root cause R3; gAmbientParams.z bit 8). Default OFF traces that visibility ray and resets "
        "GI/denoiser history on either transition.",
        legacyBitRead(8u), legacyBitStage(8u)});
    t.push_back({"voxi.legacyConeWeights", VarType::Bool, false,
        "ON reinstates the pre-fix behaviour for comparison only: the cone gather's directions are "
        "cosine-distributed AND cosine-weighted a second time, an effective cos^2 distribution "
        "instead of cos (root cause R6; gAmbientParams.z bit 16). Default OFF weights each cone once. "
        "Affects giMode 0, the non-RT fallback, and the cluster and particle passes.",
        legacyBitRead(16u), legacyBitStage(16u)});
    // One more bit of consoleLightingLegacySlot(), D3D12 only (inert on Vulkan).
    t.push_back({"voxi.legacyBlendedHistoryWrite", VarType::Bool, false,
        "ON reinstates the pre-fix behaviour for comparison only: a blended (alpha-blend or "
        "transmissive) fragment's per-pixel GI/denoiser history writes and readbacks stop being suppressed, "
        "so the opaque surface behind glass or water is overwritten by whichever pane covered it last "
        "(root cause W6; gAmbientParams.z bit 32; D3D12 only -- inert on Vulkan, which never marks a "
        "draw blended). Default OFF keeps W6's fix; VoxiRenderer::setLightingLegacyBits is the piece "
        "that resets GI/denoiser/RT history on this bit's transition, the same as the five bits above it.",
        legacyBitRead(32u), legacyBitStage(32u)});
    t.push_back({"voxi.legacyDenoisedReadback", VarType::Bool, false,
        "ON reinstates the pre-fix read of the denoised GI for comparison only: at THIS frame's "
        "pixel, although the denoiser filtered LAST frame's -- a one-frame displacement in motion, seen as GI "
        "leaking along edges. Default OFF reprojects the read to where the surface was last frame "
        "(gAmbientParams.z bit 64). A still frame is identical either way.",
        legacyBitRead(64u), legacyBitStage(64u)});
    // Measurement dials: none changes the rendered image, only measurement or scheduling.
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
        "largest idle GI allocation) after 60 consecutive quiet GI ticks -- ticks that needed no "
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
    // Blended GI measurement switch: false (default) uses ReSTIR; true uses cone gather for pricing by difference.
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
    // Fog in-scatter occlusion: OFF restores old unoccluded fog (used to glow blue in covered arcades).
    t.push_back({"voxi.fogOcclusion", VarType::Bool, false,
        "Fog in-scatter respects occlusion: enclosed air (arcades, rooms) stops glowing with sky light it cannot see. Built from the GI voxel volume, so it needs voxel GI on; off = the old unoccluded fog",
        []{ return vBool(Renderer::get().settings().fogOcclusion); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->fogOcclusion = on; }); }});
    // DIAL, not a tier; reads back what is ACTUALLY running via voxi::resolve()'s denoiser field.
    // Requires RT hardware, the RT tier not Off, something to denoise, D3D12 and MSAA 1; above 1x
    // sample count the pass skips itself and says so once at WARN.
    t.push_back({"voxi.denoiser", VarType::Bool, false,
        "AMD FidelityFX Denoiser over the ReSTIR indirect diffuse and the ray-traced sky occlusion. Allocates the thin G-buffer (velocity, view Z, normal/roughness -- nothing else in the engine wants it) and REQUIRES RT hardware, the RT tier not Off, something to denoise, D3D12 and MSAA 1; above 1x sample count the pass skips itself and says so once at WARN, and this always reads back what is ACTUALLY running, not merely what was last requested",
        []{ const Renderer& r = Renderer::get(); return vBool(voxi::resolve(r.settings(), r.deviceInfo()).denoiser.effective != 0); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->denoiser = on; }); }});
    t.push_back({"voxi.denoiserMode", VarType::U32, false,
        "Which denoiser runs: 0 off, 1 AMD FidelityFX, 2 NRD2 (docs/rendering/NRD2.md: single-frame denoising of the composed lighting, no history; D3D12 staged ray-driven only). Same value as RENDER.DENOISER and --denoiser; reads back what is ACTUALLY running",
        []{ const Renderer& r = Renderer::get(); return vU32(voxi::resolve(r.settings(), r.deviceInfo()).denoiser.effective != 0 ? voxi::denoiserMode(r.settings()) : 0u); },
        [](ConsoleBatch& b, VarValue v){ const u32 m=v.as.u; b.dialSetters.push_back([m](void* sp){ voxi::setDenoiserMode(*static_cast<Settings*>(sp), m); }); },
        [](const VarValue& v, std::string& err) -> bool {
            if (v.as.u > 2) { err = "denoiserMode must be 0, 1 or 2"; return false; }
            return true;
        }});
    // NRD2's fixed tile parameters (Settings::nrd2Params; nrd2_resolve.hlsli). Live: copied every frame.
    {
        static const char* kNrd2Field[6] = {"Logit1", "Logit2", "Logit3", "DepthSens", "NormalSens", "LumSens"};
        static const char* kNrd2Help[6] = {
            "level logit for the 1/2 level (own pixel 0); higher leans on it",
            "level logit for the 1/4 level (own pixel 0)",
            "level logit for the 1/8 level (own pixel 0)",
            "log2 relative-depth sensitivity of the upsample taps (higher = sharper at depth edges)",
            "log2 normal-cosine power of the upsample taps (higher = sharper at creases)",
            "log2 luminance sensitivity (higher = an outlier gives way to the levels more)"};
        for (u32 i = 0; i < 12; ++i) {
            const std::string name = std::string("voxi.nrd2") + (i < 6 ? "Diff" : "Spec") + kNrd2Field[i % 6];
            t.push_back({name, VarType::F32, false,
                std::string("NRD2 ") + (i < 6 ? "diffuse" : "specular") + ": " + kNrd2Help[i % 6] +
                    ". Developer dial (phase 1 defaults; the network replaces them per tile in phase 4)",
                [i]{ return vF32(Renderer::get().settings().nrd2Params[i]); },
                [i](ConsoleBatch& b, VarValue v){ const f32 n=v.as.f; b.dialSetters.push_back([i, n](void* sp){ static_cast<Settings*>(sp)->nrd2Params[i] = n; }); }});
        }
    }
    t.push_back({"voxi.nrd2Bypass", VarType::Bool, false,
        "Developer: NRD2 recomposes its diffuse/specular split without filtering, to check the split against voxi.denoiserMode 0",
        []{ return vBool(Renderer::get().settings().nrd2Bypass); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->nrd2Bypass = on; }); }});
    t.push_back({"voxi.nrd2Network", VarType::Bool, false,
        "NRD2's trained network sets the per-tile parameters when its weights pass the held-out gate (Tools > Train Neural Denoiser); 0 = the voxi.nrd2* defaults everywhere",
        []{ return vBool(Renderer::get().settings().nrd2Network); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->nrd2Network = on; }); }});
    t.push_back({"voxi.nrd2Stab", VarType::Bool, false,
        "NRD2's temporal stage (FidelityFX-style reproject, prefilter, temporal blend): on frames without TAA jitter (camera moving, or RENDER.TAA 0) the filtered lighting is blended with last frame's inside a min/max box of this frame's values; 0 = single-frame NRD2 everywhere (A/B)",
        []{ return vBool(Renderer::get().settings().nrd2Stab); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->nrd2Stab = on; }); }});
    t.push_back({"voxi.nrd2StabFrames", VarType::U32, false,
        "NRD2 temporal history length at rest, in frames (1-64, default 32); it shortens with screen speed to min(8, this) from 8 px per frame and is off from 128 px per frame",
        []{ return vU32(Renderer::get().settings().nrd2StabFrames); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->nrd2StabFrames = n; }); },
        [](const VarValue& v, std::string& err) -> bool {
            if (v.as.u < 1 || v.as.u > 64) { err = "nrd2StabFrames must be 1 to 64"; return false; }
            return true;
        }});
    t.push_back({"voxi.nrd2Despeckle", VarType::U32, false,
        "NRD2 input despeckle before the pyramid: a pixel over 2x the 5th brightest of its 5x5 neighbours is clamped to that, so a lone bright sample does not spread into a blotch. 0 off, 1 diffuse, 2 specular, 3 both (default)",
        []{ return vU32(Renderer::get().settings().nrd2Despeckle); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->nrd2Despeckle = n; }); },
        [](const VarValue& v, std::string& err) -> bool {
            if (v.as.u > 3) { err = "nrd2Despeckle must be 0 to 3"; return false; }
            return true;
        }});
    t.push_back({"voxi.nrd2Speckle", VarType::U32, false,
        "NRD2 speckle removal after the resolve: 0 none (default), 1 blur (an edge-aware Gaussian of voxi.nrd2BlurRadius pixels on this frame's lighting, stopped at depth and normal edges)",
        []{ return vU32(Renderer::get().settings().nrd2Speckle); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->nrd2Speckle = n; }); },
        [](const VarValue& v, std::string& err) -> bool {
            if (v.as.u > 1) { err = "nrd2Speckle must be 0 (none) or 1 (blur)"; return false; }
            return true;
        }});
    t.push_back({"voxi.nrd2BlurRadius", VarType::F32, false,
        "NRD2 speckle blur radius in pixels (1-32, default 8): larger hides bigger blotches and softens lamp shadow edges more",
        []{ return vF32(Renderer::get().settings().nrd2BlurRadius); },
        [](ConsoleBatch& b, VarValue v){ const f32 n=v.as.f; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->nrd2BlurRadius = n; }); },
        [](const VarValue& v, std::string& err) -> bool {
            if (!(v.as.f >= 1.0f && v.as.f <= 32.0f)) { err = "nrd2BlurRadius must be 1 to 32"; return false; }
            return true;
        }});
    t.push_back({"voxi.nrd2CombineRef", VarType::U32, false,
        "NRD2 combine reference at inference: 0 the coarsest level, 1 the median of the own pixel and the three levels (default; keeps a lone bright sample from growing into a disc)",
        []{ return vU32(Renderer::get().settings().nrd2CombineRef); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->nrd2CombineRef = n; }); },
        [](const VarValue& v, std::string& err) -> bool {
            if (v.as.u > 1) { err = "nrd2CombineRef must be 0 or 1"; return false; }
            return true;
        }});
    t.push_back({"voxi.nrd2MidCap", VarType::F32, false,
        "NRD2: cap on the 1/4 pyramid level's logit at inference (-16 to 16, default 0; 16 = uncapped)",
        []{ return vF32(Renderer::get().settings().nrd2MidCap); },
        [](ConsoleBatch& b, VarValue v){ const f32 n=v.as.f; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->nrd2MidCap = n; }); },
        [](const VarValue& v, std::string& err) -> bool {
            if (!(v.as.f >= -16.0f && v.as.f <= 16.0f)) { err = "nrd2MidCap must be -16 to 16"; return false; }
            return true;
        }});
    t.push_back({"voxi.nrd2CoarseCap", VarType::F32, false,
        "NRD2: cap on the 1/8 pyramid level's logit at inference, network or defaults (-16 to 16, default 0; 16 = uncapped). Lower keeps lamp light out of near-field shadows and halos, at a little more noise",
        []{ return vF32(Renderer::get().settings().nrd2CoarseCap); },
        [](ConsoleBatch& b, VarValue v){ const f32 n=v.as.f; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->nrd2CoarseCap = n; }); },
        [](const VarValue& v, std::string& err) -> bool {
            if (!(v.as.f >= -16.0f && v.as.f <= 16.0f)) { err = "nrd2CoarseCap must be -16 to 16"; return false; }
            return true;
        }});
    t.push_back({"voxi.nrd2DespeckleCap", VarType::F32, false,
        "NRD2 despeckle cap, times the 5th brightest of the 5x5 neighbours (1-8, default 1): lower removes more spots and loses more of their light",
        []{ return vF32(Renderer::get().settings().nrd2DespeckleCap); },
        [](ConsoleBatch& b, VarValue v){ const f32 n=v.as.f; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->nrd2DespeckleCap = n; }); },
        [](const VarValue& v, std::string& err) -> bool {
            if (!(v.as.f >= 1.0f && v.as.f <= 8.0f)) { err = "nrd2DespeckleCap must be 1 to 8"; return false; }
            return true;
        }});
    // NRD2 half-rate tracing per feature (Settings::nrd2HalfRate*): skipped pixels filled from this frame.
    {
        struct HalfRateDial { const char* name; bool Settings::* field; const char* help; };
        static const HalfRateDial kNrd2HalfRate[4] = {
            {"voxi.nrd2HalfRateGi",    &Settings::nrd2HalfRateGi,
             "ReSTIR GI on a checkerboard (with voxi.rayDrivenStages 2)"},
            {"voxi.nrd2HalfRateRefl",  &Settings::nrd2HalfRateRefl,
             "glossy reflections on a checkerboard (with voxi.rtReflectionHalfRate; mirrors stay full rate)"},
            {"voxi.nrd2HalfRateAo",    &Settings::nrd2HalfRateAo,
             "sky occlusion on a checkerboard (with voxi.rtSkyOcclusionHalfRate)"},
            {"voxi.nrd2HalfRateLamps", &Settings::nrd2HalfRateLamps,
             "lamp visibility on a checkerboard (Stage B's 5x5 lamp filter fills the rest)"}};
        for (const HalfRateDial& d : kNrd2HalfRate) {
            bool Settings::* f = d.field;
            t.push_back({d.name, VarType::Bool, false,
                std::string("NRD2 half rate: ") + d.help +
                    ", the skipped half filled from its traced neighbours this frame. Off = full rate",
                [f]{ return vBool(Renderer::get().settings().*f); },
                [f](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([f, on](void* sp){ static_cast<Settings*>(sp)->*f = on; }); }});
        }
    }
    t.push_back({"voxi.denoiseReflections", VarType::Bool, false,
        "Ray-traced reflections through the denoiser's reflection pipeline while voxi.denoiser runs (staged ray-driven only)",
        []{ return vBool(Renderer::get().settings().denoiseReflections); },
        [](ConsoleBatch& b, VarValue v){ const bool on=v.as.b; b.dialSetters.push_back([on](void* sp){ static_cast<Settings*>(sp)->denoiseReflections = on; }); }});
    // ---- denoiser tuning -- LIVE: VoxiRenderer re-issues the denoiser's tuning every frame.
    // Only meaningful while voxi.denoiser is on; harmless otherwise. Ranges match Voxi.cpp's clamps.
    t.push_back({"voxi.denoiserMaxSamples", VarType::U32, false,
        "Denoiser history length, in frames -- latency/noise trade, not a dispatch toggle: higher "
        "converges quieter but lags longer behind a moving light or camera. Default 32 (engine "
        "clamps to [1,255])",
        []{ return vU32(Renderer::get().settings().denoiserMaxSamples); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->denoiserMaxSamples = n; }); },
        [](const VarValue& v, std::string& err) -> bool {
            if (v.as.u < 1 || v.as.u > 255) { err = "denoiserMaxSamples must be 1..255"; return false; }
            return true;
        }});
    t.push_back({"voxi.denoiserHistoryClipWeight", VarType::F32, false,
        "Width of the neighbourhood box the denoiser clips its history to: SMALLER rejects stale "
        "history harder (less ghosting, more grain), LARGER trusts it longer. Default 0.5 (engine "
        "clamps to [0.01,4])",
        []{ return vF32(Renderer::get().settings().denoiserHistoryClipWeight); },
        [](ConsoleBatch& b, VarValue v){ const f32 n=v.as.f; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->denoiserHistoryClipWeight = n; }); },
        [](const VarValue& v, std::string& err) -> bool {
            if (!(v.as.f >= 0.01f && v.as.f <= 4.0f)) { err = "denoiserHistoryClipWeight must be 0.01..4"; return false; }
            return true;
        }});
    t.push_back({"voxi.denoiserSunMovingSamples", VarType::U32, false,
        "Denoiser history cap while the sun moves (and one frame after), so a drag's bounce light "
        "does not lag the sun. At or above voxi.denoiserMaxSamples turns it off. Default 4 (engine "
        "clamps to [1,255])",
        []{ return vU32(Renderer::get().settings().denoiserSunMovingSamples); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->denoiserSunMovingSamples = n; }); },
        [](const VarValue& v, std::string& err) -> bool {
            if (v.as.u < 1 || v.as.u > 255) { err = "denoiserSunMovingSamples must be 1..255"; return false; }
            return true;
        }});
    t.push_back({"voxi.rtShadowDenoise", VarType::U32, false,
        "Spatial denoise radius for the ray-traced sun shadow, in pixels (engine clamps to [0,3])",
        []{ return vU32(Renderer::get().settings().rtShadowDenoise); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->rtShadowDenoise = n; }); }});
    // 0 = rasteriser finds the first surface (Low, product decision), 1 = ray-traced primary (Medium/High/Epic).
    // Derived from the RT tier on a tier change unless set in the same line.
    t.push_back({"voxi.rtRenderMode", VarType::U32, false,
        "0 = rasteriser finds the first surface, 1 = a primary ray per pixel does -- gives up hardware early-Z. Derived from the RT tier on a tier change (Off/Low 0, Medium/High/Epic 1) unless set in the same line",
        []{ return vU32(Renderer::get().settings().rtRenderMode); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->rtRenderMode = n; }); }});
    // Ray-driven mode options: 0 = single pass, 1 = staged, 2 = staged + half-rate GI (default).
    // Only meaningful once voxi.rtRenderMode resolves to 1.
    t.push_back({"voxi.rayDrivenStages", VarType::U32, false,
        "0 = single pass (one ray-driven draw; the baseline and fallback), 1 = staged: a "
        "visibility compute pass, lighting compute passes, then the same shading draw (same image "
        "as 0, measured 40-45% faster). 2 = staged + half-rate GI (DEFAULT): the ReSTIR GI stage "
        "traces half the pixels per frame in a checkerboard and the denoiser reconstructs the rest -- "
        "a further ~1.4 ms, image within 0.4% of 1 still, slightly noisier in motion. 2 differs "
        "from 1 only while ReSTIR GI and the denoiser are on (otherwise it behaves as 1, logged "
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
    t.push_back({"voxi.ptMode", VarType::U32, false,
        "Path Tracing method: 0 ReSTIR path tracing, 1 the reference path tracer (independent paths averaged while the camera is still)",
        []{ return vU32(Renderer::get().settings().ptMode); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->ptMode = n; }); }});

    // Read-only status: why tiers are stuck, device capabilities.
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

// Post is per-device state, accessed via setConsoleDevice() once per frame.
inline rhi::IDevice*& consoleDeviceSlot() { static rhi::IDevice* d = nullptr; return d; }
inline void setConsoleDevice(rhi::IDevice* d) { consoleDeviceSlot() = d; }
inline rhi::IDevice* consoleDevice() { return consoleDeviceSlot(); }

// Post entries clamp before calling setPostProcess, reporting the clamp as a diff.
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

    // Not an f32, so clamped by hand, same [0,2] shape.
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

// Path tracer's legacy environment switch, accessed via raw slot like consoleGiPoisonViewSlot().
inline bool& consolePtLegacyEnvSlot() { static bool v = false; return v; }

// Direct rhi::IDevice toggles staged through ConsoleBatch::deviceSetters (immediate calls, not a struct).
inline void registerRhiVars(std::vector<ConsoleVar>& t) {
    t.push_back({"rhi.depthPrepass", VarType::Bool, false,
        "Same-frame depth-only pass ahead of the opaque colour walk, so an occluded fragment never reaches PSMainVoxi's shadow lookup/cone trace/fog -- off by default (identical to every render before this existed), on with --depth-prepass or here",
        []{ rhi::IDevice* d = consoleDevice(); return vBool(d && d->depthPrepassEnabled()); },
        [](ConsoleBatch& b, VarValue v){
            const bool on = v.as.b;
            b.deviceSetters.push_back([on](rhi::IDevice& d){ d.setDepthPrepassEnabled(on); });
        }});
    // pt.*, registered here because this table has no module guard. See PtSceneView::setLegacyEnvironment.
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
// occlusion.* -- ONE entry for OcclusionCullerImpl::debugForceWaitIdle_. Others are private SandboxApp state.
inline bool& consoleOcclusionForceWaitIdleSlot() { static bool v = true; return v; }

// occlusion.showCulled -- false-cull finder: culled entities tinted magenta show which culling skips.
inline bool& consoleOcclusionShowCulledSlot() { static bool v = false; return v; }

// occlusion.cullUnderSuppression -- run occlusion test even while a render feature paints the scene.
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

// Runs a staged batch through setSettings calls, setPostProcess, then deviceSetters, diffs against read().
inline SetOutcome commitBatch(ConsoleBatch& b) {
    SetOutcome out;
#if AVER_MODULE_VOXI
    if (!b.tierSetters.empty()) {
        voxi::Settings snap = voxi::Renderer::get().settings();
        for (auto& fn : b.tierSetters) fn(&snap);
        voxi::Renderer::get().setSettings(snap);
    }
    if (!b.dialSetters.empty()) {
        // RE-READ here to see tier-phase derivations, not a stale snapshot.
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

// Short welcome banner for the transcript's empty state.
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
    // Grammar and worked examples.
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

// Walks IDevice::gpuTiming()'s parent-indexed node list as an indented tree (inclusive/exclusive ms, percent).
inline bool handleFrameTime(SandboxApp&, Engine& e, const std::vector<std::string>&, const ConsolePrint& print) {
    if (!e.device()) { print(LogLevel::Error, "No render device is attached."); return false; }
    const rhi::GpuTimingReport r = e.device()->gpuTiming();
    const f64 gpuTotalMs = rhi::formatGpuTiming(
        r, [&](const std::string& line) { print(LogLevel::Info, line); });
    if (!r.supported || r.nodes.empty() || r.framesAccumulated == 0) return true;

    // GPU total is the sum of top-level marked spans only; CPU frame is this instant's e.time().dt, not an average.
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

// `vars` (no args): unchanged line-for-line. `vars <text>` filters by name/description, grouped by prefix.
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

// `set name value [name value ...]`: all-or-nothing. Every pair is validated before anything is staged.
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

    // One line per field: commitBatch's clamp/derivation note if it produced one, else confirmation it landed as asked.
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
    // ---- per-history reset commands: bisect a burned-in artifact by hand, one history at a time.
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
    t.push_back({"resetdenoiserhistory",
        "Force the AMD FidelityFX denoiser to throw away its own internal temporal history on the next "
        "frame, without a resize (Denoiser::forceHistoryReset). "
        "Try this if neither resetgihistory nor resetrthistory cleared the artifact.",
        [](SandboxApp&, Engine&, const std::vector<std::string>&, const ConsolePrint& print) -> bool {
            voxi::Renderer::get().requestDenoiserHistoryReset();
            print(LogLevel::Info, "Requested: denoiser history reset. Watch the Output Log for "
                                   "'[Denoise] history reset' next frame.");
            return true;
        }});
    t.push_back({"resetallhistory",
        "Run every reset* command above in one call: GI reservoir, RT shadow/reflection/AO, and the denoiser. "
        "Do NOT run this FIRST during a bisection -- it clears everything at once and tells you "
        "nothing about which buffer was actually poisoned; try the individual commands one at a time "
        "first. The voxel-cone GI volume needs none of these -- it clears itself every rebuild.",
        [](SandboxApp&, Engine&, const std::vector<std::string>&, const ConsolePrint& print) -> bool {
            voxi::Renderer::get().requestGiHistoryReset();
            voxi::Renderer::get().requestRtHistoryReset();
            voxi::Renderer::get().requestDenoiserHistoryReset();
            print(LogLevel::Info, "Requested: GI + RT/shadow/reflection/AO + denoiser history reset, all "
                                   "next frame. Watch the Output Log for three separate lines.");
            return true;
        }});
#endif
    return t;
}
} // namespace detail

// Cached like GraphNodeDefs.hpp's own catalog(): built once, on first call.
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
