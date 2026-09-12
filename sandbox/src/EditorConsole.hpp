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

#if AVER_MODULE_VOXI
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
        "Ray-traced sun shadow quality tier; changing it derives rtShadowRays/rtPixelsPerRayTile/rtShadowDenoise/rtRenderMode unless set in the same line",
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
    // ---- refraction: how a translucent surface bends what is behind it (Settings::refractionMode's
    // own comment has the full three-mode writeup). Voxi.cpp does not range-check this field the way
    // it does rtRenderMode/giMode (both hard-clamp out-of-range back to a safe default) -- so unlike
    // every other voxi.* validate in this file, THIS ONE also stands in for a check the engine itself
    // does not perform, not merely an earlier, friendlier copy of one it does.
    t.push_back({"voxi.refractionMode", VarType::U32, false,
        "How a translucent surface bends what is behind it: 0 = off (straight sample), 1 = screen-space offset (nearly free, the Medium/Low rung), 2 = ray-traced hit point (costs a ray, the High/Epic rung)",
        []{ return vU32(Renderer::get().settings().refractionMode); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->refractionMode = n; }); },
        [](const VarValue& v, std::string& err) -> bool {
            if (v.as.u > 2) { err = "refractionMode must be 0 (off), 1 (screen-space) or 2 (ray-traced) -- the engine does not clamp this field itself"; return false; }
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
    // only clamps and range-checks it, so it belongs with the other dials that setSettings leaves
    // alone unless a caller touches them directly.
    //
    // THE READ CLOSURE IS THE HONESTY MECHANISM voxi.rtRenderMode etc. already rely on, not a special
    // case added for this field: it reads Renderer::get().settings().giMode LIVE, the same object
    // Voxi.cpp's setSettings just clamped, so `set voxi.giMode 1` on a device with no RayQuery hardware
    // (or with rayTracing forced Off for lack of it -- see Voxi.cpp's refuse() path) stages 1, commits
    // it, and then this same read() call -- already wired into commitBatch's "requested vs now" diff,
    // see ConsoleBatch's own comment -- reports back 0. `get`/`vars` show the same live value, so this
    // variable can never claim ReSTIR GI is running when the engine silently refused it.
    t.push_back({"voxi.giMode", VarType::U32, false,
        "Which estimator answers the diffuse GI bounce: 0 = voxel cone gather (default), 1 = RTXDI ReSTIR GI. Needs RayQuery hardware and rayTracing != Off -- the engine clamps back to 0 when either is missing, and this always reads back what is ACTUALLY running, not merely what was last requested",
        []{ return vU32(Renderer::get().settings().giMode); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->giMode = n; }); }});
    t.push_back({"voxi.rtShadowDenoise", VarType::U32, false,
        "Spatial denoise radius for the ray-traced sun shadow, in pixels (engine clamps to [0,3])",
        []{ return vU32(Renderer::get().settings().rtShadowDenoise); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->rtShadowDenoise = n; }); }});
    t.push_back({"voxi.rtRenderMode", VarType::U32, false,
        "EXPERIMENTAL: 0 = rasteriser finds the first surface, 1 = a primary ray per pixel does -- gives up hardware early-Z; opt-in only, no quality tier turns this on",
        []{ return vU32(Renderer::get().settings().rtRenderMode); },
        [](ConsoleBatch& b, VarValue v){ const u32 n=v.as.u; b.dialSetters.push_back([n](void* sp){ static_cast<Settings*>(sp)->rtRenderMode = n; }); }});
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
        "Which tone curve: 0 = per-channel Narkowicz/Hill, 1 = ACES matrixed, 2 = ACES on luminance only so hue/saturation survive any exposure (the default) (clamped to [0,2])",
        []{ rhi::IDevice* d = consoleDevice(); return vU32(d ? d->postProcess().tonemap : 0u); },
        [](ConsoleBatch& b, VarValue v){
            if (!b.seededPost) { b.post = b.device ? b.device->postProcess() : rhi::PostSettings{}; b.seededPost = true; }
            b.post.tonemap = std::min(v.as.u, 2u);
            b.touchedPost = true;
        }});
    t.push_back({"post.maxRadiance", VarType::F32, false, "Ceiling applied to scene radiance immediately before the tonemap; 0 disables it (clamped >= 0)",
        readField(&rhi::PostSettings::maxRadiance), stageClamped(&rhi::PostSettings::maxRadiance, 0.0f, 1e6f)});
}

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
}

inline std::vector<ConsoleVar> buildVarTable() {
    std::vector<ConsoleVar> t;
#if AVER_MODULE_VOXI
    registerVoxiVars(t);
#endif
    registerPostVars(t);
    registerRhiVars(t);
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
    if (!v->help.empty()) line += "  -- " + v->help;
    print(LogLevel::Info, line);
    return true;
}

inline bool handleVarsList(SandboxApp&, Engine&, const std::vector<std::string>&, const ConsolePrint& print) {
    for (const ConsoleVar& v : allVars()) {
        std::string line = v.name + " = " + formatValue(v.read());
        if (v.readOnly) line += "  (read-only)";
        if (!v.help.empty()) line += "  -- " + v.help;
        print(LogLevel::Info, line);
    }
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
        if (!noted) print(LogLevel::Info, s.var->name + " = " + formatValue(s.var->read()));
    }
    return true;
}

namespace detail {
inline std::vector<ConsoleCommandDesc> buildConsoleCatalog() {
    std::vector<ConsoleCommandDesc> t;
    t.push_back({"help", "List every console command", &handleHelp});
    t.push_back({"frametime", "Print the current per-pass GPU frame-time breakdown", &handleFrameTime});
    t.push_back({"get", "get <name> -- print an engine variable's current value", &handleVarGet});
    t.push_back({"set", "set <name> <value> [<name> <value> ...] -- set one or more engine variables", &handleVarSet});
    t.push_back({"vars", "List every readable/settable engine variable", &handleVarsList});
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

} // namespace aver::editor
