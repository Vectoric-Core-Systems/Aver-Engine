#pragma once
// The converter's shared state and the pieces that live in FabricProject / FabricResources / FabricScene / FabricConvert.
#include "FabricConvert.hpp"
#include "FabricMath.hpp"
#include "FabricReport.hpp"

#include "aver/formats/ImportedMaterial.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/formats/godot/GodotProjectFs.hpp"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace aver::fabric {

namespace gd = aver::fmt::godot;
using Var = gd::GodotVariant;
using VType = gd::GodotVType;

// ---- Variant helpers (FabricResources.cpp) -------------------------------------------------------------------------
Var makeNil();
Var makeString(const std::string& s);
Var makeResource(const std::string& assetPath);                  // Resource("path"): an Aver asset path
Var makeInt(long long v);
Var makeFloat(double v);
Var makeBool(bool v);
Var makeVector3(double x, double y, double z);
Var makeObject(const std::string& cls, std::vector<std::pair<std::string, Var>> props);
Var makeDict(std::vector<std::pair<std::string, Var>> entries);
double numOf(const Var* v, double def);
bool boolOf(const Var* v, bool def);
V3 vec3Of(const Var* v, const V3& def);
std::string stringOf(const Var* v, const std::string& def = {});
std::string lower(std::string s);
std::string safeName(const std::string& in, const std::string& fallback);   // [A-Za-z0-9_-] only
std::string pascalCase(const std::string& in);
std::string stemOf(const std::string& path);
std::string fnvHex(const std::string& s);   // 8 hex digits, for generated file names

// ---- scripts (FabricConvert.cpp) -----------------------------------------------------------------------------------
struct ScriptInfo {
    std::string path;          // res://scripts/player.gd
    std::string kind;          // gdscript | csharp
    std::string className;     // the C# class the converted script becomes (unique, a valid identifier)
    std::string extends;       // the Godot class it derives from ("Node" when none is written)
    std::string extendsPath;   // gdscript: extends "res://base.gd"
    bool declaredClassName = false;   // class_name / the C# class name was in the source
    bool exists = true;
    bool autoload = false;
    std::vector<std::string> uses;   // "res://main.tscn:Player" for every node that attaches it
    std::string uid;
};

struct Where {
    std::string file;   // res:// path of the document
    std::string node;   // scene-relative node path, "" outside a node
    std::string type;   // the node's Godot class
};

// ---- the scene tree the converter builds (FabricScene.cpp) ---------------------------------------------------------
struct Prop {
    std::string key;
    Var value;                              // as written; ExtResource / SubResource are resolved against `doc` on emit
    const gd::GodotDocBase* doc = nullptr;
    int line = 0;
};

struct FNode;

struct Connection {
    std::string signal;
    FNode* to = nullptr;
    std::string method;
    int flags = 0;
    int unbinds = 0;
    std::vector<Var> binds;
    const gd::GodotDocBase* doc = nullptr;
};

struct FNode {
    std::string name;
    std::string type;                // the Godot class ("Node3D"); for an instance root, the class of the instanced scene's root
    std::string sceneFile;           // the scene this node was authored in
    std::string instanceOf;          // res:// scene this node is the root of an instance of, "" otherwise
    std::string scriptPath;          // res:// path of the attached script, "" none
    std::vector<std::string> groups;
    std::vector<Prop> props;         // everything except the transform and the script
    Xform xf;
    bool hasXform = false;
    int rotationOrder = 2;
    bool unique = false;
    bool persistent = false;
    FNode* parent = nullptr;
    FNode* owner = nullptr;          // null for the scene root
    std::vector<std::unique_ptr<FNode>> children;
    std::vector<Connection> connections;
    int line = 0;
    bool fromModel = false;          // a node made from an imported model file, no .tscn behind it
    std::string reportFile;          // where the node is written down, for the report
    std::string reportPath;

    FNode* findChild(const std::string& childName) const;
};

// The Godot class family the converter treats specially.
bool isNode3DClass(const std::string& type);
bool isControlClass(const std::string& type);
bool is2DClass(const std::string& type);
// Classes a project's GDExtension plugins register (FabricExtensions.cpp), name -> whether it is spatial. Such a node is placed as a
// GDExtensionNode / GDExtensionNode3D with the Prop extension_class (docs/plans/fabric/G3_GDEXT.md section 7).
void setExtensionClasses(const std::map<std::string, bool>& classes);
bool isExtensionClass(const std::string& type, bool* is3D = nullptr);

struct ClassChoice {
    std::string fabricClass;         // the registered C# class to place
    Rating rating = Rating::Converted;
    std::string reason;
    bool carrier = false;            // placed as a stand-in: `_class` metadata names the real class
};
ClassChoice chooseClass(const std::string& godotType);

// ---- the converter -------------------------------------------------------------------------------------------------
struct EnvInfo {
    bool found = false;
    bool hasSky = false, skyPhysical = false;
    double zenith[3] = {0.24, 0.45, 0.85}, horizon[3] = {0.72, 0.83, 0.95}, ground[3] = {0.24, 0.23, 0.21};
    double skyLight = 1.0;
    bool hasFog = false;
    double fogDensityPerCm = 0, fogColor[3] = {1, 1, 1}, fogHeightCm = 0, fogFalloff = 0;
    int tonemapMode = 0;
    double tonemapExposure = 1.0;
    bool glow = false;
    double glowIntensity = 0.8;
    std::string source;              // "res://main.tscn:WorldEnvironment"
};

struct LevelScan;   // what one level's emission collects: the sun, the camera, the environment (FabricScene.cpp)

// One GDExtension plugin found in the Godot project (FabricExtensions.cpp).
struct ExtensionInfo {
    std::string resPath;                   // res://addons/x/x.gdextension
    std::string contentPath;               // addons/x/x.gdextension, under Content/
    std::string entrySymbol;
    std::string compatibilityMinimum, compatibilityMaximum;
    std::vector<std::string> classes;      // the [icons] keys: the classes the plugin registers
    std::vector<std::string> libraries;    // content-relative files copied (libraries and dependencies)
    std::vector<std::string> missing;      // desktop libraries the project does not hold
    bool hostLibrary = false;              // a library for the OS the converter runs on was found
};

struct Options {
    bool dryRun = false;       // convert into a scratch folder and keep only FabricReport.md / .jsonl
    bool scenesOnly = false;   // scenes, assets, project, GDExtensions; no script conversion
    bool scriptsOnly = false;  // only the script step, into an already converted project
    bool overwrite = false;    // replace an existing converted project
};

class Converter {
public:
    Converter(std::string godotDir, std::string outDir, Host host);
    int run();
    Options options_;

    // ---- FabricExtensions.cpp --------------------------------------------------------------------------------------
    // Finds the project's .gdextension files, copies them with their libraries into Content/ and reports them. Runs before the scenes
    // so nodes of the plugins' classes are placed as GDExtensionNode(3D).
    void convertExtensions();
    std::vector<ExtensionInfo> extensions_;

    // ---- the script step (FabricScripts.cpp) -----------------------------------------------------------------------
    // Runs Aver.Fabric.Convert on the manifest and folds its report into ours. Returns false on a hard failure.
    bool convertScripts();
    bool scriptsRan_ = false;

    // ---- state -----------------------------------------------------------------------------------------------------
    std::string godotDir_, outDir_;
    Host host_;
    gd::GodotProjectFs fs_;
    gd::GodotProject project_;
    gd::GodotDiagnostics diag_;
    Report report_;
    std::string name_;          // the Aver project's name
    std::string projectDir_;    // <out>/<Name>
    std::string contentDir_;    // <out>/<Name>/Content
    std::string fabricDir_;     // <out>/<Name>/Fabric

    // ---- FabricProject.cpp -----------------------------------------------------------------------------------------
    bool convertProject(const std::string& startLevelAsset);
    void convertInput();
    void convertLocalization();
    double setting(const char* path, double def) const;
    std::string settingString(const char* path, const std::string& def) const;
    bool settingBool(const char* path, bool def) const;
    std::string startLevelAsset_;
    EnvInfo projectEnv_;        // from the first scene that had a WorldEnvironment, the start scene first

    // ---- FabricConvert.cpp -----------------------------------------------------------------------------------------
    void registerScripts();
    ScriptInfo* script(const std::string& resPath);
    bool writeScriptsManifest();
    bool writeProjectManifest();
    std::map<std::string, ScriptInfo> scripts_;
    std::set<std::string> usedClassNames_;
    std::vector<std::string> sceneLevels_;   // content-relative level assets written
    std::vector<std::string> prefabs_;

    // ---- FabricResources.cpp ---------------------------------------------------------------------------------------
    // Resolves ExtResource / SubResource anywhere in `v` into Aver asset paths or inline Object(...) values.
    Var convertValue(const Var& v, const gd::GodotDocBase& doc, const Where& w, const std::string& prop);
    // A path to a texture file copied into Content/Textures, or "" (and a report entry).
    std::string copyTexture(const std::string& resPath, const Where& w);
    // The cooked material for a resource of material type; the asset path "Materials/<stem>.ocmat" or "".
    std::string materialAsset(const std::string& key, const std::string& type, const std::vector<gd::GodotProperty>& props,
                              const gd::GodotDocBase& doc, const std::string& nameHint, const Where& w);
    // A generated primitive mesh; the asset path or "".
    std::string primitiveMeshAsset(const std::string& type, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc,
                                   const Where& w);
    // An imported model file; the parts it produced (cached per file).
    const ModelResult* modelFor(const std::string& resPath, const Where& w);
    const gd::GodotResource* resourceDoc(const std::string& resPath);
    std::map<std::string, std::unique_ptr<gd::GodotResource>> resourceDocs_;
    std::map<std::string, std::string> meshAssets_, materialAssets_, textureAssets_;
    std::map<std::string, ModelResult> models_;
    std::set<std::string> materialStemsUsed_;
    std::set<std::string> materialTextureSources_;   // res:// textures a cooked material already carries
    Var shapeObject(const std::string& type, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w);
    Var genericResourceDict(const std::string& type, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc,
                            const Where& w, const char* why);
    void convertStandaloneResources();
    void reportProjectFiles();

    // ---- FabricScene.cpp -------------------------------------------------------------------------------------------
    bool convertScenes();
    const gd::GodotScene* sceneDoc(const std::string& resPath);
    std::unique_ptr<FNode> buildScene(const std::string& resPath, int depth, const Where& from);
    bool emitLevel(const std::string& sceneRes, FNode& root, fmt::OcWorldData& level, bool isStart);
    // Places `fn` and everything below it; `parent` indexes level.placements (-1 for a top-level node).
    void emitNode(FNode& fn, int parent, const Xform& parentWorld, fmt::OcWorldData& level, LevelScan& scan);
    bool writePrefabs();
    std::set<std::string> instancedScenes_;
    std::map<std::string, std::unique_ptr<gd::GodotScene>> sceneDocs_;
    std::map<std::string, bool> sceneLoadFailed_;
    std::string levelAssetFor(const std::string& sceneRes) const;   // "Maps/props/crate.ocworld"
    std::string startSceneRes_;
};

}  // namespace aver::fabric
