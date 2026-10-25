// OcSurfTest -- the .ocsurf physical-surface table (SHAPES_MATERIALS): parse, defaults, the canonical write, and every way
// it should refuse. With the material library built in (OCSURF_WITH_OCMAT) it also covers the .ocmat PHYSSURFACE record that
// names a slot's surface: it parses, it writes only when set, and a material that never named one writes the bytes it always did.
//
// THE ROUND TRIP IS THE POINT, as it is for .oclanes: a writer and a reader that disagree corrupt the file every time the editor
// saves it. THE REFUSALS ARE THE OTHER HALF: a table nobody reads the output of must fail with a reason, not load half of itself.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcSurf.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#ifdef OCSURF_WITH_OCMAT
#  include "aver/formats/OcMat.hpp"
#endif

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string withCrlf(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

static bool refused(const std::string& text, const std::string& reasonPart) {
    fmt::OcSurfData d;
    std::string err;
    const bool ok = fmt::parseOcSurf(text, d, &err);
    return !ok && d.surfaces.empty() && err.find(reasonPart) != std::string::npos;
}

static const char* kTable = R"(OCSURF 1
# the surfaces of a small project
SURFACE Grass  friction 0.6  restitution 0.05  tag 1
SURFACE Ice    friction 0.03       # trailing comment
SURFACE Metal  friction 0.35 restitution 0.2   tag 4

SURFACE Decal
)";

static void testParse() {
    AVER_INFO("=== parse ===");
    fmt::OcSurfData d;
    std::string err;
    check(fmt::parseOcSurf(kTable, d, &err), "the example parses: " + err);
    check(d.surfaces.size() == 4, "four surfaces");
    if (d.surfaces.size() != 4) return;
    check(d.surfaces[0].name == "Grass" && d.surfaces[0].friction == 0.6f && d.surfaces[0].restitution == 0.05f && d.surfaces[0].tag == 1, "Grass has all three numbers");
    check(d.surfaces[1].name == "Ice" && d.surfaces[1].friction == 0.03f && d.surfaces[1].restitution < 0.0f && d.surfaces[1].tag == 0,
          "Ice states friction only: restitution is unset (defers to the body), tag 0");
    check(d.surfaces[2].name == "Metal" && d.surfaces[2].tag == 4, "Metal keeps file order and its tag");
    check(d.surfaces[3].name == "Decal" && d.surfaces[3].friction < 0.0f && d.surfaces[3].restitution < 0.0f && d.surfaces[3].tag == 0,
          "a bare SURFACE names a surface and changes nothing");

    fmt::OcSurfData crlf;
    check(fmt::parseOcSurf(withCrlf(kTable), crlf, &err) && crlf.surfaces.size() == 4, "a CRLF file parses the same");

    fmt::OcSurfData header;
    check(fmt::parseOcSurf("OCSURF 1\n", header, &err) && header.surfaces.empty(), "a file with only the header is a table of no surfaces");

    fmt::OcSurfData clamp;
    check(fmt::parseOcSurf("OCSURF 1\nSURFACE A friction 50 restitution -3\n", clamp, &err) && clamp.surfaces[0].friction == 10.0f && clamp.surfaces[0].restitution < 0.0f,
          "a stated value clamps to 10 and a negative one is unset");
    fmt::OcSurfData caseInsensitive;
    check(fmt::parseOcSurf("ocsurf 1\nsurface A FRICTION 0.5 Tag 2\n", caseInsensitive, &err) && caseInsensitive.surfaces.size() == 1 &&
          caseInsensitive.surfaces[0].friction == 0.5f && caseInsensitive.surfaces[0].tag == 2, "record and key words are case-insensitive");
    check(fmt::ocSurfNameValid("Wet_Grass2") && !fmt::ocSurfNameValid("") && !fmt::ocSurfNameValid("Wet Grass") && !fmt::ocSurfNameValid("a-b") &&
          fmt::ocSurfNameValid(std::string(63, 'a')) && !fmt::ocSurfNameValid(std::string(64, 'a')), "name rules: letters, digits and underscore, 1 to 63");
}

static void testRoundTrip() {
    AVER_INFO("=== round trip ===");
    fmt::OcSurfData d;
    std::string err;
    fmt::parseOcSurf(kTable, d, &err);
    const std::string canon = fmt::writeOcSurf(d);
    check(canon == "OCSURF 1\nSURFACE Grass friction 0.6 restitution 0.05 tag 1\nSURFACE Ice friction 0.03\nSURFACE Metal friction 0.35 restitution 0.2 tag 4\nSURFACE Decal\n",
          "the canonical form keeps only the keys that are set");
    fmt::OcSurfData back;
    check(fmt::parseOcSurf(canon, back, &err) && fmt::writeOcSurf(back) == canon, "write-then-parse-then-write is byte-identical");
    bool same = back.surfaces.size() == d.surfaces.size();
    for (size_t i = 0; same && i < d.surfaces.size(); ++i)
        same = back.surfaces[i].name == d.surfaces[i].name && back.surfaces[i].friction == d.surfaces[i].friction &&
               back.surfaces[i].restitution == d.surfaces[i].restitution && back.surfaces[i].tag == d.surfaces[i].tag;
    check(same, "every number reads back exactly");

    // A third of a friction is not a round decimal: the writer must keep every digit the f32 needs.
    fmt::OcSurfData third;
    third.surfaces.push_back({"Third", 1.0f / 3.0f, 0.1f, -7});
    fmt::OcSurfData thirdBack;
    check(fmt::parseOcSurf(fmt::writeOcSurf(third), thirdBack, &err) && thirdBack.surfaces[0].friction == third.surfaces[0].friction &&
          thirdBack.surfaces[0].restitution == third.surfaces[0].restitution && thirdBack.surfaces[0].tag == -7, "1/3, 0.1 and a negative tag survive exactly");
}

static void testRefusals() {
    AVER_INFO("=== refusals ===");
    check(refused("", "OCSURF"), "an empty file has no header");
    check(refused("SURFACE A\n", "OCSURF 1"), "a record before the header");
    check(refused("OCSURF 2\nSURFACE A\n", "newer"), "a newer version is refused, saying so");
    check(refused("OCSURF 0\n", "version"), "version 0 is refused");
    check(refused("OCSURF x\n", "version"), "a non-numeric version is refused");
    check(refused("OCSURF 1\nOCSURF 1\n", "second"), "a second header");
    check(refused("OCSURF 1\nSURFACE\n", "name"), "SURFACE with no name");
    check(refused("OCSURF 1\nSURFACE Bad-Name\n", "name"), "a name with a hyphen");
    check(refused("OCSURF 1\nSURFACE A\nSURFACE A\n", "twice"), "a repeated name (line 3)");
    check(refused("OCSURF 1\nSURFACE A friction\n", "no value"), "a key with no value");
    check(refused("OCSURF 1\nSURFACE A friction abc\n", "friction"), "a non-numeric friction");
    check(refused("OCSURF 1\nSURFACE A friction 0.5x\n", "friction"), "a numeric prefix is not a number");
    check(refused("OCSURF 1\nSURFACE A restitution nan\n", "restitution"), "a NaN restitution");
    check(refused("OCSURF 1\nSURFACE A tag 1.5\n", "tag"), "a fractional tag");

    std::string many = "OCSURF 1\n";
    for (int i = 0; i < 4096; ++i) many += "SURFACE S" + std::to_string(i) + "\n";
    fmt::OcSurfData d;
    std::string err;
    check(fmt::parseOcSurf(many, d, &err) && d.surfaces.size() == 4096, "4096 surfaces are accepted");
    many += "SURFACE One_More\n";
    check(refused(many, "4096"), "4097 are refused");
    check(refused("OCSURF 1\nSURFACE A\nSURFACE B friction oops\n", "line 3"), "the reason names the line");

    // Forward compatibility: what a newer engine added is skipped, not fatal.
    fmt::OcSurfData fwd;
    check(fmt::parseOcSurf("OCSURF 1\nSURFACE A friction 0.4 sound Thud tag 3\nFUTURE thing 1\nSURFACE B\n", fwd, &err) && fwd.surfaces.size() == 2 &&
          fwd.surfaces[0].friction == 0.4f && fwd.surfaces[0].tag == 3, "an unknown key and an unknown record are skipped, the rest of the line still reads");
}

static void testFiles() {
    AVER_INFO("=== load and save ===");
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aver_ocsurf_test" / "Content" / "Physics";
    std::filesystem::remove_all(dir.parent_path().parent_path());
    const std::string path = (dir / "Surfaces.ocsurf").string();

    fmt::OcSurfData d;
    std::string err;
    fmt::parseOcSurf(kTable, d, &err);
    check(fmt::saveOcSurf(path, d, &err), "saveOcSurf creates the folder and writes: " + err);
    fmt::OcSurfData loaded;
    check(fmt::loadOcSurf(path, loaded, &err) && fmt::writeOcSurf(loaded) == fmt::writeOcSurf(d), "loadOcSurf reads back what was saved");
    check(!fmt::loadOcSurf((dir / "Missing.ocsurf").string(), loaded, &err) && err.find("cannot open") != std::string::npos, "a missing file is refused with a reason");

    { std::ofstream bad(path, std::ios::binary | std::ios::trunc); bad << "OCSURF 1\nSURFACE A friction nope\n"; }
    check(!fmt::loadOcSurf(path, loaded, &err), "a damaged file does not load");
    std::filesystem::remove_all(dir.parent_path().parent_path());
}

#ifdef OCSURF_WITH_OCMAT
static void testMaterialRecord() {
    AVER_INFO("=== .ocmat PHYSSURFACE ===");
    pbr::MaterialDesc d;
    fmt::OcMatExtras ex;
    std::string err;
    check(fmt::parseOcmat("OCMAT 1\nNAME Pavement\nPHYSSURFACE Concrete_Wet\n", d, &ex, &err) && ex.physSurface == "Concrete_Wet", "PHYSSURFACE parses into the extras");
    check(fmt::parseOcmat("OCMAT 1\nNAME Pavement\n", d, &ex, &err) && ex.physSurface.empty(), "a material without the record has none");
    check(fmt::parseOcmat("OCMAT 1\nphyssurface Ice\n", d, &ex, &err) && ex.physSurface == "Ice", "the keyword is case-insensitive");
    check(fmt::parseOcmat("OCMAT 1\nPHYSSURFACE bad-name\n", d, &ex, &err) && ex.physSurface.empty(), "an invalid name is skipped, the material still loads");
    check(fmt::parseOcmat("OCMAT 1\nPHYSSURFACE\n", d, &ex, &err) && ex.physSurface.empty(), "a record with no name is skipped");

    // Write: only when set, so a material that never named a surface writes the bytes it always did.
    fmt::OcMatExtras none;
    const std::string before = fmt::writeOcmat(d, &none);
    check(before.find("PHYSSURFACE") == std::string::npos, "an unset surface writes no record");
    fmt::OcMatExtras named;
    named.physSurface = "Ice";
    const std::string after = fmt::writeOcmat(d, &named);
    check(after == before + "\nPHYSSURFACE Ice\n", "a set surface adds exactly one record and nothing else changes");
    pbr::MaterialDesc back;
    fmt::OcMatExtras backEx;
    check(fmt::parseOcmat(after, back, &backEx, &err) && backEx.physSurface == "Ice" && fmt::writeOcmat(back, &backEx) == after, "it round-trips");
}
#endif

int main() {
    AVER_INFO("==================================================");
    AVER_INFO(".ocsurf: the physical-surface table");
    testParse();
    testRoundTrip();
    testRefusals();
    testFiles();
#ifdef OCSURF_WITH_OCMAT
    testMaterialRecord();
#endif
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
