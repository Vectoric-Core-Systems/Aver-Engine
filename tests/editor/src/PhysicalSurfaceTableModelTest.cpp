// PhysicalSurfaceTableModelTest -- the rules behind Project Settings > Physics > Surfaces and the material inspector's Physical Surface
// combo, without a window (sandbox/src/PhysicalSurfaceTableModel.hpp, header-only): duplicate names, a hash collision, the 4096 cap,
// the row edits, that the text Save writes is exactly what fmt::writeOcSurf gives, and the .ocmat PHYSSURFACE patch (which must leave
// every other byte of the file alone, GRAPH blocks and comments included).
#include "PhysicalSurfaceTableModel.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <string>

using namespace aver;
namespace sm = aver::editor::surfmodel;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static fmt::OcSurfData table(std::initializer_list<const char*> names) {
    fmt::OcSurfData d;
    for (const char* n : names) {
        fmt::OcSurfRecord r;
        r.name = n;
        d.surfaces.push_back(r);
    }
    return d;
}

static void testProblems() {
    AVER_INFO("=== problems ===");
    check(sm::problems(table({"Grass", "Ice", "Metal"})).empty() && sm::canSave(table({"Grass", "Ice"})), "a clean table has no problem and can be saved");
    check(sm::canSave(fmt::OcSurfData{}), "an empty table can be saved (a project that names no surface)");

    auto p = sm::problems(table({"Grass", "Grass"}));
    check(p.size() == 1 && p[0].row == 1 && p[0].text.find("row 1") != std::string::npos, "a repeated name is a problem on the second row, naming the first");
    p = sm::problems(table({"Good", "", "Bad Name", "a-b"}));
    check(p.size() == 3 && p[0].row == 1 && p[1].row == 2 && p[2].row == 3, "an empty name and two invalid names are each a problem on their row");
    check(!sm::canSave(table({"Good", ""})), "and a table with one cannot be saved");

    // A collision of the 32-bit ids, injected: "Ice" and "Snow" both hash to 7.
    const sm::IdFn colliding = [](std::string_view n) { return (n == "Ice" || n == "Snow") ? 7 : 100 + static_cast<i32>(n.size()); };
    p = sm::problems(table({"Ice", "Grass", "Snow"}), colliding);
    check(p.size() == 1 && p[0].row == 2 && p[0].text.find("'Ice'") != std::string::npos, "a hash collision is a problem on the later row, naming the earlier");
    check(sm::problems(table({"Ice", "Snow"})).empty(), "(the real hash keeps those two apart)");

    fmt::OcSurfData big;
    for (u32 i = 0; i < fmt::kOcSurfMaxSurfaces + 1; ++i) {
        fmt::OcSurfRecord r;
        r.name = "S" + std::to_string(i);
        big.surfaces.push_back(r);
    }
    p = sm::problems(big);
    bool capped = false;
    for (const auto& r : p) capped = capped || r.text.find("4096") != std::string::npos;
    check(capped, "4097 rows are a problem naming the cap");
}

static void testEdits() {
    AVER_INFO("=== row edits ===");
    fmt::OcSurfData d = table({"Surface", "Surface2", "Ice"});
    check(sm::uniqueName(d) == "Surface3", "uniqueName skips the names that are taken");
    check(sm::uniqueName(d, "Rubber") == "Rubber" && sm::uniqueName(d, "Ice") == "Ice2" && sm::uniqueName(d, "bad name") == "Surface3", "a free base is kept, a taken one is numbered, an invalid one falls back");
    const usize at = sm::addRow(d, "Metal");
    check(at == 3 && d.surfaces[3].name == "Metal" && d.surfaces[3].friction < 0.0f && d.surfaces[3].restitution < 0.0f && d.surfaces[3].tag == 0, "addRow appends an unset surface");
    check(sm::moveRow(d, 3, -1) && d.surfaces[2].name == "Metal" && d.surfaces[3].name == "Ice", "moveRow swaps with the neighbour");
    check(!sm::moveRow(d, 0, -1) && !sm::moveRow(d, 3, 1) && !sm::moveRow(d, 9, 1), "and refuses at the ends and past them");
    check(sm::removeRow(d, 0) && d.surfaces.size() == 3 && d.surfaces[0].name == "Surface2" && !sm::removeRow(d, 9), "removeRow drops a row and refuses a bad index");
    check(sm::names(d).size() == 3 && sm::names(d)[1] == "Metal", "names lists them in order");
}

static void testSaveAndDiff() {
    AVER_INFO("=== save text and changed rows ===");
    fmt::OcSurfData d;
    std::string err;
    check(fmt::parseOcSurf("OCSURF 1\nSURFACE Grass friction 0.6 restitution 0.05 tag 1\nSURFACE Ice friction 0.03\n", d, &err), "a table parses: " + err);
    check(sm::saveText(d) == fmt::writeOcSurf(d), "Save writes what fmt::writeOcSurf gives");
    fmt::OcSurfData back;
    check(fmt::parseOcSurf(sm::saveText(d), back, &err) && sm::saveText(back) == sm::saveText(d), "and it round-trips");

    fmt::OcSurfData now = d;
    check(sm::changedRows(d, now).empty(), "no edit, no changed row");
    now.surfaces[1].friction = 0.02f;
    sm::addRow(now, "Rubber");
    const auto changed = sm::changedRows(d, now);
    check(changed.size() == 2 && changed[0] == 1 && changed[1] == 2, "a changed number and a new row are the rows to register");
    now.surfaces[0].name = "Lawn";
    check(sm::changedRows(d, now).size() == 3, "a renamed row counts as new");
}

static void testPatch() {
    AVER_INFO("=== .ocmat PHYSSURFACE patch ===");
    const std::string mat = "OCMAT 1\nNAME Pavement\nMETALLIC 0\n# a hand note\nGRAPH {\n  node 1\n}\nROUGHNESS 0.8\n";
    const std::string set = sm::patchPhysSurface(mat, "Concrete");
    check(set == mat + "\nPHYSSURFACE Concrete\n", "a material without the record gets one appended after a blank line, nothing else changes");
    check(sm::readPhysSurface(set) == "Concrete" && sm::readPhysSurface(mat).empty(), "readPhysSurface finds it, and finds none in the original");

    const std::string replaced = sm::patchPhysSurface(set, "Ice");
    check(replaced == mat + "\nPHYSSURFACE Ice\n", "changing it replaces the record, not adds a second");
    check(sm::patchPhysSurface(replaced, "Ice") == replaced, "patching to the same name is a no-op");
    check(sm::patchPhysSurface(replaced, "") == mat, "an empty name removes the record and the blank line before it");
    check(sm::patchPhysSurface(mat, "") == mat, "removing a record that is not there changes nothing");

    const std::string mid = "OCMAT 1\nPHYSSURFACE Old\nNAME X\n";
    check(sm::patchPhysSurface(mid, "New") == "OCMAT 1\nNAME X\n\nPHYSSURFACE New\n", "a record in the middle moves to the end");
    check(sm::readPhysSurface("OCMAT 1\nphyssurface   Low  \n") == "Low", "the keyword is case-insensitive and the name is the first token");
    check(sm::readPhysSurface("OCMAT 1\nPHYSSURFACEX Nope\n").empty(), "PHYSSURFACEX is not the record");

    const std::string crlf = "OCMAT 1\r\nNAME A\r\n";
    const std::string crlfSet = sm::patchPhysSurface(crlf, "Ice");
    check(crlfSet == "OCMAT 1\r\nNAME A\r\n\r\nPHYSSURFACE Ice\r\n", "a CRLF file stays CRLF");
}

int main() {
    AVER_INFO("==================================================");
    AVER_INFO("Physical surface table model");
    testProblems();
    testEdits();
    testSaveAndDiff();
    testPatch();
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
