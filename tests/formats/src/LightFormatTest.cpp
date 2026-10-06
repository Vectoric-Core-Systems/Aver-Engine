// LightFormatTest -- IES (LM-63) profiles and the .ocworld LIGHT record.
//
// The IES half checks what a lighting artist would otherwise see only as a wrong-looking lamp: the
// symmetry unfolding (a quadrant file must light all four quadrants), the sphere-mean normalisation
// that keeps luminous flux, and the refusals (type A/B, truncated tables). The LIGHT half is the usual
// round trip plus "an old level without the record still loads".
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/IesProfile.hpp"
#include "aver/formats/OcWorld.hpp"

#include <algorithm>
#include <cmath>
#include <string>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool closeTo(f32 a, f32 b, f32 tol = 1e-3f) { return std::fabs(a - b) <= tol * std::max(1.0f, std::fabs(b)); }

// Sphere mean of a baked table, by the same sin-weighted sum the baker normalises with.
static f64 tableMean(const fmt::IesTable& t) {
    f64 w = 0.0, s = 0.0;
    for (u32 j = 0; j < fmt::kIesTableV; ++j) {
        const f64 sinT = std::sin(static_cast<f64>(j) * 3.14159265358979 / (fmt::kIesTableV - 1));
        for (u32 i = 0; i < fmt::kIesTableH; ++i) {
            s += t.relative[static_cast<usize>(j) * fmt::kIesTableH + i] * sinT;
            w += sinT;
        }
    }
    return s / w;
}

// The LIGHT lines of a written level, joined.
static std::string lightLines(const std::string& text) {
    std::string out;
    usize pos = 0;
    while (pos < text.size()) {
        usize eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        if (text.compare(pos, 6, "LIGHT ") == 0) out += text.substr(pos, eol - pos) + "\n";
        pos = eol + 1;
    }
    return out;
}

int main() {
    AVER_INFO("=== LightFormatTest ===");
    std::string err;

    AVER_INFO("=== IES: rotationally symmetric downlight ===");
    {
        const char* down =
            "IESNA:LM-63-2002\n[TEST] unit-test downlight\n[MANUFAC] nobody\nTILT=NONE\n"
            "1 1000 1 4 1 1 2 0 0 0\n1 1 100\n"
            "0 30 60 90\n0\n"
            "1000 800 300 0\n";
        fmt::IesProfile p;
        check(fmt::parseIes(down, p, &err), "downlight parses: " + err);
        check(p.valid() && p.vertical.size() == 4 && p.horizontal.size() == 1, "4 vertical angles, 1 horizontal");
        check(p.name == "unit-test downlight", "the [TEST] keyword names it");
        check(closeTo(fmt::iesCandela(p, 0.0f, 0.0f), 1000.0f), "peak at nadir");
        check(closeTo(fmt::iesCandela(p, 15.0f, 123.0f), 900.0f), "interpolates between samples, ignores azimuth");
        check(closeTo(fmt::iesCandela(p, 75.0f, 0.0f), 150.0f), "75 degrees is halfway 300 -> 0");
        check(closeTo(fmt::iesCandela(p, 120.0f, 0.0f), 0.0f), "nothing above the horizon for a 0-90 file");

        fmt::IesTable t;
        check(fmt::bakeIesTable(p, t) && t.valid(), "bakes");
        check(closeTo(static_cast<f32>(tableMean(t)), 1.0f, 1e-4f), "the table's sphere mean is 1 (flux preserved)");
        check(closeTo(t.relative[0], t.peakOverMean, 1e-4f), "peakOverMean is the nadir value");
        check(t.peakOverMean > 1.5f, "a downlight concentrates: peak well above the mean");
        const u32 h = fmt::kIesTableH;
        check(t.relative[0] == t.relative[h - 1] && t.relative[5 * h] == t.relative[5 * h + 17],
              "a rotationally symmetric profile has equal columns");
        check(t.relative[(fmt::kIesTableV - 1) * h] == 0.0f, "zero straight up");
    }

    AVER_INFO("=== IES: isotropic profile normalises to exactly 1 ===");
    {
        const char* iso =
            "IESNA91\nTILT=NONE\n1 1 1 3 1 1 2 0 0 0\n1 1 10\n0 90 180\n0\n50 50 50\n";
        fmt::IesProfile p;
        fmt::IesTable t;
        check(fmt::parseIes(iso, p, &err) && fmt::bakeIesTable(p, t), "parses and bakes: " + err);
        bool all1 = true;
        for (f32 v : t.relative) all1 = all1 && closeTo(v, 1.0f, 1e-4f);
        check(all1 && closeTo(t.peakOverMean, 1.0f, 1e-4f), "every texel 1, peak/mean 1");
    }

    AVER_INFO("=== IES: horizontal symmetry unfolding ===");
    {
        // Quadrant symmetry: horizontal 0..90 only. C0 is dim, C90 bright.
        const char* quad =
            "IESNA:LM-63-1995\nTILT=NONE\n1 1000 1 2 2 1 2 0 0 0\n1 1 50\n0 90\n0 90\n"
            "100 100\n"     // C0
            "300 300\n";    // C90
        fmt::IesProfile p;
        check(fmt::parseIes(quad, p, &err), "quadrant file parses: " + err);
        check(closeTo(fmt::iesCandela(p, 0.0f, 0.0f), 100.0f), "C0");
        check(closeTo(fmt::iesCandela(p, 0.0f, 90.0f), 300.0f), "C90");
        check(closeTo(fmt::iesCandela(p, 0.0f, 270.0f), 300.0f), "C270 mirrors C90");
        check(closeTo(fmt::iesCandela(p, 0.0f, 180.0f), 100.0f), "C180 mirrors C0");
        check(closeTo(fmt::iesCandela(p, 0.0f, 45.0f), 200.0f), "C45 is the midpoint");
        check(closeTo(fmt::iesCandela(p, 0.0f, 135.0f), 200.0f), "C135 mirrors C45");

        // Bilateral: 0..180. C90 is the plane of symmetry's far edge.
        const char* bil =
            "IESNA:LM-63-1995\nTILT=NONE\n1 1000 1 2 3 1 2 0 0 0\n1 1 50\n0 90\n0 90 180\n"
            "100 100\n"
            "200 200\n"
            "400 400\n";
        check(fmt::parseIes(bil, p, &err), "bilateral file parses: " + err);
        check(closeTo(fmt::iesCandela(p, 0.0f, 270.0f), 200.0f), "C270 mirrors C90");
        check(closeTo(fmt::iesCandela(p, 0.0f, 225.0f), 300.0f), "C225 mirrors C135 (midpoint of 200 and 400)");
        check(closeTo(fmt::iesCandela(p, 0.0f, 360.0f), 100.0f), "360 wraps to C0");
    }

    AVER_INFO("=== IES: format variants and refusals ===");
    {
        fmt::IesProfile p;
        // Commas, a candela multiplier and a TILT=INCLUDE block.
        const char* inc =
            "IESNA:LM-63-1995\nTILT=INCLUDE\n1\n2\n0 90\n1 0.5\n"
            "1, 1000, 2.0, 2, 1, 1, 2, 0, 0, 0\n1, 1, 10\n0, 90\n0\n10, 20\n";
        check(fmt::parseIes(inc, p, &err), "commas and an included tilt table parse: " + err);
        check(closeTo(fmt::iesCandela(p, 0.0f, 0.0f), 20.0f) && closeTo(fmt::iesCandela(p, 90.0f, 0.0f), 40.0f),
              "the candela multiplier is applied");

        check(!fmt::parseIes("IESNA91\n1 1 1 1 1 1 2 0 0 0\n", p, &err) && err.find("TILT") != std::string::npos,
              "no TILT= line is refused with a reason");
        check(!fmt::parseIes("IESNA91\nTILT=NONE\n1 1 1 2 1 2 2 0 0 0\n1 1 10\n-90 90\n0\n5 5\n", p, &err) &&
                  err.find("Type C") != std::string::npos,
              "photometric type B is refused, naming Type C");
        check(!fmt::parseIes("IESNA91\nTILT=NONE\n1 1 1 3 1 1 2 0 0 0\n1 1 10\n0 90 180\n0\n5 5\n", p, &err) &&
                  err.find("truncated") != std::string::npos,
              "a short candela table is refused");
        check(!fmt::parseIes("IESNA91\nTILT=NONE\n1 1 1 2 1 1 2 0 0 0\n1 1 10\n90 0\n0\n5 5\n", p, &err),
              "descending vertical angles are refused");
        check(!fmt::parseIes("IESNA91\nTILT=NONE\n1 1 1 2 1 1 2 0 0 0\n1 1 10\n0 90\n0\n0 0\n", p, &err) &&
                  err.find("no emission") != std::string::npos,
              "an all-zero profile is refused");
    }

    AVER_INFO("=== half float conversion ===");
    {
        check(fmt::floatToHalf(1.0f) == 0x3C00, "1.0");
        check(fmt::floatToHalf(0.5f) == 0x3800, "0.5");
        check(fmt::floatToHalf(0.0f) == 0, "0");
        check(fmt::floatToHalf(-2.0f) == 0xC000, "-2.0");
        check(fmt::floatToHalf(65504.0f) == 0x7BFF, "largest half");
        check(fmt::floatToHalf(1.0e9f) == 0x7C00, "overflow is infinity");
        check(fmt::floatToHalf(5.9604645e-8f) == 0x0001, "smallest subnormal");
        check(fmt::floatToHalf(1.0009765625f) == 0x3C01, "1 + 2^-10");
    }

    AVER_INFO("=== .ocworld LIGHT record ===");
    {
        fmt::OcWorldData w;
        w.name = "LightRoundTrip";
        fmt::OcLight a;
        a.name = "Desk Lamp";
        a.kind = fmt::OcLightKind::Spot;
        a.x = 100.5; a.y = -20; a.z = 240; a.yaw = 30; a.pitch = -45; a.roll = 0;
        a.colour[0] = 1.0; a.colour[1] = 0.8; a.colour[2] = 0.6;
        a.intensityCd = 2500; a.rangeCm = 1200; a.innerDeg = 10; a.outerDeg = 35; a.radiusCm = 3;
        a.ies = "Lights/downlight.ies";
        a.cookie = "Textures/gobo.png";
        a.iesPeak = true;
        fmt::OcLight b;
        b.kind = fmt::OcLightKind::Rect;
        b.x = 0; b.y = 0; b.z = 300;
        b.intensityCd = 8000; b.widthCm = 120; b.heightCm = 60; b.castShadows = false;
        w.lights = {a, b};

        const std::string text = fmt::writeOcworld(w);
        fmt::OcWorldData back;
        check(fmt::parseOcworld(text, back, &err), "written level parses: " + err);
        check(back.lights.size() == 2, "both lights survive");
        if (back.lights.size() == 2) {
            const fmt::OcLight& x = back.lights[0];
            check(x.name == "Desk Lamp" && x.kind == fmt::OcLightKind::Spot, "name (percent-encoded) and kind");
            check(x.x == 100.5 && x.y == -20 && x.z == 240 && x.yaw == 30 && x.pitch == -45, "pose");
            check(x.colour[1] == 0.8 && x.intensityCd == 2500 && x.rangeCm == 1200, "colour, intensity, range");
            check(x.innerDeg == 10 && x.outerDeg == 35 && x.radiusCm == 3, "cone and radius");
            check(x.ies == "Lights/downlight.ies" && x.cookie == "Textures/gobo.png", "asset references");
            check(x.iesPeak && x.castShadows, "flags");
            const fmt::OcLight& y = back.lights[1];
            check(y.kind == fmt::OcLightKind::Rect && y.widthCm == 120 && y.heightCm == 60, "rect size");
            check(!y.castShadows && !y.iesPeak && y.ies.empty() && y.cookie.empty(), "noshadow, no assets");
        }
        check(lightLines(fmt::writeOcworld(back)) == lightLines(text), "writing it again gives the same LIGHT lines");

        fmt::OcWorldData none;
        check(fmt::parseOcworld("OCWORLD 1\nNAME Old\n", none, &err) && none.lights.empty(),
              "a level written before LIGHT existed has no lights");
        fmt::OcWorldData junk;
        check(fmt::parseOcworld("OCWORLD 1\nLIGHT kind banana pos 1 2 3 wibble 7 intensity 5\n", junk, &err) &&
                  junk.lights.size() == 1 && junk.lights[0].kind == fmt::OcLightKind::Point &&
                  junk.lights[0].x == 1 && junk.lights[0].intensityCd == 5,
              "an unknown kind and unknown tokens fall back, the rest still reads");
    }

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
