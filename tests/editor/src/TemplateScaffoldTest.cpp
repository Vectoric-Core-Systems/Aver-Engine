// listTemplatesIn() / listTemplates() / scaffoldProjectFromTemplate(): the New Project template
// picker's machinery.
//
// THREE THINGS HAVE TO BE TRUE FOR THE PICKER TO BE SAFE: a missing, empty or malformed templates
// root must all degrade to "found nothing" rather than an error (so Blank keeps working no matter
// what state templates\ is in); a template's own Content must survive scaffolding BYTE-FOR-BYTE,
// with nothing inside it ever text-substituted (a graph's CLASS name is the template's design, not
// the project's name); and the ONLY thing genuinely name-bearing is the fresh manifest this writes.
#include "ProjectScaffold.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <filesystem>
#include <string>

using namespace aver;

namespace {
int gChecks = 0, gFailed = 0;

// Returns what it was given, so a check that GATES further checks can be written as
// `if (check(...)) { ... }` -- asserting on a file that was never written produces a cascade of
// failures pointing at the wrong thing.
bool check(bool ok, const std::string& what) {
    ++gChecks;
    if (ok) { AVER_INFO("  ok    {}", what); return true; }
    ++gFailed;
    AVER_ERROR("  FAIL  {}", what);
    return false;
}

// Writes one <id>.octemplate manifest under `root`, in the exact shape a real template ships.
void makeTemplate(const std::filesystem::path& root, const std::string& id, const std::string& body) {
    std::error_code ec;
    std::filesystem::create_directories(root / id, ec);
    writeFileText((root / id / (id + ".octemplate")).string(), body);
}

// A minimal but REAL Content tree: a map and a "graph" file deep enough to prove the copy is
// recursive, carrying a CLASS line the way a real .ocgraph would -- this is the line
// scaffoldProjectFromTemplate() must never touch.
void makeTemplateContent(const std::filesystem::path& templateDir) {
    std::error_code ec;
    std::filesystem::create_directories(templateDir / "Content" / "Maps", ec);
    std::filesystem::create_directories(templateDir / "Content" / "Scripts", ec);
    writeFileText((templateDir / "Content" / "Maps" / "Default.ocmap").string(),
                  "OCMAP 1\nNAME Range\n");
    writeFileText((templateDir / "Content" / "Scripts" / "AN_Player.ocgraph").string(),
                  "OCGRAPH 1\nCLASS AN_Player Character\nNAME Player\n");
}
} // namespace

int main() {
    std::error_code ec;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path(ec) / "aver-templatescaffold-test";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    // ---- a missing, empty or malformed root must all report zero templates, not an error ----
    AVER_INFO("=== degrading gracefully ===");
    {
        const std::string missing = (root / "NoSuchDirectory").string();
        check(editor::listTemplatesIn(missing).empty(), "a MISSING root lists no templates");
    }
    {
        const std::string empty = (root / "EmptyRoot").string();
        std::filesystem::create_directories(empty, ec);
        check(editor::listTemplatesIn(empty).empty(), "an EMPTY root (exists, no subfolders) lists no templates");
    }
    {
        const std::filesystem::path malformedRoot = root / "MalformedRoot";
        // A subfolder with no manifest at all -- just a stray directory, e.g. a leftover ".git".
        std::filesystem::create_directories(malformedRoot / "NotATemplate", ec);
        // A subfolder whose manifest exists but has no TEMPLATE header.
        makeTemplate(malformedRoot, "NoHeader", "NAME Broken\nDESCRIPTION missing its header\n");
        // A subfolder whose manifest has a header but no NAME.
        makeTemplate(malformedRoot, "NoName", "TEMPLATE 1\nDESCRIPTION missing its name\n");
        const auto found = editor::listTemplatesIn(malformedRoot.string());
        check(found.empty(), "a root with ONLY malformed entries lists no templates (logged, not failed)");
    }
    {
        // Mixed: one good template beside the malformed ones above -- the bad entries must not take
        // the good one down with them.
        const std::filesystem::path mixedRoot = root / "MixedRoot";
        makeTemplate(mixedRoot, "NoHeader", "NAME Broken\n");
        makeTemplate(mixedRoot, "Good", "TEMPLATE 1\nNAME Good One\nDESCRIPTION a real template\n");
        const auto found = editor::listTemplatesIn(mixedRoot.string());
        check(found.size() == 1, "one malformed sibling does not hide a valid template");
        check(!found.empty() && found[0].id == "Good", "...and the valid one is the one that survives");
    }

    // ---- parsing: every field, and STARTMAP's default ----
    AVER_INFO("=== parsing every field ===");
    {
        const std::filesystem::path parseRoot = root / "ParseRoot";
        makeTemplate(parseRoot, "Full",
                     "TEMPLATE 1\n"
                     "# a comment line, which must not be mistaken for a key\n"
                     "NAME Full Template\n"
                     "DESCRIPTION Every field is set. # trailing comment too\n"
                     "PREVIEW preview.png\n"
                     "STARTMAP Maps/Arena.ocmap\n");
        makeTemplate(parseRoot, "Sparse", "TEMPLATE 1\nNAME Sparse Template\n");

        const auto found = editor::listTemplatesIn(parseRoot.string());
        check(found.size() == 2, "both templates in the parse root are found");

        const editor::TemplateInfo* full = nullptr;
        const editor::TemplateInfo* sparse = nullptr;
        for (const auto& t : found) {
            if (t.id == "Full") full = &t;
            if (t.id == "Sparse") sparse = &t;
        }
        check(full && full->name == "Full Template", "NAME parses");
        check(full && full->description == "Every field is set.", "DESCRIPTION parses, comment truncated");
        check(full && full->startMap == "Maps/Arena.ocmap", "STARTMAP parses when given");
        check(full && !full->previewPath.empty() &&
              std::filesystem::path(full->previewPath).filename() == "preview.png",
              "PREVIEW resolves to an absolute path beside the manifest");

        check(sparse && sparse->name == "Sparse Template", "a sparse manifest still parses its NAME");
        check(sparse && sparse->description.empty(), "...DESCRIPTION defaults to empty");
        check(sparse && sparse->startMap == "Maps/Default.ocmap", "...STARTMAP defaults to Maps/Default.ocmap");
        check(sparse && sparse->previewPath.empty(), "...PREVIEW defaults to no image (picker falls back)");
    }

    // ---- scaffolding: the Content tree survives verbatim, only the manifest is fresh ----
    AVER_INFO("=== scaffolding from a template ===");
    {
        const std::filesystem::path tmplRoot = root / "ScaffoldSrc" / "Demo";
        std::filesystem::create_directories(tmplRoot, ec);
        writeFileText((tmplRoot / "Demo.octemplate").string(),
                      "TEMPLATE 1\nNAME Demo\nDESCRIPTION a demo template\n");
        makeTemplateContent(tmplRoot);

        editor::TemplateInfo tmpl;
        tmpl.dir = tmplRoot.string();
        tmpl.id = "Demo";
        tmpl.name = "Demo";
        tmpl.startMap = "Maps/Default.ocmap";

        const std::filesystem::path dstLoc = root / "Projects";
        std::filesystem::create_directories(dstLoc, ec);

        fmt::ProjectDesc out;
        std::string err;
        const bool ok = editor::scaffoldProjectFromTemplate(dstLoc.string(), "Arena", tmpl, out, &err);
        check(ok, "scaffolding from a template succeeds: " + err);
        check(out.name == "Arena", "the manifest's NAME is the PROJECT name, not the template's");

        const std::filesystem::path projectRoot = dstLoc / "Arena";
        std::string mapText, graphText;
        readFileText((projectRoot / "Content" / "Maps" / "Default.ocmap").string(), mapText);
        readFileText((projectRoot / "Content" / "Scripts" / "AN_Player.ocgraph").string(), graphText);
        check(mapText == "OCMAP 1\nNAME Range\n", "the copied map is BYTE-IDENTICAL to the template's");
        check(graphText == "OCGRAPH 1\nCLASS AN_Player Character\nNAME Player\n",
              "the copied graph's CLASS AN_Player and NAME Player survive VERBATIM -- "
              "not rewritten to the project's name");

        std::string manifestText;
        readFileText(out.manifestPath, manifestText);
        check(manifestText.find("NAME Arena") != std::string::npos,
              "the written manifest names the project");
        check(manifestText.find("STARTMAP Maps/Default.ocmap") != std::string::npos,
              "...and carries the template's STARTMAP");
        check(!std::filesystem::exists(projectRoot / "Demo.octemplate", ec),
              "the template's OWN manifest is never copied into the project");

        AVER_INFO("=== refusing an existing folder, exactly like blank's scaffoldProject() ===");
        fmt::ProjectDesc again;
        err.clear();
        const bool second = editor::scaffoldProjectFromTemplate(dstLoc.string(), "Arena", tmpl, again, &err);
        check(!second, "a second scaffold at the same location refuses");
        check(!err.empty(), "...and says why");
        std::string mapTextAfter;
        readFileText((projectRoot / "Content" / "Maps" / "Default.ocmap").string(), mapTextAfter);
        check(mapText == mapTextAfter, "...and the refusal did not touch the existing project");

        AVER_INFO("=== a template manifest with no Content refuses cleanly, not a crash ===");
        const std::filesystem::path emptyTmplDir = root / "ScaffoldSrc" / "NoContent";
        std::filesystem::create_directories(emptyTmplDir, ec);
        writeFileText((emptyTmplDir / "NoContent.octemplate").string(), "TEMPLATE 1\nNAME No Content\n");
        editor::TemplateInfo noContent;
        noContent.dir = emptyTmplDir.string();
        noContent.id = "NoContent";
        noContent.name = "No Content";
        noContent.startMap = "Maps/Default.ocmap";
        fmt::ProjectDesc missingOut;
        err.clear();
        const bool third =
            editor::scaffoldProjectFromTemplate(dstLoc.string(), "Empty", noContent, missingOut, &err);
        check(!third, "a template with no Content\\ folder refuses rather than writing a broken project");
        check(!err.empty(), "...and says why");
        check(!std::filesystem::exists(dstLoc / "Empty", ec),
              "...and leaves no half-written folder behind");
    }

#ifdef AVER_REPO_ROOT
    // ---- the real, shipped templates\ directory ----
    //
    // THE ENGINE SHIPS A TEMPLATE AGAIN, so this asserts it is found rather than that the directory is
    // absent. It briefly asserted the opposite, in the window where templates/ had been removed as
    // project-content-in-the-engine and the replacement had not yet been promoted; leaving that
    // assertion in place now would be a test pinning down the precise opposite of what ships, which is
    // worse than no test at all because it passes.
    AVER_INFO("=== the real templates/ directory this repo ships ===");
    {
        const std::string realRoot = std::string(AVER_REPO_ROOT) + "/templates";
        const auto found = editor::listTemplatesIn(realRoot);
        bool sawFirstPerson = false;
        for (const auto& t : found) if (t.id == "FirstPerson") sawFirstPerson = true;
        check(sawFirstPerson, "templates/FirstPerson/FirstPerson.octemplate is found and parses");

        // This test binary's own executableDir() sits in the same build\bin as Sandbox.exe, so
        // listTemplates()'s real executableDir()-walk should find the SAME templates\ directory a
        // shipped editor would, from the identical staged layout (bin\ and templates\ as siblings).
        // That layout is not automatic -- it is what scripts/payload.allowlist's templates/** line
        // buys, and staging it anywhere else would leave it on disk and invisible here.
        const auto walked = editor::listTemplates();
        bool walkedSawFirstPerson = false;
        for (const auto& t : walked) if (t.id == "FirstPerson") walkedSawFirstPerson = true;
        check(walkedSawFirstPerson,
              "listTemplates()'s executableDir()-walk ALSO finds it -- the shipped-editor discovery path");
    }
#endif

    // ---- a project name is a FOLDER name, and the generators paste it after `namespace ` --------
    //
    // validateProjectName deliberately allows spaces, hyphens and dots, because it is naming a
    // directory. Every .cs generator then pasted that name straight into a namespace declaration,
    // so a project called "My Game" emitted `namespace My Game;` into its starter material, its
    // actor scripts and its behaviours -- and the whole Scripts assembly failed at the first
    // Compile C#, pointing at files nobody had written by hand.
    AVER_INFO("=== a project name becomes a LEGAL C# namespace ===");
    {
        using aver::editor::csharpNamespaceFor;
        using aver::editor::validateProjectName;

        // The names below must be ones validateProjectName ACCEPTS -- a sanitiser for names the
        // editor already refuses would prove nothing.
        std::string why;
        check(validateProjectName("My Game", &why), "'My Game' is a name the editor accepts: " + why);
        check(csharpNamespaceFor("My Game") == "My_Game",
              "and it becomes My_Game, got '" + csharpNamespaceFor("My Game") + "'");

        check(validateProjectName("Sky-Forge v2.1", &why), "'Sky-Forge v2.1' is accepted too: " + why);
        check(csharpNamespaceFor("Sky-Forge v2.1") == "Sky_Forge_v2_1",
              "hyphens and dots go too, got '" + csharpNamespaceFor("Sky-Forge v2.1") + "'");

        check(csharpNamespaceFor("2Fast") == "_2Fast",
              "a leading digit is prefixed rather than left illegal, got '" +
                  csharpNamespaceFor("2Fast") + "'");
        check(csharpNamespaceFor("Plain") == "Plain", "an already-legal name is untouched");
        check(csharpNamespaceFor("") == "Game", "and nothing at all falls back rather than emitting `namespace ;`");
        check(csharpNamespaceFor("...") == "Game", "as does a name with no legal character in it");

        // THE POINT OF THE WHOLE EXERCISE, asserted end to end on a really-scaffolded project
        // rather than on the generator in isolation: scaffold "My Game" and read what landed on
        // disk. A unit test of the sanitiser alone would still pass if a generator forgot to call
        // it, which is exactly the mistake being fixed.
        const std::filesystem::path nsRoot = root / "nsproj";
        std::filesystem::create_directories(nsRoot, ec);
        fmt::ProjectDesc made;
        std::string serr;
        if (check(aver::editor::scaffoldProject(nsRoot.string(), "My Game", made, &serr),
                  "a project named 'My Game' scaffolds: " + serr)) {
            const std::filesystem::path surfaces =
                nsRoot / "My Game" / "Content" / "Materials" / "Surfaces.cs";
            std::string text;
            if (check(aver::readFileText(surfaces.string(), text),
                      "its starter material was written")) {
                check(text.find("namespace My_Game.Materials;") != std::string::npos,
                      "and declares the sanitised namespace");
                check(text.find("namespace My Game") == std::string::npos,
                      "never the raw one -- that is the line that would not compile");
            }
        }
    }

    std::filesystem::remove_all(root, ec);
    if (gFailed) { AVER_ERROR("=== {} of {} checks FAILED ===", gFailed, gChecks); return 1; }
    AVER_INFO("=== all {} template-scaffold checks passed ===", gChecks);
    return 0;
}
