// .ocworld reader and writer: the text scanner and serialiser for the native world format.
#include "aver/formats/OcWorld.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/core/Hash.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

// Token `i` as a double, or `dflt` when the line is shorter than that.
f64 tokF(const std::vector<std::string_view>& t, usize i, f64 dflt = 0.0) {
    return i < t.size() ? parseF64(t[i]) : dflt;
}

// Formats a number for the text form: enough digits to round-trip, no trailing noise.
std::string num(f64 v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

} // namespace

// Parses an .ocworld or .ocmap from memory. Unknown records are skipped.
//
// NESTING, ADDED FOR THE WORLD OUTLINER'S HIERARCHY, and spelled with WORDS on purpose:
//
//     PLACE  Meshes/table.ocmesh  0 0 0  0 0 0  1  M_Wood
//     BEGIN
//       CHILD  Meshes/lamp.ocmesh  10 0 80  0 0 0  1  M_Brass
//       BEGIN
//         CHILD  Meshes/bulb.ocmesh  0 0 12  0 0 0  1  M_Glass
//       END
//       CHILD  Meshes/book.ocmesh  -5 0 80  0 0 0  1  M_Paper
//     END
//
// BEGIN opens a scope on the most recent placement, END closes the innermost, and CHILD / CHILDG
// are placements parented to the innermost open scope (CHILDG carries non-uniform scale, mirroring
// PLACE / PLACEG). It is braces spelled as words -- and that IS the design, not decoration.
//
// WHY NOT BRACES, AND WHY NOT INDENTATION. Both fail CATASTROPHICALLY in a build that predates this,
// and differently:
//   * `PLACE ... {` hits the PLACE token loop's catch-all -- `else if (p.material.empty())
//     p.material = t[i]` -- so an older reader sets that placement's material to "{". That name is
//     then interned as a surface and WRITTEN BACK on the next save, destroying the placement's real
//     material. Silent, and it corrupts the file.
//   * Indentation is discarded by trim/splitWhitespace before any of this runs, so an older reader
//     loads a child at its parent-relative offset AS A WORLD POSITION -- and both builds' writers
//     then emit identical bytes for two different scenes, which is the worst property a format can
//     have.
// BEGIN / CHILD / END are unknown RECORDS instead, and this parser's record chain has no `else` --
// the "unknown records are skipped, not failed" contract FormatTest pins at :387, :768 and :887. So
// an older build loses the children rather than misplacing them: a hierarchical level opens as its
// root placements only, visibly missing objects rather than silently wrong ones, and no material is
// harmed. As with any unmodelled data, an old build that then saves drops them -- the same class as
// the levelPcgVolumes_ carry-through the editor's saveLevel already has.
//
// THE COST, stated rather than discovered later: this was a stateless line loop whose only carried
// variable was sawHeader, with exactly one failure mode. It now carries a scope stack and has a
// second one.
bool parseOcworld(std::string_view text, OcWorldData& out, std::string* err) {
    out = OcWorldData{};
    bool sawHeader = false;
    // Indices into out.placements: the open BEGIN scopes, innermost last, and the most recent
    // placement a BEGIN could attach to.
    std::vector<i32> scope;
    i32 lastPlacement = -1;

    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view rawLine = text.substr(pos, nl - pos);
        pos = nl + 1;

        std::string_view line = stripTrailingSemicolon(truncateHash(rawLine));
        if (line.empty()) continue;

        std::vector<std::string_view> t = splitWhitespace(line);
        if (t.empty()) continue;
        std::string_view key = t[0];

        if (equalsCI(key, "OCWORLD") || equalsCI(key, "OCMAP")) {
            out.version = t.size() > 1 ? parseI32(t[1], 1) : 1;
            sawHeader = true;
        } else if (equalsCI(key, "ID")) {
            out.contentId = t.size() > 1 ? parseU64(t[1]) : 0;
        } else if (equalsCI(key, "NAME")) {
            out.name = t.size() > 1 ? std::string(t[1]) : std::string();
        } else if (equalsCI(key, "BUILD")) {
            out.build = static_cast<u32>(t.size() > 1 ? parseU64(t[1]) : 0);
        } else if (equalsCI(key, "ALGO")) {
            out.algo = static_cast<u32>(t.size() > 1 ? parseU64(t[1]) : 3);
        } else if (equalsCI(key, "GAMEMODE") && t.size() >= 2) {
            // The rest of the line, so a class name containing spaces survives. Names are authored
            // by whoever wrote the C# class, and nothing forbids one.
            out.gameMode = std::string(t[1]);
            for (usize i = 2; i < t.size(); ++i) { out.gameMode += ' '; out.gameMode += std::string(t[i]); }
        } else if (equalsCI(key, "SPAWN")) {
            out.hasSpawn = true;
            out.spawnX = tokF(t, 1); out.spawnY = tokF(t, 2);
            out.spawnZ = tokF(t, 3); out.spawnYaw = tokF(t, 4);
        } else if (equalsCI(key, "CAMERA")) {
            // THE EDITOR'S VIEWPOINT, NOT THE PLAYER SPAWN above -- see OcWorldData::hasCamera for
            // why those are deliberately two records rather than one.
            //
            // tokF yields 0 for a token that is not present, so a short CAMERA line degrades to
            // zeros instead of failing the parse. The speed is optional in exactly that way, and 0
            // there means "unstated" rather than "stand still".
            out.hasCamera = true;
            out.camX = tokF(t, 1); out.camY = tokF(t, 2); out.camZ = tokF(t, 3);
            out.camYaw = tokF(t, 4); out.camPitch = tokF(t, 5);
            out.camSpeed = tokF(t, 6);
        } else if (equalsCI(key, "SUN")) {
            out.hasSun = true;
            // `dir` and `elev`/`azim` are alternative spellings of the same field; the later token
            // wins. Degrees exist because the physical sky makes elevation the only input that
            // matters, and nobody authors a time of day as a unit vector.
            f64 elevDeg = 0.0, azimDeg = 0.0;
            bool sawElev = false, sawAzim = false;
            for (usize i = 1; i < t.size(); ++i) {
                if (equalsCI(t[i], "dir") && i + 3 < t.size()) {
                    out.sunDir[0] = parseF64(t[i+1]); out.sunDir[1] = parseF64(t[i+2]); out.sunDir[2] = parseF64(t[i+3]);
                    sawElev = sawAzim = false;
                } else if (equalsCI(t[i], "elev") && i + 1 < t.size()) {
                    elevDeg = parseF64(t[i+1]); sawElev = true;
                } else if (equalsCI(t[i], "azim") && i + 1 < t.size()) {
                    azimDeg = parseF64(t[i+1]); sawAzim = true;
                } else if (equalsCI(t[i], "color") && i + 3 < t.size()) {
                    out.sunColor[0] = parseF64(t[i+1]); out.sunColor[1] = parseF64(t[i+2]); out.sunColor[2] = parseF64(t[i+3]);
                } else if (equalsCI(t[i], "lux") && i + 1 < t.size()) {
                    out.sunLux = parseF64(t[i+1]);
                } else if (equalsCI(t[i], "kelvin") && i + 1 < t.size()) {
                    out.sunTemperatureK = parseF64(t[i+1]);
                } else if (equalsCI(t[i], "angular") && i + 1 < t.size()) {
                    out.sunAngularDeg = parseF64(t[i+1]);
                }
            }
            if (sawElev || sawAzim) {
                const f64 kDeg = 3.14159265358979 / 180.0;
                const f64 ce = std::cos(elevDeg * kDeg);
                out.sunDir[0] = ce * std::cos(azimDeg * kDeg);
                out.sunDir[1] = ce * std::sin(azimDeg * kDeg);
                out.sunDir[2] = std::sin(elevDeg * kDeg);
            }
        } else if (equalsCI(key, "SKY")) {
            out.hasSky = true;
            for (usize i = 1; i < t.size(); ++i) {
                if (equalsCI(t[i], "model") && i + 1 < t.size()) {
                    out.skyPhysical = !equalsCI(t[i+1], "authored");
                } else if (equalsCI(t[i], "mie") && i + 1 < t.size()) {
                    out.skyMieScatter = parseF64(t[i+1]);
                } else if (equalsCI(t[i], "multiscatter") && i + 1 < t.size()) {
                    out.skyMultiScatter = parseF64(t[i+1]);
                } else if (equalsCI(t[i], "steps") && i + 1 < t.size()) {
                    out.skyViewSteps = parseI32(t[i+1], 0);
                } else if (equalsCI(t[i], "aerial") && i + 1 < t.size()) {
                    out.skyAerialSteps = parseI32(t[i+1], 0);
                } else if (equalsCI(t[i], "zenith") && i + 3 < t.size()) {
                    out.skyZenith[0] = parseF64(t[i+1]); out.skyZenith[1] = parseF64(t[i+2]); out.skyZenith[2] = parseF64(t[i+3]);
                } else if (equalsCI(t[i], "horizon") && i + 3 < t.size()) {
                    out.skyHorizon[0] = parseF64(t[i+1]); out.skyHorizon[1] = parseF64(t[i+2]); out.skyHorizon[2] = parseF64(t[i+3]);
                } else if (equalsCI(t[i], "dome") && i + 1 < t.size()) {
                    out.skyDomeExponent = parseF64(t[i+1]);
                } else if (equalsCI(t[i], "ground") && i + 3 < t.size()) {
                    out.skyGroundAlbedo[0] = parseF64(t[i+1]); out.skyGroundAlbedo[1] = parseF64(t[i+2]); out.skyGroundAlbedo[2] = parseF64(t[i+3]);
                } else if (equalsCI(t[i], "groundblend") && i + 1 < t.size()) {
                    out.skyGroundBlend = parseF64(t[i+1]);
                } else if (equalsCI(t[i], "skylight") && i + 1 < t.size()) {
                    out.skyLight = parseF64(t[i+1]);
                } else if (equalsCI(t[i], "mieextinction") && i + 1 < t.size()) {
                    out.skyMieExtinction = parseF64(t[i+1]);
                } else if (equalsCI(t[i], "miephase") && i + 1 < t.size()) {
                    out.skyMiePhaseG = parseF64(t[i+1]);
                } else if (equalsCI(t[i], "rayleighkm") && i + 1 < t.size()) {
                    out.skyRayleighKm = parseF64(t[i+1]);
                } else if (equalsCI(t[i], "miekm") && i + 1 < t.size()) {
                    out.skyMieKm = parseF64(t[i+1]);
                } else if (equalsCI(t[i], "planetkm") && i + 1 < t.size()) {
                    out.skyPlanetKm = parseF64(t[i+1]);
                } else if (equalsCI(t[i], "airkm") && i + 1 < t.size()) {
                    out.skyAirDepthKm = parseF64(t[i+1]);
                }
            }
        } else if (equalsCI(key, "FOG")) {
            out.hasFog = true;
            for (usize i = 1; i < t.size(); ++i) {
                if (equalsCI(t[i], "density") && i + 1 < t.size()) out.fogDensity = parseF64(t[i+1]);
                else if (equalsCI(t[i], "color") && i + 3 < t.size()) {
                    out.fogColor[0] = parseF64(t[i+1]); out.fogColor[1] = parseF64(t[i+2]); out.fogColor[2] = parseF64(t[i+3]);
                }
                else if (equalsCI(t[i], "falloff")    && i + 1 < t.size()) out.fogFalloff    = parseF64(t[i+1]);
                else if (equalsCI(t[i], "height")     && i + 1 < t.size()) out.fogHeight     = parseF64(t[i+1]);
                else if (equalsCI(t[i], "start")      && i + 1 < t.size()) out.fogStart      = parseF64(t[i+1]);
                else if (equalsCI(t[i], "maxopacity") && i + 1 < t.size()) out.fogMaxOpacity = parseF64(t[i+1]);
            }
        } else if (equalsCI(key, "CLOUDS")) {
            // The record's PRESENCE is "this level has an opinion about clouds"; the on|off word is
            // the opinion. See OcWorldEnv::hasClouds for why those are two facts and not one.
            out.hasClouds = true;
            for (usize i = 1; i < t.size(); ++i) {
                if      (equalsCI(t[i], "on"))  out.cloudsEnabled = true;
                else if (equalsCI(t[i], "off")) out.cloudsEnabled = false;
                else if (equalsCI(t[i], "coverage") && i + 1 < t.size()) out.cloudCoverage    = parseF64(t[i+1]);
                else if (equalsCI(t[i], "density")  && i + 1 < t.size()) out.cloudDensity     = parseF64(t[i+1]);
                else if (equalsCI(t[i], "bottom")   && i + 1 < t.size()) out.cloudBottom      = parseF64(t[i+1]);
                else if (equalsCI(t[i], "top")      && i + 1 < t.size()) out.cloudTop         = parseF64(t[i+1]);
                else if (equalsCI(t[i], "feature")  && i + 1 < t.size()) out.cloudFeatureSize = parseF64(t[i+1]);
                else if (equalsCI(t[i], "wind")     && i + 2 < t.size()) {
                    out.cloudWind[0] = parseF64(t[i+1]); out.cloudWind[1] = parseF64(t[i+2]);
                }
            }
        } else if (equalsCI(key, "LANDSCAPE")) {
            OcLandscapePlacement lp;
            for (usize i = 1; i < t.size(); ++i) {
                if      (equalsCI(t[i], "name")     && i + 1 < t.size()) lp.name     = std::string(t[++i]);
                else if (equalsCI(t[i], "section")  && i + 1 < t.size()) lp.section  = std::string(t[++i]);
                else if (equalsCI(t[i], "material") && i + 1 < t.size()) lp.material = std::string(t[++i]);
                else if (equalsCI(t[i], "at")      && i + 3 < t.size()) {
                    lp.x = parseF64(t[i+1]); lp.y = parseF64(t[i+2]); lp.z = parseF64(t[i+3]);
                    i += 3;
                } else if (equalsCI(t[i], "extent") && i + 1 < t.size()) {
                    lp.extentCm = parseF64(t[++i]);
                }
            }
            out.landscapes.push_back(std::move(lp));
        } else if (equalsCI(key, "PCGVOLUME")) {
            OcPcgVolume v;
            for (usize i = 1; i < t.size(); ++i) {
                if      (equalsCI(t[i], "name")    && i + 1 < t.size()) v.name          = std::string(t[++i]);
                else if (equalsCI(t[i], "seed")    && i + 1 < t.size()) v.seed          = static_cast<i32>(parseF64(t[++i]));
                else if (equalsCI(t[i], "cell")    && i + 1 < t.size()) v.cellSizeCm    = parseF64(t[++i]);
                else if (equalsCI(t[i], "octaves") && i + 1 < t.size()) v.octaves       = static_cast<i32>(parseF64(t[++i]));
                else if (equalsCI(t[i], "floor")   && i + 1 < t.size()) v.coverageFloor = parseF64(t[++i]);
                else if (equalsCI(t[i], "bias")    && i + 1 < t.size()) v.coverageBias  = parseF64(t[++i]);
                else if (equalsCI(t[i], "samples") && i + 1 < t.size()) v.samplesPerAxis = static_cast<i32>(parseF64(t[++i]));
                else if (equalsCI(t[i], "radius")  && i + 1 < t.size()) v.radiusChunks   = static_cast<i32>(parseF64(t[++i]));
                // A BARE TOKEN, not `infinite 1`. It is a statement about what the field IS rather
                // than a value it carries, and it reads that way in the file.
                else if (equalsCI(t[i], "infinite")) v.infinite = true;
                else if (equalsCI(t[i], "bounds") && i + 6 < t.size()) {
                    v.infinite = false;
                    v.boundsMin[0] = parseF64(t[i+1]); v.boundsMin[1] = parseF64(t[i+2]); v.boundsMin[2] = parseF64(t[i+3]);
                    v.boundsMax[0] = parseF64(t[i+4]); v.boundsMax[1] = parseF64(t[i+5]); v.boundsMax[2] = parseF64(t[i+6]);
                    i += 6;
                }
            }
            // Guarded rather than trusted: a cell size of zero divides by zero in every sampler
            // that reads this, and the file is authored by hand.
            if (v.cellSizeCm <= 0.0) v.cellSizeCm = 1600.0;
            if (v.octaves < 1) v.octaves = 1;
            out.pcgVolumes.push_back(std::move(v));
        } else if (equalsCI(key, "SCATTER")) {
            OcScatterSpecies sp;
            // density is read into locals first: two tokens have to arrive together or not at all, and
            // the struct's own unbounded default must survive untouched when the line has no `density`
            // clause -- exactly PCGVOLUME's `bounds` shape, one line up.
            bool sawDensity = false;
            f64 dMin = 0.0, dMax = 0.0;
            for (usize i = 1; i < t.size(); ++i) {
                if      (equalsCI(t[i], "mesh")     && i + 1 < t.size()) sp.meshPath = std::string(t[++i]);
                else if (equalsCI(t[i], "material") && i + 1 < t.size()) sp.material = std::string(t[++i]);
                else if (equalsCI(t[i], "volume")   && i + 1 < t.size()) sp.volume = std::string(t[++i]);
                else if (equalsCI(t[i], "weight")   && i + 1 < t.size()) sp.weight = parseF64(t[++i]);
                else if (equalsCI(t[i], "scale")    && i + 2 < t.size()) {
                    sp.scaleMin = parseF64(t[i+1]); sp.scaleMax = parseF64(t[i+2]); i += 2;
                } else if (equalsCI(t[i], "density") && i + 2 < t.size()) {
                    dMin = parseF64(t[i+1]); dMax = parseF64(t[i+2]); sawDensity = true; i += 2;
                } else if (equalsCI(t[i], "collide") && i + 1 < t.size()) {
                    sp.collisionRadiusCm = parseF64(t[++i]);
                } else if (equalsCI(t[i], "noyaw")) {
                    sp.randomizeYaw = false;
                }
            }
            if (sawDensity) { sp.densityMin = dMin; sp.densityMax = dMax; }
            out.scatterSpecies.push_back(std::move(sp));
        } else if (equalsCI(key, "WATER")) {
            OcWaterPlacement wp;
            for (usize i = 1; i < t.size(); ++i) {
                if      (equalsCI(t[i], "name")  && i + 1 < t.size()) wp.name    = std::string(t[++i]);
                else if (equalsCI(t[i], "level") && i + 1 < t.size()) wp.levelCm = parseF64(t[++i]);
                // A BARE TOKEN, matching PCGVOLUME's own `infinite`: a statement about what the
                // surface IS rather than a value it carries.
                else if (equalsCI(t[i], "infinite")) wp.infinite = true;
                // Another bare token, same shape as `infinite`: a statement about what this surface
                // IS, carrying no value of its own.
                else if (equalsCI(t[i], "simulate")) wp.simulate = true;
                else if (equalsCI(t[i], "bounds") && i + 4 < t.size()) {
                    wp.infinite = false;
                    wp.boundsMin[0] = parseF64(t[i+1]); wp.boundsMin[1] = parseF64(t[i+2]);
                    wp.boundsMax[0] = parseF64(t[i+3]); wp.boundsMax[1] = parseF64(t[i+4]);
                    i += 4;
                }
                // The four solver knobs, each a `key value` pair rather than a bare token like
                // `simulate` -- and each on its OWN branch here rather than folded into a shared
                // "numeric key" helper, matching every other multi-field record in this parser
                // (PCGVOLUME, SCATTER). An old parser reading a NEW file simply never matches any of
                // these four `else if`s and falls out of the loop having skipped them, the same
                // silent-skip every future token this loop has never heard of already gets -- there
                // is no `else` clause here to make that anything other than automatic. A new parser
                // reading an OLD file never sees these keys at all, so wp keeps its -1 "not
                // authored" defaults, which is the whole reason OcWaterPlacement's own comment gives
                // for choosing -1 over 0.
                else if (equalsCI(t[i], "compliance") && i + 1 < t.size()) wp.compliance = parseF64(t[++i]);
                else if (equalsCI(t[i], "damping")    && i + 1 < t.size()) wp.damping    = parseF64(t[++i]);
                else if (equalsCI(t[i], "iterations") && i + 1 < t.size())
                    wp.iterations = static_cast<i32>(parseF64(t[++i]));
                else if (equalsCI(t[i], "pressure")   && i + 1 < t.size()) wp.pressure   = parseF64(t[++i]);
                // THE MATERIAL LAYER, same `key value` shape as the four solver knobs just above and
                // for the identical reasons (own branch per key, silent-skip for an old parser
                // reading a new file or a new parser reading an old one -- see those four keys' own
                // comment just above for the full reasoning, which applies here unchanged).
                else if (equalsCI(t[i], "preset")     && i + 1 < t.size()) wp.preset    = std::string(t[++i]);
                else if (equalsCI(t[i], "density")    && i + 1 < t.size()) wp.density   = parseF64(t[++i]);
                else if (equalsCI(t[i], "viscosity")  && i + 1 < t.size()) wp.viscosity = parseF64(t[++i]);
                // THE SURFACE MATERIAL -- an .ocmat name, NOT a solver preset. `preset` two lines up
                // is the physics one; see OcWaterPlacement::material's own comment for why the two
                // are kept apart so deliberately.
                else if (equalsCI(t[i], "material")   && i + 1 < t.size()) wp.material  = std::string(t[++i]);
            }
            out.waters.push_back(std::move(wp));
        } else if (equalsCI(key, "WAVE")) {
            OcGerstnerWave gw;
            for (usize i = 1; i < t.size(); ++i) {
                if      (equalsCI(t[i], "water") && i + 1 < t.size()) gw.water = std::string(t[++i]);
                else if (equalsCI(t[i], "dir")   && i + 2 < t.size()) {
                    gw.dirX = parseF64(t[i+1]); gw.dirZ = parseF64(t[i+2]); i += 2;
                } else if (equalsCI(t[i], "wavelength") && i + 1 < t.size()) gw.wavelengthCm = parseF64(t[++i]);
                else if (equalsCI(t[i], "amplitude")    && i + 1 < t.size()) gw.amplitudeCm  = parseF64(t[++i]);
                else if (equalsCI(t[i], "steepness")    && i + 1 < t.size()) gw.steepness    = parseF64(t[++i]);
            }
            out.waves.push_back(std::move(gw));
        } else if (equalsCI(key, "BEGIN")) {
            // OPENS A SCOPE ON THE MOST RECENT PLACEMENT. See the grammar note above parseOcworld.
            if (lastPlacement < 0) {
                if (err) *err = "BEGIN with no placement before it to attach children to";
                return false;
            }
            scope.push_back(lastPlacement);
        } else if (equalsCI(key, "END")) {
            if (scope.empty()) {
                if (err) *err = "END with no matching BEGIN";
                return false;
            }
            scope.pop_back();
        } else if (equalsCI(key, "PLACE") || equalsCI(key, "PLACEG")
                || equalsCI(key, "CHILD") || equalsCI(key, "CHILDG")) {
            const bool g     = equalsCI(key, "PLACEG") || equalsCI(key, "CHILDG");
            const bool child = equalsCI(key, "CHILD")  || equalsCI(key, "CHILDG");
            if (child && scope.empty()) {
                if (err) *err = "CHILD outside any BEGIN/END scope -- nothing to parent it to";
                return false;
            }
            OcWorldPlacement p;
            p.parent = child ? scope.back() : -1;
            p.asset = t.size() > 1 ? std::string(t[1]) : std::string();
            p.x = tokF(t, 2); p.y = tokF(t, 3); p.z = tokF(t, 4);
            p.yaw = tokF(t, 5); p.pitch = tokF(t, 6); p.roll = tokF(t, 7);
            usize next;
            if (g) {
                p.sx = tokF(t, 8, 1.0); p.sy = tokF(t, 9, 1.0); p.sz = tokF(t, 10, 1.0);
                next = 11;
            } else {
                const f64 s = tokF(t, 8, 1.0);
                p.sx = p.sy = p.sz = (s == 0.0 ? 1.0 : s);
                next = 9;
            }
            for (usize i = next; i < t.size(); ++i) {
                if (equalsCI(t[i], "nocollide")) p.collide = false;
                // Bare token, beside `nocollide`. BEFORE the material fallback below, or the
                // word would be swallowed as a surface name -- which is how a trailing bare
                // token silently becomes a material called "snap".
                else if (equalsCI(t[i], "snap")) p.snapToGround = true;
                // `class <name>`, ALSO before the material fallback, for the identical reason: a
                // keyword-plus-argument pair, not a bare flag, so the argument token is consumed
                // (`++i`) rather than falling through and being read back as a material name.
                else if (equalsCI(t[i], "class") && i + 1 < t.size()) { p.className = std::string(t[++i]); }
                else if (p.material.empty()) p.material = std::string(t[i]);
            }
            p.objectId = fnv1a64(std::string_view(p.asset));
            lastPlacement = static_cast<i32>(out.placements.size());
            out.placements.push_back(std::move(p));
        }
    }

    // THE PARSER'S SECOND FAILURE MODE, and its first new one since it was written. Everything else
    // in this function either matches a record or silently skips it -- the "unknown records are
    // skipped, not failed" contract FormatTest pins in three places -- and the only way to fail was
    // a missing header. Nesting adds state that can be left dangling, and a file whose BEGIN is
    // never closed has to say so: the alternative is silently adopting the rest of the level as
    // children of one table leg.
    if (!scope.empty()) {
        if (err) *err = "unbalanced BEGIN/END: " + std::to_string(scope.size()) +
                        " scope(s) still open at end of file";
        return false;
    }

    if (!sawHeader) {
        if (err) *err = "not an .ocworld/.ocmap file (no OCWORLD or OCMAP header line)";
        return false;
    }
    if (out.contentId == 0 && !out.name.empty()) out.contentId = fnv1a64(std::string_view(out.name));
    return true;
}

// Loads an .ocworld from disk.
bool loadOcworld(const std::string& path, OcWorldData& out, std::string* err) {
    std::string text;
    if (!readFileText(path, text)) {
        if (err) *err = "could not read " + path;
        return false;
    }
    return parseOcworld(text, out, err);
}

// See the header comment for the real-project evidence behind this -- and for why it is a content
// scan rather than a header check. Reproduces parseOcworld's own line-scan (truncate at '#', trim,
// split on whitespace) rather than calling it, because parseOcworld's own answer (whether the file
// "parsed") is no signal at all here: it parses every one of this repo's four real level files,
// legacy and OCWORLD-grammar alike, and reports success for all of them.
bool levelFileIsLegacyOcmap(const std::string& path) {
    std::string text;
    if (!readFileText(path, text)) return false;   // the real loader is what reports a read failure

    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string_view rawLine(text.data() + pos, nl - pos);
        pos = nl + 1;

        const std::string_view line = stripTrailingSemicolon(truncateHash(rawLine));
        if (line.empty()) continue;

        const std::vector<std::string_view> t = splitWhitespace(line);
        if (t.empty()) continue;
        const std::string_view key = t[0];
        // The six record kinds only OcMap.cpp's parseOcmap has a branch for -- see this header's
        // own comment for the two real files (ElectricDreams', FirstPerson's own Default.ocmap)
        // that rule out testing the header token instead.
        if (equalsCI(key, "ROOT") || equalsCI(key, "CLIENT") || equalsCI(key, "SURFACE") ||
            equalsCI(key, "GROUND") || equalsCI(key, "KILLZ") || equalsCI(key, "DEFORM"))
            return true;
    }
    return false;
}

// Serialises a world to the text form. PLACE for a uniform scale, PLACEG otherwise.
std::string writeOcworld(const OcWorldData& w) {
    std::string s;
    s.reserve(256 + w.placements.size() * 96);
    s += "OCWORLD 1\n";
    s += "# Written by the Aver Engine editor. Centimetres, +X forward, +Y right, +Z up.\n";

    char idbuf[32];
    const u64 id = w.contentId ? w.contentId : fnv1a64(std::string_view(w.name));
    std::snprintf(idbuf, sizeof idbuf, "0x%016llX", static_cast<unsigned long long>(id));
    s += "ID "; s += idbuf; s += "\n";
    s += "NAME "; s += (w.name.empty() ? std::string("untitled") : w.name); s += "\n";
    s += "BUILD "; s += std::to_string(w.build); s += "\n";
    s += "ALGO "; s += std::to_string(w.algo); s += "\n";

    // Omitted when empty: no override is the default, and a blank GAMEMODE line would read back as
    // a class named "" rather than as an absence.
    if (!w.gameMode.empty()) s += "GAMEMODE " + w.gameMode + "\n";
    if (w.hasSpawn) {
        s += "SPAWN " + num(w.spawnX) + " " + num(w.spawnY) + " " + num(w.spawnZ) + " " + num(w.spawnYaw) + "\n";
    }
    // WRITTEN ONLY WHEN THE LEVEL HAS ONE, so every level authored before this record existed keeps
    // round-tripping byte-identically instead of gaining a line the moment it is opened and saved.
    if (w.hasCamera) {
        s += "CAMERA " + num(w.camX) + " " + num(w.camY) + " " + num(w.camZ) +
             " " + num(w.camYaw) + " " + num(w.camPitch) + " " + num(w.camSpeed) + "\n";
    }
    if (w.hasSun) {
        // The VECTOR is written, because it round-trips exactly where degrees do not. The elevation
        // rides along as a trailing comment so the line is still readable by a person -- the parser
        // truncates at '#', so it cannot be read back and drift.
        const f64 len = std::sqrt(w.sunDir[0]*w.sunDir[0] + w.sunDir[1]*w.sunDir[1] + w.sunDir[2]*w.sunDir[2]);
        const f64 kRad = 180.0 / 3.14159265358979;
        const f64 elev = len > 1e-9 ? std::asin(w.sunDir[2] / len) * kRad : 0.0;
        const f64 azim = std::atan2(w.sunDir[1], w.sunDir[0]) * kRad;
        s += "SUN dir " + num(w.sunDir[0]) + " " + num(w.sunDir[1]) + " " + num(w.sunDir[2]) +
             " color " + num(w.sunColor[0]) + " " + num(w.sunColor[1]) + " " + num(w.sunColor[2]) +
             " lux " + num(w.sunLux) +
             " kelvin " + num(w.sunTemperatureK) +
             " angular " + num(w.sunAngularDeg) +
             "   # elev " + num(elev) + " azim " + num(azim) + "\n";
    }
    if (w.hasSky) {
        s += "SKY model ";
        s += w.skyPhysical ? "physical" : "authored";
        // WRITTEN UNCONDITIONALLY, unlike the air overrides below, because zero is an authored value
        // for every one of them: a black zenith, a ground that reflects nothing, a sky light turned
        // off. A `>= 0` guard would make each of those unsettable, which is the trap the air's
        // sentinel avoids only because those quantities are strictly positive.
        s += " zenith " + num(w.skyZenith[0]) + " " + num(w.skyZenith[1]) + " " + num(w.skyZenith[2]);
        s += " horizon " + num(w.skyHorizon[0]) + " " + num(w.skyHorizon[1]) + " " + num(w.skyHorizon[2]);
        s += " dome " + num(w.skyDomeExponent);
        s += " ground " + num(w.skyGroundAlbedo[0]) + " " + num(w.skyGroundAlbedo[1]) + " " + num(w.skyGroundAlbedo[2]);
        s += " groundblend " + num(w.skyGroundBlend);
        s += " skylight " + num(w.skyLight);
        if (w.skyMieScatter    >= 0.0) s += " mie " + num(w.skyMieScatter);
        if (w.skyMieExtinction >= 0.0) s += " mieextinction " + num(w.skyMieExtinction);
        if (w.skyMiePhaseG     >= 0.0) s += " miephase " + num(w.skyMiePhaseG);
        if (w.skyRayleighKm    >= 0.0) s += " rayleighkm " + num(w.skyRayleighKm);
        if (w.skyMieKm         >= 0.0) s += " miekm " + num(w.skyMieKm);
        if (w.skyPlanetKm      >= 0.0) s += " planetkm " + num(w.skyPlanetKm);
        if (w.skyAirDepthKm    >= 0.0) s += " airkm " + num(w.skyAirDepthKm);
        if (w.skyMultiScatter  >= 0.0) s += " multiscatter " + num(w.skyMultiScatter);
        if (w.skyViewSteps     >  0)   s += " steps " + std::to_string(w.skyViewSteps);
        if (w.skyAerialSteps   >  0)   s += " aerial " + std::to_string(w.skyAerialSteps);
        s += "\n";
    }
    if (w.hasFog) {
        s += "FOG exp density " + num(w.fogDensity) +
             " color " + num(w.fogColor[0]) + " " + num(w.fogColor[1]) + " " + num(w.fogColor[2]) +
             " falloff " + num(w.fogFalloff) +
             " height " + num(w.fogHeight) +
             " start " + num(w.fogStart) +
             " maxopacity " + num(w.fogMaxOpacity) + "\n";
    }
    // OMITTED ENTIRELY when the level never spoke about clouds, so a file that predates this record
    // does not grow one and every level in the tree stays byte-identical through a load and save.
    if (w.hasClouds) {
        s += "CLOUDS ";
        s += w.cloudsEnabled ? "on" : "off";
        s += " coverage " + num(w.cloudCoverage) +
             " density " + num(w.cloudDensity) +
             " bottom " + num(w.cloudBottom) +
             " top " + num(w.cloudTop) +
             " feature " + num(w.cloudFeatureSize) +
             " wind " + num(w.cloudWind[0]) + " " + num(w.cloudWind[1]) + "\n";
    }

    if (!w.landscapes.empty()) {
        s += "\n";
        for (const OcLandscapePlacement& lp : w.landscapes) {
            s += "LANDSCAPE name " + (lp.name.empty() ? std::string("unnamed") : lp.name) +
                 " section " + lp.section;
            // Omitted when unset, for the same reason `extent` is below: empty means "the renderer
            // keeps its own default", and writing a blank token back would not round-trip.
            if (!lp.material.empty()) s += " material " + lp.material;
            s += " at " + num(lp.x) + " " + num(lp.y) + " " + num(lp.z);
            // OMITTED WHEN UNSET, same reasoning as PCGVOLUME's `samples` just below: 0 is this
            // field's "nothing declared, ask the section file" sentinel, and a level that never
            // stated an extent should not come back from a save claiming zero.
            if (lp.extentCm > 0.0) s += " extent " + num(lp.extentCm);
            s += "\n";
        }
    }

    if (!w.pcgVolumes.empty()) {
        s += "\n";
        for (const OcPcgVolume& v : w.pcgVolumes) {
            s += "PCGVOLUME name " + (v.name.empty() ? std::string("unnamed") : v.name) +
                 " seed " + std::to_string(v.seed) +
                 " cell " + num(v.cellSizeCm) +
                 " octaves " + std::to_string(v.octaves) +
                 " floor " + num(v.coverageFloor) +
                 " bias " + num(v.coverageBias);
            // Omitted when unset, rather than written as `samples 0`: zero is this field's "the
            // runtime keeps its own default" sentinel, and a level that never mentioned sampling
            // should not come back from a save claiming to have asked for none.
            if (v.samplesPerAxis > 0) s += " samples " + std::to_string(v.samplesPerAxis);
            // Same unset-is-not-a-value rule as `samples` directly above.
            if (v.radiusChunks > 0) s += " radius " + std::to_string(v.radiusChunks);
            // The bounds token LAST, because parsing `bounds` consumes the six numbers after it and
            // anything following them would have to be re-found. Writing it last means the reader
            // never has to.
            if (v.infinite) {
                s += " infinite";
            } else {
                s += " bounds " + num(v.boundsMin[0]) + " " + num(v.boundsMin[1]) + " " + num(v.boundsMin[2]) +
                     " " + num(v.boundsMax[0]) + " " + num(v.boundsMax[1]) + " " + num(v.boundsMax[2]);
            }
            s += "\n";
        }
    }

    if (!w.scatterSpecies.empty()) {
        s += "\n";
        for (const OcScatterSpecies& sp : w.scatterSpecies) {
            s += "SCATTER mesh " + sp.meshPath +
                 " material " + sp.material;
            // Omitted when empty, same unset-is-not-a-value rule the density band below follows:
            // empty means "the first non-Sky volume", which is what every species meant before this
            // token existed, so a level that never named a volume must not come back naming one.
            if (!sp.volume.empty()) s += " volume " + sp.volume;
            s += " weight " + num(sp.weight) +
                 " scale " + num(sp.scaleMin) + " " + num(sp.scaleMax);
            // OMITTED, DELIBERATELY, rather than printed as ~1.79769e+308: an unbounded band is the
            // default every species starts from, and a level a person can still read should never
            // have to spell out float's own sentinel. The threshold is half of max rather than max
            // itself so a value merely CLOSE to the sentinel (never produced by this writer, but not
            // impossible from a hand edit) still round-trips as an explicit, printed band.
            const bool unbounded = sp.densityMin <= -std::numeric_limits<f64>::max() / 2.0 &&
                                    sp.densityMax >=  std::numeric_limits<f64>::max() / 2.0;
            if (!unbounded) s += " density " + num(sp.densityMin) + " " + num(sp.densityMax);
            s += " collide " + num(sp.collisionRadiusCm);
            // A BARE TOKEN, matching PLACE's `nocollide` and PCGVOLUME's `infinite`: a statement about
            // what the species IS, not a value it carries.
            if (!sp.randomizeYaw) s += " noyaw";
            s += "\n";
        }
    }

    if (!w.waters.empty()) {
        s += "\n";
        for (const OcWaterPlacement& wp : w.waters) {
            s += "WATER name " + (wp.name.empty() ? std::string("unnamed") : wp.name) +
                 " level " + num(wp.levelCm);
            // The bounds token LAST, same reasoning as PCGVOLUME's own bounds: parsing it consumes
            // the numbers after it, so writing it last means the reader never has to re-find them.
            if (wp.infinite) {
                s += " infinite";
            } else {
                s += " bounds " + num(wp.boundsMin[0]) + " " + num(wp.boundsMin[1]) + " " +
                     num(wp.boundsMax[0]) + " " + num(wp.boundsMax[1]);
            }
            // AFTER bounds, because bounds consumes the four tokens following it and a keyword
            // written between them would be read as a number.
            if (wp.simulate) s += " simulate";
            // The four solver knobs, each written ONLY when the record actually names one -- the
            // same "omitted means default" rule GAMEMODE, SCATTER's `volume` and PCGVOLUME's
            // optional clauses all follow, checked against -1 rather than against a bool because
            // OcWaterPlacement carries no separate "was this authored" flag; the sentinel IS the
            // carrier. Each is a self-contained `key value` pair, so writing them in this order does
            // not commit a reader to reading them in this order -- unlike `bounds`, nothing here
            // needs to know how many tokens follow it before it can stop consuming.
            if (wp.compliance >= 0.0) s += " compliance " + num(wp.compliance);
            if (wp.damping    >= 0.0) s += " damping "    + num(wp.damping);
            if (wp.iterations >= 0)   s += " iterations " + num(wp.iterations);
            if (wp.pressure   >= 0.0) s += " pressure "   + num(wp.pressure);
            // THE MATERIAL LAYER, written AFTER the four solver knobs -- order matters for none of
            // these seven (unlike `bounds`, each is a self-contained `key value` pair), so this is
            // simply parse order, matching the four knobs' own "omitted means default" rule.
            if (!wp.preset.empty())  s += " preset "    + wp.preset;
            if (wp.density   >= 0.0) s += " density "   + num(wp.density);
            if (wp.viscosity >= 0.0) s += " viscosity " + num(wp.viscosity);
            // The SURFACE material, last, and gated on non-empty like `preset` for the same reason:
            // a record that names none must round-trip without gaining a token. Written after
            // `preset` deliberately, so a human reading the line meets the solver material and the
            // render material in the same order the struct declares them.
            if (!wp.material.empty()) s += " material " + wp.material;
            s += "\n";
        }
    }

    if (!w.waves.empty()) {
        s += "\n";
        for (const OcGerstnerWave& gw : w.waves) {
            s += "WAVE";
            // Omitted when empty, same "no override is the default" rule GAMEMODE and SCATTER's
            // `volume` follow: empty means "the first declared WATER", and a wave that never named
            // one must not come back from a save claiming to have named one.
            if (!gw.water.empty()) s += " water " + gw.water;
            s += " dir " + num(gw.dirX) + " " + num(gw.dirZ) +
                 " wavelength " + num(gw.wavelengthCm) +
                 " amplitude " + num(gw.amplitudeCm) +
                 " steepness " + num(gw.steepness) + "\n";
        }
    }

    s += "\n";
    // DEPTH-FIRST FROM THE ROOTS, so the nesting in the file IS the parent relation and no index is
    // ever written down. See OcWorldPlacement::parent for why that matters: a stored index has to be
    // kept in step with every reorder, and this tree already has a scar from that mistake.
    //
    // The children lists are built once rather than rescanning the vector per parent, which would be
    // quadratic on a level with thousands of placements -- the ordinary case, not a corner.
    const usize n = w.placements.size();
    std::vector<std::vector<i32>> kids(n);
    std::vector<i32> roots;
    for (usize i = 0; i < n; ++i) {
        const i32 par = w.placements[i].parent;
        // A PARENT OUT OF RANGE, OR ITSELF, IS TREATED AS A ROOT rather than dropped or trusted.
        // parseOcworld cannot produce one, but writeOcworld also serves callers that built the
        // vector by hand, and the alternatives are both worse: trusting it walks off the end, and
        // dropping the placement loses geometry to a bookkeeping error nobody would see.
        if (par >= 0 && par < static_cast<i32>(n) && par != static_cast<i32>(i))
            kids[static_cast<usize>(par)].push_back(static_cast<i32>(i));
        else
            roots.push_back(static_cast<i32>(i));
    }

    const auto line = [&](const OcWorldPlacement& p, const char* keyword, const char* keywordG) {
        if (p.uniform()) {
            s += keyword; s += " " + p.asset + " " +
                 num(p.x) + " " + num(p.y) + " " + num(p.z) + " " +
                 num(p.yaw) + " " + num(p.pitch) + " " + num(p.roll) + " " + num(p.sx);
        } else {
            s += keywordG; s += " " + p.asset + " " +
                 num(p.x) + " " + num(p.y) + " " + num(p.z) + " " +
                 num(p.yaw) + " " + num(p.pitch) + " " + num(p.roll) + " " +
                 num(p.sx) + " " + num(p.sy) + " " + num(p.sz);
        }
        if (!p.material.empty()) { s += " "; s += p.material; }
        if (!p.collide) s += " nocollide";
        if (p.snapToGround) s += " snap";
        // Omitted when empty, same "no override is the default" rule as GAMEMODE above.
        if (!p.className.empty()) { s += " class "; s += p.className; }
        s += "\n";
    };

    // ITERATIVE, not recursive: a hand-built vector can describe a cycle, and a cycle in a recursive
    // emit is a stack overflow rather than a diagnosable error. `emitted` bounds the walk to each
    // placement once, so the worst a malformed parent chain can do is leave a subtree unwritten.
    std::vector<bool> emitted(n, false);
    struct Frame { i32 index; usize next; bool opened; };
    std::vector<Frame> stack;
    for (const i32 root : roots) {
        if (emitted[static_cast<usize>(root)]) continue;
        emitted[static_cast<usize>(root)] = true;
        line(w.placements[static_cast<usize>(root)], "PLACE ", "PLACEG");
        stack.push_back(Frame{root, 0, false});
        while (!stack.empty()) {
            Frame& f = stack.back();
            const std::vector<i32>& ch = kids[static_cast<usize>(f.index)];
            if (f.next >= ch.size()) {
                if (f.opened) {
                    s.append(static_cast<usize>(stack.size() - 1) * 2, ' ');
                    s += "END\n";
                }
                stack.pop_back();
                continue;
            }
            if (!f.opened) {
                f.opened = true;
                s.append(static_cast<usize>(stack.size() - 1) * 2, ' ');
                s += "BEGIN\n";
            }
            const i32 c = ch[f.next++];
            if (emitted[static_cast<usize>(c)]) continue;
            emitted[static_cast<usize>(c)] = true;
            // Indentation is COSMETIC, exactly as it is everywhere else in this format -- trim and
            // splitWhitespace discard it before any parse sees it. The nesting is carried by the
            // records; the spaces are for whoever opens the file.
            s.append(stack.size() * 2, ' ');
            line(w.placements[static_cast<usize>(c)], "CHILD ", "CHILDG");
            stack.push_back(Frame{c, 0, false});
        }
    }
    return s;
}

// Writes a world to disk, creating parent directories.
bool saveOcworld(const std::string& path, const OcWorldData& w, std::string* err) {
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    const std::string text = writeOcworld(w);

    // WRITE TO A TEMPORARY AND SWAP, via writeFileTextAtomic (aver/platform/FileSystem.hpp) -- the
    // same pattern aver::fmt::saveOcSave (OcSave.cpp) and aver_settings_flush (Settings.cpp)
    // already ship with, lifted to the shared platform layer. This function used to open `path`
    // directly with ios::trunc, which zeroes the file the instant it opens -- before writeOcworld's
    // result has landed a single byte -- so a crash, a kill, or a full disk between the open and
    // the write destroyed the level being saved rather than merely failing to update it.
    if (!writeFileTextAtomic(path, text)) {
        if (err) *err = "could not write " + path;
        return false;
    }
    return true;
}

} // namespace aver::fmt
