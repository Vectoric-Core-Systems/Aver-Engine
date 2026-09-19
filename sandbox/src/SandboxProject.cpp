// Runtime side: applying a project's manifest and render settings, and the frame budget.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

#include "SandboxApp.hpp"
#include "aver/game/GameTick.hpp"

namespace aver {
void SandboxApp::applyProject(Engine& e) {
    // Never in a capture or headless run: e.window() IS the test, since a headless run has no
    // window and a --frames capture opens one unactivated -- a top-most splash must not land in a
    // screenshot.
    // "\\splash.png", NOT "\splash.png" -- \s is not an escape MSVC knows, so it dropped the
    // backslash (C4129) and asked for "...binsplash.png", which never existed. The loading screen
    // has drawn with no image since it was written; the identical mistake was in GameApp.cpp's
    // script-directory test, both reported by the compiler on every build and never read.
    // A MEMBER, NOT A LOCAL, and that is the fix for it vanishing midway. Scoped to this
    // function it closed when the STAGED work ended -- and the staged work is the head of the
    // load, not the tail: meshes, materials and the first voxelisation keep arriving over the
    // frames after applyProject returns, which is exactly what was still appearing behind a
    // screen that had already gone. It is closed from the frame loop instead, on the same "the
    // draw count stopped changing" rule startupComplete uses for the startup splash.
    projectLoading_ = std::make_unique<LoadingScreen>(
        e, e.window() != nullptr && maxFrames_ == 0, executableDir() + "\\splash.png");
    // Restart the settle detector: this is a NEW load, and whatever the previous scene settled
    // at must not count as this one already being finished.
    startupSettleCount_ = -2;
    startupSettleFrames_ = 0;
    projectLoadingFrames_ = 0;
    LoadingScreen& loading = *projectLoading_;
    loading.stage("Opening project");

    project_ = browser_.project();
    editor::setActorEditorContentRoot(project_.contentDir());

    editor::setAnimEditorContentRoot(project_.contentDir());
    loading.stage("Applying project settings");
    applyProjectRenderSettings();
    // ---- RENDER.EXPOSURE / BLOOM / AUTOEXPOSURE / TONEMAP: THE POST CHAIN A PROJECT COULD NOT
    // AUTHOR. docs/RUNTIME-DEDUP.md carried the gap as one line of owner decision -- "A project
    // cannot author exposure/bloom/tonemap, so a shipped game uses compiled defaults" -- and this
    // is the editor's half of closing it. Every one of these was settable from a command line
    // (--exposure/--bloom/--auto-exposure at SandboxMain.cpp:986-988, --tonemap at :419) and from
    // the Post Process panel (SandboxPanels.cpp:1912-1927), and by nothing the FILE could hold, so
    // the editor showed the afternoon's work and the packaged game rendered rhi::PostSettings'
    // compiled defaults instead. Applying the keys here is what makes the viewport show what the
    // packaged game will show.
    //
    // HERE RATHER THAN IN applyProjectRenderSettings, for exactly the reason the PHYSICS.*/AUDIO.*
    // block below gives at length: everything past that function's two early returns is gated on
    // hasRenderSettings() AND on voxiAttached_, and the whole function is an empty stub with Voxi
    // compiled out (SandboxLevelEdit.cpp:152). post_ is an rhi::PostSettings pushed straight at the
    // device every frame (SandboxApp.cpp:2733); a camera's exposure has nothing to do with whether
    // a GI renderer is attached, and a build without one still has a camera.
    //
    // THE COMMAND LINE OUTRANKS THE MANIFEST, the rule applyProjectRenderSettings' own "COMMAND
    // LINE OUTRANKS THE MANIFEST" block records being broken five separate times. The shape is
    // identical here: setPost (SandboxSelfTests.cpp:198) applies the flags ONCE at startup and this
    // function runs LATER, when a project opens, so an unconditional assignment would silently
    // discard what the human typed. postExposureFromCli_/postBloomFromCli_/postAutoExpFromCli_ are
    // already the "was a flag given" record setPost keeps for loadEditorPreferences' identical
    // problem, so the same three bools answer it here rather than a second, drifting copy.
    //
    // A CAPTURE RUN STILL GETS THE MANIFEST'S EXPOSURE AND BLOOM, deliberately unlike the stored
    // editor preferences, which loadEditorPreferences refuses to restore under --frames: a
    // preference is one machine's editor.ini and would move a gate probe on that machine and not
    // another, while these keys are IN THE PROJECT -- the same bytes for everyone who checks it
    // out, which is the property a baseline needs. Auto exposure is the exception, below.
    if (project_.postExposure >= 0.0f && !postExposureFromCli_)
        post_.exposure = project_.postExposure;
    if (project_.postBloom >= 0.0f && !postBloomFromCli_)
        post_.bloomIntensity = project_.postBloom;
    if (project_.postAutoExposure >= 0 && !postAutoExpFromCli_) {
        post_.autoExposure = project_.postAutoExposure != 0;
        // THE CAPTURE RULE IS CALLED, NOT RE-SPELLED. applyCaptureExposureRule
        // (SandboxSelfTests.cpp:214) owns "a --frames run has auto-exposure off unless the run
        // asked for it by name" and main() calls it at startup -- a manifest key that switched the
        // adaptation back on afterwards would undo that for every capture of that project, and
        // eye adaptation is the one post setting whose value is decided per FRAME from a histogram
        // of the frame before, so a gate would be measuring the warm-up rather than the change
        // under test. Passing false is not a guess: this branch runs only when postAutoExpFromCli_
        // is false, which is precisely the question that parameter asks.
        applyCaptureExposureRule(/*explicitlyRequested=*/false);
        if (project_.postAutoExposure != 0 && !post_.autoExposure)
            AVER_INFO("[Project] RENDER.AUTOEXPOSURE 1 in {} is not applied in a run with a frame "
                      "limit; pass --auto-exposure to ask for it by name", project_.manifestPath);
    }
    // RENDER.TONEMAP HAS NO "WAS --tonemap GIVEN" BOOL TO ASK, which is why it is decided against
    // the compiled default instead: setPost keeps three such bools, setTonemap
    // (SandboxSelfTests.cpp:209) keeps none -- it writes post_.tonemap and returns. Nothing else in
    // the editor writes that field before a project opens (the Post Process panel has no tone-curve
    // control at all, and the post.tonemap console var writes the DEVICE's copy, which onRender
    // overwrites from post_ on the next frame anyway), so "still holds rhi::PostSettings' own 2"
    // (RHI.hpp:217) is a sound stand-in for "no flag was given". IT HAS ONE BLIND CASE AND THE
    // HONEST THING IS TO NAME IT: `--tonemap 2` against a manifest saying 0 or 1 loses, because a
    // flag asking for the default is indistinguishable from no flag. A postTonemapFromCli_ beside
    // the other three would close that, and it is a change to SandboxApp.hpp and setTonemap rather
    // than to this file.
    if (project_.postTonemap >= 0 && post_.tonemap == rhi::PostSettings{}.tonemap) {
        // Clamped to the three curves that exist, the same ceiling the post.tonemap console var
        // applies (EditorConsole.hpp:1069): the parser stores whatever integer the file holds, and
        // a mode nothing implements would select by falling off the end of a switch in the shader.
        post_.tonemap = project_.postTonemap > 2 ? 2u : static_cast<u32>(project_.postTonemap);
    }
    // THE EFFECTIVE VALUES, AFTER PRECEDENCE -- not what the manifest asked for. A log line saying
    // what the file requested would be exactly as reassuring and exactly as useless in the case
    // that matters, which is a flag and a key disagreeing.
    if (project_.postExposure >= 0.0f || project_.postBloom >= 0.0f ||
        project_.postAutoExposure >= 0 || project_.postTonemap >= 0)
        AVER_INFO("[Project] post chain from {}: exposure {:.3f}, bloom {:.3f}, auto-exposure {}, "
                  "tonemap {}", project_.manifestPath, post_.exposure, post_.bloomIntensity,
                  post_.autoExposure ? "on" : "off", post_.tonemap);
    startContentWatch();
    pendingUpgrade_ = editor::inspectProject(project_);
    upgradeAsked_ = false;
    if (!pendingUpgrade_.empty())
        AVER_INFO("[Editor] project '{}' predates {} of this editor's project files; offering to upgrade",
                  project_.name, pendingUpgrade_.fixes.size());
    if (e.window())
        e.window()->setTitle("Aver Engine \xE2\x80\x94 Editor \xE2\x80\x94 " + project_.name);
#if AVER_MODULE_PBR
    loading.stage("Loading materials");
    releaseProjectMaterials();
    content_.adopt(project_);
    loadProjectMaterials();
#endif
#if AVER_MODULE_SCENE
    loading.stage("Loading meshes");
    releaseProjectMeshes(e);
    loadProjectMeshes(e);
#endif
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    loading.stage("Loading particle effects");
    content_.loadProjectParticleEffects();
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
    // Moved here from onRender's per-frame call: pipeline/shader creation belongs at a
    // project-load point, not mid-frame between beginFrame/endFrame, where reentrancy with an
    // in-flight command list was unverified. Runs exactly once regardless (lodMeshPipelineTried_ latches).
    loading.stage("Building virtualized-geometry pipelines");
    ensureLodMeshPipeline(e);
#endif
#if AVER_MODULE_SCENE
    loading.stage("Loading level");
    loadStartMap(e);
#endif
    // ---- PHYSICS.* AND AUDIO.* FROM THE MANIFEST, HERE RATHER THAN IN applyProjectRenderSettings
    // WHAT THE OLD PLACEMENT HID. Both calls used to sit at the bottom of applyProjectRenderSettings
    // -- wholly inside that function's "#if AVER_MODULE_VOXI", and past both of its early returns
    // (`if (!project_.hasRenderSettings()) return;` and `if (!voxiAttached_) ... return;`). So a
    // project that stated PHYSICS.GRAVITY or AUDIO.MASTER but no RENDER.* key at all had them
    // silently discarded by the editor, with nothing logged, and a build with Voxi off discarded
    // them unconditionally -- that configuration's applyProjectRenderSettings is an empty stub
    // (SandboxLevelEdit.cpp:152). Gravity and a mix have nothing to do with whether a GI renderer
    // is attached or whether this manifest happens to mention one.
    //
    // THE RUNTIME WAS ALREADY RIGHT AND THE EDITOR, THE NOMINAL BEHAVIOUR REFERENCE, WAS NOT:
    // GameApp::openProject applies both unconditionally (GameApp.cpp, "That coupling is not
    // copied"), and docs/RUNTIME-DEDUP.md records this as the one place where the two hosts
    // disagreed in the editor's disfavour.
    //
    // THE POSITION IS COPIED FROM THE RUNTIME ON PURPOSE, not landed wherever it fit: GameTick.hpp
    // says outright that it decides nothing about WHEN a host calls these, so matching by accident
    // would be worth nothing. openProject applies them after its level load and after
    // applyProjectRenderSettings, and before initScripting/spawnClassPlacements -- which is this
    // line. Physics has already been STARTED by then in both hosts (SandboxApp::onInit calls
    // game::startPhysics before any project can open, as GameApp::onInit does before openProject),
    // and applyProjectPhysics asks aver_phys_ready() itself rather than trusting that, so a world
    // that does not exist yet produces "will apply once the world exists" instead of silence.
#if AVER_MODULE_PHYSICS
    game::applyProjectPhysics(project_);
#endif
// AVER_WITH_AUDIO_ABI, not a plausible-looking AVER_MODULE_AUDIO -- there is no such macro,
// and an #if on one compiles this whole block to nothing while the build stays green. It means
// "this build links the mixer seam", and it is defined on every host that does.
//
// IT USED TO BE CALLED AVER_SOUND_EDITOR_AUDIO, and the name was the bug. Named after a TAB IN THE
// EDITOR, it was defined on exactly one target, so the game runtime was excluded from the audio
// device by a macro nobody read as a capability -- a packaged game was silent and Audio.Load
// succeeded into nothing. Renamed for what it actually gates.
#if AVER_WITH_AUDIO_ABI
    game::applyProjectAudioMix(project_);
#endif
#if AVER_MODULE_SCRIPTING
    loading.stage("Starting scripts");
    if (scripts_.ready() && scriptsDir_.empty() && project_.valid()) {
        const std::string bin = editor::scriptsBinaryDir(project_);
        const i32 n = scripts_.loadScripts(bin);
        if (n > 0) AVER_INFO("[Scripting] {} project behaviour(s) live from {}", n, bin);
        else AVER_INFO("[Scripting] no built scripts in {} - use Tools > Reload Scripts", bin);
    }
    // GRAPH-AS-CLASS: registration, for composition-root parity with GameApp.cpp's own
    // initScripting() call -- a seam wired in one root and not the other is the exact defect
    // aee2404 exists to fix. Independent of scriptsDir_/the --scripts override: a graph class
    // lives under the project's CONTENT directory, declared regardless of which C# source loaded.
    if (scripts_.ready() && project_.valid()) {
        const i32 graphClasses = scripts_.declareGraphClasses(project_.contentDir());
        if (graphClasses > 0)
            AVER_INFO("[Graph] {} graph class(es) declared from '{}'", graphClasses, project_.contentDir());
    }
#endif
#if AVER_MODULE_FRAMEWORK
    // AFTER graph classes are declared, and AFTER "Loading level" already collected
    // level_.classPlacements(). GATED ON scripts_.ready(), unlike GameApp: a project opened from the
    // COMMAND LINE reaches this point BEFORE the scripting host bootstraps (see the "GRAPH-AS-
    // CLASS CATCH-UP" comment), and spawning before any class could be declared would only
    // manufacture "not declared" warnings.
    // scripts_.ready() is itself under AVER_MODULE_SCRIPTING, separate from the
    // AVER_MODULE_FRAMEWORK this block sits inside -- FRAMEWORK's only enforced dependency is
    // SCENE, not SCRIPTING. A scripting-off tree has no graph classes declared either way, so the
    // call below degrades to the same harmless path.
#if AVER_MODULE_SCRIPTING
    if (scripts_.ready())
#endif
        spawnClassPlacements();
#endif
}

#if AVER_MODULE_VOXI
// Pushes the manifest's render settings into Voxi. Defers until the device info is known.
// The manifest's voxi::Settings half, SPLIT OUT so it can run BEFORE VoxiRenderer::init().
//
// WHY IT HAS TO RUN FIRST. init() calls createVoxelVolume(settings_.voxelResolution) and that is
// the only place the volume is ever created -- nothing in the renderer watches voxelResolution
// afterwards. Applying the manifest after init therefore wrote 512 into the struct and left a
// 128^3 volume running: a project asking for Epic GI got a grid 64x smaller than it stated, and
// said "applied render settings" while doing it. Seeding the settings before init means the
// volume is simply CREATED at the right size, so there is no resize, no descriptor rebind, and
// no GPU resource torn down mid-frame -- the failure mode that has removed this device twice.
//
// SAFE TO CALL BEFORE ATTACH, deliberately: it touches only the voxi::Renderer singleton's
// settings, never voxiRenderer_ or the device. That is the same thing the startup block does
// before init.
void SandboxApp::applyProjectVoxiSettings() {
    if (!project_.valid() || !project_.hasRenderSettings()) return;

    voxi::Renderer& vx = voxi::Renderer::get();

    // TWO-PHASE MANIFEST APPLY, now the shared helper both hosts use (Lane 2,
    // ProjectRenderApply.hpp) rather than this file's own hand-rolled copy: tiers committed and
    // read back FIRST, for the reason this function used to document at length here -- setSettings
    // decides "did the caller set this" BY VALUE, so a knob asked for at the value it already
    // holds is indistinguishable from one never asked for, and the tier's derived rung silently
    // wins if both land in the same call. Then every knob, against the POST-derivation tiers, and
    // committed again. See applyManifestTwoPhase's own comment for the exact R2 regression this
    // ordering exists to avoid, and applyManifestKnobs' for the N6 fix riding along in the same
    // call: a manifest that states a tier but omits one of its nine derived knobs now follows
    // whatever THIS commit's tier resolved to, not whatever was already live before this project
    // opened.
    voxi::Settings x = vx.settings();
    voxi::applyManifestTwoPhase(project_, x, [&]() { vx.setSettings(x); x = vx.settings(); });

    // The GI volume, which is not a voxi::Settings field at all -- it is set through
    // setVolume(centre, extent), so it has to be applied separately from the knob block above.
    if (project_.hasGiVolume) {
        giCenter_ = Vec3{project_.giCenter[0], project_.giCenter[1], project_.giCenter[2]};
        giExtent_ = project_.giExtent;
    }
}

void SandboxApp::applyProjectRenderSettings() {
    if (!project_.valid()) return;

    // MIRRORS, NOT A DELTA -- which is why these two run BEFORE the hasRenderSettings() guard
    // rather than beside the knobs they look like they belong with. Everything past that guard
    // applies a manifest key only when the key is stated (the "-1 means unstated" rule), so a
    // project that states nothing DELIBERATELY inherits the live value. These two are the
    // opposite: projectBackend_ is the Project Settings "Renderer" dropdown's backing field and
    // frameBudgetMs_ is a live controller target, and each is supposed to say what THIS project
    // asked for. Behind the guard, opening a project with no RENDER.* keys at all left the
    // PREVIOUS project's backend showing in the dropdown and its frame budget still throttling
    // GI -- and because the Rendering page writes the whole block back (project_.backend =
    // projectBackend_, below), the next edit to any control on that page baked the inherited
    // backend into a manifest that had never asked for it.
    projectBackend_ = project_.backend;
    // NOT WHEN --frame-budget SAID SO. This runs on every project apply, and the manifest's
    // default is -1, so an unconditional assignment silently switched the flag back off one
    // frame after it was set -- the controller was wired, reachable and never once ran.
    if (!frameBudgetForced_) frameBudgetMs_ = project_.frameBudgetMs;
#if AVER_MODULE_SR
    // optimisation-wave-2, 3.3 A: MIRRORS, NOT A DELTA, for the SAME reason projectBackend_ and
    // frameBudgetMs_ are up here rather than below the hasRenderSettings() guard -- but unlike
    // those two (and unlike occlusionCullEnabled_ down in the guarded block, whose -1 means
    // "unstated, keep whatever was already live"), -1 here is ITSELF the project's real, explicit
    // default ("follow the Overall preset"), not a sentinel for "nothing to apply". A project with
    // no RENDER.AVERSR key must reset this back to -1, unconditionally, or a project opened right
    // after one that pinned a level would silently inherit that teammate's pin.
    averSrProjectDefault_ = project_.averSr;
#endif

    if (!project_.hasRenderSettings()) return;
    if (!voxiAttached_) { projectRenderPending_ = true; return; }
    projectRenderPending_ = false;

    // Idempotent by the time we get here on the startup path -- the settings were already seeded
    // before init(). Still called, because the mid-session project-open path reaches this
    // function with a device that has been attached for a while.
    applyProjectVoxiSettings();
    voxi::Renderer& vx = voxi::Renderer::get();

    // THE THREE THAT ARE NOT VOXI SETTINGS. LOD select, occlusion culling and the depth
    // pre-pass are editor-side per-frame flags rather than fields on voxi::Settings, so they
    // are applied directly rather than through setSettings. Same manifest keys, same -1 means
    // unstated rule.
    if (project_.lodSelect >= 0)
        setLodSelect(project_.lodSelect != 0,
                     project_.lodThresholdPx >= 0.0f ? project_.lodThresholdPx : lodErrorThresholdPx_);
    else if (project_.lodThresholdPx >= 0.0f)
        setLodSelect(lodSelectEnabled_, project_.lodThresholdPx);
    if (project_.occlusionCull >= 0) occlusionCullEnabled_ = project_.occlusionCull != 0;
    // projectBackend_ and frameBudgetMs_ were applied above the hasRenderSettings() guard --
    // see the comment there for why they cannot live down here with the rest of the block.
    if (project_.depthPrepass  >= 0) depthPrepassOverride_ = project_.depthPrepass != 0;

    // PHYSICS.* AND AUDIO.* USED TO BE APPLIED HERE, and that is why they now are not: everything
    // from this point on is reached only past the hasRenderSettings() and voxiAttached_ returns
    // above, which is the correct gate for a Voxi knob and the wrong one for gravity and a master
    // volume. Both calls moved up into applyProject -- see the block there for what the coupling
    // was hiding.

    // ---- THE COMMAND LINE OUTRANKS THE MANIFEST, AND UNTIL NOW IT DID NOT ----
    // THE BUG THIS FIXES: CLI overrides apply ONCE at startup, but this function runs LATER when a
    // project opens and overwrites whatever the flags asked for. `--rt-render-mode 0` on a
    // manifest saying RENDER.RTRENDERMODE 1 silently ran in mode 1, WITHOUT DIAGNOSTIC, and a
    // raster-versus-ray-driven comparison came back "pixel-identical" for the excellent reason
    // that both halves were ray-driven.
    // THE SECOND INSTANCE OF THIS DEFECT: the first was --pt-scene (e2830db). Fixing one flag and
    // not the rule left every other RENDER.* knob carrying the same bug; this closes the rule.
    // A FLAG IS AN INSTRUCTION FROM A HUMAN STANDING RIGHT THERE; a manifest is a recorded
    // preference. When they disagree the human wins, out loud.
    //
    // CAPTURED HERE, BEFORE the override block below can touch either field: the A2 conflict
    // check further down needs to tell "the manifest alone asks for this" apart from "a flag is
    // what produced this", and the only honest way to do that is to remember what the manifest
    // (applyProjectVoxiSettings(), already applied above) resolved to BEFORE any flag gets a say.
    const u32  manifestRtRenderMode  = vx.settings().rtRenderMode;
    const bool manifestPathTracingOn = vx.settings().pathTracing != voxi::Quality::Off;
    // ---- PHASE A (N7): TIER FLAGS AND FORCE-OFFS, COMMITTED ALONE, FIRST ----
    // THE BUG THIS SPLIT FIXES. A tier flag (--gi/--rt/--pt) and a knob flag for THAT SAME tier
    // (e.g. --rt-rays) used to land in one merged setSettings call below. take()'s own "ignore an
    // override already equal to the live value" rule reads the knob against the OLD tier's live
    // value -- so an explicit --rt-rays 4 that happened to equal the OLD tier's own rtShadowRays
    // looked like a no-op to take(), was never marked overridden, and rode into that one
    // setSettings() call still at its pre-flag value. setSettings' own tier-derivation then saw
    // the tier change AND an "untouched" knob in the SAME call and rederived the knob to the NEW
    // tier's ladder rung, silently discarding the 4 the flag asked for: PTTest (RENDER.RAYTRACING
    // 4, RENDER.RTSHADOWRAYS 4) opened with `--rt 1 --rt-rays 4` lost the explicit 4 to Low's
    // derived 1, with nothing logged, purely because the coincidence made the flag look unchanged.
    // Committing the tier ALONE here means Phase B below reads back settings that already carry
    // the NEW tier's derived knobs, so its take() calls compare each flag against those -- the
    // same coincidence can no longer hide an override.
    {
        auto k = vx.settings();
        bool overridden = false;
        const auto take = [&](int ov, u32& dst, const char* name) {
            if (ov < 0 || static_cast<u32>(ov) == dst) return;
            AVER_INFO("[Sandbox] {}: the command line asked for {} and the project manifest for "
                      "{}; the command line wins", name, ov, dst);
            dst = static_cast<u32>(ov);
            overridden = true;
        };
        // THE TIER KNOBS TOO: `--gi 2` against RENDER.GI 4 was still silently discarded an hour
        // after the "rule" was supposedly closed. A rule with five of nine cases is not a rule.
        // These three used to carry a 0-means-absent sentinel, so --gi 0/--rt 0/--pt 0 could not be
        // expressed either; they now use -1 like every other override here. `--pt 0` in particular
        // had to work before a raster-versus-ray-driven measurement could mean anything.
        take(giOverride_, reinterpret_cast<u32&>(k.globalIllumination), "--gi");
        take(rtOverride_, reinterpret_cast<u32&>(k.rayTracing),         "--rt");
        take(ptOverride_, reinterpret_cast<u32&>(k.pathTracing),        "--pt");
        // THE THIRD INSTANCE, predicted above: --no-gi/--no-rt are BOOLEANS, not the -1-sentinel
        // integers `take` understands, so closing the rule for integers left these two behind.
        // Measured cost: on RENDER.RAYTRACING 4, `--no-rt` was silently discarded and an A/B built
        // on it said ray tracing cost -0.3ms (appeared FASTER to turn on) when the real answer was
        // 6.7ms. A silently-failing override manufactures a wrong conclusion, confidently.
        // MOVED UP INTO THIS SAME PHASE (N7): forceOff changes a TIER exactly like take(giOverride_,
        // ...) above does, so it belongs beside the other tier changes, committed before any knob
        // flag is ever compared against what follows from it.
        const auto forceOff = [&](bool want, voxi::Quality& dst, const char* name) {
            if (!want || dst == voxi::Quality::Off) return;
            AVER_INFO("[Sandbox] {}: the command line asked for Off and the project manifest for "
                      "{}; the command line wins", name, static_cast<int>(dst));
            dst = voxi::Quality::Off;
            overridden = true;
        };
        forceOff(giForceOff_, k.globalIllumination, "--no-gi");
        forceOff(rtForceOff_, k.rayTracing,         "--no-rt");
        // COMMIT THE TIERS ALONE. Phase B below re-reads vx.settings() fresh, so whatever
        // setSettings just derived from a tier change here (rtShadowRays, giUpdateInterval, and
        // the rest of the nine tier-derived knobs) is what Phase B's take() calls are compared
        // against -- not the pre-flag values this block started from.
        if (overridden) vx.setSettings(k);
    }

    // ---- PHASE B (N7): EVERY KNOB FLAG, AGAINST THE POST-TIER SETTINGS ----
    // Read AFTER Phase A's commit, deliberately -- see that phase's own comment for why. THIS
    // CALL TRULY HAS NO TIER CHANGES IN IT (an earlier version of this comment claimed that for
    // the single merged call above and was wrong -- the tier takes used to live in this same
    // block): nothing below ever touches k.globalIllumination/rayTracing/pathTracing again, so
    // setSettings' own change-gated derivation sees no tier change here and leaves every knob this
    // block writes exactly as written.
    {
        auto k = vx.settings();
        bool overridden = false;
        const auto take = [&](int ov, u32& dst, const char* name) {
            if (ov < 0 || static_cast<u32>(ov) == dst) return;
            AVER_INFO("[Sandbox] {}: the command line asked for {} and the project manifest for "
                      "{}; the command line wins", name, ov, dst);
            dst = static_cast<u32>(ov);
            overridden = true;
        };
        // Wired in from the start rather than after it bites: this is the third knob-shaped
        // feature added since that rule was written, and the previous two both had to be fixed
        // afterwards (--pt-scene in e2830db, --no-rt/--no-gi in fac36a3).
        take(rtRenderModeOverride_,    k.rtRenderMode,       "--rt-render-mode");
        take(refractionOverride_,      k.refractionMode,     "--refraction");
        take(rtShadowDenoiseOverride_, k.rtShadowDenoise,    "--rt-shadow-denoise");
        take(ptBouncesOverride_,       k.ptBounces,          "--pt-bounces");
        take(layeredBsdfOverride_,     reinterpret_cast<u32&>(k.layeredBsdf), "--layered-bsdf");
        // --msaa WAS THE NINTH CASE, and it was still missing. The block above says outright
        // that "a rule with five of nine cases is not a rule" -- this is the one that was left.
        // It is applied at startup (msaaOverride_ is read where settings are first built) and
        // then SILENTLY OVERWRITTEN by RENDER.MSAA when the project opens, so `--msaa 1` against
        // a manifest saying 2 measured the manifest and said nothing. Found while trying to
        // price MSAA in ray-driven mode, where the pass is one fullscreen triangle and gains
        // nothing from multisampling -- a measurement that would have been quietly meaningless.
        if (msaaOverride_ > 0)           take(msaaOverride_, reinterpret_cast<u32&>(k.msaa), "--msaa");
        if (rtRaysOverride_ > 0)         take(rtRaysOverride_,        k.rtShadowRays,      "--rt-rays");
        // WIRED IN WITH THE FLAG, not after it bites -- the block above records four separate
        // occasions where a knob was added and this list was not updated, each one silently
        // letting a manifest outrank the command line and each one corrupting a measurement
        // before anyone noticed. No `if` guard: take() already treats a negative as absent, and
        // this override's sentinel IS -1 precisely so that 0 stays expressible.
        take(giSkyOccRaysOverride_,      k.giSkyOcclusionRays, "--gi-sky-occlusion-rays");
        take(giSkyOccTileOverride_,      k.giSkyOcclusionTile, "--gi-sky-occlusion-tile");
        // --gi-intensity F, the multiplier on the cone-traced bounce (gVoxelParams.y). Another
        // slider with no command-line twin, so "is the bounce strong enough" could not be swept
        // -- and that is the question behind "the bounce lighting isn't working". Applied here,
        // last, so it outranks RENDER.GIINTENSITY from the manifest like every other flag.
        if (giIntensityOverride_ >= 0.0f) {
            if (k.giIntensity != giIntensityOverride_)
                AVER_INFO("[Project] --gi-intensity {} outranks the recorded {}",
                          giIntensityOverride_, k.giIntensity);
            k.giIntensity = giIntensityOverride_;
            // `overridden` GATES THE PUSH AT THE BOTTOM OF THIS BLOCK -- `if (overridden)
            // vx.setSettings(k)` -- and take() sets it for you. Writing k directly without this
            // line makes the whole override a no-op whenever no OTHER flag happens to be
            // present, and it fails exactly the way this block's own comments describe: the log
            // says the flag outranked the manifest, and nothing changes.
            //
            // THE FIFTH INSTANCE OF THE BUG THIS BLOCK DOCUMENTS, and it was written here, in
            // this commit, directly underneath four paragraphs warning about it. It survived
            // review and was caught only by a measurement: --gi-intensity 0, 2 and 4 produced
            // three byte-identical images. The log line above is what makes that debuggable --
            // a flag that claims to have won and then loses is the shape being guarded against.
            overridden = true;
        }
        if (rtPixelsPerRayOverride_ > 0) take(rtPixelsPerRayOverride_, k.rtPixelsPerRayTile, "--rt-pixels-per-ray");
        // THE FOURTH INSTANCE OF THE SAME BUG CLASS, closed. --gi-update-interval had no
        // manifest key AND no entry here, while giUpdateInterval IS tier-derived inside
        // setSettings -- so opening a project whose RENDER.GI differed from the live tier
        // silently re-derived over the flag with nothing logged. The three fixes before this one
        // each closed a subset and left this behind; the rule is that a flag exists so a human
        // at the keyboard can override recorded state, which means it is applied LAST and a
        // disagreement is said out loud.
        if (giUpdateIntervalOverride_ > 0) take(giUpdateIntervalOverride_, k.giUpdateInterval, "--gi-update-interval");
        // --gi-mode HAS a manifest key (RENDER.GIMODE) and, until this line, no entry here -- the
        // same gap the paragraph above just closed for --gi-update-interval, on the flag this task
        // exists to add. Concretely: PTTest.ocproject records GIMODE 1, so without this take(),
        // `--gi-mode 0` opening that project would be silently re-outranked by the recorded 1 and
        // an A/B meant to compare voxel cones against RTXDI ReSTIR would compare ReSTIR to itself
        // and report the two estimators as identical.
        if (giModeOverride_ >= 0) take(giModeOverride_, k.giMode, "--gi-mode");
        // --restir-visibility HAS a manifest key (RENDER.RESTIRVISIBILITY) and, without this line,
        // no entry here -- the identical gap the paragraph above just closed for --gi-mode, on
        // optimisation-wave-2's own U1 flag. Beside --gi-mode rather than in its own block: both
        // read the RESTIR estimator's own behaviour and a by-hand A/B against a manifest that
        // already pins one needs the flag to win the same way --gi-mode's does.
        if (restirVisibilityOverride_ >= 0) take(restirVisibilityOverride_, k.giRestirVisibility, "--restir-visibility");
        // take() on a bool field needs an lvalue of the field's own type, so the flag is staged
        // through a u32 and assigned back -- RENDER.DENOISER must not outrank a human who just
        // typed --denoiser, which is the whole point of this pass.
        if (denoiserOverride_ >= 0) {
            u32 den = k.denoiser ? 1u : 0u;
            take(static_cast<u32>(denoiserOverride_ != 0), den, "--denoiser");
            k.denoiser = den != 0;
        }
        if (overridden) vx.setSettings(k);
    }

    // A2: A SELF-CONTRADICTORY MANIFEST NEVER SAID SO. RENDER.RTRENDERMODE 1 (ray-driven primary
    // visibility) and a RENDER.PATHTRACING level that is not Off can both be recorded in the same
    // .ocproject -- PTTest.ocproject does exactly this. Only one can ever paint the scene: the
    // beginFrame() election awards it to the first registered feature whose suppressesScene() is
    // true, and Voxi (ray-driven) always registers before PtSceneView can (see
    // syncPtSceneView()'s own comment), so ray-driven always wins. Until now nothing said so at
    // load time -- the Path Tracing page's Quality combo just silently did nothing (see its
    // ptSceneViewSuppressedByRayDriven_ tag, added alongside this).
    //
    // checkPtRtConflict() (PtRenderConflict.hpp) is pure and takes the EFFECTIVE settings, i.e.
    // vx.settings() AFTER the flag>manifest block immediately above already resolved precedence --
    // not the raw project_.rtRenderMode/project_.pathTracing manifest fields. A `--rt-render-mode
    // 0` or `--pt 0` that already fixed the contradiction must never be reported as one; that
    // would be exactly the silent-then-wrong-message shape this file's own history warns about
    // (see the block above's "COMMAND LINE OUTRANKS THE MANIFEST" comment).
    //
    // decidedByCli is computed from manifestRtRenderMode/manifestPathTracingOn (captured BEFORE
    // the override block, above) actually CHANGING -- not merely from a flag being present. A
    // redundant `--rt-render-mode 1` against a manifest that already said 1 must still read as
    // "the manifest says this", not "the command line decided it": nothing the human typed moved
    // this outcome away from what the file alone already produced.
    {
        const voxi::Settings& fs = vx.settings();
        const bool rtRenderModeChangedByCli  = fs.rtRenderMode != manifestRtRenderMode;
        const bool pathTracingOnChangedByCli =
            (fs.pathTracing != voxi::Quality::Off) != manifestPathTracingOn;
        const aver::editor::PtRtConflict conflict = aver::editor::checkPtRtConflict(
            fs.rtRenderMode, fs.pathTracing != voxi::Quality::Off,
            rtRenderModeChangedByCli, pathTracingOnChangedByCli);
        if (conflict.conflicts) {
            if (conflict.decidedByCli)
                AVER_WARN("[Project] {} asks for both ray-driven primary visibility "
                          "(RENDER.RTRENDERMODE 1) and Path Tracing (RENDER.PATHTRACING {}); "
                          "the command line decided ray-driven wins and paints the scene, so "
                          "Path Tracing's view will not be shown. Pass --rt-render-mode 0 (or "
                          "--pt 0) to see it instead.",
                          project_.manifestPath, static_cast<int>(fs.pathTracing));
            else
                AVER_WARN("[Project] {} sets RENDER.RTRENDERMODE 1 AND RENDER.PATHTRACING {} -- "
                          "only one can paint the scene, and ray-driven primary visibility wins "
                          "the election (it registers before Path Tracing's view ever can), so "
                          "Path Tracing's view will never be shown. Set RENDER.RTRENDERMODE 0 in "
                          "the manifest, or switch Ray Tracing > \"Finds the first surface\" to "
                          "Rasteriser in the Rendering settings, to see it instead.",
                          project_.manifestPath, static_cast<int>(fs.pathTracing));
        }
    }

    // MANIFEST CONTRADICTIONS (section 2's load-time report rule, RenderSettingsResolver.hpp):
    // once per project apply, warn about any of the four fields the manifest actually STATED
    // (project_.X >= 0) whose EFFECTIVE value -- after everything above, flag precedence and
    // hardware clamping included -- does not match what was asked. A RT-Off manifest that also
    // pins RENDER.GIMODE 1 is the case this exists for: F-d (Lane 1) stores giMode as requested
    // rather than clamping it, so nothing else in this file would ever say the pin is not actually
    // running. A reason that maps to a Feature refuse() ALREADY logged this same apply
    // (vx.refusalLogged) is skipped -- see refusalFeatureFor's own comment for exactly which
    // reasons that covers -- so a device limitation is never reported twice from two call sites.
    {
        voxi::FieldReport reports[4];
        const voxi::ManifestAsks asks{project_.giMode, project_.denoiser, project_.rtRenderMode,
                                      project_.refractionMode};
        const u32 nReports =
            voxi::manifestContradictions(vx.settings(), vx.deviceInfo(), asks, reports);
        for (u32 i = 0; i < nReports; ++i) {
            voxi::Feature feature;
            if (voxi::refusalFeatureFor(reports[i].reason, feature) && vx.refusalLogged(feature))
                continue;
            AVER_WARN("[Project] {} sets RENDER.{} {}, but {} The recorded value is not what is "
                      "actually running.",
                      project_.manifestPath, reports[i].manifestKey, reports[i].requested,
                      voxi::disableReasonText(reports[i].reason));
        }
    }

    voxiRenderer_.setSettings(vx.settings());
    // A manifest that names Path Tracing explicitly should actually (de)register PtSceneView at
    // load: PathTracing needs an explicit register/unregister step (syncPtSceneView(), called
    // later this frame or on the next onUpdate() for a project opened mid-session via this same
    // function at line ~4615). Guarded on
    // project_.pathTracing >= 0 -- i.e. actually PRESENT -- so a project stating nothing never
    // silently overrides a view --pt-scene or the settings combo already asked for.
    // A COMMAND-LINE FLAG OUTRANKS THE MANIFEST, AND NEITHER IS ALLOWED TO BE SILENT.
    // The paragraph above guarded RENDER.PATHTRACING being ABSENT, not PRESENT-and-Off: that
    // overwrote the want flag with no log line, so `--pt-scene` against such a project silently
    // rendered raster while claiming otherwise -- wrong in the plausible-looking, expensive way.
    // Precedence stays the same as everywhere else: an explicit CLI flag wins over stored project
    // state; the settings combo and toggle-test flags are NOT covered by ptSceneViewFromCli_,
    // since those are live edits a mid-session project open should still override.
    if (project_.pathTracing >= 0) {
        const bool want = (vx.settings().pathTracing != voxi::Quality::Off);
        if (ptSceneViewFromCli_ && !want) {
            // ASSERTED, not merely left alone: an earlier version only declined to write the
            // manifest value, which is not the same -- anything that had set the flag false in
            // between left the view off while this line claimed the command line had won.
            ptSceneViewWantEnabled_ = true;
            AVER_WARN("[Project] RENDER.PATHTRACING in {} asks for Path Tracing Off, but "
                      "--pt-scene was given -- the command line wins and the path-traced view "
                      "stays on", project_.manifestPath);
        } else {
            if (want != ptSceneViewWantEnabled_)
                AVER_INFO("[Project] RENDER.PATHTRACING {} the path-traced view",
                          want ? "enables" : "disables");
            ptSceneViewWantEnabled_ = want;
        }
    }
    AVER_INFO("[Project] applied render settings from {}", project_.manifestPath);
}

#endif

void SandboxApp::frameBudgetTick(f32 dt, voxi::Settings& vs) {
    // OFF IN CAPTURE RUNS unless asked for by name. A controller that retunes quality mid-run
    // makes every bounded measurement incomparable, which is most of how this engine is
    // checked. --frame-budget is the way to exercise it in one anyway, including in a gate.
    frameBudget_.budgetMs = frameBudgetMs_;
    if (maxFrames_ != 0 && !frameBudgetForced_) { frameBudget_.rung = 0; return; }
    // The warm-up, hitch rejection, averaging, hysteresis and rung table are the shared
    // controller's (FrameBudget.hpp), identical in the standalone runtime.
    if (voxi::frameBudgetTick(frameBudget_, dt, vs))
        AVER_INFO("[Sandbox] frame budget {:.1f}ms: {:.1f}ms average -> rung {} "
                  "(GI every {} frame(s), {} cone(s))",
                  frameBudget_.budgetMs, frameBudget_.avgMs, frameBudget_.rung, vs.giUpdateInterval,
                  vs.giCones);
}

} // namespace aver
