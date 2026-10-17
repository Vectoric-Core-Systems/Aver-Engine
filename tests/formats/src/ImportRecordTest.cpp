// The .ocimport record (ImportRecord.hpp): parse, write, round trip, unknown lines kept, CRLF, relative and
// absolute sources, and a refusal with a reason for each malformed shape. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/formats/ImportRecord.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static const char* kSample =
    "OCIMPORT 1\n"
    "FORMAT fbx\n"
    "SOURCE Source/Hero.fbx\n"
    "SOURCESIZE 1843201\n"
    "SOURCEMTIME 133901234567890000\n"
    "OPTIONS scale=100;rootMotion=auto\n"
    "OUTPUT mesh Hero.ocmesh\n"
    "OUTPUT skeleton Hero.ocskel\n"
    "OUTPUT anim Hero_Walk.ocanim\n";

// Parses and expects failure; the reason must contain `needle`.
static void refused(const std::string& text, const std::string& needle, const std::string& what) {
    fmt::ImportRecord r;
    std::string why;
    const bool ok = fmt::parseImportRecord(text, r, &why);
    check(!ok && why.find(needle) != std::string::npos, what + "  (reason: " + why + ")");
}

int main() {
    AVER_INFO("ImportRecordTest");

    AVER_INFO("=== parse and write ===");
    {
        fmt::ImportRecord r;
        std::string why;
        check(fmt::parseImportRecord(kSample, r, &why), "the sample parses");
        check(r.version == 1 && r.format == "fbx", "version and format");
        check(r.source == "Source/Hero.fbx" && r.sourceSize == 1843201ull && r.sourceMTime == 133901234567890000ll,
              "source, size and time");
        check(r.options == "scale=100;rootMotion=auto", "options text, with its inner characters");
        check(r.outputs.size() == 3 && r.outputs[0].kind == "mesh" && r.outputs[0].file == "Hero.ocmesh" &&
              r.outputs[2].kind == "anim" && r.outputs[2].file == "Hero_Walk.ocanim", "outputs in order");
        check(fmt::writeImportRecordText(r) == kSample, "writing it back is byte-identical");
        fmt::ImportRecord again;
        check(fmt::parseImportRecord(fmt::writeImportRecordText(r), again, &why) && again == r, "and it round trips");
    }

    AVER_INFO("=== unknown lines are kept ===");
    {
        const std::string text = std::string(kSample) + "FUTURE keep=this one\n# a comment\n";
        fmt::ImportRecord r;
        check(fmt::parseImportRecord(text, r, nullptr), "a record with lines this version does not know parses");
        check(r.extraLines.size() == 2 && r.extraLines[0] == "FUTURE keep=this one" && r.extraLines[1] == "# a comment",
              "they are kept verbatim and in order");
        check(fmt::writeImportRecordText(r) == text, "and written back");
    }

    AVER_INFO("=== CRLF and blank lines ===");
    {
        std::string crlf;
        for (const char* p = kSample; *p; ++p) { if (*p == '\n') crlf += '\r'; crlf += *p; }
        crlf = "\r\n" + crlf + "\r\n\r\n";
        fmt::ImportRecord a, b;
        check(fmt::parseImportRecord(crlf, a, nullptr) && fmt::parseImportRecord(kSample, b, nullptr) && a == b,
              "CRLF and blank lines read the same as LF");
    }

    AVER_INFO("=== sources ===");
    {
        fmt::ImportRecord r;
        r.source = "Source/My Hero.fbx";
        r.format = "fbx";
        const std::string text = fmt::writeImportRecordText(r);
        fmt::ImportRecord back;
        check(fmt::parseImportRecord(text, back, nullptr) && back.source == "Source/My Hero.fbx", "a source path with a space survives");
        check(fmt::importRecordSourcePath(r, "C:/Proj") == "C:/Proj/Source/My Hero.fbx", "a relative source joins the project folder");
        check(fmt::importRecordSourcePath(r, "C:/Proj/") == "C:/Proj/Source/My Hero.fbx", "and a trailing slash is not doubled");
        r.source = "D:/Art/Hero.fbx";
        check(fmt::importRecordSourcePath(r, "C:/Proj") == "D:/Art/Hero.fbx", "a drive path is absolute");
        r.source = "/home/a/Hero.fbx";
        check(fmt::importRecordSourcePath(r, "/proj") == "/home/a/Hero.fbx", "a rooted path is absolute");
        check(fmt::importRecordPathFor("C:/P/Content/Meshes/", "Hero") == "C:/P/Content/Meshes/Hero.ocimport", "the record path");
    }

    AVER_INFO("=== outputs ===");
    {
        fmt::ImportRecord r;
        fmt::parseImportRecord(kSample, r, nullptr);
        check(fmt::importRecordHasOutput(r, "Hero.ocmesh"), "an output is found");
        check(fmt::importRecordHasOutput(r, "hero.OCMESH"), "case-insensitively");
        check(!fmt::importRecordHasOutput(r, "Other.ocmesh"), "another file is not");
        r.outputs.push_back({"material", "..\\..\\Materials\\Mat.ocmat"});
        check(fmt::importRecordHasOutput(r, "../../Materials/Mat.ocmat"), "either slash matches");
    }

    AVER_INFO("=== malformed records are refused with a reason ===");
    {
        refused("", "empty", "an empty record");
        refused("FORMAT fbx\nSOURCE a.fbx\n", "OCIMPORT", "no header");
        refused("OCIMPORT\nFORMAT fbx\nSOURCE a.fbx\n", "version", "a header with no version");
        refused("OCIMPORT x\nFORMAT fbx\nSOURCE a.fbx\n", "version", "a version that is not a number");
        refused("OCIMPORT 99\nFORMAT fbx\nSOURCE a.fbx\n", "newer", "a record newer than this reader");
        refused("OCIMPORT 1\nSOURCE a.fbx\n", "FORMAT", "no FORMAT");
        refused("OCIMPORT 1\nFORMAT fbx\n", "SOURCE", "no SOURCE");
        refused("OCIMPORT 1\nFORMAT fbx\nSOURCE a.fbx\nSOURCESIZE big\n", "line 4", "a SOURCESIZE that is not a number names its line");
        refused("OCIMPORT 1\nFORMAT fbx\nSOURCE a.fbx\nSOURCEMTIME 12x\n", "SOURCEMTIME", "a SOURCEMTIME that is not a number");
        refused("OCIMPORT 1\nFORMAT fbx\nSOURCE a.fbx\nOUTPUT mesh\n", "OUTPUT", "an OUTPUT with no file");
    }

    AVER_INFO("=== files ===");
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path(ec) / "aver_importrecord_test";
        fs::create_directories(dir, ec);
        const std::string path = fmt::importRecordPathFor(dir.string(), "Hero");
        fmt::ImportRecord r;
        fmt::parseImportRecord(kSample, r, nullptr);
        std::string why;
        check(fmt::saveImportRecord(path, r, &why), "a record saves");
        fmt::ImportRecord back;
        check(fmt::loadImportRecord(path, back, &why) && back == r, "and loads back equal");
        check(!fmt::loadImportRecord((dir / "missing.ocimport").string(), back, &why) && !why.empty(), "a missing file is refused with a reason");
        fs::remove_all(dir, ec);
    }

    AVER_INFO(g_failures == 0 ? "ImportRecordTest: PASS" : "ImportRecordTest: FAIL");
    return g_failures == 0 ? 0 : 1;
}
