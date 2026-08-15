// .ocparticle reader and writer. See OcParticle.hpp for the round-trip contract and why this
// format's numeric parsing is strict where the rest of this module's readers are permissive.
//
// THE GRAMMAR, IN FULL (one record per line; `#` starts a comment; one trailing `;` tolerated,
// matching every other OC-dialect text format in this tree):
//
//   OCPARTICLE 1                                  -- header, required, exactly once, version 1
//   NAME <text to end of line>                    -- optional authoring label
//   SHAPE point|sphere|box <x> <y> <z>             -- EmitterShape + shapeSize (Vec3, always all 3)
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
//   TEX {guid:0x<hex>}                             -- optional; omitted entirely when textureId==0
//   GI on|off                                      -- ParticleEffect::receivesGI (particles DECIDED 4)
//
// Every record above except NAME and TEX is always written (ParticleEffect always carries a value
// for it — there is no "unset" state to omit, unlike .ocmat's optional texture slots). NAME and TEX
// are omitted when empty/zero, the same way .ocmat omits a texture slot with no reference.
//
// GI defaults to "on" (ParticleEffect::receivesGI's own default) so an existing, hand-authored file
// with no GI line at all still parses to the same effect it always did — every particle sampling the
// scene's GI volume where one is installed, unlit where none is. An effect that must NOT be dimmed
// by ambient bounce light (an ember, a spark -- its own light source, not a passive receiver of
// someone else's) writes "GI off" explicitly; nothing here special-cases what kind of effect that
// is, only whether this one flag is set.
#include "aver/formats/OcParticle.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/platform/FileSystem.hpp"

#include <charconv>
#include <cstdio>
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
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return false;
    out = static_cast<f32>(v);
    return true;
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
        case particles::EmitterShape::Sphere: return "sphere";
        case particles::EmitterShape::Box:    return "box";
        case particles::EmitterShape::Point:  default: return "point";
    }
}
const char* blendWord(rhi::BlendMode b) {
    switch (b) {
        case rhi::BlendMode::Opaque:              return "opaque";
        case rhi::BlendMode::AlphaBlend:           return "alphablend";
        case rhi::BlendMode::Additive:             return "additive";
        case rhi::BlendMode::PremultipliedAlpha:   default: return "premultiplied";
    }
}

// Which record kind, if any, a line of EXISTING text belongs to — used only by writeOcparticle's
// merge path, to replace each kind in place at its own first occurrence rather than collapsing
// everything into one inserted block. `Other` covers blank lines, comments, and anything this
// format does not model; those are always copied through verbatim, at their original position.
// Identical shape and reasoning to OcGraph.cpp's OwnedLineKind/classifyLine — see there for why a
// whole-block replace is the wrong strategy.
enum class OwnedLineKind {
    Header, Name, Shape, Blend, Emission, Lifetime, Direction, Speed, Gravity, Damping, Size,
    ColorStart, ColorEnd, Tex, Gi, Other
};

OwnedLineKind classifyLine(std::string_view line, bool sawHeaderYet) {
    const std::vector<std::string_view> t = splitWhitespace(trim(truncateHash(line)));
    if (t.empty()) return OwnedLineKind::Other;   // blank line, or a comment (truncateHash ate it)
    if (!sawHeaderYet && equalsCI(t[0], "OCPARTICLE")) return OwnedLineKind::Header;
    if (equalsCI(t[0], "NAME"))      return OwnedLineKind::Name;
    if (equalsCI(t[0], "SHAPE"))     return OwnedLineKind::Shape;
    if (equalsCI(t[0], "BLEND"))     return OwnedLineKind::Blend;
    if (equalsCI(t[0], "EMISSION"))  return OwnedLineKind::Emission;
    if (equalsCI(t[0], "LIFETIME"))  return OwnedLineKind::Lifetime;
    if (equalsCI(t[0], "DIRECTION")) return OwnedLineKind::Direction;
    if (equalsCI(t[0], "SPEED"))     return OwnedLineKind::Speed;
    if (equalsCI(t[0], "GRAVITY"))   return OwnedLineKind::Gravity;
    if (equalsCI(t[0], "DAMPING"))   return OwnedLineKind::Damping;
    if (equalsCI(t[0], "SIZE"))      return OwnedLineKind::Size;
    if (equalsCI(t[0], "TEX"))       return OwnedLineKind::Tex;
    if (equalsCI(t[0], "GI"))        return OwnedLineKind::Gi;
    if (equalsCI(t[0], "COLOR") && t.size() > 1) {
        if (equalsCI(t[1], "start")) return OwnedLineKind::ColorStart;
        if (equalsCI(t[1], "end"))   return OwnedLineKind::ColorEnd;
    }
    return OwnedLineKind::Other;
}

} // namespace

// Parses .ocparticle text into a ParticleEffect. Returns false with `err` set. See the file header
// for the full grammar and the header comment for the strictness contract.
bool parseOcparticle(std::string_view text, particles::ParticleEffect& outEffect, OcParticleExtras* extras,
                      std::string* err) {
    // PARSED INTO A LOCAL AND COMMITTED ONLY ON SUCCESS. Writing straight into the caller's struct
    // meant a file that failed on its fourth record left the caller holding the first three records'
    // real values mixed with defaults for everything after -- a half-effect that a caller ignoring
    // the bool would render as though it had loaded. Both real callers do check the bool, so this
    // was never reachable in the engine; it is fixed because "returns false AND leaves your struct
    // alone" is the contract every other reader here keeps, and the next caller should not have to
    // know that this one was different.
    // RESET UP FRONT, THEN PARSE INTO A LOCAL AND COMMIT AT THE END. The reset is the house style
    // parseOcmat and parseOcgraph already use, and it is kept deliberately: a caller who ignores the
    // return value sees this format's own defaults rather than whatever they happened to be holding.
    // The local is the fix. Writing records straight into the caller's struct meant a file that
    // failed on its fourth record left them with the first three records' REAL values standing in
    // front of the defaults -- a half-effect that reads as a successfully loaded one. Now a failed
    // parse leaves exactly the defaults, at any point of failure, which is what the reset was always
    // supposed to guarantee and did not.
    outEffect = particles::ParticleEffect{};
    if (extras) *extras = OcParticleExtras{};
    particles::ParticleEffect out{};
    OcParticleExtras ex{};

    bool sawHeader = false;

    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        const std::string_view rawLine = text.substr(pos, nl - pos);
        pos = nl + 1;

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
            if (t.size() < 2 || !strictI32(t[1], version) || version != 1) {
                if (err) *err = "unsupported or missing OCPARTICLE version (want 'OCPARTICLE 1')";
                return false;
            }
            sawHeader = true;
        } else if (equalsCI(key, "NAME")) {
            ex.name = std::string(trim(after(key)));
        } else if (equalsCI(key, "SHAPE")) {
            if (t.size() != 5) {
                if (err) *err = "SHAPE requires exactly 4 fields: SHAPE point|sphere|box x y z";
                return false;
            }
            particles::EmitterShape shape;
            if (equalsCI(t[1], "point"))       shape = particles::EmitterShape::Point;
            else if (equalsCI(t[1], "sphere")) shape = particles::EmitterShape::Sphere;
            else if (equalsCI(t[1], "box"))    shape = particles::EmitterShape::Box;
            else {
                if (err) *err = "unknown SHAPE kind '" + std::string(t[1]) + "' (want point, sphere or box)";
                return false;
            }
            f32 x, y, z;
            if (!strictF32(t[2], x) || !strictF32(t[3], y) || !strictF32(t[4], z)) {
                if (err) *err = "SHAPE has a malformed number";
                return false;
            }
            out.shape = shape;
            out.shapeSize = Vec3{x, y, z};
        } else if (equalsCI(key, "BLEND")) {
            if (t.size() != 2) { if (err) *err = "BLEND requires exactly 1 field"; return false; }
            if (equalsCI(t[1], "opaque"))            out.blend = rhi::BlendMode::Opaque;
            else if (equalsCI(t[1], "alphablend"))   out.blend = rhi::BlendMode::AlphaBlend;
            else if (equalsCI(t[1], "premultiplied")) out.blend = rhi::BlendMode::PremultipliedAlpha;
            else if (equalsCI(t[1], "additive"))     out.blend = rhi::BlendMode::Additive;
            else {
                if (err) *err = "unknown BLEND mode '" + std::string(t[1]) + "'";
                return false;
            }
        } else if (equalsCI(key, "EMISSION")) {
            if (t.size() != 4) {
                if (err) *err = "EMISSION requires exactly 3 fields: EMISSION rate burstCount maxParticles";
                return false;
            }
            f32 rate; u32 burst, maxP;
            if (!strictF32(t[1], rate) || !strictU32(t[2], burst) || !strictU32(t[3], maxP)) {
                if (err) *err = "EMISSION has a malformed number";
                return false;
            }
            out.emissionRate = rate; out.burstCount = burst; out.maxParticles = maxP;
        } else if (equalsCI(key, "LIFETIME")) {
            if (t.size() != 3) {
                if (err) *err = "LIFETIME requires exactly 2 fields: LIFETIME min max";
                return false;
            }
            f32 lo, hi;
            if (!strictF32(t[1], lo) || !strictF32(t[2], hi)) {
                if (err) *err = "LIFETIME has a malformed number";
                return false;
            }
            out.lifetimeMin = lo; out.lifetimeMax = hi;
        } else if (equalsCI(key, "DIRECTION")) {
            if (t.size() != 5) {
                if (err) *err = "DIRECTION requires exactly 4 fields: DIRECTION x y z spreadDeg";
                return false;
            }
            f32 x, y, z, spread;
            if (!strictF32(t[1], x) || !strictF32(t[2], y) || !strictF32(t[3], z) || !strictF32(t[4], spread)) {
                if (err) *err = "DIRECTION has a malformed number";
                return false;
            }
            out.direction = Vec3{x, y, z};
            out.spreadDeg = spread;
        } else if (equalsCI(key, "SPEED")) {
            if (t.size() != 3) { if (err) *err = "SPEED requires exactly 2 fields: SPEED min max"; return false; }
            f32 lo, hi;
            if (!strictF32(t[1], lo) || !strictF32(t[2], hi)) {
                if (err) *err = "SPEED has a malformed number";
                return false;
            }
            out.speedMin = lo; out.speedMax = hi;
        } else if (equalsCI(key, "GRAVITY")) {
            if (t.size() != 4) { if (err) *err = "GRAVITY requires exactly 3 fields: GRAVITY x y z"; return false; }
            f32 x, y, z;
            if (!strictF32(t[1], x) || !strictF32(t[2], y) || !strictF32(t[3], z)) {
                if (err) *err = "GRAVITY has a malformed number";
                return false;
            }
            out.gravity = Vec3{x, y, z};
        } else if (equalsCI(key, "DAMPING")) {
            if (t.size() != 2) { if (err) *err = "DAMPING requires exactly 1 field"; return false; }
            f32 v;
            if (!strictF32(t[1], v)) { if (err) *err = "DAMPING has a malformed number"; return false; }
            out.damping = v;
        } else if (equalsCI(key, "SIZE")) {
            if (t.size() != 3) { if (err) *err = "SIZE requires exactly 2 fields: SIZE start end"; return false; }
            f32 s0, s1;
            if (!strictF32(t[1], s0) || !strictF32(t[2], s1)) {
                if (err) *err = "SIZE has a malformed number";
                return false;
            }
            out.sizeStart = s0; out.sizeEnd = s1;
        } else if (equalsCI(key, "COLOR")) {
            if (t.size() != 6) {
                if (err) *err = "COLOR requires exactly 5 fields: COLOR start|end r g b a";
                return false;
            }
            f32 c[4];
            for (u32 i = 0; i < 4; ++i) {
                if (!strictF32(t[2 + i], c[i])) { if (err) *err = "COLOR has a malformed number"; return false; }
            }
            if (equalsCI(t[1], "start"))      { for (u32 i = 0; i < 4; ++i) out.colorStart[i] = c[i]; }
            else if (equalsCI(t[1], "end"))   { for (u32 i = 0; i < 4; ++i) out.colorEnd[i]   = c[i]; }
            else {
                if (err) *err = "COLOR must say 'start' or 'end', got '" + std::string(t[1]) + "'";
                return false;
            }
        } else if (equalsCI(key, "TEX")) {
            if (t.size() != 2) { if (err) *err = "TEX requires exactly 1 field: TEX {guid:0x...}"; return false; }
            const std::string_view tok = t[1];
            if (tok.size() < 2 || tok.front() != '{' || tok.back() != '}') {
                if (err) *err = "TEX must be {guid:0x...}";
                return false;
            }
            const std::string_view inner = trim(tok.substr(1, tok.size() - 2));
            if (!startsWithCI(inner, "guid:")) {
                if (err) *err = "TEX must be {guid:0x...}";
                return false;
            }
            u64 id = 0;
            if (!strictU64(trim(inner.substr(5)), id) || id == 0) {
                if (err) *err = "TEX guid is malformed or zero";
                return false;
            }
            out.textureId = id;
        } else if (equalsCI(key, "GI")) {
            if (t.size() != 2) { if (err) *err = "GI requires exactly 1 field: GI on|off"; return false; }
            if (equalsCI(t[1], "on"))       out.receivesGI = true;
            else if (equalsCI(t[1], "off")) out.receivesGI = false;
            else {
                if (err) *err = "GI must say 'on' or 'off', got '" + std::string(t[1]) + "'";
                return false;
            }
        }
        // Any other key: forward-compat, unknown record. Skipped here, preserved verbatim by
        // writeOcparticle's merge below.
    }

    if (!sawHeader) {
        if (err) *err = "not an .ocparticle file: no OCPARTICLE header line";
        return false;
    }

    // RANGES, CHECKED HERE RATHER THAN PER-RECORD, because these are the only fields whose own
    // struct comments state a range, and a file is not valid until it is fully read anyway. Checked
    // at all because the simulator trusts them: a negative DAMPING is a velocity MULTIPLIER above
    // one, so a "0.4 damping" typo'd to -0.4 does not damp gently, it accelerates every particle
    // exponentially until the effect fills the screen. An author should be told which line was
    // wrong, not shown that.
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

    // The single commit point. Everything above wrote locals, so a caller who ignored the bool on a
    // malformed file still has the struct they came in with rather than a half-parsed effect.
    outEffect = out;
    if (extras) *extras = ex;
    return true;
}

// Loads an .ocparticle file. A missing or unreadable file is an error, never a default effect.
bool loadOcparticle(const std::string& path, particles::ParticleEffect& out, OcParticleExtras* extras,
                     std::string* err) {
    std::string text;
    if (!readFileText(path, text)) {
        if (err) *err = "could not read " + path;
        return false;
    }
    return parseOcparticle(text, out, extras, err);
}

// Renders an effect as .ocparticle text, merging into `existing` (see OcParticle.hpp).
std::string writeOcparticle(const particles::ParticleEffect& e, const OcParticleExtras* extras,
                             std::string_view existing) {
    const OcParticleExtras defaults{};
    const OcParticleExtras& ex = extras ? *extras : defaults;

    // The regenerated content for each record kind, built fresh from (e, ex) regardless of what
    // (if anything) `existing` had. Shared by both branches below, exactly like OcGraph.cpp's
    // nameLine/nodeBlock/etc.
    const std::string nameLine = ex.name.empty() ? std::string() : ("NAME " + ex.name + "\n");
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

    std::vector<OwnedLineKind> kinds(lines.size(), OwnedLineKind::Other);
    {
        bool sawHeader = false;
        for (usize i = 0; i < lines.size(); ++i) {
            kinds[i] = classifyLine(lines[i], sawHeader);
            if (kinds[i] == OwnedLineKind::Header) sawHeader = true;
        }
    }

    bool placedName = false, placedShape = false, placedBlend = false, placedEmission = false;
    bool placedLifetime = false, placedDirection = false, placedSpeed = false, placedGravity = false;
    bool placedDamping = false, placedSize = false, placedColorStart = false, placedColorEnd = false;
    bool placedTex = false, placedGi = false;

    std::string out;
    out.reserve(existing.size() + nameLine.size() + shapeLine.size() + blendLine.size() +
                emissionLine.size() + lifetimeLine.size() + directionLine.size() + speedLine.size() +
                gravityLine.size() + dampingLine.size() + sizeLine.size() + colorStartLine.size() +
                colorEndLine.size() + texLine.size() + giLine.size() + 64);

    for (usize i = 0; i < lines.size(); ++i) {
        switch (kinds[i]) {
        case OwnedLineKind::Header:
            out += "OCPARTICLE 1\n";
            break;
        case OwnedLineKind::Name:
            if (!placedName) { placedName = true; out += nameLine; }   // empty nameLine = line removed
            break;                                                     // any later duplicate is dropped
        case OwnedLineKind::Shape:
            if (!placedShape) { placedShape = true; out += shapeLine; }
            break;
        case OwnedLineKind::Blend:
            if (!placedBlend) { placedBlend = true; out += blendLine; }
            break;
        case OwnedLineKind::Emission:
            if (!placedEmission) { placedEmission = true; out += emissionLine; }
            break;
        case OwnedLineKind::Lifetime:
            if (!placedLifetime) { placedLifetime = true; out += lifetimeLine; }
            break;
        case OwnedLineKind::Direction:
            if (!placedDirection) { placedDirection = true; out += directionLine; }
            break;
        case OwnedLineKind::Speed:
            if (!placedSpeed) { placedSpeed = true; out += speedLine; }
            break;
        case OwnedLineKind::Gravity:
            if (!placedGravity) { placedGravity = true; out += gravityLine; }
            break;
        case OwnedLineKind::Damping:
            if (!placedDamping) { placedDamping = true; out += dampingLine; }
            break;
        case OwnedLineKind::Size:
            if (!placedSize) { placedSize = true; out += sizeLine; }
            break;
        case OwnedLineKind::ColorStart:
            if (!placedColorStart) { placedColorStart = true; out += colorStartLine; }
            break;
        case OwnedLineKind::ColorEnd:
            if (!placedColorEnd) { placedColorEnd = true; out += colorEndLine; }
            break;
        case OwnedLineKind::Tex:
            if (!placedTex) { placedTex = true; out += texLine; }      // empty texLine = line removed
            break;
        case OwnedLineKind::Gi:
            if (!placedGi) { placedGi = true; out += giLine; }
            break;
        case OwnedLineKind::Other:
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

// Writes an .ocparticle file, creating parent directories. Reads whatever is already at `path` and
// hands it to writeOcparticle as the merge base -- the same fix saveOcgraph needed (see its own
// comment): without this, every save would silently discard comments and anything unrecognised.
bool saveOcparticle(const std::string& path, const particles::ParticleEffect& e,
                     const OcParticleExtras* extras, std::string* err) {
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);

    std::string existing;
    {
        std::ifstream in(path, std::ios::binary);
        if (in) existing.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { if (err) *err = "could not open " + path + " for writing"; return false; }
    const std::string text = writeOcparticle(e, extras, existing);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) { if (err) *err = "write failed for " + path; return false; }
    return true;
}

} // namespace aver::fmt
