// Project scaffolding: creates a new project tree, writes its manifest, Scripts.csproj, starter
// material and C# script templates, and upgrades an older project's layout and references.

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
// engine tree could not be located from the executable. `tail` is the path under the engine root.
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

// Reference path to Aver.Scripting, which carries AverBehaviour.
std::string scriptingProjectReference(const std::string& scriptsDir) {
    return engineProjectReference(scriptsDir, "scripting/csharp/Aver.Scripting/Aver.Scripting.csproj");
}

// Reference path to Aver.Framework, which carries the actor types and Aver.Scene transitively.
std::string frameworkProjectReference(const std::string& scriptsDir) {
    return engineProjectReference(scriptsDir, "scripting/csharp/Aver.Framework/Aver.Framework.csproj");
}

// Reference path to Aver.UI, the game's HUD.
std::string uiProjectReference(const std::string& scriptsDir) {
    return engineProjectReference(scriptsDir, "scripting/csharp/Aver.UI/Aver.UI.csproj");
}

// Reference path to Aver.Materials, the material authoring surface.
std::string materialsProjectReference(const std::string& scriptsDir) {
    return engineProjectReference(scriptsDir, "scripting/csharp/Aver.Materials/Aver.Materials.csproj");
}

// Builds the .ocproject manifest text.
std::string manifestText(const std::string& name) {
    std::string s;
    s += "OCPROJECT 1\n";
    s += "# Created by the Aver Engine editor. This project lives OUTSIDE the engine tree and\n";
    s += "# references it; see the engine's docs/PROJECTS.md.\n";
    s += "NAME " + name + "\n";
    s += "ENGINE " + std::string(kEngineName) + " " + std::string(kEngineVersion) + "\n";
    // What made it, as opposed to ENGINE's "what it needs at least". This is the value the upgrade
    // chain reads to decide whether a later engine has to migrate the project, so a project stamped
    // here today is one a 0.4 editor can carry forward without guessing at its age.
    s += "CREATEDWITH " + std::string(kEngineVersion) + "\n";
    s += "CONTENT Content\n";
    s += "STARTMAP Maps/Default.ocmap\n";
    s += "# AUTHOR <your name>\n";
    return s;
}

// The starting level a new project opens.
//
// WHY THIS EXISTS AT ALL: until now the manifest named STARTMAP Maps/Default.ocmap and NOTHING
// WROTE ONE, so every new project opened to an empty world and a log line explaining that the start
// map did not exist. A first impression of "nothing happened" is a bad one when the engine is in
// fact working perfectly.
//
// Every number below is authored rather than inherited, because the engine's own defaults are tuned
// for the sandbox's test scene and not for somebody's first level.
std::string startLevelText(const std::string& name) {
    std::string s;
    s += "OCMAP 1\n";
    s += "# The level a new project opens. Everything here is editable: these are starting values,\n";
    s += "# not engine defaults, and changing them changes only this level.\n";
    s += "NAME " + name + "\n\n";
    // THE SKY A NEW PROJECT OPENS WITH IS SKYFORGE'S, deliberately and value for value. SkyForge is
    // the project this engine is actually developed against, so it is the sky that has been looked
    // at every day and tuned by eye; a template that differed from it meant the first thing anyone
    // saw was NOT the thing the engine was tuned to produce. Keep these in step with
    // SkyForge/Content/Maps/Default.ocworld if that one is ever retuned.
    //
    // AUTHORED IN DEGREES, which is the whole point of a derived sky: elevation is the only input
    // that matters and a direction vector invites getting its sign wrong. 59.5 degrees is high
    // afternoon rather than noon: an overhead sun flattens every surface it lights and hides the
    // shadow work, while this one still shows normal maps, shadow softness and the atmosphere's
    // forward scattering at once.
    s += "SUN elev 59.5 azim 53.2 color 1 0.98 0.92 lux 100000\n";
    // PHYSICAL sky, the engine's real model: Rayleigh, Cornette-Shanks Mie, an ozone tent and a
    // Chapman-function transmittance.
    //
    // NO OVERRIDES: the same "SkyForge is the tuned target" reasoning as the paragraph above, just
    // applied to the engine's OWN AtmosphereProfile defaults (Atmosphere.hpp) instead of a per-level
    // one. SkyForge's own Default.ocworld carries no SKY overrides either, so the two are identical
    // by construction, not by being kept in step by hand.
    //
    // MEASURED, NOT DESCRIBED, AGAIN: an earlier version of this comment described the defaults
    // BEFORE this session's atmosphere pass, and called near-white at the zenith "the tuned value" --
    // it was, relative to what the old defaults could reach, but it was also a symptom of the same
    // washed-out sky this session tracked down and fixed (Atmosphere.hpp's mieScatter, mieExtinction
    // and multiScatterGain retuned against AtmosphereTest's own measured-clear-sky calibration; see
    // that file). Re-measured after: sky high (74,92,114), horizon (178,200,204) -- genuinely blue at
    // both, not a few codes apart from white.
    s += "SKY model physical\n";
    // 2e-5 per centimetre, matching SkyForge. Thin enough to read as air rather than as weather,
    // and the cooler blue-grey tint belongs with an unhazed sky -- the old 0.62 0.7 0.82 was
    // compensating for the Mie warmth that is now gone.
    s += "FOG exp density 0.00002 color 0.7 0.78 0.88\n\n";
    // The sky as a PCG field, so a new project has a working example of the record and a seed to
    // change. INFINITE because a sky has no bounds; 1600 cm cells because that is one chunk.
    // `floor` is a DENSITY floor and so reads inverted as cover: 0.45 is about 55% cloud.
    s += "PCGVOLUME name Sky seed 3 cell 1600 octaves 4 floor 0.45 bias 1.6 infinite\n\n";
    s += "# A ground plane and two shapes, so the sun, the shadows and the fog have something to\n";
    s += "# fall on. Delete them once your own content is in.\n";
    s += "PLACEG Meshes/cube.ocmesh 0 0 -10 0 0 0 4000 4000 10 M_Floor\n";
    s += "PLACE  Meshes/cube.ocmesh 0 300 100 0 0 0 100 M_Wall\n";
    s += "PLACE  Meshes/sphere.ocmesh 400 -200 120 0 0 0 120 M_Metal\n";
    return s;
}


// The project's own F# project, holding its PCG rules.
//
// WHY THIS IS SCAFFOLDED AND NOT LEFT TO THE USER: Scripts.csproj already carries a CONDITIONAL
// reference to Scripts.FSharp.fsproj, guarded by Exists(), and nothing ever created the file it
// points at. The hook existed with nothing on the end of it.
//
// NAMED Scripts.FSharp.fsproj, NOT Scripts.fsproj, and that is load-bearing: both projects sit in
// this directory and both default AssemblyName to their own filename, so a Scripts.fsproj would
// emit a second Scripts.dll into the same output folder and one would overwrite the other.
//
// RAW STRING LITERALS THROUGHOUT. The generated text is XML and F#, both full of characters that
// need escaping in a quoted C++ string, and this file has already shipped one bug from exactly that
// -- a `--` inside an XML comment that made every generated .csproj unloadable. A raw literal has
// no escapes to get wrong and reads as the file it produces.
std::string fsprojText(const std::string& scriptsDir) {
    const std::string pcg = engineProjectReference(scriptsDir, "scripting/fsharp/Aver.Pcg/Aver.Pcg.fsproj");
    const std::string fw  = frameworkProjectReference(scriptsDir);

    std::string s = R"FS(<Project Sdk="Microsoft.NET.Sdk">

  <!-- This project's PCG rules, in F#.

       Referenced by Scripts.csproj only when this file EXISTS, so deleting it is a supported way
       to opt out and the C# still builds.

       File order below is significant in F#: a file may only use what is compiled before it. -->
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework>
    <AssemblyName>Scripts.FSharp</AssemblyName>
    <GenerateDocumentationFile>false</GenerateDocumentationFile>
    <SatelliteResourceLanguages>en</SatelliteResourceLanguages>
  </PropertyGroup>

  <ItemGroup>
    <Compile Include="Sky.fs" />
  </ItemGroup>

)FS";
    if (!pcg.empty() || !fw.empty()) {
        s += "  <ItemGroup>\n";
        if (!pcg.empty()) s += "    <ProjectReference Include=\"" + pcg + "\" />\n";
        if (!fw.empty())  s += "    <ProjectReference Include=\"" + fw  + "\" />\n";
        s += "  </ItemGroup>\n\n";
    }
    s += "</Project>\n";
    return s;
}

// The project's sky, as a PCG graph. Every number is authored here rather than in the level, which
// is the point: the sky becomes code the project owns instead of literals typed into an .ocworld.
std::string skyScriptText(const std::string& projectName) {
    std::string s = "module " + projectName + ".Sky\n\n";
    s += R"FS(// This project's sky, as a PCG graph.
//
// The sky is an INFINITE density field -- a sky has no bounds -- built from the same Aver.Pcg types
// the rest of the PCG library uses. Change the numbers in `spec` and the sky changes: the coverage
// the engine renders at is MEASURED from the field rather than declared, so the knobs really drive
// it rather than sitting beside it.
//
// WHAT THE ENGINE DOES WITH THIS, stated plainly, because the gap matters. The cloud raymarch has
// its own noise and does not evaluate this field per sample. What crosses the boundary is the SEED,
// hashed into a translation of the renderer's noise domain so two seeds give two skies, and the
// coverage measured below. So this graph PARAMETERISES the sky rather than replacing the renderer's
// noise -- worth knowing before you tune octaves expecting to watch them appear.
//
// Call Apply() once, from your game mode's OnBeginPlay. Until something calls it, the engine renders
// whatever the level authored.

open Aver.Pcg

/// How this project's sky is shaped.
let spec =
    {| Seed = 3                 // which sky this is; change it for a different one
       CellSizeCm = 1600.0f     // world size of one lattice cell; 1600 is one chunk
       Octaves = 4              // more octaves, finer structure
       CoverageFloor = 0.45f    // a DENSITY floor, so HIGHER means LESS cloud
       BottomCm = 150000.0f
       TopCm = 280000.0f
       WindXCmPerSec = 900.0f
       WindYCmPerSec = 260.0f |}

/// The infinite density field this sky is.
let field () : InfiniteSpec =
    let baseField = Pcg.infiniteExpanse spec.Seed spec.CellSizeCm spec.Octaves
    { baseField with CoverageFloor = spec.CoverageFloor; CoverageBias = 1.6f }

/// The fraction of the field that clears its floor, on a coarse 16x16 lattice.
/// Coarse on purpose: this runs once at level load and feeds a single scalar.
let measureCoverage () =
    let f = field ()
    let span = spec.CellSizeCm * 16.0f
    let z = (spec.BottomCm + spec.TopCm) * 0.5f
    let mutable hits = 0
    for iy in 0 .. 15 do
        for ix in 0 .. 15 do
            let x = float32 ix / 16.0f * span
            let y = float32 iy / 16.0f * span
            if Pcg.sampleInfinite f x y z > 0.0f then hits <- hits + 1
    float32 hits / 256.0f

/// Publishes this sky to the engine. Call once from OnBeginPlay.
let Apply () =
    Aver.Framework.Sky.SetClouds(
        spec.Seed,
        measureCoverage (),
        1.0f,
        spec.BottomCm,
        spec.TopCm,
        0.00002f,
        spec.WindXCmPerSec,
        spec.WindYCmPerSec)
)FS";
    return s;
}

// The four engine assemblies a project compiles against, in the order they are written.
struct EngineRefs {
    std::string scripting;   // AverBehaviour
    std::string framework;   // Actor / Pawn / GameMode, and Aver.Scene behind it
    std::string ui;          // the game's HUD
    std::string materials;   // [AverMaterial] surfaces
    bool any() const { return !scripting.empty() || !framework.empty() || !ui.empty() || !materials.empty(); }
};

// Resolves all four engine references for a project's Scripts folder.
EngineRefs engineRefs(const std::string& scriptsDir) {
    return EngineRefs{
        scriptingProjectReference(scriptsDir),
        frameworkProjectReference(scriptsDir),
        uiProjectReference(scriptsDir),
        materialsProjectReference(scriptsDir),
    };
}

// Builds the starter Content\Materials\Surfaces.cs text: one complete, plain material.
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

// Builds the Scripts.csproj text and checks the result for XML-illegal `--` inside its comments.
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
    // No `--` in any string that lands inside an XML comment: XML forbids it, MSBuild says MSB4025.
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
    s += "  <ItemGroup>\n";
    s += "    <!-- Surfaces authored in C#, under Content\\Materials. Compile .NET runs avermatc over\n";
    s += "         the built assembly and writes the .ocmat files the engine loads into\n";
    s += "         <project>\\Binaries\\Materials. -->\n";
    s += "    <Compile Include=\"..\\Materials\\**\\*.cs\" />\n";
    s += "  </ItemGroup>\n\n";
    s += "  <ItemGroup>\n";
    s += "    <!-- F#, if this project has any. The editor builds THIS project by name, and MSBuild\n";
    s += "         picks a compiler from each project's extension, so a sibling .fsproj is built by\n";
    s += "         the same command and the same button with no editor change at all.\n";
    s += "\n";
    s += "         Conditional so the reference is inert until the file exists: an unconditional\n";
    s += "         reference to a missing project fails the build for every author who never writes\n";
    s += "         a line of F#. This is what makes the Compile .NET button's name true rather than\n";
    s += "         aspirational.\n";
    s += "\n";
    s += "         NAMED Scripts.FSharp, NOT Scripts.fsproj, and the difference is not cosmetic.\n";
    s += "         Both projects live in this directory and both default AssemblyName to their own\n";
    s += "         filename, so a Scripts.fsproj would emit a second Scripts.dll into the same output\n";
    // Commas, not dashes. The rule two hundred lines up says no `--` inside an XML comment, and
    // this sentence broke it: MSBuild rejects the whole file with MSB4025, so EVERY scaffolded
    // project shipped a Scripts.csproj that would not load. The guard below caught it at run time
    // and the project was still written, so the failure landed on the user rather than here.
    s += "         folder and one would overwrite the other. It still COMPILES, because the\n";
    s += "         reference resolves at build time, and then fails at load, which is the worst\n";
    s += "         place to find out. Measured, not guessed. -->\n";
    s += "    <ProjectReference Include=\"Scripts.FSharp.fsproj\"\n";
    s += "                      Condition=\"Exists('$(MSBuildThisFileDirectory)Scripts.FSharp.fsproj')\" />\n";
    s += "  </ItemGroup>\n\n";
    s += "</Project>\n";

    // XML forbids `--` inside a comment; a project that contains one cannot load at all (MSB4025).
    //
    // THIS REPAIRS RATHER THAN REPORTS, and that is the fix. It used to log an error and `break`,
    // and then return the string anyway -- so the broken .csproj was written, every scaffolded
    // project had a Scripts.csproj MSBuild would not load, and the only sign was one line in a log
    // the user had no reason to read. A guard that detects a fatal defect and then ships it is
    // worse than no guard, because it reads like the case is handled.
    //
    // Collapsing the pair is safe: this only ever runs inside a comment, where the text is prose,
    // and a single dash reads the same. The loud log stays, because prose with `--` in it is still
    // a mistake at the source and should be fixed there rather than relied on being patched here.
    {
        bool inComment = false;
        u32 repaired = 0;
        for (usize i = 0; i + 1 < s.size(); ++i) {
            if (!inComment && s.compare(i, 4, "<!--") == 0) { inComment = true; i += 3; continue; }
            if (inComment && s.compare(i, 3, "-->") == 0)   { inComment = false; i += 2; continue; }
            if (inComment && s[i] == '-' && s[i + 1] == '-') {
                s.erase(i, 1);   // "--" becomes "-"; re-test this index in case of "---"
                ++repaired;
                --i;
            }
        }
        if (repaired)
            AVER_ERROR("[Editor] the generated Scripts.csproj had {} '--' sequence(s) inside an XML "
                       "comment and they were collapsed to keep the file loadable; fix the prose in "
                       "ProjectScaffold.cpp rather than leaving it to this repair", repaired);
    }
    return s;
}

// Builds the .cs text for one actor-kind template: the right base type, attribute and overrides.
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

// Builds the .cs text for a new script of any kind.
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

// True for every kind the engine calls hooks on, i.e. everything but a plain class.
bool csKindIsScript(CsKind kind) { return kind != CsKind::PlainClass; }

// True for the kinds that derive from an Aver.Framework actor type.
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

// The word for a kind, for logs and UI text.
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

// Checks a project name as a folder name. Returns false and fills `err` with the reason.
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

// Checks a name that becomes a C# or C++ class name. Returns false and fills `err` with the reason.
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
    // The union of the C# and C++ keywords a generated `class <Name>` could collide with.
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

// Writes a file, refusing to overwrite an existing one. False on failure, with the reason in `err`.
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

// Creates a whole new project at location\name and loads its manifest into `out`.
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

    // The start map the manifest names. Until this existed, STARTMAP pointed at a file nothing
    // wrote, so every new project opened to an empty world and a log line saying the map did not
    // exist yet -- a first impression of "nothing happened" from an engine that was working.
    //
    // A WARNING AND NOT A FAILURE if it cannot be written: the project itself is valid without a
    // level, and refusing to create it over a missing starter map would be losing the whole thing
    // over the least important part of it.
    {
        const std::string startMap = content + "\\Maps\\Default.ocmap";
        if (!writeFileText(startMap, startLevelText(name)))
            AVER_WARN("[Editor] project created, but could not write {} - it will open empty", startMap);
    }

    {
        const std::string scriptsDir = content + "\\Scripts";
        const std::string csproj = scriptsDir + "\\" + kCsprojName;
        if (!writeFileText(csproj, csprojText(engineRefs(scriptsDir))))
            AVER_WARN("[Editor] project created, but could not write {}", csproj);
    }
    // The F# side. A WARNING AND NOT A FAILURE for the starter map's reason: a project with no F#
    // is perfectly valid, and Scripts.csproj references it only when the file exists.
    //
    // WRITTEN ONLY IF Aver.Pcg CAN ACTUALLY BE REFERENCED, which it could not on a shipped build
    // until the payload started carrying scripting/fsharp. The failure that taught this: the
    // reference walk returned empty, fsprojText correctly omitted the ProjectReference -- and
    // Sky.fs was written anyway, still opening Aver.Pcg. A tester's brand-new project failed to
    // build before he had typed a line of it, with FS0039 naming a namespace he had never heard of.
    //
    // Scaffolding source that cannot compile is worse than scaffolding nothing: the author has to
    // understand a subsystem he did not ask for in order to delete it. So if the reference is not
    // there, neither is the F#, and the C# project builds exactly as it would for anyone who chose
    // not to write any. The allowlist entry is the real fix; this is what keeps the next gap in the
    // payload from reaching an author as a broken project.
    {
        const std::string scriptsDir = content + "\\Scripts";
        if (engineProjectReference(scriptsDir, "scripting/fsharp/Aver.Pcg/Aver.Pcg.fsproj").empty()) {
            AVER_WARN("[Editor] no Aver.Pcg to reference from this install, so '{}' gets no F# "
                      "starter -- the C# side is unaffected", name);
        } else {
            const std::string fsproj = scriptsDir + "\\Scripts.FSharp.fsproj";
            if (!writeFileText(fsproj, fsprojText(scriptsDir)))
                AVER_WARN("[Editor] project created, but could not write {}", fsproj);
            const std::string sky = scriptsDir + "\\Sky.fs";
            if (!writeFileText(sky, skyScriptText(name)))
                AVER_WARN("[Editor] project created, but could not write {}", sky);
        }
    }
    {
        const std::string starter = content + "\\Materials\\Surfaces.cs";
        if (!writeFileText(starter, starterMaterialText(name)))
            AVER_WARN("[Editor] project created, but could not write {}", starter);
    }

    if (!fmt::loadOcproject(manifest, out, err)) return false;
    AVER_INFO("[Editor] created project '{}' at {}", name, root);
    return true;
}

// Path of a project's Scripts.csproj. Empty if the project has no scripts folder.
std::string scriptsCsprojPath(const fmt::ProjectDesc& proj) {
    const std::string dir = proj.scriptsDir();
    return dir.empty() ? std::string() : dir + "\\" + kCsprojName;
}

// Where Compile C# puts the built script assembly.
std::string scriptsBinaryDir(const fmt::ProjectDesc& proj) {
    return proj.dir.empty() ? std::string() : proj.dir + "\\Binaries\\Scripts";
}

// Writes a new .cs file into the project's Scripts folder, creating Scripts.csproj if absent.
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
    if (fileExists(path)) {
        if (err) *err = name + ".cs already exists in Content\\Scripts.";
        return false;
    }
    if (!writeNewFile(path, scriptText(proj.name, name, kind), err)) return false;
    if (outPath) *outPath = path;

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

// ---------------------------------------------------------------- upgrading an older project

namespace {

// One engine assembly a project should reference: its display label and its path under the engine.
struct RefSpec { const char* label; const char* tail; };

const RefSpec kEngineRefSpecs[] = {
    {"Aver.Scripting", "scripting/csharp/Aver.Scripting/Aver.Scripting.csproj"},
    {"Aver.Framework", "scripting/csharp/Aver.Framework/Aver.Framework.csproj"},
    {"Aver.UI",        "scripting/csharp/Aver.UI/Aver.UI.csproj"},
    {"Aver.Materials", "scripting/csharp/Aver.Materials/Aver.Materials.csproj"},
};

// Every ProjectReference Include="..." value in the file, in order.
std::vector<std::string> projectReferences(const std::string& xml) {
    std::vector<std::string> out;
    usize i = 0;
    while ((i = xml.find("<ProjectReference", i)) != std::string::npos) {
        // The element runs to its own '>'; Include and Condition are both read from inside it.
        const usize close = xml.find('>', i);
        const usize inc = xml.find("Include=\"", i);
        if (inc == std::string::npos || (close != std::string::npos && inc > close)) break;
        const usize a = inc + 9;
        const usize b = xml.find('"', a);
        if (b == std::string::npos) break;
        const std::string include = xml.substr(a, b - a);

        // A REFERENCE GUARDED BY Exists() IS NOT MISSING WHEN IT IS ABSENT -- being absent is the
        // case it was written for. The scaffolder emits exactly one of these:
        //
        //     <ProjectReference Include="Scripts.FSharp.fsproj"
        //                       Condition="Exists('$(MSBuildThisFileDirectory)Scripts.FSharp.fsproj')" />
        //
        // so every project that had never added F# reported a dead reference, and the editor met
        // anyone opening a BRAND NEW project with "this was created by an earlier version and is
        // missing files a project now needs". Nothing was missing and nothing needed upgrading; the
        // first thing the engine said to a new user was untrue.
        const std::string element = xml.substr(i, close == std::string::npos ? 0 : close - i);
        if (element.find("Condition=") != std::string::npos &&
            element.find("Exists(") != std::string::npos) {
            i = b;
            continue;
        }

        out.push_back(include);
        i = b;
    }
    return out;
}

} // namespace

// Lists what an older project is missing: content folders, the csproj, engine references, dead
// references and the materials glob.
ProjectUpgrade inspectProject(const fmt::ProjectDesc& proj) {
    ProjectUpgrade up;
    if (!proj.valid()) return up;

    const std::string content = proj.contentDir();
    const std::string scriptsDir = proj.scriptsDir();

    for (const char* d : {"Maps", "Meshes", "Materials", "Textures", "Sounds", "Scripts"}) {
        const std::string path = content + "\\" + d;
        if (!fileExists(path))
            up.fixes.push_back({ProjectFix::Kind::CreateFolder, std::string("Create Content\\") + d, path});
    }

    const std::string csproj = scriptsCsprojPath(proj);
    if (csproj.empty()) return up;

    if (!fileExists(csproj)) {
        up.fixes.push_back({ProjectFix::Kind::CreateCsproj,
                            "Create Content\\Scripts\\Scripts.csproj", csproj});
        return up;
    }

    std::string xml;
    if (!readFileText(csproj, xml)) return up;

    for (const RefSpec& r : kEngineRefSpecs) {
        // Matched on the project file's leaf name: only that is stable across spellings of the path.
        const std::string leaf = std::filesystem::path(r.tail).filename().string();
        if (xml.find(leaf) != std::string::npos) continue;
        const std::string ref = engineProjectReference(scriptsDir, r.tail);
        up.fixes.push_back({ProjectFix::Kind::AddReference, std::string("Reference ") + r.label,
                            ref.empty() ? std::string("(engine tree not found from the editor)") : ref});
    }

    // ---- references whose target is not on disk ----
    for (const std::string& ref : projectReferences(xml)) {
        std::error_code ec;
        // Normalised before the test: an unresolved `..` run can exceed MAX_PATH and report absent.
        const std::filesystem::path abs =
            (std::filesystem::path(scriptsDir) / ref).lexically_normal();
        if (std::filesystem::exists(abs, ec)) continue;
        std::string repointed;
        for (const RefSpec& r : kEngineRefSpecs) {
            const std::string leaf = std::filesystem::path(r.tail).filename().string();
            if (ref.find(leaf) == std::string::npos) continue;
            repointed = engineProjectReference(scriptsDir, r.tail);
            break;
        }
        up.fixes.push_back({ProjectFix::Kind::RepointReference,
                            "Repoint a reference whose target no longer exists",
                            ref + (repointed.empty() ? std::string("  ->  (cannot resolve; edit by hand)")
                                                     : "  ->  " + repointed)});
    }

    if (xml.find("..\\Materials\\") == std::string::npos && xml.find("../Materials/") == std::string::npos)
        up.fixes.push_back({ProjectFix::Kind::AddMaterialsGlob,
                            "Compile the C# materials under Content\\Materials",
                            "<Compile Include=\"..\\Materials\\**\\*.cs\" />"});
    return up;
}

// Applies every fix inspectProject listed: creates folders, writes the csproj, and edits the XML.
bool applyProjectUpgrade(const fmt::ProjectDesc& proj, const ProjectUpgrade& up, std::string* err) {
    if (!proj.valid()) { if (err) *err = "no project"; return false; }
    if (up.empty()) return true;

    const std::string scriptsDir = proj.scriptsDir();
    const std::string csproj = scriptsCsprojPath(proj);

    for (const ProjectFix& f : up.fixes) {
        if (f.kind == ProjectFix::Kind::CreateFolder) {
            if (!createDirectories(f.detail)) { if (err) *err = "could not create " + f.detail; return false; }
        } else if (f.kind == ProjectFix::Kind::CreateCsproj) {
            if (!writeFileText(csproj, csprojText(engineRefs(scriptsDir)))) {
                if (err) *err = "could not write " + csproj;
                return false;
            }
        }
    }

    // The .csproj edits, in one read-modify-write.
    std::string xml;
    if (!fileExists(csproj) || !readFileText(csproj, xml)) return true;
    bool touched = false;

    for (const ProjectFix& f : up.fixes) {
        if (f.kind != ProjectFix::Kind::RepointReference) continue;
        const usize arrow = f.detail.find("  ->  ");
        if (arrow == std::string::npos) continue;
        const std::string from = f.detail.substr(0, arrow);
        const std::string to = f.detail.substr(arrow + 6);
        if (to.empty() || to[0] == '(') continue;
        const usize at = xml.find(from);
        if (at == std::string::npos) continue;
        xml.replace(at, from.size(), to);
        touched = true;
    }

    std::string additions;
    for (const ProjectFix& f : up.fixes)
        if (f.kind == ProjectFix::Kind::AddReference && !f.detail.empty() && f.detail[0] != '(')
            additions += "    <ProjectReference Include=\"" + f.detail + "\" />\n";

    bool wantGlob = false;
    for (const ProjectFix& f : up.fixes)
        if (f.kind == ProjectFix::Kind::AddMaterialsGlob) wantGlob = true;

    if (!additions.empty() || wantGlob) {
        std::string block;
        block += "\n  <!-- Added by the Aver Engine editor when this project was upgraded.\n";
        block += "       Everything above is as you left it. -->\n";
        if (!additions.empty()) block += "  <ItemGroup>\n" + additions + "  </ItemGroup>\n";
        if (wantGlob) {
            block += "  <ItemGroup>\n";
            block += "    <Compile Include=\"..\\Materials\\**\\*.cs\" />\n";
            block += "  </ItemGroup>\n";
        }
        const usize close = xml.rfind("</Project>");
        if (close == std::string::npos) { if (err) *err = "the .csproj has no </Project>"; return false; }
        xml.insert(close, block);
        touched = true;
    }

    if (touched && !writeFileText(csproj, xml)) {
        if (err) *err = "could not write " + csproj;
        return false;
    }
    AVER_INFO("[Editor] project '{}' upgraded: {} change(s)", proj.name, up.fixes.size());
    return true;
}

// Copies a whole project tree beside itself. See the header for why it never overwrites.
std::string copyProjectTree(const std::string& manifestPath, const std::string& versionTag,
                            std::string* err) {
    std::error_code ec;
    const std::filesystem::path src = std::filesystem::path(manifestPath).parent_path();
    if (!std::filesystem::is_directory(src, ec)) {
        if (err) *err = "no project folder beside " + manifestPath;
        return {};
    }
    const std::string stem = std::filesystem::path(manifestPath).stem().string();

    std::filesystem::path dst = src.parent_path() / (stem + " (" + versionTag + ")");
    for (int n = 2; std::filesystem::exists(dst, ec) && n < 100; ++n)
        dst = src.parent_path() / (stem + " (" + versionTag + ") " + std::to_string(n));
    if (std::filesystem::exists(dst, ec)) {
        if (err) *err = "could not find a free folder name beside " + src.string();
        return {};
    }

    std::filesystem::copy(src, dst, std::filesystem::copy_options::recursive, ec);
    if (ec) { if (err) *err = "copy failed: " + ec.message(); return {}; }

    // The manifest keeps its own file name inside the new folder, so the copy opens like any other
    // project. Checked rather than assumed: a copy that silently produced no manifest would hand
    // the caller a path to open that cannot be opened.
    const std::filesystem::path out = dst / std::filesystem::path(manifestPath).filename();
    if (!std::filesystem::exists(out, ec)) {
        if (err) *err = "the copy has no manifest at " + out.string();
        return {};
    }
    return out.string();
}

} // namespace aver::editor
