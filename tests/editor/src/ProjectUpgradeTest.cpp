// inspectProject/applyProjectUpgrade on script references to an extension's assembly.
//
// Projects converted before an extension's managed half was installed as an extension reference it as a C# project inside the
// engine tree, which no longer exists; a project whose engine moved has a HintPath to the old Extensions folder. Both must come out
// of the upgrade referencing the installed assembly, and the manifest must say which extension the scripts need.
#include "ProjectScaffold.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/platform/FileSystem.hpp"

#include <filesystem>
#include <string>
#include <vector>

using namespace aver;
namespace fs = std::filesystem;

namespace {
int gChecks = 0, gFailed = 0;

void check(bool ok, const std::string& what) {
    ++gChecks;
    if (ok) { AVER_INFO("  ok    {}", what); return; }
    ++gFailed;
    AVER_ERROR("  FAIL  {}", what);
}

// A current-layout project whose Scripts.csproj holds `refs` (and names every engine assembly, so only the extension references are
// in question). Returns the manifest path.
std::string makeProject(const fs::path& root, const std::string& name, const std::string& manifestExtra, const std::string& refs) {
    std::error_code ec;
    for (const char* d : {"Maps", "Meshes", "Materials", "Textures", "Sounds", "Scripts"})
        fs::create_directories(root / name / "Content" / d, ec);
    const std::string manifest = (root / name / (name + ".ocproject")).string();
    writeFileText(manifest, "OCPROJECT 1\nNAME " + name + "\nCONTENT Content\n# kept comment\n" + manifestExtra);
    writeFileText((root / name / "Content" / "Scripts" / "Player.cs").string(), "class Player {}\n");
    writeFileText((root / name / "Content" / "Scripts" / "Scripts.csproj").string(),
                  "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
                  "  <!-- Aver.Scripting.csproj Aver.Framework.csproj Aver.UI.csproj Aver.Materials.csproj -->\n"
                  "  <ItemGroup>\n" + refs + "    <Compile Include=\"..\\Materials\\**\\*.cs\" />\n  </ItemGroup>\n"
                  "</Project>\n");
    return manifest;
}

fmt::ProjectDesc load(const std::string& manifest) {
    fmt::ProjectDesc d;
    std::string err;
    check(fmt::loadOcproject(manifest, d, &err), "loads " + fs::path(manifest).filename().string() + " " + err);
    return d;
}

usize count(const editor::ProjectUpgrade& up, editor::ProjectFix::Kind k) {
    usize n = 0;
    for (const editor::ProjectFix& f : up.fixes) n += f.kind == k ? 1 : 0;
    return n;
}

std::string csprojOf(const fmt::ProjectDesc& d) {
    std::string xml;
    readFileText(editor::scriptsCsprojPath(d), xml);
    return xml;
}

// The HintPath in `xml`, resolved against the Scripts folder.
fs::path resolvedHint(const fmt::ProjectDesc& d, const std::string& xml) {
    const usize a = xml.find("<HintPath>");
    const usize b = xml.find("</HintPath>");
    if (a == std::string::npos || b == std::string::npos) return {};
    std::string hint = xml.substr(a + 10, b - a - 10);
    for (char& c : hint) if (c == '\\') c = '/';
    return (fs::path(d.scriptsDir()) / hint).lexically_normal();
}
} // namespace

int main(int argc, char** argv) {
    std::error_code ec;
    // `ProjectUpgradeTest --project <manifest>` upgrades one real project against the extensions installed beside this executable and
    // prints each fix; not a registered test.
    if (argc == 3 && std::string(argv[1]) == "--project") {
        const fmt::ProjectDesc d = load(argv[2]);
        const editor::ProjectUpgrade up = editor::inspectProject(d);
        for (const editor::ProjectFix& f : up.fixes) AVER_INFO("  fix   {}: {}", f.summary, f.detail);
        std::string err;
        const bool ok = editor::applyProjectUpgrade(d, up, &err);
        AVER_INFO("{} fix(es) {}{}", up.fixes.size(), ok ? "applied" : "failed: ", err);
        return ok && gFailed == 0 ? 0 : 1;
    }
    const fs::path root = fs::temp_directory_path(ec) / "aver-projectupgrade-test";
    fs::remove_all(root, ec);
    fs::create_directories(root / "Engine" / "Extensions" / "Ext" / "managed", ec);
    const fs::path dll = root / "Engine" / "Extensions" / "Ext" / "managed" / "Ext.Runtime.dll";
    writeFileText(dll.string(), "not-an-assembly");
    const std::vector<editor::ExtensionAssembly> installed = {{"Ext", "1.2.0", "Ext.Runtime", dll.string()}};
    using K = editor::ProjectFix::Kind;

    AVER_INFO("=== a dead ProjectReference to the extension's old C# project ===");
    {
        const fmt::ProjectDesc d = load(makeProject(root, "Old", "",
            "    <ProjectReference Include=\"..\\..\\..\\EngineGone\\scripting\\csharp\\Ext.Runtime\\Ext.Runtime.csproj\" />\n"));
        const editor::ProjectUpgrade up = editor::inspectProject(d, installed);
        check(count(up, K::UseExtensionAssembly) == 1, "it is offered as the installed extension's assembly");
        check(count(up, K::AddExtension) == 1, "the missing EXTENSION line is offered");
        check(count(up, K::RepointReference) == 0, "and not as a reference to edit by hand");
        check(up.fixes.size() == 2, "nothing else is offered (" + std::to_string(up.fixes.size()) + ")");

        std::string err;
        check(editor::applyProjectUpgrade(d, up, &err), "the upgrade applies " + err);
        const std::string xml = csprojOf(d);
        check(xml.find("Ext.Runtime.csproj") == std::string::npos, "the dead ProjectReference is gone");
        check(xml.find("<Reference Include=\"Ext.Runtime\">") != std::string::npos, "a Reference to the assembly is added");
        check(xml.find("<Private>false</Private>") != std::string::npos, "without a private copy beside the scripts");
        check(fs::equivalent(resolvedHint(d, xml), dll, ec), "its HintPath reaches the installed assembly");
        check(xml.find("..\\Materials\\**\\*.cs") != std::string::npos, "the rest of the file is kept");

        const fmt::ProjectDesc after = load(d.manifestPath);
        check(after.extensions.size() == 1 && after.extensions[0].name == "Ext" && after.extensions[0].version == "1.2.0",
              "the manifest now has EXTENSION Ext 1.2.0");
        std::string text;
        readFileText(d.manifestPath, text);
        check(text.find("# kept comment") != std::string::npos, "the manifest's other lines are kept");
        check(editor::inspectProject(after, installed).empty(), "the upgraded project is current");
    }

    AVER_INFO("=== a HintPath into a moved engine's Extensions folder ===");
    {
        const fmt::ProjectDesc d = load(makeProject(root, "Moved", "EXTENSION Ext 1.0.0\n",
            "    <Reference Include=\"Ext.Runtime\">\n      <HintPath>..\\..\\..\\OldEngine\\Extensions\\Ext\\managed\\Ext.Runtime.dll</HintPath>\n"
            "      <Private>false</Private>\n    </Reference>\n"));
        const editor::ProjectUpgrade up = editor::inspectProject(d, installed);
        check(count(up, K::RepointReference) == 1 && up.fixes.size() == 1, "only the HintPath is offered for repointing");
        std::string err;
        check(editor::applyProjectUpgrade(d, up, &err), "the upgrade applies " + err);
        check(fs::equivalent(resolvedHint(d, csprojOf(d)), dll, ec), "the HintPath reaches the installed assembly");
        const fmt::ProjectDesc after = load(d.manifestPath);
        check(after.extensions.size() == 1 && after.extensions[0].version == "1.0.0", "the existing EXTENSION line is left alone");
        check(editor::inspectProject(after, installed).empty(), "the upgraded project is current");
    }

    AVER_INFO("=== a good HintPath, no EXTENSION line ===");
    {
        const std::string hint = (root / "Engine" / "Extensions" / "Ext" / "managed" / "Ext.Runtime.dll").string();
        const fmt::ProjectDesc d = load(makeProject(root, "Unrecorded", "",
            "    <Reference Include=\"Ext.Runtime\">\n      <HintPath>" + hint + "</HintPath>\n    </Reference>\n"));
        const editor::ProjectUpgrade up = editor::inspectProject(d, installed);
        check(count(up, K::AddExtension) == 1 && up.fixes.size() == 1, "only the EXTENSION line is offered");
    }

    AVER_INFO("=== references the extensions do not explain ===");
    {
        const fmt::ProjectDesc d = load(makeProject(root, "Other", "",
            "    <ProjectReference Include=\"..\\Gone\\Something.csproj\" />\n"));
        const editor::ProjectUpgrade up = editor::inspectProject(d, installed);
        check(count(up, K::RepointReference) == 1 && count(up, K::AddExtension) == 0,
              "an unrelated dead reference is still left to edit by hand, with no EXTENSION line");

        const fmt::ProjectDesc fresh = load(makeProject(root, "NoExt", "",
            "    <ProjectReference Include=\"..\\..\\..\\EngineGone\\Ext.Runtime\\Ext.Runtime.csproj\" />\n"));
        const editor::ProjectUpgrade none = editor::inspectProject(fresh, {});
        check(count(none, K::UseExtensionAssembly) == 0 && count(none, K::RepointReference) == 1,
              "with the extension not installed, the old reference is reported as before");
    }

    fs::remove_all(root, ec);
    AVER_INFO("ProjectUpgradeTest: {} checks, {} failed", gChecks, gFailed);
    return gFailed == 0 ? 0 : 1;
}
