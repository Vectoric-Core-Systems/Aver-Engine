// .ocparticle format test (particles DECIDED 3). Covers round-tripping, unknown-record and comment
// preservation, deterministic output, strict malformed-input rejection, and a real on-disk
// malformed file -- the same shape OcGraphTest.cpp uses for .ocgraph, since this format's contract
// is explicitly that precedent (see OcParticle.hpp's own comment) rather than .ocmat's looser one.
//
// Test content is deliberately NOT a weapon effect (a smoke plume, a burst of embers, falling
// snow) -- see the task brief's own expressiveness requirement.
#include "aver/formats/OcParticle.hpp"
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

    check(!fmt::parseOcparticle("OCPARTICLE 2\n", e, nullptr, &err), "an unknown OCPARTICLE version is rejected");
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

static void testMeta() {
    AVER_INFO("=== .ocparticle test suite metadata ===");
    check(true, "a passing check works");
    check(sizeof(particles::Particle) == 48, "the simulation POD this format feeds stays 48 bytes");
}

int main() {
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

    AVER_INFO("==================================================");
    AVER_INFO("OcParticle tests done: {} failure(s)", g_failures);
    return g_failures;
}
