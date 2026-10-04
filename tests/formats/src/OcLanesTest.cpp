// OcLanesTest -- the .oclanes traffic-lane sidecar: parse, round-trip, and every way it should refuse.
//
// THE ROUND TRIP IS THE POINT, as it is for .ocrig: a writer and a reader that disagree corrupt the
// file every time it is saved, and quietly. It is held to MORE here, though -- lane points are
// compared with ==, not a tolerance, because writeOcLanes promises the shortest text that reads back
// as the same f32, and a tolerance would let that promise rot to six digits unnoticed.
//
// THE REFUSALS ARE THE OTHER HALF. A lane file is written by a generator script nobody reads the
// output of, and its damage shows up as one car stopping at one junction. Every malformed shape below
// must be refused WITH A REASON, because "the lanes did not load" is not something anyone can act on.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcLanes.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool sameLane(const fmt::OcLane& a, const fmt::OcLane& b) {
    if (a.id != b.id || a.speedLimit != b.speedLimit || a.width != b.width || a.flags != b.flags ||
        a.pts.size() != b.pts.size() || a.next != b.next) return false;
    for (usize i = 0; i < a.pts.size(); ++i)
        if (a.pts[i].x != b.pts[i].x || a.pts[i].y != b.pts[i].y || a.pts[i].z != b.pts[i].z) return false;
    return true;
}

static bool sameLanes(const fmt::OcLanesData& a, const fmt::OcLanesData& b) {
    if (a.lanes.size() != b.lanes.size()) return false;
    for (usize i = 0; i < a.lanes.size(); ++i)
        if (!sameLane(a.lanes[i], b.lanes[i])) return false;
    return true;
}

static std::string withCrlf(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

// Lane 13 is named by a NEXT that comes BEFORE it, lane 11's NEXT names 12 and 13 which are declared
// further down, lane 12 is a dead end, and a comment trails a record.
static const char* kLanes = R"(OCLANES 1
# two lanes, a ramp and a way back
NEXT 13 10          # before the lane it names
LANE 10 1300 350 0 3  0 0 0  1000 0 0  2000 0 0   # trailing comment
LANE 11 2200 350 5 2  2000 0 0  9000 0 50
NEXT 10 11
NEXT 11 10 12 13    # 12 and 13 are declared further down

LANE 12 800.5 300 2 2  9000 0 50  9000 5000 50
LANE 13 1300 350 0 2  -500 0 0  0 0 0
)";

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO(".oclanes");

    AVER_INFO("=== it parses ===");
    fmt::OcLanesData d;
    std::string err;
    check(fmt::parseOcLanes(kLanes, d, &err), "the fixture parses: " + err);
    check(d.lanes.size() == 4, "all four lanes arrived, got " + std::to_string(d.lanes.size()));
    if (d.lanes.size() == 4) {
        const fmt::OcLane& a = d.lanes[0];
        check(a.id == 10 && a.speedLimit == 1300.0f && a.width == 350.0f && a.flags == 0,
              "lane 10: id, speed limit, width and flags");
        check(a.pts.size() == 3 && a.pts[2].x == 2000.0f && a.pts[2].y == 0.0f,
              "with three points, the last at 2000 cm along +X");
        check(a.next.size() == 1 && a.next[0] == 11, "and one successor, from a NEXT written after it");

        const fmt::OcLane& b = d.lanes[1];
        check(b.flags == (fmt::kOcLaneHighway | fmt::kOcLaneRamp), "lane 11: flags 5 are highway + ramp");
        check(b.pts.size() == 2 && b.pts[1].z == 50.0f, "its second point is raised 50 cm");
        check(b.next.size() == 3 && b.next[0] == 10 && b.next[1] == 12 && b.next[2] == 13,
              "and its successors keep the order the file wrote them, forward references and all");

        const fmt::OcLane& c = d.lanes[2];
        check(c.speedLimit == 800.5f && c.width == 300.0f && c.flags == fmt::kOcLaneBridge,
              "lane 12: a fractional speed limit, and the bridge bit alone");
        check(c.next.empty(), "and no NEXT at all means a dead end");

        const fmt::OcLane& e = d.lanes[3];
        check(e.next.size() == 1 && e.next[0] == 10, "lane 13: a NEXT that came BEFORE its LANE still lands on it");
        check(e.pts[0].x == -500.0f, "and a negative coordinate reads as negative");
    }

    AVER_INFO("=== CRLF, and a last line with no newline ===");
    {
        fmt::OcLanesData crlf;
        check(fmt::parseOcLanes(withCrlf(kLanes), crlf, &err), "the same file with CRLF line ends parses: " + err);
        check(sameLanes(crlf, d), "to exactly the same lanes");

        std::string noNewline = kLanes;
        while (!noNewline.empty() && noNewline.back() == '\n') noNewline.pop_back();
        fmt::OcLanesData bare;
        check(fmt::parseOcLanes(noNewline, bare, &err), "a file whose last line has no newline parses: " + err);
        check(sameLanes(bare, d), "to exactly the same lanes");
    }

    AVER_INFO("=== write -> parse gives back the same data, exactly ===");
    {
        // Awkward floats on purpose: 0.1 and 1234.5678 have no short binary form, 123456.78 is past
        // what %g keeps, and 1e10 / 1e-3 pull fixed notation both ways.
        fmt::OcLanesData awkward;
        fmt::OcLane lane;
        lane.id = -7;                                  // any int is an id; a ring road follows itself
        lane.speedLimit = 1234.5678f;
        lane.width = 0.1f;
        lane.flags = 7;
        lane.pts = {Vec3{123456.78f, -0.001f, 1e-3f}, Vec3{16777216.0f, 1e10f, -3.4e20f}, Vec3{0.1f, 0.2f, 0.3f}};
        lane.next = {-7};
        awkward.lanes.push_back(lane);
        awkward.lanes.insert(awkward.lanes.end(), d.lanes.begin(), d.lanes.end());

        const std::string text = fmt::writeOcLanes(awkward);
        fmt::OcLanesData back;
        check(fmt::parseOcLanes(text, back, &err), "what it wrote parses again: " + err);
        check(sameLanes(back, awkward), "and every lane is identical field for field, with == on the floats");
        check(fmt::writeOcLanes(back) == text, "a second write is byte-identical to the first");
        check(text.find("NEXT 12") == std::string::npos, "a lane with no successors writes no NEXT line");
        check(text.rfind("OCLANES 1\n", 0) == 0, "and the file begins with its header");
    }

    AVER_INFO("=== a file round-trips through disk ===");
    {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "aver-oclanes-test";
        std::filesystem::create_directories(dir, ec);
        const std::string path = (dir / "demo.oclanes").string();
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            f << fmt::writeOcLanes(d);
        }
        fmt::OcLanesData disk;
        check(fmt::loadOcLanes(path, disk, &err), "it loads: " + err);
        check(sameLanes(disk, d), "with its lanes");
        err.clear();
        check(!fmt::loadOcLanes((dir / "nope.oclanes").string(), disk, &err),
              "a missing file is refused rather than silently empty");
        check(!err.empty(), "  ...and says why: " + err);
        std::filesystem::remove_all(dir, ec);
    }

    AVER_INFO("=== what it refuses, and each says why ===");
    {
        const std::string one = "LANE 1 1300 350 0 2 0 0 0 1 1 1\n";
        const struct { std::string text; const char* what; } bad[] = {
            {one,                                                             "a lane before any header"},
            {"OCLANES 2\n",                                                   "a version it does not know"},
            {"OCLANES\n",                                                     "a header with no version"},
            {"OCLANES 1\nOCLANES 1\n",                                        "a second header"},
            {"OCLANES 1\nWIBBLE 3\n",                                         "a record it does not recognise"},
            {"OCLANES 1\nLANE 1 1300 350 0\n",                                "a LANE cut off before its point count"},
            {"OCLANES 1\nLANE 1 1300 350 0 1 0 0 0\n",                        "a lane of one point"},
            {"OCLANES 1\nLANE 1 1300 350 0 0\n",                              "a lane of none"},
            {"OCLANES 1\nLANE 1 1300 350 0 3 0 0 0 1 1 1\n",                  "fewer points than the count says"},
            {"OCLANES 1\nLANE 1 1300 350 0 2 0 0 0 1 1 1 2 2 2\n",            "more points than the count says"},
            {"OCLANES 1\nLANE 1 1300 350 0 2 0 0 0 1 1\n",                    "a point missing a coordinate"},
            {"OCLANES 1\nLANE 1 1300 350 0 2 0 0 0 1 1 abc\n",                "a coordinate that is not a number"},
            {"OCLANES 1\nLANE 1 1300 350 0 2 0 0 0 1 1 12x\n",                "a number with trailing junk, which is not read as its prefix"},
            {"OCLANES 1\nLANE 1 1300 350 0 2 0 0 0 1 1 nan\n",                "a NaN coordinate"},
            {"OCLANES 1\nLANE 1 1300 350 0 2 0 0 0 1 1 inf\n",                "an infinite coordinate"},
            {"OCLANES 1\nLANE 1 0 350 0 2 0 0 0 1 1 1\n",                     "a speed limit of zero"},
            {"OCLANES 1\nLANE 1 1300 -5 0 2 0 0 0 1 1 1\n",                   "a negative width"},
            {"OCLANES 1\nLANE 1 1300 350 -1 2 0 0 0 1 1 1\n",                 "negative flags"},
            {"OCLANES 1\nLANE x 1300 350 0 2 0 0 0 1 1 1\n",                  "a lane id that is not a number"},
            {"OCLANES 1\n" + one + one,                                       "the same lane id twice"},
            {"OCLANES 1\nLANE 1 1300 350 0 4000000000 0 0 0 1 1 1\n",         "a point count in the billions, without reserving for it"},
            {"OCLANES 1\nNEXT\n",                                             "a NEXT with no lane id"},
            {"OCLANES 1\nNEXT 9 1\n",                                         "a NEXT for a lane nothing declares"},
            {"OCLANES 1\n" + one + "NEXT 1 2\n",                              "a NEXT whose successor nothing declares"},
            {"OCLANES 1\n" + one + "NEXT 1 x\n",                              "a successor that is not a number"},
            {"OCLANES 1\n" + one + "NEXT 1\nNEXT 1\n",                        "two NEXT records for one lane"},
        };
        for (const auto& c : bad) {
            fmt::OcLanesData out;
            std::string why;
            const bool ok = fmt::parseOcLanes(c.text, out, &why);
            check(!ok, std::string(c.what) + " is refused");
            if (!ok) {
                check(!why.empty(), std::string("  ...and says why: ") + why);
                check(out.lanes.empty(), "  ...leaving no half-read lanes behind");
            }
        }

        // The line number is what makes a refusal actionable in a file of a few thousand lines.
        fmt::OcLanesData out;
        std::string why;
        check(!fmt::parseOcLanes("OCLANES 1\n\nLANE 1 1300 350 0 1 0 0 0\n", out, &why) &&
              why.find("line 3") != std::string::npos, "a bad record names its line: " + why);
        why.clear();
        check(!fmt::parseOcLanes("OCLANES 1\n" + one + "\n\nNEXT 1 2\n", out, &why) &&
              why.find("line 5") != std::string::npos, "and so does a NEXT that only fails once the whole file is read: " + why);
    }

    AVER_INFO("=== empty and near-empty files ===");
    {
        fmt::OcLanesData out;
        std::string why;
        check(!fmt::parseOcLanes("", out, &why), "a file with nothing in it is refused");
        check(why.find("OCLANES") != std::string::npos, "  ...naming the header it wanted: " + why);
        check(!fmt::parseOcLanes("   \n# only a comment\n\n", out, &why), "so is one with only comments and blanks");

        check(fmt::parseOcLanes("OCLANES 1\n", out, &why) && out.lanes.empty(),
              "a header and no lanes is a level with no traffic, and is legal: " + why);
        check(fmt::parseOcLanes("# preface\nOCLANES 1\n# nothing else\n", out, &why) && out.lanes.empty(),
              "comments around the header change nothing: " + why);

        // A parse into a struct that already holds lanes replaces them whether it succeeds or not.
        fmt::OcLanesData reused = d;
        check(fmt::parseOcLanes("OCLANES 1\n", reused, &why) && reused.lanes.empty(),
              "a successful parse replaces what the struct held");
        reused = d;
        check(!fmt::parseOcLanes("OCLANES 1\nWIBBLE\n", reused, &why) && reused.lanes.empty(),
              "and a failed one leaves it empty, not holding the old lanes");
    }

    AVER_INFO("=== a level's worth of lanes ===");
    {
        // The generator writes about two thousand lanes. This is the size the format is for, and a
        // chain of them so every lane has a successor to resolve through the deferred NEXT pass.
        constexpr int kLaneCount = 2000, kPoints = 40;
        fmt::OcLanesData big;
        for (int i = 0; i < kLaneCount; ++i) {
            fmt::OcLane lane;
            lane.id = 1000 + i;
            lane.speedLimit = 1300.0f + static_cast<f32>(i % 7) * 100.0f;
            lane.flags = static_cast<u32>(i % 8);
            for (int j = 0; j < kPoints; ++j)
                lane.pts.emplace_back(static_cast<f32>(i) * 100.37f + static_cast<f32>(j) * 25.123f,
                                      static_cast<f32>(j) * 3.7f, static_cast<f32>(i % 5) * 0.25f);
            lane.next = {1000 + (i + 1) % kLaneCount};
            big.lanes.push_back(std::move(lane));
        }
        const std::string text = fmt::writeOcLanes(big);
        fmt::OcLanesData back;
        check(fmt::parseOcLanes(text, back, &err), "2,000 lanes of 40 points parse: " + err);
        check(back.lanes.size() == static_cast<usize>(kLaneCount), "none lost");
        check(sameLanes(back, big), "and all identical to what was written");
    }

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
