// .ocparticle format test (particles DECIDED 3). Covers round-tripping, unknown-record and comment
// preservation, deterministic output, strict malformed-input rejection, and a real on-disk
// malformed file -- the same shape OcGraphTest.cpp uses for .ocgraph, since this format's contract
// is explicitly that precedent (see OcParticle.hpp's own comment) rather than .ocmat's looser one.
//
// Test content is deliberately NOT a weapon effect (a smoke plume, a burst of embers, falling
// snow) -- see the task brief's own expressiveness requirement.
#include "aver/formats/OcParticle.hpp"
#include "aver/particles/ParticleModules.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace aver;

static int g_failures = 0;

// Logs one assertion and counts the failures.
static void check(bool cond, const std::string& what) {
    if (cond) {
        AVER_INFO("   PASS  {}", what);
    } else {
        AVER_ERROR("   FAIL  {}", what);
        ++g_failures;
    }
}

static bool near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// A full .ocparticle exercising every record, describing a rising, widening, fading smoke column --
// one of the four expressiveness-test shapes, not a weapon effect.
static const char* kFull = R"(OCPARTICLE 1
# Rising smoke column -- widens and fades as it climbs.
NAME Rising Smoke Column
SHAPE box 40 40 10
BLEND additive

EMISSION 35 8 400
LIFETIME 2.5 4
DIRECTION 0 0 1 25
SPEED 60 90
GRAVITY 0 0 -5
DAMPING 0.2
SIZE 20 65
COLOR start 0.9 0.85 0.8 0.35
COLOR end 0.6 0.6 0.65 0

FUTURERECORD a field no reader today understands yet
TEX {guid:0x00000000DEADBEEF}
)";

// Parses kFull and checks every record's VALUES, not merely that parsing returned true.
static void testFullParse() {
    AVER_INFO("=== .ocparticle: every implemented record ===");
    particles::ParticleEffect e;
    fmt::OcParticleExtras ex;
    std::string err;
    if (!fmt::parseOcparticle(kFull, e, &ex, &err)) {
        AVER_ERROR("   parse failed: {}", err);
        ++g_failures;
        return;
    }

    check(ex.name == "Rising Smoke Column", "NAME keeps its spaces");
    check(e.shape == particles::EmitterShape::Box, "SHAPE box parsed as EmitterShape::Box");
    check(near(e.shapeSize.x, 40.0f) && near(e.shapeSize.y, 40.0f) && near(e.shapeSize.z, 10.0f),
          "SHAPE box half-extents (3)");
    check(e.blend == rhi::BlendMode::Additive, "BLEND additive parsed");

    check(near(e.emissionRate, 35.0f), "EMISSION rate");
    check(e.burstCount == 8, "EMISSION burstCount");
    check(e.maxParticles == 400, "EMISSION maxParticles");

    check(near(e.lifetimeMin, 2.5f) && near(e.lifetimeMax, 4.0f), "LIFETIME min/max");

    check(near(e.direction.x, 0.0f) && near(e.direction.y, 0.0f) && near(e.direction.z, 1.0f),
          "DIRECTION vector");
    check(near(e.spreadDeg, 25.0f), "DIRECTION spreadDeg");

    check(near(e.speedMin, 60.0f) && near(e.speedMax, 90.0f), "SPEED min/max");
    check(near(e.gravity.x, 0.0f) && near(e.gravity.y, 0.0f) && near(e.gravity.z, -5.0f), "GRAVITY vector");
    check(near(e.damping, 0.2f), "DAMPING");
    check(near(e.sizeStart, 20.0f) && near(e.sizeEnd, 65.0f), "SIZE start/end");

    check(near(e.colorStart[0], 0.9f) && near(e.colorStart[3], 0.35f), "COLOR start (4)");
    check(near(e.colorEnd[0], 0.6f) && near(e.colorEnd[3], 0.0f), "COLOR end (4)");

    check(e.textureId == 0xDEADBEEFull, "TEX {guid:0x...} parsed as an id");
}

// A header-only file loads as ParticleEffect's own defaults -- nothing invented, nothing crashed.
static void testHeaderOnlyDefaults() {
    AVER_INFO("=== .ocparticle: header-only file loads as defaults ===");
    particles::ParticleEffect e;
    std::string err;
    check(fmt::parseOcparticle("OCPARTICLE 1\n", e, nullptr, &err), "a header-only file parses: " + err);

    const particles::ParticleEffect d{};   // ParticleEffect's own default member initialisers
    check(e.shape == d.shape, "default shape");
    check(near(e.emissionRate, d.emissionRate), "default emissionRate");
    check(e.burstCount == d.burstCount, "default burstCount");
    check(e.maxParticles == d.maxParticles, "default maxParticles");
    check(near(e.lifetimeMin, d.lifetimeMin) && near(e.lifetimeMax, d.lifetimeMax), "default lifetime");
    check(near(e.speedMin, d.speedMin) && near(e.speedMax, d.speedMax), "default speed");
    check(near(e.damping, d.damping), "default damping");
    check(near(e.sizeStart, d.sizeStart) && near(e.sizeEnd, d.sizeEnd), "default size");
    check(e.blend == d.blend, "default blend");
    check(e.textureId == 0, "default textureId is 0 (no texture)");
}

// Tests that an effect round-trips exactly: write -> parse -> write must be bit-identical, matching
// OcGraphTest.cpp's testRoundTrip and the task's own "load, save, load again" requirement.
static void testRoundTrip() {
    AVER_INFO("=== .ocparticle round-trip (in memory) ===");

    particles::ParticleEffect e;
    e.shape = particles::EmitterShape::Sphere;
    e.shapeSize = Vec3{15.0f, 0.0f, 0.0f};
    e.emissionRate = 12.5f;
    e.burstCount = 30;
    e.maxParticles = 256;
    e.lifetimeMin = 0.8f; e.lifetimeMax = 1.6f;
    e.direction = Vec3{0.0f, 0.0f, 1.0f};
    e.spreadDeg = 180.0f;   // an omni burst -- embers, not a cone
    e.speedMin = 200.0f; e.speedMax = 400.0f;
    e.gravity = Vec3{0.0f, 0.0f, -981.0f};
    e.damping = 0.35f;
    e.sizeStart = 6.0f; e.sizeEnd = 2.0f;
    e.colorStart[0] = 1.0f; e.colorStart[1] = 0.6f; e.colorStart[2] = 0.15f; e.colorStart[3] = 1.0f;
    e.colorEnd[0]   = 0.4f; e.colorEnd[1]   = 0.05f; e.colorEnd[2] = 0.0f;  e.colorEnd[3]   = 0.0f;
    e.blend = rhi::BlendMode::Additive;
    e.textureId = 0x1234ull;
    fmt::OcParticleExtras ex;
    ex.name = "Ember Burst";

    const std::string text1 = fmt::writeOcparticle(e, &ex);
    check(!text1.empty(), "write produces non-empty text");

    particles::ParticleEffect e2;
    fmt::OcParticleExtras ex2;
    std::string err;
    check(fmt::parseOcparticle(text1, e2, &ex2, &err), "parsed output round-trips: " + err);

    const std::string text2 = fmt::writeOcparticle(e2, &ex2);
    check(text1 == text2, "second write reproduces the first byte for byte");

    check(ex2.name == "Ember Burst", "NAME survived round-trip");
    check(e2.shape == particles::EmitterShape::Sphere, "shape survived round-trip");
    check(near(e2.shapeSize.x, 15.0f), "sphere radius survived round-trip");
    check(near(e2.spreadDeg, 180.0f), "an omni spread (180) survives round-trip exactly");
    check(near(e2.gravity.z, -981.0f), "gravity survived round-trip");
    check(e2.textureId == 0x1234ull, "textureId survived round-trip");
    check(e2.blend == rhi::BlendMode::Additive, "blend survived round-trip");
}

// The literal requirement: load, save, load again produces byte-identical output for a file the
// writer produced -- exercised through REAL DISK I/O this time, not just in-memory strings.
static void testFileRoundTrip() {
    AVER_INFO("=== .ocparticle round-trip (real file) ===");
    const std::string dir = std::getenv("TEMP") ? std::getenv("TEMP") : ".";
    const std::string path = dir + "/aver-ocparticle-test-roundtrip.ocparticle";

    particles::ParticleEffect e;
    e.shape = particles::EmitterShape::Point;
    e.emissionRate = 500.0f;   // dense: falling snow
    e.direction = Vec3{0.0f, 0.0f, -1.0f};
    e.speedMin = 40.0f; e.speedMax = 60.0f;
    e.gravity = Vec3{0.0f, 0.0f, -30.0f};
    e.sizeStart = 3.0f; e.sizeEnd = 3.0f;
    fmt::OcParticleExtras ex;
    ex.name = "Falling Snow";

    std::string err;
    check(fmt::saveOcparticle(path, e, &ex, &err), "first save succeeds: " + err);

    std::string text1;
    check(readFileText(path, text1), "the saved file is readable");

    particles::ParticleEffect e2;
    fmt::OcParticleExtras ex2;
    check(fmt::loadOcparticle(path, e2, &ex2, &err), "load of the just-saved file succeeds: " + err);
    check(fmt::saveOcparticle(path, e2, &ex2, &err), "second save succeeds: " + err);

    std::string text2;
    check(readFileText(path, text2), "the re-saved file is readable");
    check(text1 == text2, "load -> save -> load -> save produces a byte-identical file on disk");
}

// Unknown records and comments must survive a save -- the requirement this format explicitly took
// from .ocgraph rather than .ocmat (which drops its own GRAPH{} block on rewrite).
static void testUnknownRecordsAndComments() {
    AVER_INFO("=== .ocparticle: unknown records and comments survive a save ===");

    const std::string original =
        "OCPARTICLE 1\n"
        "NAME Original\n"
        "SHAPE point 0 0 0\n"
        "BLEND premultiplied\n"
        "\n"
        "EMISSION 10 0 512\n"
        "LIFETIME 1 1\n"
        "DIRECTION 0 0 1 0\n"
        "SPEED 100 100\n"
        "GRAVITY 0 0 0\n"
        "DAMPING 0\n"
        "SIZE 10 10\n"
        "COLOR start 1 1 1 1\n"
        "COLOR end 1 1 1 0\n"
        "# A hand-written note explaining the fade curve\n"
        "MYSTERY some future field nobody reads yet\n";

    particles::ParticleEffect e;
    fmt::OcParticleExtras ex;
    std::string err;
    check(fmt::parseOcparticle(original, e, &ex, &err), "an effect with an unknown record parses: " + err);
    check(ex.name == "Original", "known fields are parsed");

    const std::string rewritten = fmt::writeOcparticle(e, &ex, original);
    check(rewritten.find("MYSTERY some future field nobody reads yet") != std::string::npos,
          "the unknown record 'MYSTERY' survives a save");
    check(rewritten.find("# A hand-written note explaining the fade curve") != std::string::npos,
          "the comment survives a save");

    particles::ParticleEffect e2;
    fmt::OcParticleExtras ex2;
    check(fmt::parseOcparticle(rewritten, e2, &ex2, &err), "the rewritten effect still parses: " + err);

    const std::string again = fmt::writeOcparticle(e2, &ex2, rewritten);
    check(again.find("MYSTERY some future field nobody reads yet") != std::string::npos,
          "the unknown record survives a SECOND save");
    check(again == rewritten, "the second merge is idempotent -- bit-identical to the first");
}

// Same data, written twice, must produce identical bytes.
static void testDeterministic() {
    AVER_INFO("=== .ocparticle: deterministic output ===");
    particles::ParticleEffect e;
    e.shape = particles::EmitterShape::Box;
    e.shapeSize = Vec3{800.0f, 800.0f, 0.0f};   // a wide sheet: a waterfall's mist
    e.emissionRate = 220.0f;
    e.direction = Vec3{0.0f, 1.0f, -0.2f};
    e.spreadDeg = 10.0f;
    fmt::OcParticleExtras ex;
    ex.name = "Waterfall Mist";

    const std::string t1 = fmt::writeOcparticle(e, &ex);
    const std::string t2 = fmt::writeOcparticle(e, &ex);
    check(t1 == t2, "identical effects produce identical output");
}

// Checks which malformed inputs are refused, in memory.
static void testMalformedInput() {
    AVER_INFO("=== .ocparticle: malformed input is rejected ===");
    particles::ParticleEffect e;
    std::string err;

    check(!fmt::parseOcparticle("NAME Test\nSHAPE point 0 0 0\n", e, nullptr, &err),
          "input without an OCPARTICLE header is rejected");
    check(err.find("OCPARTICLE") != std::string::npos, "error message mentions the missing header");

    check(!fmt::parseOcparticle("OCPARTICLE 3\n", e, nullptr, &err), "an unknown OCPARTICLE version is rejected");
    check(fmt::parseOcparticle("OCPARTICLE 2\n", e, nullptr, &err), "OCPARTICLE 2 is a known version (wave 10)");
    check(!fmt::parseOcparticle("OCPARTICLE\n", e, nullptr, &err), "a header with no version number is rejected");
    check(!fmt::parseOcparticle("", e, nullptr, &err), "a completely empty file is rejected");
    check(err.find("OCPARTICLE") != std::string::npos, "...and the empty-file error also names the header");

    check(!fmt::parseOcparticle("OCPARTICLE 1\nSHAPE box 10 20\n", e, nullptr, &err),
          "a TRUNCATED SHAPE (missing z) is rejected");
    check(err.find("SHAPE") != std::string::npos, "...naming SHAPE");

    check(!fmt::parseOcparticle("OCPARTICLE 1\nSHAPE triangle 1 1 1\n", e, nullptr, &err),
          "an unknown SHAPE kind is rejected");

    check(!fmt::parseOcparticle("OCPARTICLE 1\nSHAPE box 1 1 notanumber\n", e, nullptr, &err),
          "a malformed number in SHAPE is rejected, not silently read as zero");

    check(!fmt::parseOcparticle("OCPARTICLE 1\nBLEND multiply\n", e, nullptr, &err),
          "an unknown BLEND mode is rejected");

    check(!fmt::parseOcparticle("OCPARTICLE 1\nEMISSION 10 0\n", e, nullptr, &err),
          "a truncated EMISSION (missing maxParticles) is rejected");

    check(!fmt::parseOcparticle("OCPARTICLE 1\nCOLOR middle 1 1 1 1\n", e, nullptr, &err),
          "COLOR must say 'start' or 'end'");

    check(!fmt::parseOcparticle("OCPARTICLE 1\nTEX not-a-guid\n", e, nullptr, &err),
          "a malformed TEX reference is rejected");
    check(!fmt::parseOcparticle("OCPARTICLE 1\nTEX {guid:0x0}\n", e, nullptr, &err),
          "TEX guid 0 is rejected (0 means 'no texture' and must not be spelled explicitly)");

    // What must NOT fail: a record kind this format has never heard of is forward-compat, not an
    // error, matching .ocgraph's own "unknown records are ignored during parse" rule.
    check(fmt::parseOcparticle("OCPARTICLE 1\nSOMETHING_FUTURE 1 2 3\n", e, nullptr, &err),
          "an unrecognised record KIND is tolerated, not fatal");
}

// A missing file and an on-disk empty/truncated file are errors the loader reports, never a crash
// and never a default effect that looks like it worked -- exercised through real disk I/O this time,
// including the one deliberately malformed file the task requires.
static void testFileErrors() {
    AVER_INFO("=== .ocparticle: missing, empty and malformed FILES ===");
    const std::string dir = std::getenv("TEMP") ? std::getenv("TEMP") : ".";

    {
        particles::ParticleEffect e;
        std::string err;
        const std::string missing = dir + "/aver-ocparticle-test-does-not-exist.ocparticle";
        check(!fmt::loadOcparticle(missing, e, nullptr, &err), "loading a nonexistent path fails");
        check(err.find("could not read") != std::string::npos, "...and says so");
    }
    {
        const std::string path = dir + "/aver-ocparticle-test-empty.ocparticle";
        { std::ofstream f(path, std::ios::binary | std::ios::trunc); }   // zero bytes
        particles::ParticleEffect e;
        std::string err;
        check(!fmt::loadOcparticle(path, e, nullptr, &err), "loading an empty on-disk file fails");
        check(err.find("OCPARTICLE") != std::string::npos, "...and names the missing header");
    }
    {
        // THE DELIBERATELY MALFORMED FILE the task requires: a real file on disk, truncated
        // mid-record (SHAPE with only 2 of its 3 numbers), read through the real loadOcparticle
        // entry point -- not merely parseOcparticle on an in-memory literal.
        const std::string path = dir + "/aver-ocparticle-test-malformed.ocparticle";
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            f << "OCPARTICLE 1\n"
                 "NAME Truncated Dust\n"
                 "SHAPE box 400 100\n";   // missing the third (z) number
        }
        particles::ParticleEffect e;
        std::string err;
        const bool ok = fmt::loadOcparticle(path, e, nullptr, &err);
        check(!ok, "loading the deliberately malformed on-disk file fails");
        check(err.find("SHAPE") != std::string::npos, "...and the error names SHAPE, the record that was cut short");
        // The return value is the contract a caller must honour ("never a silent default effect
        // that looks like it worked" means the CALLER must check `ok`, which is what the checks
        // above exercise). `out` itself is reset to ParticleEffect{}'s own defaults before parsing
        // even begins -- the same reset-before-parse shape parseOcmat/parseOcgraph already use --
        // so it is neither the truncated file's partial data nor a zeroed sentinel; it is simply
        // never inspected by correct calling code, which never reads `out` without checking `ok`.
        //
        // THE RESET ALONE DID NOT ACTUALLY DELIVER THAT, which is why the pre-populated case below
        // exists. Records used to be written straight into `out` as they parsed, so a file that
        // failed on its fourth record left the first three records' real values standing in front of
        // the defaults. The parse now builds a local and commits it only on success, so the
        // guarantee this comment always claimed is now the one the code makes.
        const particles::ParticleEffect d{};
        check(near(e.emissionRate, d.emissionRate) && e.maxParticles == d.maxParticles,
              "on failure, `out` sits at ParticleEffect{}'s own defaults, not at the truncated "
              "file's partial SHAPE data");

        // THE CASE THE DEFAULT-CONSTRUCTED CHECK ABOVE CANNOT SEE. Starting from a default struct,
        // "reset to defaults" and "left holding partial data" are only distinguishable for fields
        // the truncated file actually set -- so this repeats it from a struct pre-populated with
        // values that are NOT the defaults, where a leaked partial parse is unmistakable.
        particles::ParticleEffect pre{};
        pre.emissionRate = 999.0f;
        pre.maxParticles = 12345;
        pre.shapeSize = Vec3{7, 7, 7};
        std::string err2;
        const bool ok2 = fmt::loadOcparticle(path, pre, nullptr, &err2);
        check(!ok2, "the same malformed file still fails when `out` arrives pre-populated");
        check(near(pre.emissionRate, d.emissionRate) && pre.maxParticles == d.maxParticles,
              "...and the caller's own values are replaced by the defaults, not left standing");
        check(near(pre.shapeSize.x, d.shapeSize.x) && near(pre.shapeSize.y, d.shapeSize.y),
              "...including shapeSize, which the truncated SHAPE record itself had begun to write");
    }
}

// Every field whose struct comment states a range is refused outside it, because the simulator
// trusts them: a negative DAMPING is a velocity multiplier above 1, so a mistyped sign does not damp
// gently, it accelerates every particle until the effect fills the screen.
static void testRangesRefused() {
    AVER_INFO("=== .ocparticle out-of-range fields ===");
    struct Case { const char* what; const char* body; const char* wantIn; };
    const Case cases[] = {
        {"a negative DAMPING",            "DAMPING -0.4",           "DAMPING"},
        {"a DAMPING of exactly 1",        "DAMPING 1.0",            "DAMPING"},
        {"an alpha above 1",              "COLOR start 1 1 1 4.0",  "COLOR"},
        {"a negative colour component",   "COLOR end -1 0 0 1",     "COLOR"},
        {"a lifetime range that inverts", "LIFETIME 9 2",           "LIFETIME"},
        {"a spread beyond the sphere",    "DIRECTION 0 0 -1 400",   "spread"},
    };
    for (const Case& c : cases) {
        const std::string text = std::string("OCPARTICLE 1\nNAME R\n") + c.body + "\n";
        particles::ParticleEffect e{};
        std::string err;
        const bool ok = fmt::parseOcparticle(text, e, nullptr, &err);
        check(!ok, std::string("refused: ") + c.what);
        check(err.find(c.wantIn) != std::string::npos,
              std::string("...and the error names the offending record for ") + c.what);
    }
    // The boundaries themselves are legal, so a valid file is not caught by the guard above.
    particles::ParticleEffect ok0{};
    std::string e0;
    check(fmt::parseOcparticle("OCPARTICLE 1\nNAME R\nDAMPING 0.0\nDIRECTION 0 0 -1 180\n", ok0, nullptr, &e0),
          "the legal boundaries (DAMPING 0, spread 180) still parse");
}


// ---------------------------------------------------------------------------------------------------------------
// Wave 10: OCPARTICLE 2 -- a system of emitters, user parameters, a module stack and events.
static const char* kSystemV2 = R"(OCPARTICLE 2
NAME Sparks
PARAM Intensity float 1.5
PARAM Wind vec3 10 0 -4

EMITTER Sparks
SIM gpu
SHAPE cylinder 20 40 shell
RENDER ribbon trail 12 4 fade 1
BLEND additive

EMISSION 200 0 20000
LIFETIME 0.5 1.5
DIRECTION 0 0 1 30
SPEED 300 600
GRAVITY 0 0 -981
DAMPING 0.1
SIZE 3 1
SIZECURVE 0 1 0.3 2 1 0.5
COLOR start 1 0.7 0.2 1
COLOR end 0.4 0.1 0 0
LIGHTING lit
GI off
SORT none
FOG off
BOUNDS 600
CULL 8000
MODULE spawn.sizescale min=0.6 max=1.4
MODULE update.curlnoise freq=0.01 strength=@Intensity.x scroll=0.3 octaves=2
MODULE update.force x=@Wind.x y=@Wind.y z=@Wind.z
MODULE update.collideplane nz=1 d=0 restitution=0.5 off
ONEVENT death SPAWN Smoke 3 inherit 0.5

EMITTER Smoke
SPAWNAT off
EMISSION 0 0 512
SPRITE velocity stretch 0.5
SUBUV 4 4 16 fps 24 blend
)";

static void testV2Parse() {
    AVER_INFO("=== .ocparticle v2: a two-emitter system parses ===");
    particles::ParticleSystemDef def;
    fmt::OcParticleExtras ex;
    std::string err;
    if (!fmt::parseOcparticleSystem(kSystemV2, def, &ex, &err)) {
        AVER_ERROR("   parse failed: {}", err);
        ++g_failures;
        return;
    }
    check(def.name == "Sparks" && ex.name == "Sparks", "the system NAME");
    check(def.params.size() == 2 && def.params[0].name == "Intensity" && def.params[1].kind == 1, "two PARAMs, float and vec3");
    check(near(def.params[1].v[0], 10.0f) && near(def.params[1].v[2], -4.0f), "the vec3 default");
    check(def.emitters.size() == 2 && def.emitters[0].name == "Sparks" && def.emitters[1].name == "Smoke", "two named emitters");
    const particles::ParticleEffect& e = def.emitters[0];
    check(e.simTier == particles::SimTier::Gpu, "SIM gpu");
    check(e.shape == particles::EmitterShape::Cylinder && near(e.shapeSize.x, 20.0f) && near(e.shapeSize.y, 40.0f) && e.shapeShell,
          "SHAPE cylinder r h shell");
    check(e.render == particles::RenderKind::Ribbon && !e.linked && e.trailPoints == 12 && near(e.trailSegmentCm, 4.0f), "RENDER ribbon trail");
    check(e.sizeCurve.n == 3 && near(e.sizeCurve.t[1], 0.3f) && near(e.sizeCurve.v[1][0], 2.0f), "SIZECURVE, three stops");
    check(e.lighting == particles::Lighting::Lit && !e.receivesGI, "LIGHTING lit and GI off");
    check(e.sort == particles::SortMode::None && !e.fog, "SORT none, FOG off");
    check(near(e.boundsRadiusCm, 600.0f) && near(e.cullDistanceCm, 8000.0f), "BOUNDS and CULL");
    check(e.modules.size() == 4, "four MODULE records, authored order");
    if (e.modules.size() == 4) {
        check(e.modules[0].kind == particles::ModuleKind::SpawnSizeScale && near(e.modules[0].p[0].value, 0.6f), "spawn.sizescale min");
        check(e.modules[1].kind == particles::ModuleKind::UpdateCurlNoise && e.modules[1].p[1].userSlot == 0 && e.modules[1].p[1].comp == 0,
              "curlnoise strength bound to @Intensity.x");
        check(e.modules[2].p[2].userSlot == 1 && e.modules[2].p[2].comp == 2, "force z bound to @Wind.z");
        check(!e.modules[3].enabled, "a module can be switched off");
    }
    check(e.events.size() == 1 && e.events[0].on == 0 && e.events[0].emitter == 1 && e.events[0].count == 3 && near(e.events[0].inheritVelocity, 0.5f),
          "ONEVENT death SPAWN Smoke 3 inherit 0.5");
    const particles::ParticleEffect& s = def.emitters[1];
    check(!s.spawnAtTarget, "SPAWNAT off");
    check(s.align == particles::SpriteAlign::Velocity && near(s.stretch, 0.5f), "SPRITE velocity stretch");
    check(s.subuvCols == 4 && s.subuvFrames == 16 && near(s.subuvFps, 24.0f) && s.subuvBlend, "SUBUV 4 4 16 fps 24 blend");
}

static void testV2RoundTrip() {
    AVER_INFO("=== .ocparticle v2: write -> parse -> write is byte-identical ===");
    particles::ParticleSystemDef def;
    std::string err;
    check(fmt::parseOcparticleSystem(kSystemV2, def, nullptr, &err), "parse: " + err);
    const std::string t1 = fmt::writeOcparticleSystem(def);
    check(t1.rfind("OCPARTICLE 2", 0) == 0, "a system with modules is written as OCPARTICLE 2");
    particles::ParticleSystemDef def2;
    check(fmt::parseOcparticleSystem(t1, def2, nullptr, &err), "the written text parses: " + err);
    const std::string t2 = fmt::writeOcparticleSystem(def2);
    check(t1 == t2, "the second fresh write equals the first");
    const std::string merged = fmt::writeOcparticleSystem(def2, t1);
    check(merged == t1, "merging into its own output changes nothing");
    check(def2.emitters.size() == 2 && def2.emitters[0].modules.size() == 4 && def2.params.size() == 2, "structure survives");
    check(def2.emitters[0].modules[1].p[1].userSlot == 0, "a parameter binding survives");

    // Unknown records and comments survive a v2 save.
    const std::string withExtras = std::string(kSystemV2) + "MYSTERY from the future\n# a note\n";
    particles::ParticleSystemDef def3;
    check(fmt::parseOcparticleSystem(withExtras, def3, nullptr, &err), "a v2 file with an unknown record parses: " + err);
    const std::string again = fmt::writeOcparticleSystem(def3, withExtras);
    check(again.find("MYSTERY from the future") != std::string::npos && again.find("# a note") != std::string::npos,
          "an unknown record and a comment survive a v2 save");
    check(fmt::writeOcparticleSystem(def3, again) == again, "and the second merge is idempotent");
}

static void testV1StaysV1() {
    AVER_INFO("=== .ocparticle: a v1 effect is written back as v1 until something v2-only is authored ===");
    particles::ParticleEffect e;
    std::string err;
    fmt::OcParticleExtras ex;
    check(fmt::parseOcparticle(kFull, e, &ex, &err), "kFull parses: " + err);
    const std::string out = fmt::writeOcparticle(e, &ex, kFull);
    check(out.rfind("OCPARTICLE 1", 0) == 0, "the header stays version 1");
    check(out.find("MODULE") == std::string::npos && out.find("EMITTER") == std::string::npos, "no v2 records appear");
    check(fmt::writeOcparticle(e, &ex, out) == out, "and a second v1 save is idempotent");

    e.modules.push_back(particles::makeDefaultModule(particles::ModuleKind::UpdateForce));
    const std::string up = fmt::writeOcparticle(e, &ex, kFull);
    check(up.rfind("OCPARTICLE 2", 0) == 0 && up.find("MODULE update.force") != std::string::npos, "adding a module upgrades to version 2");
    check(up.find("FUTURERECORD") != std::string::npos, "the upgrade keeps the unknown record");
    particles::ParticleEffect back;
    check(fmt::parseOcparticle(up, back, nullptr, &err) && back.modules.size() == 1, "the upgraded file parses with its module");

    // Removing the module again returns the file to v1 and drops the MODULE line.
    e.modules.clear();
    const std::string down = fmt::writeOcparticle(e, &ex, up);
    check(down.rfind("OCPARTICLE 1", 0) == 0 && down.find("MODULE") == std::string::npos, "removing the last v2-only feature writes version 1 again");
}

static void testV2Refusals() {
    AVER_INFO("=== .ocparticle v2: every refusal reports a line ===");
    struct Case { const char* what; const char* text; const char* wantIn; };
    const Case cases[] = {
        {"an unknown module key", "OCPARTICLE 2\nMODULE update.nothing x=1\n", "line 2"},
        {"an unknown module parameter", "OCPARTICLE 2\nMODULE update.force q=1\n", "unknown parameter"},
        {"an @ to an undeclared parameter", "OCPARTICLE 2\nMODULE update.force x=@Nope\n", "PARAM"},
        {"a malformed module number", "OCPARTICLE 2\nMODULE update.force x=abc\n", "malformed"},
        {"an event naming a missing emitter", "OCPARTICLE 2\nEMITTER A\nONEVENT death SPAWN Ghost 1\n", "does not exist"},
        {"curve times that do not increase", "OCPARTICLE 2\nSIZECURVE 0 1 0 2\n", "SIZECURVE"},
        {"a single-stop curve", "OCPARTICLE 2\nSIZECURVE 0 1\n", "SIZECURVE"},
        {"a bad PARAM kind", "OCPARTICLE 2\nPARAM A matrix 1\n", "PARAM"},
        {"a duplicate PARAM", "OCPARTICLE 2\nPARAM A float 1\nPARAM A float 2\n", "duplicate"},
        {"an unknown SIM word", "OCPARTICLE 2\nSIM fpga\n", "SIM"},
        {"a SUBUV with more frames than cells", "OCPARTICLE 2\nSUBUV 2 2 9 life 0\n", "SUBUV"},
    };
    for (const Case& c : cases) {
        particles::ParticleSystemDef d;
        std::string err;
        check(!fmt::parseOcparticleSystem(c.text, d, nullptr, &err), std::string("refused: ") + c.what);
        check(err.find(c.wantIn) != std::string::npos, std::string("...with a useful error for ") + c.what + ": " + err);
        check(d.emitters.size() == 1, std::string("...and `out` is left at the defaults for ") + c.what);
    }

    // More than 16 modules / 8 emitters / 16 params.
    {
        std::string t = "OCPARTICLE 2\n";
        for (int i = 0; i < 17; ++i) t += "MODULE update.force\n";
        particles::ParticleSystemDef d; std::string err;
        check(!fmt::parseOcparticleSystem(t, d, nullptr, &err), "17 modules on one emitter are refused");
        std::string t2 = "OCPARTICLE 2\n";
        for (int i = 0; i < 9; ++i) t2 += "EMITTER E" + std::to_string(i) + "\n";
        check(!fmt::parseOcparticleSystem(t2, d, nullptr, &err), "9 emitters are refused");
        std::string t3 = "OCPARTICLE 2\n";
        for (int i = 0; i < 17; ++i) t3 += "PARAM P" + std::to_string(i) + " float 0\n";
        check(!fmt::parseOcparticleSystem(t3, d, nullptr, &err), "17 user parameters are refused");
    }
    // A cycle in the event graph.
    {
        particles::ParticleSystemDef d; std::string err;
        const char* cyc = "OCPARTICLE 2\nEMITTER A\nONEVENT death SPAWN B 1\nEMITTER B\nONEVENT death SPAWN A 1\n";
        check(!fmt::parseOcparticleSystem(cyc, d, nullptr, &err), "an event cycle is refused");
        check(err.find("cycle") != std::string::npos, "...and the error says cycle");
    }
    // Forward references are fine: the child may come after its parent, and a parameter may be declared after use.
    {
        particles::ParticleSystemDef d; std::string err;
        const char* fwd = "OCPARTICLE 2\nEMITTER A\nONEVENT collide SPAWN B 2\nMODULE update.force x=@W\nEMITTER B\nPARAM W float 3\n";
        check(fmt::parseOcparticleSystem(fwd, d, nullptr, &err), "forward references to an emitter and a PARAM resolve: " + err);
    }
}

static void testDefaultsAreV1() {
    AVER_INFO("=== .ocparticle: a default effect fits version 1 ===");
    particles::ParticleSystemDef d;
    d.emitters.emplace_back();
    check(fmt::ocparticleFitsV1(d), "a default one-emitter system fits version 1");
    d.emitters[0].render = particles::RenderKind::Mesh;
    check(!fmt::ocparticleFitsV1(d), "a mesh emitter does not");
    d.emitters[0].render = particles::RenderKind::Sprite;
    d.params.push_back(particles::UserParam{});
    check(!fmt::ocparticleFitsV1(d), "a user parameter does not");
}

static void testMeta() {
    AVER_INFO("=== .ocparticle test suite metadata ===");
    check(true, "a passing check works");
    check(sizeof(particles::Particle) == 64, "the simulation POD this format feeds is the 64-byte wave-10 layout");
}

// G6 SH3: GDSHADER names a converted Godot particles shader; it forces the v2 spelling and round trips.
static void testGodotShader() {
    AVER_INFO("=== .ocparticle: GDSHADER ===");
    const char* text =
        "OCPARTICLE 2\nPARAM reg0 color 1 0 0 1\nEMITTER sparks\nSIM gpu\nSHAPE point 0 0 0\nBLEND premultiplied\nEMISSION 32 0 64\nLIFETIME 2 2\nDIRECTION 0 0 1 0\nSPEED 0 0\nGRAVITY 0 0 0\n"
        "DAMPING 0\nSIZE 50 50\nCOLOR start 1 1 1 1\nCOLOR end 1 1 1 1\nGDSHADER ge12345678_sparks\n";
    particles::ParticleSystemDef def;
    std::string err;
    check(fmt::parseOcparticleSystem(text, def, nullptr, &err), "a system with GDSHADER parses: " + err);
    check(def.emitters.size() == 1 && def.emitters[0].godotShader == "ge12345678_sparks", "the shader name is read");
    const std::string written = fmt::writeOcparticleSystem(def);
    check(written.rfind("OCPARTICLE 2", 0) == 0 && written.find("GDSHADER ge12345678_sparks") != std::string::npos, "it is written back, as version 2");
    particles::ParticleSystemDef back;
    check(fmt::parseOcparticleSystem(written, back, nullptr, &err) && back.emitters[0].godotShader == "ge12345678_sparks" && fmt::writeOcparticleSystem(back) == written, "write -> parse -> write is byte-identical");
    particles::ParticleSystemDef plain;
    plain.emitters.emplace_back();
    check(fmt::writeOcparticleSystem(plain).find("GDSHADER") == std::string::npos && fmt::ocparticleFitsV1(plain), "an effect without a shader writes no GDSHADER and stays version 1");
    check(!fmt::parseOcparticleSystem("OCPARTICLE 2\nGDSHADER\n", back, nullptr, &err) && !fmt::parseOcparticleSystem("OCPARTICLE 2\nGDSHADER a b\n", back, nullptr, &err), "a GDSHADER without exactly one name is refused");
}

int main() {
    testGodotShader();
    testMeta();
    testRangesRefused();
    testFullParse();
    testHeaderOnlyDefaults();
    testRoundTrip();
    testFileRoundTrip();
    testUnknownRecordsAndComments();
    testDeterministic();
    testMalformedInput();
    testFileErrors();
    testV2Parse();
    testV2RoundTrip();
    testV1StaysV1();
    testV2Refusals();
    testDefaultsAreV1();

    AVER_INFO("==================================================");
    AVER_INFO("OcParticle tests done: {} failure(s)", g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
