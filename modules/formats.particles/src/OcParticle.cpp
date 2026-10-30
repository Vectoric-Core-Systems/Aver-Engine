// .ocparticle reader and writer. See OcParticle.hpp for the round-trip contract and why this
// format's numeric parsing is strict where the rest of this module's readers are permissive.
//
// THE GRAMMAR, IN FULL (one record per line; `#` starts a comment; one trailing `;` tolerated,
// matching every other OC-dialect text format in this tree). Records before the first EMITTER belong to
// emitter 0; an EMITTER line starts the next one (the first EMITTER line starts emitter 0 when no record
// preceded it).
//
//   OCPARTICLE 1|2                                -- header, required, exactly once
//   NAME <text to end of line>                    -- optional authoring label (the system's name)
//   PARAM <name> float <v> | vec3 <x y z> | color <r g b a>   -- user parameters (max 16); a number may be 0x<hex> for the raw 32-bit word
//   EMITTER <name to end of line>                 -- starts an emitter block (max 8)
//   SIM auto|cpu|gpu
//   SHAPE point|sphere|box <x> <y> <z>             -- v1 spelling, always three numbers
//   SHAPE sphere <r> | cylinder <r> <h> | disc <r> -- short forms; `shell` after the numbers = surface only
//   BLEND opaque|alphablend|premultiplied|additive -- rhi::BlendMode
//   EMISSION <rate> <burstCount> <maxParticles>    -- f32, u32, u32
//   LIFETIME <min> <max>                           -- f32 seconds
//   DIRECTION <x> <y> <z> <spreadDeg>               -- Vec3 + f32 half-angle degrees
//   SPEED <min> <max>                              -- f32 cm/s
//   GRAVITY <x> <y> <z>                            -- Vec3 cm/s^2
//   DAMPING <value>                                -- f32, fraction/second
//   SIZE <start> <end>                             -- f32 centimetres
//   COLOR start <r> <g> <b> <a>                    -- f32x4, straight alpha, birth
//   COLOR end <r> <g> <b> <a>                      -- f32x4, straight alpha, death
//   SIZECURVE <t v>{2..4}                          -- overrides SIZE
//   COLORCURVE <t r g b a>{2..4}                   -- overrides COLOR start/end
//   RENDER sprite | ribbon trail <points> <segmentCm> fade <f> | ribbon linked <capacity> | mesh <path> velocity|spin|fixed
//   SPRITE camera | velocity stretch <k> | axis <x> <y> <z>
//   SUBUV <cols> <rows> <frames> life|fps <n> [blend]
//   LIGHTING unlit|ambient|lit    SORT auto|none|depth    FOG on|off
//   BOUNDS <cm>    CULL <cm>    SPAWNAT on|off
//   TEX {guid:0x<hex>}                             -- optional; omitted entirely when textureId==0
//   GI on|off                                      -- ParticleEffect::receivesGI (particles DECIDED 4)
//   MODULE <key> [off] <param>=<number|@Name[.x|.y|.z|.w]> ...   -- the module stack, authored order
//   ONEVENT death|collide SPAWN <emitter> <count> [inherit <k>]
//   CUSTOMSHADER <name>                            -- an embedded particles program (Shaders/<name>.particles.ocgraph) is the emitter's custom stage (GDSHADER reads as an alias)
//
// Every v1 record except NAME and TEX is always written (ParticleEffect always carries a value
// for it); the v2 records are written only when they differ from their defaults, so a v1 effect is written back as v1,
// byte for byte. NAME and TEX are omitted when empty/zero, the same way .ocmat omits a texture slot with no reference.
//
// GI defaults to "on" (ParticleEffect::receivesGI's own default) so an existing, hand-authored file
// with no GI line at all still parses to the same effect it always did — every particle sampling the
// scene's GI volume where one is installed, unlit where none is. An effect that must NOT be dimmed
// by ambient bounce light (an ember, a spark -- its own light source, not a passive receiver of
// someone else's) writes "GI off" explicitly; nothing here special-cases what kind of effect that
// is, only whether this one flag is set.
#include "aver/formats/OcParticle.hpp"
#include "aver/core/CharConv.hpp"   // floating from_chars where libc++ has none
#include "aver/core/Hash.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/particles/ParticleModules.hpp"
#include "aver/platform/FileSystem.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

// Formats a number for the text form: enough digits to round-trip typical authored values, no
// trailing noise. Matches OcMat.cpp/OcGraph.cpp's own `num()` exactly, for the same reason: the
// writer's output has to be something a human can read AND something the strict reader below can
// reparse back to (within a float's own precision) the value that produced it.
std::string num(f32 v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.6g", static_cast<f64>(v));
    return buf;
}

// STRICT parsing helpers. Unlike detail::parseF64/parseI32/parseU64 (which fall back to a default
// on any failure — the right call for the OTHER readers' tolerant history), this format's brief is
// "never a silent default effect that looks like it worked", so every one of these returns false,
// with the caller's field left untouched, unless the WHOLE token is a valid number.
bool strictF32(std::string_view s, f32& out) {
    s = trim(s);
    if (s.empty()) return false;
    f64 v{};
    const auto r = aver::fromChars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return false;
    out = static_cast<f32>(v);
    return true;
}
// A PARAM component: a decimal, or 0x<hex> for the raw 32-bit word. A register that holds an int or bool is a bit pattern a decimal cannot always carry (NaN words, ints past 2^24).
bool paramF32(std::string_view s, f32& out) {
    s = trim(s);
    if (s.size() > 2 && s.size() <= 10 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        u32 w{};
        const auto r = std::from_chars(s.data() + 2, s.data() + s.size(), w, 16);
        if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return false;
        std::memcpy(&out, &w, sizeof out);
        return true;
    }
    return strictF32(s, out);
}
// The writer's spelling of a PARAM component: the usual decimal when it reparses to the same word, nine digits (exact for a finite float) when not, 0x<hex> for a NaN or infinity.
std::string paramNum(f32 v) {
    char buf[40];
    if (!std::isfinite(v)) {
        u32 w{};
        std::memcpy(&w, &v, sizeof w);
        std::snprintf(buf, sizeof buf, "0x%08X", w);
        return buf;
    }
    std::string d = num(v);
    f32 back{};
    if (strictF32(d, back) && std::memcmp(&back, &v, sizeof v) == 0) return d;
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<f64>(v));
    return buf;
}
bool strictU32(std::string_view s, u32& out) {
    s = trim(s);
    if (s.empty()) return false;
    u32 v{};
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return false;
    out = v;
    return true;
}
bool strictI32(std::string_view s, i32& out) {
    s = trim(s);
    if (s.empty()) return false;
    i32 v{};
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return false;
    out = v;
    return true;
}
// Decimal or 0x-prefixed hex, matching the {guid:0x...} spelling every other .oc* format uses.
bool strictU64(std::string_view s, u64& out) {
    s = trim(s);
    if (s.empty()) return false;
    int base = 10;
    std::string_view digits = s;
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        base = 16;
        digits = digits.substr(2);
    }
    if (digits.empty()) return false;
    u64 v{};
    const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), v, base);
    if (r.ec != std::errc{} || r.ptr != digits.data() + digits.size()) return false;
    out = v;
    return true;
}

const char* shapeWord(particles::EmitterShape s) {
    switch (s) {
        case particles::EmitterShape::Sphere:   return "sphere";
        case particles::EmitterShape::Box:      return "box";
        case particles::EmitterShape::Cylinder: return "cylinder";
        case particles::EmitterShape::Disc:     return "disc";
        case particles::EmitterShape::Point:    default: return "point";
    }
}
const char* blendWord(rhi::BlendMode b) {
    switch (b) {
        case rhi::BlendMode::Opaque:              return "opaque";
        case rhi::BlendMode::AlphaBlend:           return "alphablend";
        case rhi::BlendMode::Additive:             return "additive";
        case rhi::BlendMode::Subtractive:
        case rhi::BlendMode::Multiplicative:       return "premultiplied";   // R6 modes are material-only
        case rhi::BlendMode::PremultipliedAlpha:   default: return "premultiplied";
    }
}

// Which record kind a line of EXISTING text belongs to -- used only by the writers' merge paths, to replace each kind in
// place at its own first occurrence rather than collapsing everything into one inserted block. `Other` covers blank
// lines, comments, and anything this format does not model; those are always copied through verbatim, at their original
// position. Identical shape and reasoning to OcGraph.cpp's OwnedLineKind/classifyLine.
enum class Rec : u8 {
    Header, Name, Param, EmitterHdr,
    Sim, Shape, Render, Sprite, Subuv, SizeCurve, ColorCurve, Blend, Emission, Lifetime, Direction, Speed, Gravity,
    Damping, Size, ColorStart, ColorEnd, Tex, Gi, Lighting, Sort, Fog, Bounds, Cull, SpawnAt, Module, OnEvent,
    CustomShader,
    Other, Count
};
constexpr usize kRecCount = static_cast<usize>(Rec::Count);

Rec classifyLine(std::string_view line, bool sawHeaderYet) {
    const std::vector<std::string_view> t = splitWhitespace(trim(truncateHash(line)));
    if (t.empty()) return Rec::Other;   // blank line, or a comment (truncateHash ate it)
    const std::string_view k = t[0];
    if (!sawHeaderYet && equalsCI(k, "OCPARTICLE")) return Rec::Header;
    struct Entry { const char* word; Rec rec; };
    static const Entry kTable[] = {
        {"NAME", Rec::Name}, {"PARAM", Rec::Param}, {"EMITTER", Rec::EmitterHdr}, {"SIM", Rec::Sim},
        {"SHAPE", Rec::Shape}, {"RENDER", Rec::Render}, {"SPRITE", Rec::Sprite}, {"SUBUV", Rec::Subuv},
        {"SIZECURVE", Rec::SizeCurve}, {"COLORCURVE", Rec::ColorCurve}, {"BLEND", Rec::Blend},
        {"EMISSION", Rec::Emission}, {"LIFETIME", Rec::Lifetime}, {"DIRECTION", Rec::Direction},
        {"SPEED", Rec::Speed}, {"GRAVITY", Rec::Gravity}, {"DAMPING", Rec::Damping}, {"SIZE", Rec::Size},
        {"TEX", Rec::Tex}, {"GI", Rec::Gi}, {"LIGHTING", Rec::Lighting}, {"SORT", Rec::Sort}, {"FOG", Rec::Fog},
        {"BOUNDS", Rec::Bounds}, {"CULL", Rec::Cull}, {"SPAWNAT", Rec::SpawnAt}, {"MODULE", Rec::Module},
        {"ONEVENT", Rec::OnEvent}, {"CUSTOMSHADER", Rec::CustomShader}, {"GDSHADER", Rec::CustomShader},
    };
    for (const Entry& e : kTable)
        if (equalsCI(k, e.word)) return e.rec;
    if (equalsCI(k, "COLOR") && t.size() > 1) {
        if (equalsCI(t[1], "start")) return Rec::ColorStart;
        if (equalsCI(t[1], "end"))   return Rec::ColorEnd;
    }
    return Rec::Other;
}

bool isV2OnlyRec(Rec r) {
    switch (r) {
    case Rec::Param: case Rec::EmitterHdr: case Rec::Sim: case Rec::Render: case Rec::Sprite: case Rec::Subuv:
    case Rec::SizeCurve: case Rec::ColorCurve: case Rec::Lighting: case Rec::Sort: case Rec::Fog: case Rec::Bounds:
    case Rec::Cull: case Rec::SpawnAt: case Rec::Module: case Rec::OnEvent: case Rec::CustomShader:
        return true;
    default:
        return false;
    }
}

bool isPerEmitter(Rec r) {
    return r != Rec::Header && r != Rec::Name && r != Rec::Param && r != Rec::Other;
}

// ------------------------------------------------------------------------------------------------ parsing

struct PendingEvent {
    usize emitter;
    u8 on;
    std::string child;
    u32 count;
    f32 inherit;
    usize line;
};
struct PendingBind {
    usize emitter, module;
    u32 param;
    std::string name;
    u8 comp;
    usize line;
};

bool parseCurveSize(const std::vector<std::string_view>& t, particles::Curve4& c) {
    const usize pairs = (t.size() - 1) / 2;
    if ((t.size() - 1) % 2 != 0 || pairs < 2 || pairs > 4) return false;
    particles::Curve4 out;
    out.n = static_cast<u8>(pairs);
    for (usize i = 0; i < pairs; ++i) {
        f32 tt, v;
        if (!strictF32(t[1 + i * 2], tt) || !strictF32(t[2 + i * 2], v)) return false;
        out.t[i] = tt;
        out.v[i][0] = v;
        if (i > 0 && !(out.t[i] > out.t[i - 1])) return false;
    }
    c = out;
    return true;
}
bool parseCurveColor(const std::vector<std::string_view>& t, particles::Curve4& c) {
    if ((t.size() - 1) % 5 != 0) return false;
    const usize stops = (t.size() - 1) / 5;
    if (stops < 2 || stops > 4) return false;
    particles::Curve4 out;
    out.n = static_cast<u8>(stops);
    for (usize i = 0; i < stops; ++i) {
        f32 v[5];
        for (usize k = 0; k < 5; ++k)
            if (!strictF32(t[1 + i * 5 + k], v[k])) return false;
        out.t[i] = v[0];
        for (usize k = 0; k < 4; ++k) out.v[i][k] = v[1 + k];
        if (i > 0 && !(out.t[i] > out.t[i - 1])) return false;
    }
    c = out;
    return true;
}

bool detectEventCycle(const particles::ParticleSystemDef& d) {
    const usize n = d.emitters.size();
    std::vector<u8> state(n, 0);   // 0 unseen, 1 on stack, 2 done
    std::vector<std::pair<usize, usize>> stack;   // (emitter, next event index)
    for (usize root = 0; root < n; ++root) {
        if (state[root] != 0) continue;
        stack.push_back({root, 0});
        state[root] = 1;
        while (!stack.empty()) {
            auto& [e, i] = stack.back();
            if (i < d.emitters[e].events.size()) {
                const usize child = d.emitters[e].events[i++].emitter;
                if (child >= n) continue;
                if (state[child] == 1) return true;
                if (state[child] == 0) { state[child] = 1; stack.push_back({child, 0}); }
            } else {
                state[e] = 2;
                stack.pop_back();
            }
        }
    }
    return false;
}

} // namespace

bool parseOcparticleSystem(std::string_view text, particles::ParticleSystemDef& outDef, OcParticleExtras* extras,
                           std::string* err) {
    // RESET UP FRONT, THEN PARSE INTO A LOCAL AND COMMIT AT THE END. A failed parse leaves exactly the defaults (one
    // default emitter), at any point of failure: a caller who ignores the return value must not see a half-effect that
    // reads as a successfully loaded one.
    outDef = particles::ParticleSystemDef{};
    outDef.emitters.emplace_back();
    if (extras) *extras = OcParticleExtras{};
    particles::ParticleSystemDef def;
    OcParticleExtras ex{};
    std::vector<PendingEvent> events;
    std::vector<PendingBind> binds;

    bool sawHeader = false;
    int cur = -1;                 // current emitter index; -1 before any emitter content
    bool implicitZero = false;    // emitter 0 was started by a record, not by an EMITTER line
    usize lineNo = 0;

    const auto fail = [&](const std::string& msg) {
        if (err) *err = "line " + std::to_string(lineNo) + ": " + msg;
        return false;
    };
    const auto emitter = [&]() -> particles::ParticleEffect& {
        if (cur < 0) { cur = 0; implicitZero = true; }
        while (def.emitters.size() <= static_cast<usize>(cur)) def.emitters.emplace_back();
        return def.emitters[static_cast<usize>(cur)];
    };

    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        const std::string_view rawLine = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;

        const std::string_view line = stripTrailingSemicolon(truncateHash(rawLine));
        if (line.empty()) continue;

        const std::vector<std::string_view> t = splitWhitespace(line);
        if (t.empty()) continue;
        const std::string_view key = t[0];
        const auto after = [&line](std::string_view tok) {
            return line.substr(static_cast<usize>(tok.data() - line.data()) + tok.size());
        };

        if (equalsCI(key, "OCPARTICLE")) {
            i32 version = 0;
            if (t.size() < 2 || !strictI32(t[1], version) || (version != 1 && version != 2)) {
                return fail("unsupported or missing OCPARTICLE version (want 'OCPARTICLE 1' or 'OCPARTICLE 2')");
            }
            sawHeader = true;
        } else if (equalsCI(key, "NAME")) {
            def.name = std::string(trim(after(key)));
        } else if (equalsCI(key, "PARAM")) {
            if (t.size() < 3) return fail("PARAM requires: PARAM name float v | vec3 x y z | color r g b a");
            if (def.params.size() >= particles::kMaxUserParams) return fail("more than 16 PARAM user parameters");
            particles::UserParam up;
            up.name = std::string(t[1]);
            for (const particles::UserParam& o : def.params)
                if (o.name == up.name) return fail("duplicate PARAM '" + up.name + "'");
            usize want = 0;
            if (equalsCI(t[2], "float")) { up.kind = 0; want = 1; }
            else if (equalsCI(t[2], "vec3")) { up.kind = 1; want = 3; }
            else if (equalsCI(t[2], "color")) { up.kind = 2; want = 4; }
            else return fail("PARAM kind must be float, vec3 or color, got '" + std::string(t[2]) + "'");
            if (t.size() != 3 + want) return fail("PARAM " + std::string(t[2]) + " takes " + std::to_string(want) + " number(s)");
            for (usize k = 0; k < want; ++k)
                if (!paramF32(t[3 + k], up.v[k])) return fail("PARAM has a malformed number");
            if (up.kind == 2 && want == 4) { /* colour: rgba as given */ }
            def.params.push_back(up);
        } else if (equalsCI(key, "EMITTER")) {
            if (cur < 0) cur = 0;
            else cur = cur + 1;
            if (implicitZero && cur == 0) cur = 1;   // unreachable by construction; kept for clarity
            if (static_cast<usize>(cur) >= particles::kMaxEmittersPerSystem) return fail("more than 8 EMITTER blocks");
            while (def.emitters.size() <= static_cast<usize>(cur)) def.emitters.emplace_back();
            def.emitters[static_cast<usize>(cur)].name = std::string(trim(after(key)));
        } else if (equalsCI(key, "SIM")) {
            if (t.size() != 2) return fail("SIM requires exactly 1 field: SIM auto|cpu|gpu");
            particles::ParticleEffect& e = emitter();
            if (equalsCI(t[1], "auto"))     e.simTier = particles::SimTier::Auto;
            else if (equalsCI(t[1], "cpu")) e.simTier = particles::SimTier::Cpu;
            else if (equalsCI(t[1], "gpu")) e.simTier = particles::SimTier::Gpu;
            else return fail("SIM must say auto, cpu or gpu, got '" + std::string(t[1]) + "'");
        } else if (equalsCI(key, "SHAPE")) {
            if (t.size() < 2) return fail("SHAPE requires: SHAPE point|sphere|box|cylinder|disc <numbers> [shell]");
            particles::EmitterShape shape;
            usize minNums = 0, maxNums = 0;
            if (equalsCI(t[1], "point"))         { shape = particles::EmitterShape::Point;    minNums = 3; maxNums = 3; }
            else if (equalsCI(t[1], "sphere"))   { shape = particles::EmitterShape::Sphere;   minNums = 1; maxNums = 3; }
            else if (equalsCI(t[1], "box"))      { shape = particles::EmitterShape::Box;      minNums = 3; maxNums = 3; }
            else if (equalsCI(t[1], "cylinder")) { shape = particles::EmitterShape::Cylinder; minNums = 2; maxNums = 3; }
            else if (equalsCI(t[1], "disc"))     { shape = particles::EmitterShape::Disc;     minNums = 1; maxNums = 3; }
            else return fail("unknown SHAPE kind '" + std::string(t[1]) + "' (want point, sphere, box, cylinder or disc)");
            bool shell = false;
            usize last = t.size();
            if (last > 2 && equalsCI(t[last - 1], "shell")) { shell = true; --last; }
            const usize nums = last - 2;
            if (nums < minNums || nums > maxNums || ((shape == particles::EmitterShape::Point || shape == particles::EmitterShape::Box) && nums != 3)) {
                return fail(std::string("SHAPE ") + std::string(t[1]) + " has the wrong number of fields (v1 spelling: SHAPE kind x y z)");
            }
            f32 v[3] = {0, 0, 0};
            for (usize k = 0; k < nums; ++k)
                if (!strictF32(t[2 + k], v[k])) return fail("SHAPE has a malformed number");
            particles::ParticleEffect& e = emitter();
            e.shape = shape;
            e.shapeShell = shell;
            if (nums == 3) e.shapeSize = Vec3{v[0], v[1], v[2]};
            else if (shape == particles::EmitterShape::Cylinder) e.shapeSize = Vec3{v[0], v[1], 0.0f};
            else e.shapeSize = Vec3{v[0], 0.0f, 0.0f};
        } else if (equalsCI(key, "BLEND")) {
            if (t.size() != 2) return fail("BLEND requires exactly 1 field");
            particles::ParticleEffect& e = emitter();
            if (equalsCI(t[1], "opaque"))            e.blend = rhi::BlendMode::Opaque;
            else if (equalsCI(t[1], "alphablend"))   e.blend = rhi::BlendMode::AlphaBlend;
            else if (equalsCI(t[1], "premultiplied")) e.blend = rhi::BlendMode::PremultipliedAlpha;
            else if (equalsCI(t[1], "additive"))     e.blend = rhi::BlendMode::Additive;
            else return fail("unknown BLEND mode '" + std::string(t[1]) + "'");
        } else if (equalsCI(key, "EMISSION")) {
            if (t.size() != 4) return fail("EMISSION requires exactly 3 fields: EMISSION rate burstCount maxParticles");
            f32 rate; u32 burst, maxP;
            if (!strictF32(t[1], rate) || !strictU32(t[2], burst) || !strictU32(t[3], maxP)) return fail("EMISSION has a malformed number");
            particles::ParticleEffect& e = emitter();
            e.emissionRate = rate; e.burstCount = burst; e.maxParticles = maxP;
        } else if (equalsCI(key, "LIFETIME")) {
            if (t.size() != 3) return fail("LIFETIME requires exactly 2 fields: LIFETIME min max");
            f32 lo, hi;
            if (!strictF32(t[1], lo) || !strictF32(t[2], hi)) return fail("LIFETIME has a malformed number");
            particles::ParticleEffect& e = emitter();
            e.lifetimeMin = lo; e.lifetimeMax = hi;
        } else if (equalsCI(key, "DIRECTION")) {
            if (t.size() != 5) return fail("DIRECTION requires exactly 4 fields: DIRECTION x y z spreadDeg");
            f32 x, y, z, spread;
            if (!strictF32(t[1], x) || !strictF32(t[2], y) || !strictF32(t[3], z) || !strictF32(t[4], spread))
                return fail("DIRECTION has a malformed number");
            particles::ParticleEffect& e = emitter();
            e.direction = Vec3{x, y, z};
            e.spreadDeg = spread;
        } else if (equalsCI(key, "SPEED")) {
            if (t.size() != 3) return fail("SPEED requires exactly 2 fields: SPEED min max");
            f32 lo, hi;
            if (!strictF32(t[1], lo) || !strictF32(t[2], hi)) return fail("SPEED has a malformed number");
            particles::ParticleEffect& e = emitter();
            e.speedMin = lo; e.speedMax = hi;
        } else if (equalsCI(key, "GRAVITY")) {
            if (t.size() != 4) return fail("GRAVITY requires exactly 3 fields: GRAVITY x y z");
            f32 x, y, z;
            if (!strictF32(t[1], x) || !strictF32(t[2], y) || !strictF32(t[3], z)) return fail("GRAVITY has a malformed number");
            emitter().gravity = Vec3{x, y, z};
        } else if (equalsCI(key, "DAMPING")) {
            if (t.size() != 2) return fail("DAMPING requires exactly 1 field");
            f32 v;
            if (!strictF32(t[1], v)) return fail("DAMPING has a malformed number");
            emitter().damping = v;
        } else if (equalsCI(key, "SIZE")) {
            if (t.size() != 3) return fail("SIZE requires exactly 2 fields: SIZE start end");
            f32 s0, s1;
            if (!strictF32(t[1], s0) || !strictF32(t[2], s1)) return fail("SIZE has a malformed number");
            particles::ParticleEffect& e = emitter();
            e.sizeStart = s0; e.sizeEnd = s1;
        } else if (equalsCI(key, "COLOR")) {
            if (t.size() != 6) return fail("COLOR requires exactly 5 fields: COLOR start|end r g b a");
            f32 c[4];
            for (u32 i = 0; i < 4; ++i)
                if (!strictF32(t[2 + i], c[i])) return fail("COLOR has a malformed number");
            particles::ParticleEffect& e = emitter();
            if (equalsCI(t[1], "start"))      { for (u32 i = 0; i < 4; ++i) e.colorStart[i] = c[i]; }
            else if (equalsCI(t[1], "end"))   { for (u32 i = 0; i < 4; ++i) e.colorEnd[i]   = c[i]; }
            else return fail("COLOR must say 'start' or 'end', got '" + std::string(t[1]) + "'");
        } else if (equalsCI(key, "SIZECURVE")) {
            particles::ParticleEffect& e = emitter();
            if (!parseCurveSize(t, e.sizeCurve)) return fail("SIZECURVE needs 2 to 4 'time value' pairs with increasing times");
        } else if (equalsCI(key, "COLORCURVE")) {
            particles::ParticleEffect& e = emitter();
            if (!parseCurveColor(t, e.colorCurve)) return fail("COLORCURVE needs 2 to 4 'time r g b a' stops with increasing times");
        } else if (equalsCI(key, "RENDER")) {
            if (t.size() < 2) return fail("RENDER requires: sprite | ribbon trail|linked ... | mesh <path> <orient>");
            particles::ParticleEffect& e = emitter();
            if (equalsCI(t[1], "sprite")) {
                if (t.size() != 2) return fail("RENDER sprite takes no further fields");
                e.render = particles::RenderKind::Sprite;
            } else if (equalsCI(t[1], "ribbon")) {
                if (t.size() >= 3 && equalsCI(t[2], "trail")) {
                    // ribbon trail <points> <segmentCm> fade <f>
                    u32 pts; f32 seg, fade;
                    if (t.size() != 7 || !equalsCI(t[5], "fade") || !strictU32(t[3], pts) || !strictF32(t[4], seg) || !strictF32(t[6], fade))
                        return fail("RENDER ribbon trail needs: points segmentCm fade <f>");
                    if (pts < 2 || pts > particles::kMaxTrailPoints) return fail("RENDER ribbon trail points must be in [2,32]");
                    e.render = particles::RenderKind::Ribbon;
                    e.linked = false;
                    e.trailPoints = static_cast<u8>(pts);
                    e.trailSegmentCm = seg;
                    e.trailFade = fade;
                } else if (t.size() >= 3 && equalsCI(t[2], "linked")) {
                    u32 cap;
                    if (t.size() != 4 || !strictU32(t[3], cap)) return fail("RENDER ribbon linked needs: capacity");
                    if (cap < 2 || cap > particles::kMaxLinkedCapacity) return fail("RENDER ribbon linked capacity must be in [2,4096]");
                    e.render = particles::RenderKind::Ribbon;
                    e.linked = true;
                    e.linkedCapacity = static_cast<u16>(cap);
                } else {
                    return fail("RENDER ribbon must say 'trail' or 'linked'");
                }
            } else if (equalsCI(t[1], "mesh")) {
                if (t.size() != 4) return fail("RENDER mesh needs: <path> velocity|spin|fixed");
                u8 orient;
                if (equalsCI(t[3], "velocity")) orient = 0;
                else if (equalsCI(t[3], "spin")) orient = 1;
                else if (equalsCI(t[3], "fixed")) orient = 2;
                else return fail("RENDER mesh orientation must be velocity, spin or fixed");
                e.render = particles::RenderKind::Mesh;
                e.meshPath = std::string(t[2]);
                e.meshId = fnv1a64(std::string_view(e.meshPath));
                e.meshOrient = orient;
            } else {
                return fail("unknown RENDER kind '" + std::string(t[1]) + "' (want sprite, ribbon or mesh)");
            }
        } else if (equalsCI(key, "SPRITE")) {
            if (t.size() < 2) return fail("SPRITE requires: camera | velocity stretch <k> | axis x y z");
            particles::ParticleEffect& e = emitter();
            if (equalsCI(t[1], "camera")) {
                if (t.size() != 2) return fail("SPRITE camera takes no further fields");
                e.align = particles::SpriteAlign::Camera;
            } else if (equalsCI(t[1], "velocity")) {
                f32 k;
                if (t.size() != 4 || !equalsCI(t[2], "stretch") || !strictF32(t[3], k) || k < 0.0f)
                    return fail("SPRITE velocity needs: stretch <k >= 0>");
                e.align = particles::SpriteAlign::Velocity;
                e.stretch = k;
            } else if (equalsCI(t[1], "axis")) {
                f32 x, y, z;
                if (t.size() != 5 || !strictF32(t[2], x) || !strictF32(t[3], y) || !strictF32(t[4], z))
                    return fail("SPRITE axis needs: x y z");
                e.align = particles::SpriteAlign::Axis;
                e.alignAxis = Vec3{x, y, z};
            } else {
                return fail("unknown SPRITE alignment '" + std::string(t[1]) + "'");
            }
        } else if (equalsCI(key, "SUBUV")) {
            u32 cols, rows, frames; f32 rate;
            if (t.size() < 6 || t.size() > 7 || !strictU32(t[1], cols) || !strictU32(t[2], rows) || !strictU32(t[3], frames))
                return fail("SUBUV needs: cols rows frames life|fps <n> [blend]");
            const bool life = equalsCI(t[4], "life");
            if (!life && !equalsCI(t[4], "fps")) return fail("SUBUV timing must be 'life' or 'fps'");
            if (!strictF32(t[5], rate)) return fail("SUBUV has a malformed number");
            bool blend = false;
            if (t.size() == 7) {
                if (!equalsCI(t[6], "blend")) return fail("SUBUV: the only trailing word is 'blend'");
                blend = true;
            }
            if (cols < 1 || rows < 1 || frames < 1 || cols > 64 || rows > 64 || frames > cols * rows)
                return fail("SUBUV needs 1 <= frames <= cols*rows and cols, rows in [1,64]");
            if (!life && rate < 0.0f) return fail("SUBUV fps must be >= 0");
            particles::ParticleEffect& e = emitter();
            e.subuvCols = static_cast<u16>(cols); e.subuvRows = static_cast<u16>(rows); e.subuvFrames = static_cast<u16>(frames);
            e.subuvFps = life ? 0.0f : rate;
            e.subuvBlend = blend;
        } else if (equalsCI(key, "LIGHTING")) {
            if (t.size() != 2) return fail("LIGHTING requires exactly 1 field: unlit|ambient|lit");
            particles::ParticleEffect& e = emitter();
            if (equalsCI(t[1], "unlit"))        { e.lighting = particles::Lighting::Unlit; e.receivesGI = false; }
            else if (equalsCI(t[1], "ambient")) e.lighting = particles::Lighting::Ambient;
            else if (equalsCI(t[1], "lit"))     e.lighting = particles::Lighting::Lit;
            else return fail("LIGHTING must say unlit, ambient or lit, got '" + std::string(t[1]) + "'");
        } else if (equalsCI(key, "SORT")) {
            if (t.size() != 2) return fail("SORT requires exactly 1 field: auto|none|depth");
            particles::ParticleEffect& e = emitter();
            if (equalsCI(t[1], "auto"))       e.sort = particles::SortMode::Auto;
            else if (equalsCI(t[1], "none"))  e.sort = particles::SortMode::None;
            else if (equalsCI(t[1], "depth")) e.sort = particles::SortMode::Depth;
            else return fail("SORT must say auto, none or depth, got '" + std::string(t[1]) + "'");
        } else if (equalsCI(key, "FOG") || equalsCI(key, "SPAWNAT")) {
            if (t.size() != 2) return fail(std::string(key) + " requires exactly 1 field: on|off");
            bool v;
            if (equalsCI(t[1], "on")) v = true;
            else if (equalsCI(t[1], "off")) v = false;
            else return fail(std::string(key) + " must say 'on' or 'off', got '" + std::string(t[1]) + "'");
            particles::ParticleEffect& e = emitter();
            if (equalsCI(key, "FOG")) e.fog = v; else e.spawnAtTarget = v;
        } else if (equalsCI(key, "BOUNDS") || equalsCI(key, "CULL")) {
            f32 v;
            if (t.size() != 2 || !strictF32(t[1], v)) return fail(std::string(key) + " requires exactly 1 number (cm)");
            if (v < 0.0f) return fail(std::string(key) + " must be >= 0");
            particles::ParticleEffect& e = emitter();
            if (equalsCI(key, "BOUNDS")) e.boundsRadiusCm = v; else e.cullDistanceCm = v;
        } else if (equalsCI(key, "TEX")) {
            if (t.size() != 2) return fail("TEX requires exactly 1 field: TEX {guid:0x...}");
            const std::string_view tok = t[1];
            if (tok.size() < 2 || tok.front() != '{' || tok.back() != '}') return fail("TEX must be {guid:0x...}");
            const std::string_view inner = trim(tok.substr(1, tok.size() - 2));
            if (!startsWithCI(inner, "guid:")) return fail("TEX must be {guid:0x...}");
            u64 id = 0;
            if (!strictU64(trim(inner.substr(5)), id) || id == 0) return fail("TEX guid is malformed or zero");
            emitter().textureId = id;
        } else if (equalsCI(key, "CUSTOMSHADER") || equalsCI(key, "GDSHADER")) {
            // The name of a registered embedded particles program. GDSHADER is the spelling files written before the rename carry.
            if (t.size() != 2 || t[1].empty()) return fail("CUSTOMSHADER requires exactly 1 field: the shader's name");
            emitter().customShader = std::string(t[1]);
        } else if (equalsCI(key, "GI")) {
            if (t.size() != 2) return fail("GI requires exactly 1 field: GI on|off");
            particles::ParticleEffect& e = emitter();
            if (equalsCI(t[1], "on"))       e.receivesGI = true;
            else if (equalsCI(t[1], "off")) e.receivesGI = false;
            else return fail("GI must say 'on' or 'off', got '" + std::string(t[1]) + "'");
        } else if (equalsCI(key, "MODULE")) {
            if (t.size() < 2) return fail("MODULE requires a module key");
            const particles::ModuleInfo* info = particles::findModuleInfo(t[1]);
            if (!info) return fail("unknown module '" + std::string(t[1]) + "'");
            particles::ParticleEffect& e = emitter();
            if (e.modules.size() >= particles::kMaxModulesPerEmitter) return fail("more than 16 MODULE records on one emitter");
            particles::Module m = particles::makeDefaultModule(info->kind);
            for (usize k = 2; k < t.size(); ++k) {
                if (equalsCI(t[k], "off")) { m.enabled = false; continue; }
                const usize eq = t[k].find('=');
                if (eq == std::string_view::npos || eq == 0 || eq + 1 >= t[k].size())
                    return fail("MODULE parameters are name=value, got '" + std::string(t[k]) + "'");
                const std::string_view pname = t[k].substr(0, eq), pval = t[k].substr(eq + 1);
                const int pi = particles::findModuleParam(*info, pname);
                if (pi < 0) return fail("unknown parameter '" + std::string(pname) + "' for module " + std::string(info->key));
                if (pval[0] == '@') {
                    std::string_view ref = pval.substr(1);
                    u8 comp = 0;
                    const usize dot = ref.rfind('.');
                    if (dot != std::string_view::npos) {
                        const std::string_view c = ref.substr(dot + 1);
                        if (c == "x" || c == "r") comp = 0;
                        else if (c == "y" || c == "g") comp = 1;
                        else if (c == "z" || c == "b") comp = 2;
                        else if (c == "w" || c == "a") comp = 3;
                        else return fail("unknown component '" + std::string(c) + "' in a parameter reference");
                        ref = ref.substr(0, dot);
                    }
                    if (ref.empty()) return fail("an empty parameter reference");
                    binds.push_back({static_cast<usize>(cur), e.modules.size(), static_cast<u32>(pi), std::string(ref), comp, lineNo});
                } else {
                    f32 v;
                    if (!strictF32(pval, v)) return fail("MODULE parameter '" + std::string(pname) + "' has a malformed number");
                    m.p[pi].value = v;
                }
            }
            e.modules.push_back(m);
        } else if (equalsCI(key, "ONEVENT")) {
            // ONEVENT death|collide SPAWN <emitter> <count> [inherit <k>]
            if (t.size() != 5 && t.size() != 7) return fail("ONEVENT needs: death|collide SPAWN <emitter> <count> [inherit <k>]");
            u8 on;
            if (equalsCI(t[1], "death")) on = 0;
            else if (equalsCI(t[1], "collide")) on = 1;
            else return fail("ONEVENT must say death or collide, got '" + std::string(t[1]) + "'");
            if (!equalsCI(t[2], "SPAWN")) return fail("ONEVENT: the only action is SPAWN");
            u32 count;
            if (!strictU32(t[4], count) || count == 0) return fail("ONEVENT count must be a positive integer");
            f32 inherit = 0.0f;
            if (t.size() == 7) {
                if (!equalsCI(t[5], "inherit") || !strictF32(t[6], inherit)) return fail("ONEVENT: expected 'inherit <k>'");
            }
            emitter();
            events.push_back({static_cast<usize>(cur), on, std::string(t[3]), count, inherit, lineNo});
        }
        // Any other key: forward-compat, unknown record. Skipped here, preserved verbatim by the writers' merge.
    }

    lineNo = 0;
    if (!sawHeader) {
        if (err) *err = "not an .ocparticle file: no OCPARTICLE header line";
        return false;
    }
    if (def.emitters.empty()) def.emitters.emplace_back();
    if (def.emitters.size() > particles::kMaxEmittersPerSystem) {
        if (err) *err = "more than 8 EMITTER blocks";
        return false;
    }

    // User-parameter references and event targets resolve now: PARAM and EMITTER lines may follow the lines naming them.
    for (const PendingBind& b : binds) {
        int slot = -1;
        for (usize i = 0; i < def.params.size(); ++i) if (def.params[i].name == b.name) { slot = static_cast<int>(i); break; }
        if (slot < 0) {
            if (err) *err = "line " + std::to_string(b.line) + ": '@" + b.name + "' names a PARAM that is not declared";
            return false;
        }
        const u8 kind = def.params[static_cast<usize>(slot)].kind;
        const u8 maxComp = kind == 0 ? 0 : (kind == 1 ? 2 : 3);
        if (b.comp > maxComp) {
            if (err) *err = "line " + std::to_string(b.line) + ": '@" + b.name + "' has no such component";
            return false;
        }
        particles::ModuleParam& mp = def.emitters[b.emitter].modules[b.module].p[b.param];
        mp.userSlot = static_cast<i8>(slot);
        mp.comp = b.comp;
    }
    for (const PendingEvent& ev : events) {
        usize child = def.emitters.size();
        for (usize i = 0; i < def.emitters.size(); ++i) if (def.emitters[i].name == ev.child) { child = i; break; }
        if (child >= def.emitters.size()) {
            if (err) *err = "line " + std::to_string(ev.line) + ": ONEVENT names an EMITTER that does not exist ('" + ev.child + "')";
            return false;
        }
        particles::EventSpawn es;
        es.on = ev.on;
        es.emitter = static_cast<u32>(child);
        es.count = ev.count;
        es.inheritVelocity = ev.inherit;
        def.emitters[ev.emitter].events.push_back(es);
    }
    if (detectEventCycle(def)) {
        if (err) *err = "ONEVENT rules form a cycle between emitters";
        return false;
    }

    // RANGES, CHECKED HERE RATHER THAN PER-RECORD, because these are the only fields whose own
    // struct comments state a range, and a file is not valid until it is fully read anyway. Checked
    // at all because the simulator trusts them: a negative DAMPING is a velocity MULTIPLIER above
    // one, so a "0.4 damping" typo'd to -0.4 does not damp gently, it accelerates every particle
    // exponentially until the effect fills the screen. An author should be told which line was
    // wrong, not shown that.
    for (const particles::ParticleEffect& out : def.emitters) {
        if (!(out.damping >= 0.0f && out.damping < 1.0f)) {
            if (err) *err = "DAMPING must be in [0,1); got " + std::to_string(out.damping);
            return false;
        }
        for (int i = 0; i < 4; ++i) {
            if (!(out.colorStart[i] >= 0.0f && out.colorStart[i] <= 1.0f) ||
                !(out.colorEnd[i]   >= 0.0f && out.colorEnd[i]   <= 1.0f)) {
                if (err) *err = "COLOR components must be in [0,1]";
                return false;
            }
        }
        if (out.lifetimeMin < 0.0f || out.lifetimeMax < out.lifetimeMin) {
            if (err) *err = "LIFETIME must be 0 <= min <= max";
            return false;
        }
        if (out.spreadDeg < 0.0f || out.spreadDeg > 180.0f) {
            // 180 is the whole sphere; beyond it the cone stops meaning anything.
            if (err) *err = "DIRECTION spread must be in [0,180] degrees; got " + std::to_string(out.spreadDeg);
            return false;
        }
    }

    // The single commit point. Everything above wrote locals, so a caller who ignored the bool on a
    // malformed file still has the struct they came in with rather than a half-parsed effect.
    outDef = std::move(def);
    if (extras) extras->name = outDef.name;
    return true;
}

bool parseOcparticle(std::string_view text, particles::ParticleEffect& outEffect, OcParticleExtras* extras,
                      std::string* err) {
    outEffect = particles::ParticleEffect{};
    if (extras) *extras = OcParticleExtras{};
    particles::ParticleSystemDef def;
    OcParticleExtras ex;
    if (!parseOcparticleSystem(text, def, &ex, err)) return false;
    outEffect = def.emitters.front();
    if (extras) *extras = ex;
    return true;
}

// Loads an .ocparticle file. A missing or unreadable file is an error, never a default effect.
bool loadOcparticle(const std::string& path, particles::ParticleEffect& out, OcParticleExtras* extras,
                     std::string* err) {
    std::string text;
    if (!readFileText(path, text)) {
        out = particles::ParticleEffect{};
        if (extras) *extras = OcParticleExtras{};
        if (err) *err = "could not read " + path;
        return false;
    }
    return parseOcparticle(text, out, extras, err);
}

bool loadOcparticleSystem(const std::string& path, particles::ParticleSystemDef& out, OcParticleExtras* extras,
                          std::string* err) {
    std::string text;
    if (!readFileText(path, text)) {
        out = particles::ParticleSystemDef{};
        out.emitters.emplace_back();
        if (extras) *extras = OcParticleExtras{};
        if (err) *err = "could not read " + path;
        return false;
    }
    return parseOcparticleSystem(text, out, extras, err);
}

// ------------------------------------------------------------------------------------------------ writing

bool ocparticleFitsV1(const particles::ParticleSystemDef& def) {
    using namespace particles;
    if (def.emitters.size() != 1 || !def.params.empty()) return false;
    const ParticleEffect& e = def.emitters[0];
    const ParticleEffect d{};
    if (e.shape == EmitterShape::Cylinder || e.shape == EmitterShape::Disc || e.shapeShell) return false;
    if (e.simTier != d.simTier || e.render != d.render || e.align != d.align) return false;
    if (e.alignAxis.x != d.alignAxis.x || e.alignAxis.y != d.alignAxis.y || e.alignAxis.z != d.alignAxis.z) return false;
    if (e.stretch != d.stretch || e.subuvCols != d.subuvCols || e.subuvRows != d.subuvRows ||
        e.subuvFrames != d.subuvFrames || e.subuvFps != d.subuvFps || e.subuvBlend != d.subuvBlend) return false;
    if (e.meshId != 0 || !e.meshPath.empty() || e.meshOrient != d.meshOrient) return false;
    if (e.trailPoints != d.trailPoints || e.trailSegmentCm != d.trailSegmentCm || e.trailFade != d.trailFade ||
        e.linked != d.linked || e.linkedCapacity != d.linkedCapacity) return false;
    if (e.lighting != Lighting::Ambient || e.sort != d.sort || e.fog != d.fog) return false;
    if (e.boundsRadiusCm != 0 || e.cullDistanceCm != 0 || e.spawnAtTarget != d.spawnAtTarget) return false;
    if (e.sizeCurve.n >= 2 || e.colorCurve.n >= 2) return false;
    if (!e.modules.empty() || !e.events.empty() || !e.name.empty()) return false;
    return true;
}

namespace {

struct EmitterText {
    std::string rec[kRecCount];   // each ends in "\n", or is empty (record omitted / removed)
};

std::string curveSizeText(const particles::Curve4& c) {
    std::string s = "SIZECURVE";
    for (u32 i = 0; i < c.n; ++i) s += " " + num(c.t[i]) + " " + num(c.v[i][0]);
    return s + "\n";
}
std::string curveColorText(const particles::Curve4& c) {
    std::string s = "COLORCURVE";
    for (u32 i = 0; i < c.n; ++i) {
        s += " " + num(c.t[i]);
        for (int k = 0; k < 4; ++k) s += " " + num(c.v[i][k]);
    }
    return s + "\n";
}

std::string moduleText(const particles::Module& m, const particles::ParticleSystemDef& def) {
    const particles::ModuleInfo& info = particles::moduleInfo(m.kind);
    std::string s = std::string("MODULE ") + info.key;
    if (!m.enabled) s += " off";
    for (u32 i = 0; i < info.paramCount; ++i) {
        const particles::ModuleParam& p = m.p[i];
        if (p.userSlot >= 0 && static_cast<usize>(p.userSlot) < def.params.size()) {
            static const char* kComp[4] = {"x", "y", "z", "w"};
            const particles::UserParam& up = def.params[static_cast<usize>(p.userSlot)];
            s += std::string(" ") + info.names[i] + "=@" + up.name;
            if (up.kind != 0 || p.comp != 0) s += std::string(".") + kComp[p.comp & 3u];
        } else if (p.value != info.defaults[i]) {
            s += std::string(" ") + info.names[i] + "=" + num(p.value);
        }
    }
    return s + "\n";
}

EmitterText buildEmitterText(const particles::ParticleSystemDef& def, usize index) {
    using namespace particles;
    const ParticleEffect& e = def.emitters[index];
    const ParticleEffect d{};
    EmitterText t;
    auto R = [&](Rec r) -> std::string& { return t.rec[static_cast<usize>(r)]; };

    if (e.simTier != SimTier::Auto)
        R(Rec::Sim) = std::string("SIM ") + (e.simTier == SimTier::Cpu ? "cpu" : "gpu") + "\n";

    // SHAPE: the v1 spelling for the three 0.7 shapes, the short form for the new ones.
    {
        std::string s = std::string("SHAPE ") + shapeWord(e.shape);
        if (e.shape == EmitterShape::Cylinder) s += " " + num(e.shapeSize.x) + " " + num(e.shapeSize.y);
        else if (e.shape == EmitterShape::Disc) s += " " + num(e.shapeSize.x);
        else s += " " + num(e.shapeSize.x) + " " + num(e.shapeSize.y) + " " + num(e.shapeSize.z);
        if (e.shapeShell) s += " shell";
        R(Rec::Shape) = s + "\n";
    }

    if (e.render == RenderKind::Ribbon && !e.linked)
        R(Rec::Render) = "RENDER ribbon trail " + std::to_string(e.trailPoints) + " " + num(e.trailSegmentCm) + " fade " + num(e.trailFade) + "\n";
    else if (e.render == RenderKind::Ribbon)
        R(Rec::Render) = "RENDER ribbon linked " + std::to_string(e.linkedCapacity) + "\n";
    else if (e.render == RenderKind::Mesh)
        R(Rec::Render) = "RENDER mesh " + (e.meshPath.empty() ? std::string("none") : e.meshPath) + " " +
                         (e.meshOrient == 1 ? "spin" : (e.meshOrient == 2 ? "fixed" : "velocity")) + "\n";

    if (e.align == SpriteAlign::Velocity) R(Rec::Sprite) = "SPRITE velocity stretch " + num(e.stretch) + "\n";
    else if (e.align == SpriteAlign::Axis)
        R(Rec::Sprite) = "SPRITE axis " + num(e.alignAxis.x) + " " + num(e.alignAxis.y) + " " + num(e.alignAxis.z) + "\n";

    if (e.subuvCols != 1 || e.subuvRows != 1 || e.subuvFrames != 1 || e.subuvFps != 0.0f || e.subuvBlend)
        R(Rec::Subuv) = "SUBUV " + std::to_string(e.subuvCols) + " " + std::to_string(e.subuvRows) + " " +
                        std::to_string(e.subuvFrames) + (e.subuvFps > 0.0f ? " fps " + num(e.subuvFps) : std::string(" life 0")) +
                        (e.subuvBlend ? " blend" : "") + "\n";

    R(Rec::Blend) = "BLEND " + std::string(blendWord(e.blend)) + "\n";
    R(Rec::Emission) = "EMISSION " + num(e.emissionRate) + " " + std::to_string(e.burstCount) + " " + std::to_string(e.maxParticles) + "\n";
    R(Rec::Lifetime) = "LIFETIME " + num(e.lifetimeMin) + " " + num(e.lifetimeMax) + "\n";
    R(Rec::Direction) = "DIRECTION " + num(e.direction.x) + " " + num(e.direction.y) + " " + num(e.direction.z) + " " + num(e.spreadDeg) + "\n";
    R(Rec::Speed) = "SPEED " + num(e.speedMin) + " " + num(e.speedMax) + "\n";
    R(Rec::Gravity) = "GRAVITY " + num(e.gravity.x) + " " + num(e.gravity.y) + " " + num(e.gravity.z) + "\n";
    R(Rec::Damping) = "DAMPING " + num(e.damping) + "\n";
    R(Rec::Size) = "SIZE " + num(e.sizeStart) + " " + num(e.sizeEnd) + "\n";
    if (e.sizeCurve.n >= 2) R(Rec::SizeCurve) = curveSizeText(e.sizeCurve);
    R(Rec::ColorStart) = "COLOR start " + num(e.colorStart[0]) + " " + num(e.colorStart[1]) + " " + num(e.colorStart[2]) + " " + num(e.colorStart[3]) + "\n";
    R(Rec::ColorEnd) = "COLOR end " + num(e.colorEnd[0]) + " " + num(e.colorEnd[1]) + " " + num(e.colorEnd[2]) + " " + num(e.colorEnd[3]) + "\n";
    if (e.colorCurve.n >= 2) R(Rec::ColorCurve) = curveColorText(e.colorCurve);

    if (e.lighting == Lighting::Lit) R(Rec::Lighting) = "LIGHTING lit\n";
    else if (e.lighting == Lighting::Unlit) R(Rec::Lighting) = "LIGHTING unlit\n";
    R(Rec::Gi) = std::string("GI ") + (e.receivesGI ? "on" : "off") + "\n";
    if (e.sort != d.sort) R(Rec::Sort) = std::string("SORT ") + (e.sort == SortMode::None ? "none" : "depth") + "\n";
    if (e.fog != d.fog) R(Rec::Fog) = std::string("FOG ") + (e.fog ? "on" : "off") + "\n";
    if (e.boundsRadiusCm != 0.0f) R(Rec::Bounds) = "BOUNDS " + num(e.boundsRadiusCm) + "\n";
    if (e.cullDistanceCm != 0.0f) R(Rec::Cull) = "CULL " + num(e.cullDistanceCm) + "\n";
    if (e.spawnAtTarget != d.spawnAtTarget) R(Rec::SpawnAt) = std::string("SPAWNAT ") + (e.spawnAtTarget ? "on" : "off") + "\n";
    if (!e.customShader.empty()) R(Rec::CustomShader) = "CUSTOMSHADER " + e.customShader + "\n";
    if (e.textureId != 0) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "{guid:0x%016llX}", static_cast<unsigned long long>(e.textureId));
        R(Rec::Tex) = std::string("TEX ") + buf + "\n";
    }
    for (const Module& m : e.modules) R(Rec::Module) += moduleText(m, def);
    for (const EventSpawn& ev : e.events) {
        const std::string child = ev.emitter < def.emitters.size() ? def.emitters[ev.emitter].name : std::string();
        R(Rec::OnEvent) += std::string("ONEVENT ") + (ev.on == 0 ? "death" : "collide") + " SPAWN " + child + " " +
                           std::to_string(ev.count) + (ev.inheritVelocity != 0.0f ? " inherit " + num(ev.inheritVelocity) : std::string()) + "\n";
    }
    return t;
}

std::string paramsText(const particles::ParticleSystemDef& def) {
    std::string s;
    for (const particles::UserParam& p : def.params) {
        s += "PARAM " + p.name + (p.kind == 0 ? " float" : (p.kind == 1 ? " vec3" : " color"));
        const int n = p.kind == 0 ? 1 : (p.kind == 1 ? 3 : 4);
        for (int k = 0; k < n; ++k) s += " " + paramNum(p.v[k]);
        s += "\n";
    }
    return s;
}

// Canonical order of a fresh emitter block, with a blank line between its groups.
std::string freshEmitterBody(const EmitterText& t) {
    static const Rec kOrder[] = {
        Rec::Sim, Rec::Shape, Rec::Render, Rec::Sprite, Rec::Subuv, Rec::Blend, Rec::Other /*blank*/,
        Rec::Emission, Rec::Lifetime, Rec::Direction, Rec::Speed, Rec::Gravity, Rec::Damping, Rec::Size, Rec::SizeCurve,
        Rec::ColorStart, Rec::ColorEnd, Rec::ColorCurve, Rec::Other /*blank*/,
        Rec::Lighting, Rec::Gi, Rec::Sort, Rec::Fog, Rec::Bounds, Rec::Cull, Rec::SpawnAt, Rec::Tex, Rec::CustomShader,
        Rec::Module, Rec::OnEvent,
    };
    std::string out;
    for (Rec r : kOrder) {
        if (r == Rec::Other) { if (!out.empty() && out.back() != '\n' ) continue; out += "\n"; continue; }
        out += t.rec[static_cast<usize>(r)];
    }
    return out;
}

std::string emitterHeaderLine(const particles::ParticleSystemDef& def, usize i) {
    const std::string& n = def.emitters[i].name;
    return n.empty() ? std::string("EMITTER\n") : "EMITTER " + n + "\n";
}

// The v1 writer: one emitter, no user parameters, only v1 records. Output is bit-identical to what version 1 tools wrote.
std::string writeV1Text(const particles::ParticleEffect& e, const std::string& systemName, std::string_view existing) {
    const std::string nameLine = systemName.empty() ? std::string() : ("NAME " + systemName + "\n");
    const std::string shapeLine = "SHAPE " + std::string(shapeWord(e.shape)) + " " +
        num(e.shapeSize.x) + " " + num(e.shapeSize.y) + " " + num(e.shapeSize.z) + "\n";
    const std::string blendLine = "BLEND " + std::string(blendWord(e.blend)) + "\n";
    const std::string emissionLine = "EMISSION " + num(e.emissionRate) + " " +
        std::to_string(e.burstCount) + " " + std::to_string(e.maxParticles) + "\n";
    const std::string lifetimeLine = "LIFETIME " + num(e.lifetimeMin) + " " + num(e.lifetimeMax) + "\n";
    const std::string directionLine = "DIRECTION " + num(e.direction.x) + " " + num(e.direction.y) + " " +
        num(e.direction.z) + " " + num(e.spreadDeg) + "\n";
    const std::string speedLine = "SPEED " + num(e.speedMin) + " " + num(e.speedMax) + "\n";
    const std::string gravityLine = "GRAVITY " + num(e.gravity.x) + " " + num(e.gravity.y) + " " +
        num(e.gravity.z) + "\n";
    const std::string dampingLine = "DAMPING " + num(e.damping) + "\n";
    const std::string sizeLine = "SIZE " + num(e.sizeStart) + " " + num(e.sizeEnd) + "\n";
    const std::string colorStartLine = "COLOR start " + num(e.colorStart[0]) + " " + num(e.colorStart[1]) +
        " " + num(e.colorStart[2]) + " " + num(e.colorStart[3]) + "\n";
    const std::string colorEndLine = "COLOR end " + num(e.colorEnd[0]) + " " + num(e.colorEnd[1]) +
        " " + num(e.colorEnd[2]) + " " + num(e.colorEnd[3]) + "\n";
    std::string texLine;
    if (e.textureId != 0) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "{guid:0x%016llX}", static_cast<unsigned long long>(e.textureId));
        texLine = "TEX ";
        texLine += buf;
        texLine += "\n";
    }
    const std::string giLine = std::string("GI ") + (e.receivesGI ? "on" : "off") + "\n";

    // If no existing content, build from scratch: header, comment, then every record in a fixed,
    // readable order, with a blank line ahead of each group -- matching .ocmat's own layout.
    if (trim(existing).empty()) {
        std::string out = "OCPARTICLE 1\n";
        out += "# Particle effect, written by the Aver Engine editor.\n";
        out += nameLine;
        out += shapeLine;
        out += blendLine;
        out += "\n";
        out += emissionLine;
        out += lifetimeLine;
        out += directionLine;
        out += speedLine;
        out += gravityLine;
        out += dampingLine;
        out += sizeLine;
        out += colorStartLine;
        out += colorEndLine;
        out += "\n";
        out += giLine;
        if (!texLine.empty()) { out += "\n"; out += texLine; }
        return out;
    }

    // Otherwise, merge: replace each record KIND in place, at its own first occurrence in
    // `existing`, and copy everything else -- comments, blank lines, and any record this format
    // does not model -- through untouched, at its original position. Identical strategy to
    // OcGraph.cpp's writeOcgraph; see its comment for why a whole-block replace is the wrong shape.
    std::vector<std::string_view> lines;
    {
        usize p = 0;
        while (p < existing.size()) {
            usize nl = existing.find('\n', p);
            const bool last = (nl == std::string_view::npos);
            if (last) nl = existing.size();
            lines.push_back(existing.substr(p, nl - p));
            p = nl + 1;
            if (last) break;
        }
    }

    std::vector<Rec> kinds(lines.size(), Rec::Other);
    {
        bool sawHeader = false;
        for (usize i = 0; i < lines.size(); ++i) {
            kinds[i] = classifyLine(lines[i], sawHeader);
            if (kinds[i] == Rec::Header) sawHeader = true;
        }
    }

    bool placedName = false, placedShape = false, placedBlend = false, placedEmission = false;
    bool placedLifetime = false, placedDirection = false, placedSpeed = false, placedGravity = false;
    bool placedDamping = false, placedSize = false, placedColorStart = false, placedColorEnd = false;
    bool placedTex = false, placedGi = false;

    std::string out;
    out.reserve(existing.size() + 512);

    for (usize i = 0; i < lines.size(); ++i) {
        switch (kinds[i]) {
        case Rec::Header:
            out += "OCPARTICLE 1\n";
            break;
        case Rec::Name:
            if (!placedName) { placedName = true; out += nameLine; }   // empty nameLine = line removed
            break;                                                     // any later duplicate is dropped
        case Rec::Shape:
            if (!placedShape) { placedShape = true; out += shapeLine; }
            break;
        case Rec::Blend:
            if (!placedBlend) { placedBlend = true; out += blendLine; }
            break;
        case Rec::Emission:
            if (!placedEmission) { placedEmission = true; out += emissionLine; }
            break;
        case Rec::Lifetime:
            if (!placedLifetime) { placedLifetime = true; out += lifetimeLine; }
            break;
        case Rec::Direction:
            if (!placedDirection) { placedDirection = true; out += directionLine; }
            break;
        case Rec::Speed:
            if (!placedSpeed) { placedSpeed = true; out += speedLine; }
            break;
        case Rec::Gravity:
            if (!placedGravity) { placedGravity = true; out += gravityLine; }
            break;
        case Rec::Damping:
            if (!placedDamping) { placedDamping = true; out += dampingLine; }
            break;
        case Rec::Size:
            if (!placedSize) { placedSize = true; out += sizeLine; }
            break;
        case Rec::ColorStart:
            if (!placedColorStart) { placedColorStart = true; out += colorStartLine; }
            break;
        case Rec::ColorEnd:
            if (!placedColorEnd) { placedColorEnd = true; out += colorEndLine; }
            break;
        case Rec::Tex:
            if (!placedTex) { placedTex = true; out += texLine; }      // empty texLine = line removed
            break;
        case Rec::Gi:
            if (!placedGi) { placedGi = true; out += giLine; }
            break;
        default:
            // Blank lines, comments, unknown records. (v2-only kinds never reach this writer: the caller routes any
            // existing text holding them to the v2 merge, which drops them cleanly.)
            out += lines[i];
            out += '\n';
            break;
        }
    }

    // A kind that never appeared in `existing` at all has no in-place position to take; append it,
    // each preceded by a blank line so it does not run directly into whatever came before it --
    // matching OcGraph.cpp's own tail-append fallback.
    if (!placedName && !nameLine.empty())   { out += "\n"; out += nameLine; }
    if (!placedShape)                       { out += "\n"; out += shapeLine; }
    if (!placedBlend)                       { out += "\n"; out += blendLine; }
    if (!placedEmission)                    { out += "\n"; out += emissionLine; }
    if (!placedLifetime)                    { out += "\n"; out += lifetimeLine; }
    if (!placedDirection)                   { out += "\n"; out += directionLine; }
    if (!placedSpeed)                       { out += "\n"; out += speedLine; }
    if (!placedGravity)                     { out += "\n"; out += gravityLine; }
    if (!placedDamping)                     { out += "\n"; out += dampingLine; }
    if (!placedSize)                        { out += "\n"; out += sizeLine; }
    if (!placedColorStart)                  { out += "\n"; out += colorStartLine; }
    if (!placedColorEnd)                    { out += "\n"; out += colorEndLine; }
    if (!placedGi)                          { out += "\n"; out += giLine; }
    if (!placedTex && !texLine.empty())     { out += "\n"; out += texLine; }

    return out;
}

bool existingHasV2Records(std::string_view existing) {
    usize p = 0;
    bool sawHeader = false;
    while (p < existing.size()) {
        usize nl = existing.find('\n', p);
        if (nl == std::string_view::npos) nl = existing.size();
        const Rec r = classifyLine(existing.substr(p, nl - p), sawHeader);
        if (r == Rec::Header) sawHeader = true;
        if (isV2OnlyRec(r)) return true;
        p = nl + 1;
    }
    return false;
}

// The v2 writer: fresh text, or a merge into `existing` (v1 or v2). `version` is what the header says.
std::string writeV2Text(const particles::ParticleSystemDef& def, std::string_view existing, int version) {
    const usize n = def.emitters.size();
    std::vector<EmitterText> texts;
    texts.reserve(n);
    for (usize i = 0; i < n; ++i) texts.push_back(buildEmitterText(def, i));
    const std::string nameLine = def.name.empty() ? std::string() : ("NAME " + def.name + "\n");
    const std::string paramLines = paramsText(def);
    const std::string headerLine = "OCPARTICLE " + std::to_string(version) + "\n";
    const bool explicitEmitterLines = version >= 2;

    if (trim(existing).empty()) {
        std::string out = headerLine;
        out += "# Particle effect, written by the Aver Engine editor.\n";
        out += nameLine;
        out += paramLines;
        for (usize i = 0; i < n; ++i) {
            out += "\n";
            if (explicitEmitterLines && (n > 1 || !def.emitters[i].name.empty())) out += emitterHeaderLine(def, i);
            out += freshEmitterBody(texts[i]);
        }
        return out;
    }

    std::vector<std::string_view> lines;
    {
        usize p = 0;
        while (p < existing.size()) {
            usize nl = existing.find('\n', p);
            const bool last = (nl == std::string_view::npos);
            if (last) nl = existing.size();
            lines.push_back(existing.substr(p, nl - p));
            p = nl + 1;
            if (last) break;
        }
    }
    struct L { std::string_view text; Rec rec; int emitter; };
    std::vector<L> ls;
    ls.reserve(lines.size());
    {
        bool sawHeader = false, implicitZero = false;
        int cur = -1;
        for (std::string_view line : lines) {
            Rec r = classifyLine(line, sawHeader);
            if (r == Rec::Header) sawHeader = true;
            if (r == Rec::EmitterHdr) {
                cur = cur < 0 ? 0 : cur + 1;
            } else if (isPerEmitter(r) && cur < 0) {
                cur = 0;
                implicitZero = true;
            }
            ls.push_back({line, r, (r == Rec::Header || r == Rec::Name || r == Rec::Param) ? -2 : cur});
        }
        (void)implicitZero;
    }

    std::vector<std::vector<bool>> placed(n, std::vector<bool>(kRecCount, false));
    std::vector<bool> blockSeen(n, false), blockHasHeader(n, false);
    bool placedName = false, placedParam = false, preambleFlushed = false;
    std::string out;
    out.reserve(existing.size() + 1024);

    const auto flushPreamble = [&]() {
        if (preambleFlushed) return;
        preambleFlushed = true;
        if (!placedName && !nameLine.empty()) { out += nameLine; placedName = true; }
        if (!placedParam && !paramLines.empty()) { out += paramLines; placedParam = true; }
    };
    const auto flushEmitter = [&](int idx) {
        if (idx < 0 || static_cast<usize>(idx) >= n) return;
        static const Rec kFlushOrder[] = {
            Rec::Sim, Rec::Shape, Rec::Render, Rec::Sprite, Rec::Subuv, Rec::Blend, Rec::Emission, Rec::Lifetime,
            Rec::Direction, Rec::Speed, Rec::Gravity, Rec::Damping, Rec::Size, Rec::SizeCurve, Rec::ColorStart,
            Rec::ColorEnd, Rec::ColorCurve, Rec::Lighting, Rec::Gi, Rec::Sort, Rec::Fog, Rec::Bounds, Rec::Cull,
            Rec::SpawnAt, Rec::Tex, Rec::CustomShader, Rec::Module, Rec::OnEvent};
        for (Rec r : kFlushOrder) {
            if (placed[static_cast<usize>(idx)][static_cast<usize>(r)]) continue;
            placed[static_cast<usize>(idx)][static_cast<usize>(r)] = true;
            out += texts[static_cast<usize>(idx)].rec[static_cast<usize>(r)];
        }
    };

    int lastEmitter = -2;
    for (const L& l : ls) {
        if (l.emitter >= 0 && l.emitter != lastEmitter) {
            if (lastEmitter >= 0) flushEmitter(lastEmitter);
            flushPreamble();   // Name / Param go before the first emitter block
            lastEmitter = l.emitter;
            if (static_cast<usize>(l.emitter) < n) blockSeen[static_cast<usize>(l.emitter)] = true;
        }
        const bool inRange = l.emitter < 0 || static_cast<usize>(l.emitter) < n;
        switch (l.rec) {
        case Rec::Header:
            out += headerLine;
            break;
        case Rec::Name:
            if (!placedName) { placedName = true; out += nameLine; }
            break;
        case Rec::Param:
            if (!placedParam) { placedParam = true; out += paramLines; }
            break;
        case Rec::EmitterHdr:
            if (inRange && explicitEmitterLines) { out += emitterHeaderLine(def, static_cast<usize>(l.emitter)); }
            if (inRange) blockHasHeader[static_cast<usize>(l.emitter)] = true;
            break;
        case Rec::Other:
            if (inRange) { out += l.text; out += '\n'; }
            break;
        default: {
            if (!inRange) break;   // a record of a removed emitter
            const usize i = static_cast<usize>(l.emitter);
            const usize r = static_cast<usize>(l.rec);
            if (!placed[i][r]) { placed[i][r] = true; out += texts[i].rec[r]; }
            break;   // list kinds (Module, OnEvent) are emitted whole at their first occurrence; later ones drop
        }
        }
    }
    if (lastEmitter >= 0) flushEmitter(lastEmitter);
    flushPreamble();

    // Emitters the existing text did not have: fresh blocks at the end.
    for (usize i = 0; i < n; ++i) {
        if (blockSeen[i]) continue;
        out += "\n";
        if (explicitEmitterLines && (n > 1 || !def.emitters[i].name.empty())) out += emitterHeaderLine(def, i);
        out += freshEmitterBody(texts[i]);
    }
    return out;
}

} // namespace

std::string writeOcparticleSystem(const particles::ParticleSystemDef& def, std::string_view existing) {
    particles::ParticleSystemDef d = def;
    if (d.emitters.empty()) d.emitters.emplace_back();
    if (ocparticleFitsV1(d) && !existingHasV2Records(existing)) return writeV1Text(d.emitters[0], d.name, existing);
    return writeV2Text(d, existing, ocparticleFitsV1(d) ? 1 : 2);
}

// Renders an effect as .ocparticle text, merging into `existing` (see OcParticle.hpp).
std::string writeOcparticle(const particles::ParticleEffect& e, const OcParticleExtras* extras,
                             std::string_view existing) {
    particles::ParticleSystemDef def;
    def.name = extras ? extras->name : std::string();
    def.emitters.push_back(e);
    return writeOcparticleSystem(def, existing);
}

namespace {
bool writeFileText(const std::string& path, const std::string& text, std::string* err) {
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { if (err) *err = "could not open " + path + " for writing"; return false; }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) { if (err) *err = "write failed for " + path; return false; }
    return true;
}
std::string readExisting(const std::string& path) {
    std::string existing;
    std::ifstream in(path, std::ios::binary);
    if (in) existing.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return existing;
}
} // namespace

// Writes an .ocparticle file, creating parent directories. Reads whatever is already at `path` and
// hands it to writeOcparticle as the merge base -- the same fix saveOcgraph needed (see its own
// comment): without this, every save would silently discard comments and anything unrecognised.
bool saveOcparticle(const std::string& path, const particles::ParticleEffect& e,
                     const OcParticleExtras* extras, std::string* err) {
    const std::string existing = readExisting(path);   // read BEFORE the file is truncated
    return writeFileText(path, writeOcparticle(e, extras, existing), err);
}

bool saveOcparticleSystem(const std::string& path, const particles::ParticleSystemDef& def, std::string* err) {
    const std::string existing = readExisting(path);
    return writeFileText(path, writeOcparticleSystem(def, existing), err);
}

} // namespace aver::fmt
