#pragma once
// The converter's shared state and the pieces that live in FabricProject / FabricResources / FabricScene / FabricConvert.
#include "FabricConvert.hpp"
#include "FabricMath.hpp"
#include "FabricModelTree.hpp"
#include "FabricReport.hpp"

#include "aver/formats/ImportedMaterial.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/formats/godot/GodotProjectFs.hpp"
#include "aver/formats/godot/GodotShaderEngine.hpp"

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
bool isCsgClass(const std::string& type);   // G4 R7: the CSGShape3D family (FabricGeometry.cpp)
bool isNavigationResourceType(const std::string& type);   // G5 N1: NavigationMesh (FabricNav.cpp)
bool isReplicationResourceType(const std::string& type);  // G6 NET2: SceneReplicationConfig (FabricNet.cpp)
bool isAnimTreeResourceType(const std::string& type);   // G5 A2: AnimationNode*, Curve, Curve3D, Gradient (FabricAnimTree.cpp)
bool isAudioExt(const std::string& ext);                // G5 S1: ".wav", ".ogg", ".mp3" (FabricAudio.cpp)
bool isAudioStreamResourceType(const std::string& type);   // G5 S1: AudioStreamRandomizer, Playlist, Synchronized, Interactive, Polyphonic, Generator, WAV
bool isStyleBoxType(const std::string& type);              // G5 U1: StyleBoxFlat, StyleBoxTexture, StyleBoxLine, StyleBoxEmpty (FabricUi.cpp)
bool isFontExt(const std::string& ext);                    // G5 U1: ".ttf", ".otf", ".woff", ".woff2"
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

// G5 U2 (FabricWidgets.cpp): the widget classes Aver.Fabric has, what they read, and the resources they hold.
bool isWidgetClass(const std::string& type);
bool isWidgetDynamicKey(const std::string& type, const std::string& key);                      // "popup/item_0/text", "item_0/text", "tab_0/title"
int widgetPropertyRating(const std::string& type, const std::string& key, std::string& why);   // 1 converted, 2 approximated, 0 not a property of the class
bool widgetClassChoice(const std::string& type, ClassChoice& out);
bool isWidgetResourceType(const std::string& type);                                            // LabelSettings, FontVariation, SystemFont, ButtonGroup, Shortcut, ...

// G4 R5: the name the engine knows a SubViewport by, and a baked material reaches it through ("@rt:viewport.<key>"): the level's name, then the node names from the
// level's root, e.g. "main:Main/Screen/View". Plain characters only (it travels in a .ocmat texture reference and a level Prop).
std::string viewportKeyOf(const std::string& level, const FNode& node);

// G6 SH3: the shader_type of a shading-language source: 0 sky, 1 fog, 2 particles, -1 anything else.
int envShaderKindOf(const std::string& source);

// ---- the converter -------------------------------------------------------------------------------------------------
struct EnvInfo {
    bool found = false;
    bool hasSky = false, skyPhysical = false;
    double zenith[3] = {0.24, 0.45, 0.85}, horizon[3] = {0.72, 0.83, 0.95}, ground[3] = {0.24, 0.23, 0.21};
    double skyLight = 1.0;
    bool hasFog = false;
    double fogDensityPerCm = 0, fogColor[3] = {1, 1, 1}, fogHeightCm = 0, fogFalloff = 0, fogStartCm = 0;
    int tonemapMode = 0;             // Godot's Environment.tonemap_mode
    double tonemapExposure = 1.0;
    bool glow = false;
    double glowIntensity = 0.8;
    // ---- G4 R3: the rest of the WorldEnvironment ----
    bool hasDomeExponent = false;
    double domeExponent = 0.65;      // from a panorama fit
    bool hasSkyMieScale = false;
    double skyMieScale = 1.0;        // PhysicalSkyMaterial: mie_coefficient / turbidity relative to Godot's defaults
    double tonemapWhite = 1.0;
    double cameraExposure = 1.0;     // CameraAttributes.exposure_multiplier
    bool autoExposure = false;       // CameraAttributesPractical.auto_exposure_enabled
    bool adjust = false;             // adjustment_enabled
    double adjBrightness = 1.0, adjContrast = 1.0, adjSaturation = 1.0;
    double glowThreshold = -1.0;     // glow_hdr_threshold when stated (< 0 = not stated)
    bool dof = false;
    double dofFocusCm = 1000, dofFStop = 4, dofFocalMm = 35;
    bool volumetricFog = false;      // G4 R8: volumetric_fog_enabled; the project turns Voxi's light-shaft fog on (RENDER.VOLFOG 1), where FogVolumes draw
    std::string source;              // "res://main.tscn:WorldEnvironment"
};

// The post settings a WorldEnvironment states, as POSTVOLUME keys (FabricScene.cpp); the project's RENDER.* keys carry the start level's.
struct PostKey { const char* key; double v; };
std::vector<PostKey> postKeysOf(const EnvInfo& env);

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
    // This extension's installed folder: the host's extensionDir, else <executable dir>/Extensions/FabricConnector.
    std::string extensionDir() const;

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
    bool writeNodesManifest();   // Fabric/nodes.json: what each scene's nodes became, for the script converter's typed node access
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
    // ---- FabricShader.cpp (G6 SH2) ----
    // A ShaderMaterial (its shader prepared once for the engine and written as Shaders/<name>.ocgraph) written as Materials/<name>.ocmat: GRAPHREF, the FX record
    // the render modes ask for, EMBREGS at the material's shader_parameter/* values and a TEX per texture parameter. The asset path or "".
    std::string shaderMaterialAsset(const std::string& key, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const std::string& nameHint,
                                    const Where& w);
    bool writeShadersManifest();   // Fabric/shaders.json: every shader's uniform layout, samplers and materials (Aver.Fabric's ShaderMaterial reads it)
    // G6 SH3. A ShaderMaterial whose shader is a sky, fog or particles shader (used by the environment, a FogVolume or a GPUParticles3D) becomes
    // Shaders/<name>.<kind>.ocgraph (a `DOMAIN customenv` graph, OcGraphHlsl.hpp) with the material's shader_parameter/* baked into its register table; the asset path or "".
    std::string envShaderAsset(const std::string& materialKey, int kindIndex, const std::string& source, const std::string& shaderKey, const std::string& srcFile,
                               const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const std::string& nameHint, const Where& w);
    // GPUParticles3D with a particles ShaderMaterial: the .ocparticle it plays and the ParticleEmitter component on its placement.
    void emitGpuParticles(const FNode& fn, const std::vector<std::pair<std::string, Var>>& props, aver::fmt::OcWorldPlacement& p, bool quiet);
    // A VisualShader .tres as shading-language text (and the per-node report); false when it does not convert.
    bool visualShaderSource(const gd::GodotResource& r, const std::string& file, std::string& source);
    std::map<std::string, std::string> visualShaderCode_;   // VisualShader file -> generated text (written under Fabric/visualshaders/)
    struct ShaderInfo {
        bool ok = false;
        std::string kind = "spatial";   // spatial | sky | fog | particles
        std::string res, stem, graphAsset;
        aver::fmt::godot::GdEngineShader prepared;
        std::vector<std::string> materials;                                 // Materials/<name>.ocmat written for this shader
        std::vector<std::pair<std::string, std::string>> globalTypes;       // the project's [shader_globals]: name -> Godot type
    };
    std::map<std::string, ShaderInfo> shaders_;
    std::map<std::string, std::string> shaderMaterialAssets_;
    std::set<std::string> shaderStemsUsed_;
    // G4 R5: a ViewportTexture value (a SubResource of that class) used by the node being emitted, as the texture reference a material carries
    // ("@rt:viewport.<key>"); counts the use for the SubViewport's consumer total. "" when the value is not a ViewportTexture; an unresolvable one is
    // reported and also gives "".
    std::string viewportReference(const Var* value, const gd::GodotDocBase& doc, const Where& w, const std::string& prop);
    // A generated primitive mesh; the asset path or "".
    std::string primitiveMeshAsset(const std::string& type, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc,
                                   const Where& w);
    // An imported model file; the parts it produced (cached per file).
    const ModelResult* modelFor(const std::string& resPath, const Where& w);
    // The node tree Godot's importer makes for a glTF model (cached per file); null for other model formats and unreadable files.
    const ModelTree* modelTreeFor(const std::string& resPath, const Where& w);
    std::map<std::string, std::unique_ptr<ModelTree>> modelTrees_;
    const gd::GodotResource* resourceDoc(const std::string& resPath);
    std::map<std::string, std::unique_ptr<gd::GodotResource>> resourceDocs_;
    std::map<std::string, std::string> meshAssets_, materialAssets_, textureAssets_;
    std::map<std::string, ModelResult> models_;
    std::set<std::string> materialStemsUsed_;
    std::set<std::string> materialTextureSources_;   // res:// textures a cooked material already carries
    Var shapeObject(const std::string& type, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w);
    // Environment, Sky, the sky materials and CameraAttributes as `Object(Class, ...)` for Aver.Fabric's WorldEnvironment node (G4 R3).
    Var environmentObject(const std::string& type, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w);
    Var genericResourceDict(const std::string& type, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc,
                            const Where& w, const char* why);
    void convertStandaloneResources();
    void reportProjectFiles();

    // ---- FabricGeometry.cpp (G4 R7) --------------------------------------------------------------------------------
    // A Godot ArrayMesh sub-resource's stored surfaces decoded into one .ocmesh; the asset path or "".
    std::string arrayMeshAsset(const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w);
    // A MultiMesh resource as `Object(MultiMesh, ...)` for Aver.Fabric's MultiMeshInstance3D; nil when it cannot be converted.
    Var multiMeshObject(const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w);
    // What the geometry node classes (MultiMeshInstance3D, Label3D, Sprite3D, the CSG family, GridMap) need beyond their properties: baked mesh and material
    // assets added to `props`, and child nodes (collision bodies, instanced grid items) appended to `synthetic`, which the caller emits below the node.
    void geometryNode(FNode& fn, std::vector<std::pair<std::string, Var>>& props, std::vector<std::unique_ptr<FNode>>& synthetic, ClassChoice& cc);

    // ---- FabricAnimation.cpp (G5 A1) -------------------------------------------------------------------------------
    // A Godot Animation resource (sub-resource or .tres) written as an .ocanim with property tracks; the asset path "Animations/<name>.ocanim" or "".
    std::string animationAsset(const std::string& key, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc,
                               const std::string& nameHint, const Where& w);
    // An AnimationLibrary resource as a dictionary { animation name: Resource("Animations/x.ocanim") } for Aver.Fabric's AnimationPlayer.libraries.
    Var animationLibraryValue(const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w);
    // The AnimationPlayer node's own properties, rated.
    void noteAnimationPlayer(const FNode& fn);
    std::map<std::string, std::string> animationAssets_;
    std::set<std::string> animationStemsUsed_;

    // ---- FabricAudio.cpp (G5 S1) ----
    // A sound file copied to Content/Audio/<its res:// path>; the asset path or "" (reported either way).
    std::string audioFileAsset(const std::string& resPath, const Where& w);
    // The value for an audio file used as a stream: Resource("Audio/x.wav"), or Object(AudioStreamWAV|MP3|OggVorbis, ...) when its import settings loop it.
    Var audioFileValue(const std::string& resPath, const std::string& godotType, const Where& w, const std::string& prop, int line);
    // AudioStreamRandomizer / Playlist / Synchronized / Interactive / Polyphonic / Generator / WAV data as `Object(Class, "prop": value, ...)`.
    Var audioStreamObject(const std::string& type, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w, int line);
    // Rates the properties of AudioStreamPlayer, AudioStreamPlayer3D and AudioListener3D.
    void noteAudioNode(const FNode& fn);
    // A Godot AudioBusLayout written as an .ocbus; the default layout becomes the project's AUDIO.BUSLAYOUT.
    bool convertBusLayout(const std::string& resPath, const gd::GodotResource& r);
    std::map<std::string, std::string> audioAssets_;
    std::string audioBusLayout_;

    // ---- FabricAnimTree.cpp (G5 A2) ----
    // An AnimationNode* / Curve / Curve3D / Gradient resource as `Object(Class, "prop": value, ...)` for Aver.Fabric to build (nested resources resolved, every property rated).
    Var animTreeObject(const std::string& type, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w, int line);
    // Rates the properties of AnimationTree, Skeleton3D, BoneAttachment3D, SkeletonIK3D, Path3D, PathFollow3D, SpringArm3D, RemoteTransform3D and the VisibleOnScreen nodes.
    void noteAnimTreeNode(const FNode& fn);

    // ---- FabricNav.cpp (G5 N1) ----
    // A NavigationMesh resource as `Object(NavigationMesh, "vertices": ..., "polygons": ..., "cell_size": ...)` in Godot space, every property rated; a mesh with no polygons is reported as needing a run-time bake.
    Var navigationMeshObject(const std::string& type, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w, int line);
    // Rates the properties of NavigationRegion3D, NavigationLink3D, NavigationObstacle3D and NavigationAgent3D.
    void noteNavigationNode(const FNode& fn);

    // ---- FabricNet.cpp (G6 NET2) ----
    // A SceneReplicationConfig as `Object(SceneReplicationConfig, "properties": [{"path": NodePath, "spawn": bool, "replication_mode": 0|1|2}, ...])`, every entry rated (4.0 / 4.1 sync + watch become modes).
    Var replicationConfigObject(const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w, int line);
    // A MultiplayerSpawner's `_spawnable_scenes`: the scene paths as the converted level paths, positions kept.
    Var spawnableScenesValue(const Var& v, const FNode& fn, const Prop& pr);
    // Rates the properties of MultiplayerSpawner and MultiplayerSynchronizer and the node as a whole.
    void noteMultiplayerNode(const FNode& fn);

    // ---- FabricUi.cpp (G5 U1) ----
    // A StyleBox* resource as `Object(StyleBoxFlat|Texture|Line|Empty, "prop": value, ...)` for a node's theme_override_styles or a script's value, every property rated.
    Var styleBoxObject(const std::string& type, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w, int line);
    // A Theme resource (a .tres or a sub-resource) written as Content/Themes/<name>.octheme; the asset path "Themes/<name>.octheme" or "".
    std::string themeAsset(const std::string& key, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const std::string& nameHint, const Where& w);
    // A font file copied to Content/Fonts/<its res:// path>; the asset path or "".
    std::string fontAsset(const std::string& resPath, const Where& w);
    // Rates the properties of the Control family (every Control class shares the layout, input, tooltip and theme properties).
    void noteUiNode(const FNode& fn);
    // The project's display/window/stretch/*, gui/theme/custom and gui/timers settings into the project description.
    void convertProjectUi(fmt::ProjectDesc& d);
    std::map<std::string, std::string> themeAssets_, fontAssets_;
    std::set<std::string> themeStemsUsed_;

    // ---- FabricWidgets.cpp (G5 U2) ----
    // LabelSettings, FontVariation, SystemFont, FontFile, ButtonGroup, Shortcut and input events as `Object(...)` values (a ButtonGroup as one shared Resource()), every property rated.
    Var widgetResource(const std::string& type, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w, int line, const std::string& id);
    // Content/UI/Fonts.ocfonts: one family per converted font file, the project's default font first.
    void writeFontSet(const std::string& defaultFontAsset);
    void noteFontAsset(const std::string& asset);
    std::map<std::string, std::string> buttonGroups_;
    std::set<std::string> fontFiles_;

    // ---- FabricScene.cpp -------------------------------------------------------------------------------------------
    bool convertScenes();
    const gd::GodotScene* sceneDoc(const std::string& resPath);
    std::unique_ptr<FNode> buildScene(const std::string& resPath, int depth, const Where& from);
    bool emitLevel(const std::string& sceneRes, FNode& root, fmt::OcWorldData& level, bool isStart);
    // Places `fn` and everything below it; `parent` indexes level.placements (-1 for a top-level node).
    void emitNode(FNode& fn, int parent, const Xform& parentWorld, fmt::OcWorldData& level, LevelScan& scan);
    bool writePrefabs();
    std::set<std::string> instancedScenes_;
    // One node of a scene as the runtime places it, recorded while the scene is written (see writeNodesManifest).
    struct SceneNodeRecord {
        std::string path;                  // from the scene's root ("." for the root itself)
        std::string baseClass;             // the Aver.Fabric class placed
        std::string cls;                   // the class named in the level: the script's class when a script is attached
        std::string script;                // res:// path of the attached script, "" none
        std::string owner;                 // path of the node that owns it ("" for the root): where a %Unique name is looked up
        bool unique = false;
        std::vector<std::pair<std::string, std::string>> packedScenes;   // property -> res:// scene, for the properties a .tscn is assigned to
    };
    std::map<std::string, std::vector<SceneNodeRecord>> sceneNodes_;   // top scene res:// path -> its nodes, in the order written
    std::map<std::string, std::set<std::string>> sceneNodePaths_;
    // G4 R5: sub-viewports. The node whose properties are being converted (a ViewportTexture's viewport_path is relative to that node's scene), the level being
    // emitted (the first part of a view's key) and, per view key, how many materials show it.
    const FNode* emitting_ = nullptr;
    std::string emittingLevel_;
    std::map<std::string, int> viewportUsers_;
    std::map<std::string, std::unique_ptr<gd::GodotScene>> sceneDocs_;
    std::map<std::string, bool> sceneLoadFailed_;
    std::string levelAssetFor(const std::string& sceneRes) const;   // "Maps/props/crate.ocworld"
    std::string startSceneRes_;
};

}  // namespace aver::fabric
