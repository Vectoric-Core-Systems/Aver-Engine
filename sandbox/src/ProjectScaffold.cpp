#include "ProjectScaffold.hpp"

#include "aver/platform/FileSystem.hpp"
#include "aver/core/Version.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <filesystem>

namespace aver::editor {
namespace {

constexpr const char* kCsprojName = "Scripts.csproj";

// Relative MSBuild path from a project's Scripts folder to an engine C# project, or empty if the
// engine tree could not be located from the executable.
//
// A project is a SIBLING of the engine, never inside it, so the reference has to reach back out of
// the projects root. The engine root is found by walking up from the executable rather than assumed
// to be two levels above bin/, because that only holds for the default build layout. `tail` is the
// project file's path under the engine root.
std::string engineProjectReference(const std::string& scriptsDir, const std::filesystem::path& tail) {
    std::error_code ec;
    std::filesystem::path probe = std::filesystem::path(executableDir());
    for (int up = 0; up < 8 && !probe.empty(); ++up) {
        const std::filesystem::path candidate = probe / tail;
        if (std::filesystem::exists(candidate, ec)) {
            const std::filesystem::path rel = std::filesystem::relative(candidate, scriptsDir, ec);
            std::string s = ec || rel.empty() ? candidate.string() : rel.string();
            std::replace(s.begin(), s.end(), '/', '\\'); // MSBuild convention
            return s;
        }
        if (!probe.has_parent_path() || probe.parent_path() == probe) break;
        probe = probe.parent_path();
    }
    return {};
}

std::string scriptingProjectReference(const std::string& scriptsDir) {
    return engineProjectReference(scriptsDir, "scripting/csharp/Aver.Scripting/Aver.Scripting.csproj");
}

// Aver.Framework carries the actor types (and Aver.Scene transitively). Referenced alongside
// Aver.Scripting so a project can hold both behaviours and actors without the author touching XML.
std::string frameworkProjectReference(const std::string& scriptsDir) {
    return engineProjectReference(scriptsDir, "scripting/csharp/Aver.Framework/Aver.Framework.csproj");
}

// Aver.UI is the game's HUD: layers, colours, rectangles. A leaf that references nothing.
std::string uiProjectReference(const std::string& scriptsDir) {
    return engineProjectReference(scriptsDir, "scripting/csharp/Aver.UI/Aver.UI.csproj");
}

// Aver.Materials is the material authoring surface. Referenced by default even though a new project
// has no materials yet, for the same reason Aver.Framework is: the alternative is an author who adds
// their first material and gets a compile error naming an assembly they have never heard of.
std::string materialsProjectReference(const std::string& scriptsDir) {
    return engineProjectReference(scriptsDir, "scripting/csharp/Aver.Materials/Aver.Materials.csproj");
}

std::string manifestText(const std::string& name) {
    std::string s;
    s += "OCPROJECT 1\n";
    s += "# Created by the Aver Engine editor. This project lives OUTSIDE the engine tree and\n";
    s += "# references it; see the engine's docs/PROJECTS.md.\n";
    s += "NAME " + name + "\n";
    s += "ENGINE " + std::string(kEngineName) + " " + std::string(kEngineVersion) + "\n";
    s += "CONTENT Content\n";
    // The editor cannot author or save a map yet (docs/STATUS.md 4d), so this names where the
    // start map WILL go rather than a file that exists. Loading is tolerant of the gap.
    s += "STARTMAP Maps/Default.ocmap\n";
    // Left commented rather than written empty: the editor has no author field to fill it from,
    // and `AUTHOR` with nothing after it reads as a value someone deleted.
    s += "# AUTHOR <your name>\n";
    return s;
}

// The four engine assemblies a project compiles against, in the order they are written.
//
// All four by default, and none of them optional, because the failure mode of "reference it when you
// need it" is an author who writes their first HUD or their first material and gets a compile error
// naming an assembly they have never heard of, in a file the editor generated and told them not to
// worry about. The cost of an unused reference is nothing: an assembly nothing calls is not loaded.
struct EngineRefs {
    std::string scripting;   // AverBehaviour
    std::string framework;   // Actor / Pawn / GameMode, and Aver.Scene behind it
    std::string ui;          // the game's HUD
    std::string materials;   // [AverMaterial] surfaces
    bool any() const { return !scripting.empty() || !framework.empty() || !ui.empty() || !materials.empty(); }
};

EngineRefs engineRefs(const std::string& scriptsDir) {
    return EngineRefs{
        scriptingProjectReference(scriptsDir),
        frameworkProjectReference(scriptsDir),
        uiProjectReference(scriptsDir),
        materialsProjectReference(scriptsDir),
    };
}

// A working starter surface for a new project.
//
// It exists for the reason the actor templates do: the shape of a material is not guessable from an
// empty folder, and "author it in C#" is a sentence somebody has to be shown once. Deliberately
// COMPLETE and deliberately plain -- no textures, because a new project has none, and a material
// naming files that do not exist would greet its author with four resolve warnings.
std::string starterMaterialText(const std::string& projectName) {
    std::string s;
    s += "// " + projectName + "'s surfaces.\n";
    s += "//\n";
    s += "// These .cs files are the SOURCE of every material. Compile C# runs each Configure and\n";
    s += "// writes an .ocmat into Binaries\\Materials, which is what the engine loads -- so the\n";
    s += "// generated file is a build artefact and editing it is editing something that will be\n";
    s += "// overwritten. Change this instead.\n";
    s += "//\n";
    s += "// The Details panel's \"Save to C#\" writes back here, so tuning a surface by dragging a\n";
    s += "// slider and tuning it by editing this file are the same edit.\n";
    s += "using Aver.Materials;\n\n";
    s += "namespace " + projectName + ".Materials;\n\n";
    s += "/// <summary>A plain mid-grey surface. Bind it by name from a mesh: \"M_Default\".</summary>\n";
    s += "[AverMaterial(\"M_Default\")]\n";
    s += "public sealed class Default : Material\n";
    s += "{\n";
    s += "    public static void Configure(MaterialBuilder b) => b\n";
    s += "        .Comment(\"The default surface: mid-grey, slightly rough, not metal.\")\n";
    s += "        .BaseColor(0.55f, 0.55f, 0.57f)\n";
    s += "        .Metallic(0f)\n";
    s += "        .Roughness(0.6f);\n";
    s += "}\n";
    return s;
}

std::string csprojText(const EngineRefs& refs) {
    std::string s;
    s += "<Project Sdk=\"Microsoft.NET.Sdk\">\n\n";
    s += "  <!-- Generated by the Aver Engine editor (Tools > New C# Script / New C# Class).\n";
    s += "       A class library, not an exe: the editor hosts the CLR in its own process and loads\n";
    s += "       this assembly into a collectible load context, so there is no entry point to run.\n";
    s += "       Tools > Compile Scripts builds exactly this file, into <project>\\Binaries\\Scripts,\n";
    s += "       and Tools > Reload Scripts rebuilds and swaps it in without restarting the editor.\n";
    s += "\n";
    s += "       The output directory is passed on the `dotnet build` command line rather than set\n";
    s += "       here: the editor has to know it too, and one of the two would drift.\n";
    s += "\n";
    s += "       Four references: Aver.Scripting for behaviours, Aver.Framework for actors (which\n";
    s += "       pulls Aver.Scene in behind it), Aver.UI for the game's HUD, and Aver.Materials for\n";
    // NO DOUBLE HYPHEN ANYWHERE BELOW. XML forbids `--` inside a comment outright, and the house
    // style uses it as an em dash in every other generated file, so it reads as correct and produces
    // an MSB4025 that names a column rather than a cause. This cost one build to find.
    s += "       surfaces authored in C#. All four by default, because an unused reference costs\n";
    s += "       nothing and the alternative is a compile error naming an assembly the author has\n";
    s += "       never heard of, in a file the editor generated. -->\n";
    s += "  <PropertyGroup>\n";
    s += "    <TargetFramework>net10.0</TargetFramework>\n";
    s += "    <Nullable>enable</Nullable>\n";
    s += "    <ImplicitUsings>enable</ImplicitUsings>\n";
    s += "  </PropertyGroup>\n\n";
    if (!refs.any()) {
        s += "  <!-- The engine's C# projects could not be located from the editor's install path.\n";
        s += "       Point these at <engine>\\scripting\\csharp\\<name>. -->\n";
        s += "  <!-- <ItemGroup>\n";
        s += "    <ProjectReference Include=\"..\\..\\..\\..\\Aver Engine\\scripting\\csharp\\Aver.Scripting\\Aver.Scripting.csproj\" />\n";
        s += "    <ProjectReference Include=\"..\\..\\..\\..\\Aver Engine\\scripting\\csharp\\Aver.Framework\\Aver.Framework.csproj\" />\n";
        s += "    <ProjectReference Include=\"..\\..\\..\\..\\Aver Engine\\scripting\\csharp\\Aver.UI\\Aver.UI.csproj\" />\n";
        s += "    <ProjectReference Include=\"..\\..\\..\\..\\Aver Engine\\scripting\\csharp\\Aver.Materials\\Aver.Materials.csproj\" />\n";
        s += "  </ItemGroup> -->\n\n";
    } else {
        s += "  <ItemGroup>\n";
        for (const std::string* r : {&refs.scripting, &refs.framework, &refs.ui, &refs.materials})
            if (!r->empty()) s += "    <ProjectReference Include=\"" + *r + "\" />\n";
        s += "  </ItemGroup>\n\n";
    }
    // Materials live beside the content they describe rather than among the gameplay scripts, so
    // they are compiled in from there. ONE assembly rather than two: avermatc reflects over whatever
    // it is given, and a second .csproj would be a second build to keep in step for a separation
    // nobody asked for.
    //
    // Written even when Content\Materials is empty. A glob that matches nothing is not an error, and
    // adding it later is a file edit the author would have to be told about.
    s += "  <ItemGroup>\n";
    s += "    <!-- Surfaces authored in C#, under Content\\Materials. Compile C# runs avermatc over\n";
    s += "         the built assembly and writes the .ocmat files the engine loads into\n";
    s += "         <project>\\Binaries\\Materials. -->\n";
    s += "    <Compile Include=\"..\\Materials\\**\\*.cs\" />\n";
    s += "  </ItemGroup>\n\n";
    s += "</Project>\n";

    // Checked rather than trusted, because this exact mistake shipped once already: XML forbids `--`
    // inside a comment, the house style uses it as an em dash in every other generated file, and the
    // result is an MSB4025 that names a line and column in a file the author did not write and was
    // told not to worry about. Six lines here against a project that cannot build at all.
    {
        bool inComment = false;
        for (usize i = 0; i + 1 < s.size(); ++i) {
            if (!inComment && s.compare(i, 4, "<!--") == 0) { inComment = true; i += 3; continue; }
            if (inComment && s.compare(i, 3, "-->") == 0)   { inComment = false; i += 2; continue; }
            if (inComment && s[i] == '-' && s[i + 1] == '-') {
                AVER_ERROR("[Editor] the generated Scripts.csproj contains '--' inside an XML comment "
                           "at offset {}; MSBuild will refuse to load it", i);
                break;
            }
        }
    }
    return s;
}

// The actor-kind templates. Each derives the right base and carries the [AverClass]/[AverGameMode]
// attribute plus the starter overrides that make sense for that concept. They compile against
// Aver.Framework; the header comment is honest that they do not tick yet.
std::string actorScriptText(const std::string& projectName, const std::string& scriptName, CsKind kind) {
    const char* base =
        kind == CsKind::Pawn             ? "AverPawn" :
        kind == CsKind::PlayerController ? "AverPlayerController" :
        kind == CsKind::GameMode         ? "AverGameMode" :
        kind == CsKind::GameInstance     ? "AverGameInstance" :
                                           "AverActor";
    std::string s;
    s += "// " + scriptName + ".cs - a " + csKindNoun(kind) + " in the '" + projectName + "' project.\n";
    s += "//\n";
    s += "// IT COMPILES, BUT IT DOES NOT TICK YET. The actor runtime - the framework's per-frame\n";
    s += "// tick, its class registry and possession, and the native scene/framework ABI these hooks\n";
    s += "// call - is not built yet. Compile C# builds this green and the type is real, but its hooks\n";
    s += "// are not called and its transform reads/writes reach a native side that is still coming.\n";
    s += "// AverBehaviour scripts (Tools > New C# Script > AverBehaviour) DO run today.\n";
    s += "//\n";
    s += "// The base type is the contract: deriving " + std::string(base) + " is what the COMPILER checks, so a\n";
    s += "// misspelt override is a build error rather than a hook that silently never runs.\n";
    s += "\n";
    s += "using Aver.Framework;\n";
    s += "using Aver.Scene;\n";
    s += "using Aver.Scripting;   // Log\n";
    s += "\n";
    s += "namespace " + projectName + ";\n";
    s += "\n";
    if (kind == CsKind::GameMode) {
        // [AverGameMode] names the pawn and controller it hands out, by string, so they can live in
        // other assemblies. The defaults are "Pawn"/"PlayerController"; change them to your classes.
        s += "[AverGameMode(\"" + scriptName + "\")]\n";
    } else if (kind == CsKind::Actor) {
        s += "[AverClass(\"" + scriptName + "\")]\n";   // Parent defaults to "Actor"
    } else {
        const char* parent = kind == CsKind::Pawn             ? "Pawn"
                           : kind == CsKind::PlayerController  ? "PlayerController"
                                                               : "GameInstance";
        s += "[AverClass(\"" + scriptName + "\", Parent = \"" + std::string(parent) + "\")]\n";
    }
    s += "public sealed class " + scriptName + " : " + base + "\n";
    s += "{\n";
    switch (kind) {
        case CsKind::Actor:
            s += "    public override void OnBeginPlay(BeginReason reason)\n";
            s += "    {\n";
            s += "        Log.Info($\"[" + scriptName + "] begin play ({reason}) at {Self.LocalPosition}\");\n";
            s += "    }\n\n";
            s += "    public override void OnTick(float dt)\n";
            s += "    {\n";
            s += "        // Move and rotate through Self (Self.Translate, Self.SetLocalRotation), read\n";
            s += "        // Self.LocalPosition, or spawn - once the runtime lands.\n";
            s += "    }\n";
            break;
        case CsKind::Pawn:
            s += "    public override void OnBeginPlay(BeginReason reason) { }\n\n";
            s += "    public override void OnTick(float dt)\n";
            s += "    {\n";
            s += "        // Drive along Self.Forward, steer child models, and so on.\n";
            s += "    }\n\n";
            s += "    public override void OnPossessed(Entity controller)\n";
            s += "        => Log.Info($\"[" + scriptName + "] possessed by {controller.Name}\");\n";
            break;
        case CsKind::PlayerController:
            s += "    public override void OnTick(float dt)\n";
            s += "    {\n";
            s += "        // Read input and drive Possessed - the pawn this controller holds, or Entity.None.\n";
            s += "    }\n";
            break;
        case CsKind::GameMode:
            s += "    public override void OnPostLogin(Entity controller)\n";
            s += "    {\n";
            s += "        // A player joined: spawn their pawn and possess it here. The default pawn and\n";
            s += "        // controller classes are named on [AverGameMode] above.\n";
            s += "    }\n";
            break;
        case CsKind::GameInstance:
            s += "    // Process-wide state that must survive a level change. [Editable] persists it and\n";
            s += "    // shows it in Details; a plain field would reset on a hot reload.\n";
            s += "    [Editable] public int HighScore;\n";
            break;
        default: break;
    }
    s += "}\n";
    return s;
}

std::string scriptText(const std::string& projectName, const std::string& scriptName, CsKind kind) {
    if (csKindIsActor(kind)) return actorScriptText(projectName, scriptName, kind);
    const bool behaviour = kind == CsKind::Behaviour;
    std::string s;
    s += "// " + scriptName + ".cs - a " + (behaviour ? "script" : "class") + " in the '" + projectName + "' project.\n";
    s += "//\n";
    if (behaviour) {
        s += "// THIS RUNS. The editor hosts the .NET runtime inside its own process, so Tools >\n";
        s += "// Compile Scripts (or Reload Scripts) builds this file into <project>\\Binaries\\Scripts\n";
        s += "// and the engine loads it, constructs the class below and calls the hooks on the main\n";
        s += "// thread, in the frame loop. Log lines come out of the editor's own Output Log.\n";
        s += "//\n";
        s += "// It has to derive from AverBehaviour: that is how the engine finds it, and it is why\n";
        s += "// the hooks are `override` - a misspelt one will not compile rather than silently\n";
        s += "// never running. A public parameterless constructor is required; the engine builds it.\n";
        s += "//\n";
        s += "// WHAT A SCRIPT CAN REACH TODAY: Log, and the render modules' settings and this\n";
        s += "// machine's real GPU capabilities through Voxi and Pbr. Those resolve to the SAME\n";
        s += "// modules the editor has loaded, so a change here changes what the viewport draws.\n";
        s += "//\n";
        s += "// WHAT IT CANNOT REACH YET: the scene. There are no actor, transform, component,\n";
        s += "// input or asset APIs, because the generic scene layer does not exist yet (the\n";
        s += "// engine's docs/STATUS.md 9.1). An interim object model invented here would be the\n";
        s += "// throwaway ABI that design exists to avoid, so there is deliberately nothing.\n";
        s += "//\n";
        s += "// If a hook throws, the engine logs it and disables THIS behaviour for the session.\n";
        s += "// The editor stays up and every other script keeps running.\n";
    } else {
        s += "// A plain class - no lifecycle hooks, so the engine never calls it by itself. Use it\n";
        s += "// for data, helpers, and anything a behaviour script wants to lean on.\n";
        s += "//\n";
        s += "// It is compiled into the same assembly as this project's behaviours and loaded with\n";
        s += "// them, so a behaviour can use it the moment Tools > Reload Scripts has run. Only the\n";
        s += "// hooks are special; the rest of the assembly is ordinary C#.\n";
    }
    s += "\n";
    // Only where it is used: an unused `using` in a file that exists to be a starting point
    // teaches the wrong habit, and the plain class deliberately touches no engine API.
    if (behaviour) s += "using Aver.Scripting;\n\n";
    s += "namespace " + projectName + ";\n";
    s += "\n";
    s += "public sealed class " + scriptName + (behaviour ? " : AverBehaviour\n" : "\n");
    s += "{\n";
    if (behaviour) {
        s += "    private float _elapsed;\n";
        s += "\n";
        s += "    public override void OnStart()\n";
        s += "    {\n";
        s += "        // Log, not Console.WriteLine: the editor is a GUI process with no console\n";
        s += "        // attached, so anything written there goes where nobody will look.\n";
        s += "        Log.Info($\"[" + scriptName + "] started - ray tracing tier {Voxi.RayTracingTier}, max MSAA {Voxi.MaxMsaa}x\");\n";
        s += "    }\n";
        s += "\n";
        s += "    public override void OnUpdate(float deltaSeconds)\n";
        s += "    {\n";
        s += "        _elapsed += deltaSeconds;\n";
        s += "\n";
        s += "        // Ask before setting: a feature the GPU or the renderer cannot do reports its\n";
        s += "        // real status instead of pretending, and assigning it leaves the value at Off.\n";
        s += "        // This one is visible in the viewport on the very next frame.\n";
        s += "        if (_elapsed > 2.0f && Voxi.GlobalIllumination == VoxiQuality.Off\n";
        s += "            && Voxi.IsAvailable(VoxiFeature.GlobalIllumination))\n";
        s += "        {\n";
        s += "            Voxi.GlobalIllumination = VoxiQuality.High;\n";
        s += "            Log.Info($\"[" + scriptName + "] turned global illumination on from C#\");\n";
        s += "        }\n";
        s += "    }\n";
        s += "\n";
        s += "    public override void OnShutdown()\n";
        s += "    {\n";
        s += "        // Also called just before a reload, so this is where a script gives anything back.\n";
        s += "    }\n";
    } else {
        s += "    public string Name { get; init; } = \"" + scriptName + "\";\n";
    }
    s += "}\n";
    return s;
}

} // namespace

bool csKindIsScript(CsKind kind) { return kind != CsKind::PlainClass; }

bool csKindIsActor(CsKind kind) {
    switch (kind) {
        case CsKind::Actor:
        case CsKind::Pawn:
        case CsKind::PlayerController:
        case CsKind::GameMode:
        case CsKind::GameInstance:
            return true;
        default:
            return false;
    }
}

const char* csKindNoun(CsKind kind) {
    switch (kind) {
        case CsKind::Behaviour:        return "behaviour";
        case CsKind::Actor:            return "actor";
        case CsKind::Pawn:             return "pawn";
        case CsKind::PlayerController: return "player controller";
        case CsKind::GameMode:         return "game mode";
        case CsKind::GameInstance:     return "game instance";
        case CsKind::PlainClass:       return "class";
    }
    return "script";
}

bool validateProjectName(const std::string& name, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (name.empty()) return fail("Enter a project name.");
    if (name.size() > 64) return fail("Name is too long (64 characters max).");
    for (const char c : name) {
        if (c == '/' || c == '\\') return fail("A name cannot contain a path separator.");
        if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return fail("A name cannot contain : * ? \" < > or |");
        if (static_cast<unsigned char>(c) < 0x20) return fail("A name cannot contain control characters.");
    }
    if (name.front() == ' ' || name.back() == ' ' || name.back() == '.')
        return fail("A name cannot start or end with a space, or end with a dot.");
    return true;
}

bool validateTypeName(const std::string& name, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (name.empty()) return fail("Enter a name.");
    if (name.size() > 64) return fail("Name is too long (64 characters max).");
    const char f = name.front();
    if (!((f >= 'A' && f <= 'Z') || (f >= 'a' && f <= 'z') || f == '_'))
        return fail("A type name must start with a letter or underscore.");
    for (const char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return fail("Use letters, digits and underscores only - this becomes a class name.");
    }
    // `class class` does not compile, and the file would look fine until someone built it. The
    // list is the union of the C# and C++ keywords a generated `class <Name>` could collide with:
    // one validator serves all four items, so it has to refuse names either language rejects.
    static const char* kReserved[] = {"class", "struct", "int", "float", "double", "bool", "string",
                                      "void", "public", "private", "protected", "static", "namespace",
                                      "using", "new", "delete", "this", "base", "null", "nullptr",
                                      "true", "false", "object", "const", "auto", "template",
                                      "typename", "enum", "union", "operator", "virtual", "return",
                                      "if", "else", "for", "while", "switch", "case", "default",
                                      "char", "short", "long", "unsigned", "signed", "inline"};
    for (const char* r : kReserved)
        if (name == r) return fail("That is a language keyword and cannot be a class name.");
    return true;
}

bool writeNewFile(const std::string& path, const std::string& text, std::string* err) {
    if (fileExists(path)) {
        if (err) *err = path + " already exists.";
        return false;
    }
    if (!writeFileText(path, text)) {
        if (err) *err = "Could not write " + path;
        return false;
    }
    return true;
}

bool scaffoldProject(const std::string& location, const std::string& name,
                     fmt::ProjectDesc& out, std::string* err) {
    if (!validateProjectName(name, err)) return false;
    if (location.empty()) { if (err) *err = "Choose a location."; return false; }

    const std::string root = location + "\\" + name;
    if (fileExists(root)) {
        if (err) *err = "A folder already exists at " + root;
        return false;
    }

    const std::string content = root + "\\Content";
    // The layout docs/PROJECTS.md specifies; creating them up front is what makes the Content Browser
    // and Tools > New C# Script have somewhere to point at once.
    //
    // Materials, Textures and Sounds joined the list when each became a thing a project actually
    // holds. Materials in particular is not optional decoration: Scripts.csproj globs
    // ..\Materials\**\*.cs, and while MSBuild is happy with a glob that matches nothing, an author
    // told "put your material here" and finding no `here` will put it somewhere else.
    for (const std::string& d : {content + "\\Maps", content + "\\Meshes", content + "\\Materials",
                                 content + "\\Textures", content + "\\Sounds", content + "\\Scripts"}) {
        if (!createDirectories(d)) {
            if (err) *err = "Could not create " + d;
            return false;
        }
    }

    const std::string manifest = root + "\\" + name + ".ocproject";
    if (!writeFileText(manifest, manifestText(name))) {
        if (err) *err = "Could not write " + manifest;
        return false;
    }

    // The csproj is written HERE rather than lazily on the first script, which is where it used to
    // appear. A project without one cannot Compile C#, and Compile C# is what bakes materials -- so
    // a project whose first authored thing was a material had no way to build it, and the button
    // that would have told them so was disabled for want of the file it was about to create.
    // createScript still writes it if absent, so an older project is not left without one.
    {
        const std::string scriptsDir = content + "\\Scripts";
        const std::string csproj = scriptsDir + "\\" + kCsprojName;
        if (!writeFileText(csproj, csprojText(engineRefs(scriptsDir))))
            AVER_WARN("[Editor] project created, but could not write {}", csproj);
    }
    // A starter surface, for the same reason the actor templates exist: the shape of a material is
    // not guessable, and an empty Materials folder teaches nothing. It is a complete, working
    // material that the first Compile C# turns into an .ocmat.
    {
        const std::string starter = content + "\\Materials\\Surfaces.cs";
        if (!writeFileText(starter, starterMaterialText(name)))
            AVER_WARN("[Editor] project created, but could not write {}", starter);
    }

    if (!fmt::loadOcproject(manifest, out, err)) return false;
    AVER_INFO("[Editor] created project '{}' at {}", name, root);
    return true;
}

std::string scriptsCsprojPath(const fmt::ProjectDesc& proj) {
    const std::string dir = proj.scriptsDir();
    return dir.empty() ? std::string() : dir + "\\" + kCsprojName;
}

std::string scriptsBinaryDir(const fmt::ProjectDesc& proj) {
    return proj.dir.empty() ? std::string() : proj.dir + "\\Binaries\\Scripts";
}

bool createScript(const fmt::ProjectDesc& proj, const std::string& name, CsKind kind,
                  std::string* outPath, bool* outCsproj, std::string* err) {
    if (outCsproj) *outCsproj = false;
    if (!validateTypeName(name, err)) return false;
    if (!proj.valid()) { if (err) *err = "No project is loaded."; return false; }

    const std::string dir = proj.scriptsDir();
    if (!createDirectories(dir)) {
        if (err) *err = "Could not create " + dir;
        return false;
    }

    const std::string path = dir + "\\" + name + ".cs";
    // Reported by leaf name rather than by full path: the folder is fixed and already on screen
    // above the error, so the path would just be noise around the one word that matters.
    if (fileExists(path)) {
        if (err) *err = name + ".cs already exists in Content\\Scripts.";
        return false;
    }
    if (!writeNewFile(path, scriptText(proj.name, name, kind), err)) return false;
    if (outPath) *outPath = path;

    // First file in the folder gets the project file, so an IDE opens it as a buildable project
    // instead of a loose .cs with every Aver.Scripting symbol underlined red.
    const std::string csproj = dir + "\\" + kCsprojName;
    if (!fileExists(csproj)) {
        if (writeFileText(csproj, csprojText(engineRefs(dir)))) {
            if (outCsproj) *outCsproj = true;
        } else {
            AVER_WARN("[Editor] script written, but could not create {}", csproj);
        }
    }
    AVER_INFO("[Editor] new C# {}: {}", csKindNoun(kind), path);
    return true;
}

} // namespace aver::editor
