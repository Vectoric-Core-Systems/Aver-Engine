// Editor: opening, creating and saving levels, landscape authoring records, project manifest capture and save.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

#include "SandboxApp.hpp"

namespace aver {
#if AVER_MODULE_LANDSCAPE
// Makes this level a landscape it does not have: synthesises tile (0,0) from the panel's own
// noise parameters, writes it to Content/Landscape/<level>.ocland, loads it, and RECORDS IT IN
// THE LEVEL so the next open finds it.
//
// WHY THIS DID NOT EXIST. A landscape could only become resident three ways -- a file already
// sitting next to the level under the <levelname>.ocland convention, the --landscape CLI
// override, or a LANDSCAPE record a person hand-wrote into the .ocworld. The Landscape editor
// mode, faced with a level that had none, printed "This level has no landscape section." and
// rendered no controls at all: the one mode whose whole job is terrain could not make any.
//
// synthesizeTerrainTile is the SAME generator the streaming ring already uses for the tiles
// around an authored section, so a created section and its neighbours come out of one function
// rather than two that have to agree.
bool SandboxApp::createLandscapeForLevel(rhi::IDevice* device, u32 samples, f32 spacingCm) {
    if (!project_.valid()) { setUpgradeStatus("Open a project first.", editor::NotifySeverity::Warning); return false; }
    const std::string stem = levelName_.empty() ? std::string("untitled") : levelName_;
    const std::filesystem::path dir = std::filesystem::path(project_.contentDir()) / "Landscape";
    const std::filesystem::path dst = dir / (stem + ".ocland");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    fmt::OcLandData data;
    const f32 tileSizeCm = static_cast<f32>(samples - 1) * spacingCm;
    if (!landscape::synthesizeTerrainTile(landscape::TileCoord{0, 0}, 0.0f, 0.0f, tileSizeCm,
                                          samples, landscapeNoiseParams_, data)) {
        setUpgradeStatus("Could not synthesise a landscape at that size.", editor::NotifySeverity::Error);
        return false;
    }
    std::string why;
    if (!fmt::saveOcLand(dst.string(), data, &why)) {
        AVER_ERROR("[Landscape] could not write '{}': {}", dst.string(), why);
        setUpgradeStatus("Could not write " + dst.filename().string(), editor::NotifySeverity::Error);
        return false;
    }
    if (!loadLandscape(device, dst.string())) {
        setUpgradeStatus("Wrote " + dst.filename().string() + " but could not load it back.", editor::NotifySeverity::Error);
        return false;
    }
    recordLandscapeInLevel();
    cbInvalidate(dir.string());
    setUpgradeStatus("Created " + dst.filename().string());
    AVER_INFO("[Landscape] created {} ({}x{} samples, {:.0f}cm spacing, {:.0f}cm across)",
              dst.string(), samples, samples, spacingCm, tileSizeCm);
    return true;
}

// Makes the level's LANDSCAPE record agree with what is actually resident.
//
// THE EDITOR COULD NEVER WRITE THIS RECORD. saveLevel copies levelHeader_ wholesale, so a
// landscape that arrived through the <levelname>.ocland naming convention or through
// --landscape was never named in the file -- the level looked terrain-less to anything but this
// machine's directory listing, and moving the project broke it silently.
void SandboxApp::recordLandscapeInLevel() {
    if (!landscapeLoaded_ || landscapePath_.empty() || !project_.valid()) return;
    std::error_code ec;
    std::string rel = std::filesystem::relative(landscapePath_, project_.contentDir(), ec).string();
    if (ec || rel.empty()) rel = landscapePath_;
    for (char& c : rel) if (c == '\\') c = '/';

    fmt::OcLandscapePlacement lp;
    if (!levelHeader_.landscapes.empty()) lp = levelHeader_.landscapes.front();   // keep name/material
    lp.section = rel;
    if (lp.name.empty()) lp.name = levelName_.empty() ? std::string("Landscape") : levelName_;
    lp.x = landscapeData_.originCm[0];
    lp.y = landscapeData_.originCm[1];
    lp.z = landscapeData_.originCm[2];
    levelHeader_.landscapes.assign(1, std::move(lp));
}

// Writes the in-memory section back to the path it was loaded from. A sculpt is fully functional
// in memory without this; it's the one place edits actually reach disk. Silent no-op if there is
// nothing loaded or nowhere to write it, matching the Save Landscape menu item's own guard.
void SandboxApp::saveLandscape() {
    if (!landscapeLoaded_ || landscapePath_.empty()) return;
    std::string why;
    if (fmt::saveOcLand(landscapePath_, landscapeData_, &why)) {
        landscapeDirty_ = false;
        AVER_INFO("[Landscape] saved '{}'", landscapePath_);
        // Saving is the natural commit point for a sculpt, so it is where the collision snapshot
        // catches up with the heights -- see rebuildLandscapeCollision on why this is not done
        // per stroke.
        rebuildLandscapeCollision();
    } else {
        AVER_ERROR("[Landscape] could not save '{}': {}", landscapePath_, why);
    }
}

#endif

#if AVER_MODULE_VOXI
// Copies the controls' requested values into the manifest struct, before the renderer clamps them.
// `overallFollowMask` is whatever applyOverall wrote THIS SAME EDIT (0 when no Overall Quality
// button ran) -- forwarded straight through to captureVoxiSettings' own follow-the-tier rule, so a
// group an Overall preset just moved is captured as "follow the tier", not as nine explicit pins.
void SandboxApp::captureRenderSettingsFromUi(const voxi::Settings& requested, u32 overallFollowMask) {
    voxi::Renderer& vx = voxi::Renderer::get();
    // EVERY VOXI-OWNED FIELD NOW GOES THROUGH THE ONE SHARED CAPTURE RULE (Lane 2,
    // ProjectRenderApply.hpp), replacing this function's own hand-rolled copy -- including the
    // voxelResolution-only "still following the tier" special case this function used to carry
    // alone: captureVoxiSettings' captureKnob applies the identical test to all NINE tier-derived
    // knobs (voxelResolution, giCones, giUpdateInterval; rtShadowRays, rtPixelsPerRayTile,
    // rtShadowDenoise, rtRenderMode, refractionMode; ptBounces), not just this one. `live` is
    // voxi::Renderer::get().settings() -- the settings from BEFORE this edit -- which is why this
    // call must run before vx.setSettings(s) at this function's own call site (see the comment
    // there for why "before" is what makes "live" mean what it says). The five device-clamped
    // fields (the three tier enums, meshShaders, msaa) get captureClamped's N5 rule instead, so an
    // edit on a machine without RT/PT/mesh-shader hardware never rewrites a teammate's pin for
    // hardware this device does not have.
    voxi::captureVoxiSettings(project_, requested, vx.settings(), vx.deviceInfo(), overallFollowMask);

    project_.backend = projectBackend_;
    // NOT WHEN --frame-budget SUPPLIED IT. frameBudgetMs_ holds either the project's own value or
    // a CLI test override, and this function cannot tell them apart on its own -- so writing it
    // unconditionally let a diagnostic flag become a persisted project setting. With settings now
    // saving on edit, that is not hypothetical: run once with --frame-budget 33, touch any
    // control on the Rendering page, and RENDER.FRAMEBUDGETMS 33 is in the .ocproject for good.
    // The forced flag is exactly the distinction, and it already exists for the symmetric bug on
    // the way in (applyProjectRenderSettings clobbering the flag from the manifest's default).
    if (!frameBudgetForced_) project_.frameBudgetMs = frameBudgetMs_;
    project_.lodSelect          = lodSelectEnabled_ ? 1 : 0;
    project_.lodThresholdPx     = lodErrorThresholdPx_;
    project_.occlusionCull      = occlusionCullEnabled_ ? 1 : 0;
    project_.depthPrepass       = depthPrepassOverride_ ? 1 : 0;
#if AVER_MODULE_SR
    // optimisation-wave-2, 3.3 A: NOT a voxi::Settings field (module boundary, Scalability.hpp's
    // own header comment) so it rides along beside occlusionCull/depthPrepass here rather than
    // through captureVoxiSettings' shared capture rule just above -- same "editor-side flag, same
    // manifest key, same -1 means unstated rule" shape those two already are.
    project_.averSr = averSrProjectDefault_;
#endif
    // The GI volume's placement, which had no key at all -- so a level whose geometry is not at
    // the origin could never record where its indirect light should be gathered.
    project_.hasGiVolume = true;
    project_.giCenter[0] = giCenter_.x;
    project_.giCenter[1] = giCenter_.y;
    project_.giCenter[2] = giCenter_.z;
    project_.giExtent    = giExtent_;
    projectDirty_ = true;
}

#endif

#if AVER_MODULE_VOXI
#else
void SandboxApp::applyProjectRenderSettings() {}

#endif

// Fills any unstated render-settings key with the engine's declared default, then writes the
// manifest. --save-project.
void SandboxApp::seedAndSaveProject() {
#if AVER_MODULE_VOXI
    const voxi::Settings d{};   // as declared, never as clamped
    if (project_.giQuality       < 0)    project_.giQuality       = static_cast<int>(d.globalIllumination);
    if (project_.rayTracing      < 0)    project_.rayTracing      = static_cast<int>(d.rayTracing);
    if (project_.pathTracing     < 0)    project_.pathTracing     = static_cast<int>(d.pathTracing);
    // Derived from the (now-seeded) GI tier, not from d.voxelResolution's raw struct default --
    // otherwise a manifest that only states giQuality would bake in Medium's 128 regardless of
    // the tier, and never let that tier drive the grid on a later load (see Renderer::setSettings).
    if (project_.voxelResolution <= 0)
        project_.voxelResolution = static_cast<int>(
            voxi::Renderer::voxelResolutionForQuality(static_cast<voxi::Quality>(project_.giQuality)));
    if (project_.giIntensity     < 0.0f) project_.giIntensity     = d.giIntensity;
    if (project_.giMaxDistance   < 0.0f) project_.giMaxDistance   = d.giMaxDistance;
    if (project_.rtShadowRays       < 0) project_.rtShadowRays       = static_cast<int>(d.rtShadowRays);
    if (project_.rtPixelsPerRayTile < 0) project_.rtPixelsPerRayTile = static_cast<int>(d.rtPixelsPerRayTile);
    if (project_.rtShadowDenoise    < 0) project_.rtShadowDenoise    = static_cast<int>(d.rtShadowDenoise);
    if (project_.rtRenderMode       < 0) project_.rtRenderMode       = static_cast<int>(d.rtRenderMode);
    if (project_.ptBounces          < 0) project_.ptBounces          = static_cast<int>(d.ptBounces);
    if (project_.layeredBsdf        < 0) project_.layeredBsdf        = static_cast<int>(d.layeredBsdf);
#endif
    std::string why;
    if (saveProjectManifest(&why)) AVER_INFO("[Project] --save-project wrote the manifest");
    else                           AVER_WARN("[Project] --save-project failed: {}", why);
}

// Writes the project manifest to disk. Returns false and fills why on failure.
bool SandboxApp::saveProjectManifest(std::string* why) {
    if (!project_.valid()) { if (why) *why = "no project is open"; return false; }
    std::string existing;
    readFileText(project_.manifestPath, existing);
    const std::string out = fmt::writeOcproject(project_, existing);
    // ATOMIC NOW THAT THIS RUNS ON A TIMER. A plain writeFileText truncates and then writes, so
    // the window where the manifest is empty or half-written used to be one click wide and is now
    // entered every half second of editing. Losing a project file to a crash landing inside that
    // window is exactly the trade the autosave was not meant to make -- same write-temp-then-swap
    // the level save and editor.ini already use.
    if (!writeFileTextAtomic(project_.manifestPath, out)) {
        if (why) *why = "could not write " + project_.manifestPath;
        return false;
    }
    projectDirty_ = false;
    AVER_INFO("[Project] wrote {}", project_.manifestPath);
    return true;
}

// SAVING, AS A PERSON ASKED FOR IT -- the one path Ctrl+S, the toolbar button and File > Save
// Level all take.
//
// WHY IT EXISTS: those three used to be three different behaviours. Ctrl+S reported "Saved X" or
// "Could not save X" and opened Save As when the level had never been written; the toolbar button
// and the menu item both called saveLevel(levelPath_) and THREW THE RESULT AWAY. So the button
// was silent whether it worked or not -- reported as "the save button is broken", and from the
// outside a silent success and a silent failure are the same button.
void SandboxApp::saveLevelInteractive() {
#if AVER_MODULE_SCENE
    if (levelPath_.empty()) {
        // Never written, so there is no file to overwrite. Open the same prompt the menu does
        // rather than inventing a name: Save As is what Save means for a level with no path.
        saveLevelAsName_[0] = '\0';
        std::snprintf(saveLevelAsName_, sizeof saveLevelAsName_, "%s",
                      levelName_.empty() ? "untitled" : levelName_.c_str());
        saveLevelAsError_.clear();
        wantSaveLevelAs_ = true;
        return;
    }
    const std::string name = std::filesystem::path(levelPath_).filename().string();
    if (saveLevel(levelPath_)) {
        setUpgradeStatus("Saved " + name);
    } else {
        setUpgradeStatus("Could not save " + name, editor::NotifySeverity::Error);
        AVER_ERROR("[Level] save: could not write {}", levelPath_);
    }
#endif
}

#if AVER_MODULE_SCENE
// Window::OpenRequestHook trampoline. A raw function pointer (see Window.hpp) has nowhere to
// carry `this`, so it carries `user` instead -- the same shape as RenderTickFn's own thunk
// (Engine::renderTickThunk) and for the identical reason.
 bool SandboxApp::onOpenRequestThunk(void* user, const char* path) {
    return static_cast<SandboxApp*>(user)->handleOpenRequest(path);
}

// ACCEPT or DECLINE a path forwarded from another launch. Runs SYNCHRONOUSLY on the sender's
// blocking SendMessageTimeout, so this is deliberately the cheap half -- a directory walk plus an
// equivalence check, no ImGui, no world mutation; the actual load is drained later in onUpdate.
// FOREIGN PROJECT: DECLINE, DON'T HALF-SWITCH: no code path today reloads a DIFFERENT project's
// materials/meshes/script host mid-session, so a mismatch must decline. The sender falls through
// to build its own Application/Engine as if this primary didn't exist; no second process is ever
// spawned BY the receiver.
// project_.manifestPath, NOT projectPath_: the latter is set once from the command line in onInit
// and goes stale the moment a project is opened/switched through the browser, which project_ never
// does -- comparing against projectPath_ would report "foreign" for the genuinely live project.
// std::filesystem::equivalent rather than a string compare: the two paths can differ in case or
// separator even naming the identical file.
bool SandboxApp::handleOpenRequest(const char* path) {
    const std::string owner = ownerProjectOf(path);
    // ownerProjectOf, unmodified, also answers correctly for a bare .ocproject argument: its
    // directory scan finds THAT FILE among its own siblings, so `owner` equals `path` itself, and
    // comparing against project_.manifestPath needs no separate "the forwarded thing IS a project" case.
    if (owner.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::equivalent(owner, project_.manifestPath, ec) || ec) return false;
    return true;
}

// Loads the project's start map, resolved against its content directory. Missing is not an error.
// Asks for a level to be opened. Returns false, with a reason in openLevelError_, when the file
// cannot be opened at all -- in which case NOTHING has happened yet and the current level is
// untouched.
//
// VALIDATED BEFORE ANYTHING IS TORN DOWN, and that is the whole reason this is not just a call to
// loadLevel. loadLevel's first statement is unloadLevel(eng), before it has looked at the path at
// all; if the file is then missing or unparseable it logs one AVER_WARN and returns, leaving the
// editor holding an empty world with nothing on screen to say why. That is exactly the "the world
// is empty" failure a picker would otherwise make easy to hit, so the check happens up front and
// the failure is reported where the person clicked.
bool SandboxApp::requestOpenLevel(const std::string& path, const char* why) {
    openLevelError_.clear();
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec)) {
        openLevelError_ = "That level is not there any more.";
        return false;
    }
    // Parsed, not merely stat'd. A level that exists and does not parse would empty the world
    // just as thoroughly. levelFileIsLegacyOcmap decides which grammar by CONTENT, so both are
    // tried the same way loadLevel itself will.
    {
        fmt::OcWorldData probe;
        fmt::OcMapData legacyProbe;
        std::string parseWhy;
        const bool ok = fmt::levelFileIsLegacyOcmap(path)
            ? fmt::loadOcmap(path, legacyProbe, &parseWhy)
            : fmt::loadOcworld(path, probe, &parseWhy);
        if (!ok) {
            openLevelError_ = parseWhy.empty() ? std::string("That level could not be read.")
                                               : parseWhy;
            return false;
        }
    }
    pendingOpenPath_ = path;
    pendingOpenWhy_  = why ? why : "";
    return true;
}

// Drains a pending open. Called once per frame from onUpdate, which is where an Engine& is.
void SandboxApp::applyPendingOpen(Engine& eng) {
    if (pendingOpenPath_.empty() || pendingOpenPrompt_) return;
    // THE UNDO STACK IS THE ONLY "unsaved" SIGNAL THERE IS -- there is no per-level dirty flag
    // anywhere in this editor, and unloadLevel clears the stack unconditionally, so it can only
    // be read BEFORE the load. An empty stack does not prove nothing changed; a non-empty one
    // proves the opposite, which is the direction that must never be wrong here.
    if (canUndo()) { pendingOpenPrompt_ = true; return; }
    const std::string path = pendingOpenPath_;
    pendingOpenPath_.clear();
    openLevelDirect(eng, path);
}

// A blank level. The one place that does it, so the guard above and the prompt's Discard
// button cannot drift apart about what "new" clears.
void SandboxApp::startNewLevel(Engine& eng) {
    unloadLevel(eng);
    levelName_ = "untitled";
    // CLEARED, so the next Ctrl+S cannot silently overwrite the level that was open before this
    // one. saveLevel takes whatever path it is given and asks nothing.
    levelPath_.clear();
}

// The actual open, past every guard. The ONE place that pairs loadLevel with the class-placement
// spawn it needs -- see the comment inside for why that pairing is not optional.
void SandboxApp::openLevelDirect(Engine& eng, const std::string& path) {
    loadLevel(eng, path);
    // GRAPH-AS-CLASS / any other class placement: loadLevel collects level_.classPlacements() but does
    // not spawn them -- applyProject's "Starting scripts" stage is what normally does that after
    // a fresh open. Every path that opens a level WITHOUT going through applyProject has to do it
    // here, or class placements load unspawned with no warning. The forwarded-open path did not,
    // which is why it belongs behind this function rather than calling loadLevel itself.
#if AVER_MODULE_FRAMEWORK
#if AVER_MODULE_SCRIPTING
    if (scripts_.ready())
#endif
        spawnClassPlacements();
#endif
    AVER_INFO("[Level] opened {}", path);
    // A LEVEL THAT WAS LEFT UNSAVED LEAVES A SIDECAR; this is where it gets offered. After the
    // load, so levelPath_ names the level the sidecar shadows.
    checkForRecovery();
}

// Writes the level's own entities back out to whichever format they were loaded from.
// THE SAVE HAS TO MATCH THE LOAD, so this is a two-line dispatch on levelIsLegacyOcmap_ ahead of
// everything below, not a parallel "which writer" decision made some other way (the path's
// extension, an unreliable signal). levelIsLegacyOcmap_ is the answer loadLevel already computed
// at open time; re-deriving it independently here would be a second place it could drift from the first.
bool SandboxApp::saveLevel(const std::string& path) {
    if (levelIsLegacyOcmap_) return saveLevelAsOcmap(path);

    scene::World& world = scene::World::instance();
    // STARTS FROM WHAT THE FILE SAID, not from a default-constructed OcWorldData. Everything the
    // editor does not model -- ID, BUILD, ALGO, SPAWN, the sun's lux -- rides through untouched;
    // the lines below overwrite only what the editor genuinely owns. See levelHeader_.
    fmt::OcWorldData w = levelHeader_;
    w.name = levelName_.empty() ? std::string("untitled") : levelName_;
    // THE MARKER IS THE TRUTH WHEN THERE IS ONE: saveLevel's banner used to list SPAWN among
    // fields that "ride through untouched" -- correct while nothing could edit it, wrong now that
    // a Player Start can be dragged. A level with no marker keeps whatever SPAWN it arrived with.
    {
        Vec3 sp{}; f32 sy = 0.0f;
        if (playerStartTransform(sp, sy)) {
            w.hasSpawn = true;
            w.spawnX = sp.x; w.spawnY = sp.y; w.spawnZ = sp.z; w.spawnYaw = sy;
        }
    }

    // WHERE THE AUTHOR WAS STANDING, so reopening the level returns you to it instead of to the
    // world origin. Written on every save, which is what makes "save the camera when you leave"
    // true without needing a separate exit hook -- closing a level you have saved, and Ctrl+S,
    // are the same act as far as this record is concerned.
    //
    // NOT WRITTEN WHEN --cam CHOSE THE VIEW. That flag means a machine picked the camera -- a
    // gate, a capture, a bug repro -- and stamping its synthetic viewpoint into a file a person
    // owns is the thing to avoid. An unflagged save writes the camera whoever ran it was looking
    // through, which is the whole feature.
    //
    // GATED ON camOverride_ RATHER THAN maxFrames_, which is what I reached for first. The
    // bounded-run test would have been redundant AND harmful: redundant because the level
    // autosave -- the thing that actually wrote a corrupt file from a capture once -- already
    // refuses outright when maxFrames_ != 0 (see maybeAutosave), and --save-level is a manual
    // flag no script invokes; harmful because it made this feature impossible to verify outside
    // an interactive session, and an unverifiable guard is how a silent regression gets in.
    //
    // DEGREES OUT, radians in: yaw_/pitch_ are radians everywhere in this file, and the file
    // format is degrees for the same reason SPAWN is -- a number a person may end up reading.
    if (!camOverride_) {
        constexpr f64 kDeg = 180.0 / 3.14159265358979323846;
        w.hasCamera = true;
        w.camX = camPos_.x; w.camY = camPos_.y; w.camZ = camPos_.z;
        w.camYaw = static_cast<f64>(yaw_) * kDeg;
        w.camPitch = static_cast<f64>(pitch_) * kDeg;
        w.camSpeed = flySpeed_;
    }

    // EACH RECORD GOES OUT WHENEVER THE LEVEL CARRIED IT **OR** THE AUTHOR HAS EDITED IT HERE
    // (markLevelRecordEdited), so a Details-panel edit survives a save rather than being
    // silently dropped. That second half was missing for a long time: the claim was in this
    // comment while the flags were set only by the loader, so it held for levels that already
    // had the record and for no others.
    w.hasSun    = hasLevelSun_;
    w.hasSky    = hasLevelSky_;
    w.hasFog    = hasLevelFog_;
    w.hasClouds = hasLevelClouds_;

    // THE EDITOR'S OWN MIRRORS GO BACK INTO THE ATMOSPHERE FIRST, because the Details sliders
    // bind to these members rather than to sky_, and the frame loop is what normally copies
    // them across. saveLevel can be reached on a frame where that has not happened yet -- from
    // --save-level, which runs the moment the renderer attaches -- and reading sky_ without
    // this would then write the atmosphere's defaults over the author's sliders.
    for (int i = 0; i < 3; ++i) {
        sky_.sunColor[i] = sunColor_[i];
        sky_.zenith[i]   = skyZenith_[i];
        sky_.horizon[i]  = skyHorizon_[i];
        sky_.fogColor[i] = fogColor_[i];
    }
    sky_.skyLightIntensity = sunAmbient_;
    sky_.fogDensity        = fogDensity_;

    // ONE CAPTURE, the exact inverse of the one applyLevelSky applies -- see LevelSky.hpp. This
    // was seventeen hand-written assignments that had to be kept in step with the loader by
    // eye, and were not: the sun's `lux` was read and never written, so turning Intensity down
    // and saving gave the old value straight back from levelHeader_'s passthrough.
    assets::captureLevelEnv(sky_, w);

    // Straight back out, in the order they were read. See loadLevel.
    w.pcgVolumes = levelPcgVolumes_;

    // WHICH PLACEMENT SLOT EACH ENTITY WILL OCCUPY, decided BEFORE any is written, because a
    // child may be reached before its parent -- levelEntities_ is the swap-removed dense array
    // and carries no ordering guarantee at all. Without this the parent index would be "the one
    // written so far, if we happened to have got to it", which is the shape of bug this file
    // already has a scar from (LevelClassSave.hpp: "Pairing by position would write one
    // placement's transform onto another's line").
    std::unordered_map<u32, i32> slotOf;
    {
        i32 slot = static_cast<i32>(w.placements.size());
        for (const scene::Entity e : levelEntities_) {
            if (!world.valid(e)) continue;
            if (!world.component<scene::CLocal>(e, scene::kComponentLocal)) continue;
            slotOf.emplace(static_cast<u32>(e), slot++);
        }
    }

    // A FRESH COUNTER, SEPARATE FROM THE LIVE labelCounts_ -- see the name-vs-default check inside
    // the loop below for why. Ordinals start back at zero here rather than wherever the session's
    // own counter happens to be, because what this loop needs to know is what a FRESH LOAD OF THE
    // FILE BEING WRITTEN would hand each placement, and a load always starts labelCounts_ empty
    // too (unloadLevel clears it before the next level's entities are created).
    std::unordered_map<std::string, u32> shadowLabelCounts;

    for (const scene::Entity e : levelEntities_) {
        if (!world.valid(e)) continue;
        const auto* loc = world.component<scene::CLocal>(e, scene::kComponentLocal);
        const auto* mr  = world.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
        if (!loc) continue;
        fmt::OcWorldPlacement p;
        // THE PARENT, AND WITH IT THE REASON CLocal IS NOW CORRECT TO WRITE. This loop has always
        // written loc->xf -- the LOCAL transform -- into a format that read every placement back
        // as a world position. That was harmless only because nothing in the editor could make a
        // child; the moment one exists it becomes silent position corruption on the next
        // save/load, with the child reappearing at its parent-relative offset from the origin.
        // Expressing the parent is what makes the existing write correct rather than corrupting,
        // which is why the format and this line had to land together and neither alone.
        //
        // A PARENT OUTSIDE THE LEVEL'S OWN ENTITIES writes as a root: an entity parented to
        // something the level does not own (a class instance, a preview, anything spawned at
        // runtime) has no placement to point at, and inventing one would be worse than losing
        // the relationship.
        {
            const scene::Entity par = world.parent(e);
            const auto it = par == scene::kInvalidEntity
                          ? slotOf.end() : slotOf.find(static_cast<u32>(par));
            p.parent = it == slotOf.end() ? -1 : it->second;
        }
        p.asset = world.name(e);
        p.x = loc->xf.position.x; p.y = loc->xf.position.y; p.z = loc->xf.position.z;
        const Vec3 euler = eulerDegFromQuat(loc->xf.rotation);
        p.roll = euler.x; p.pitch = euler.y; p.yaw = euler.z;
        p.sx = loc->xf.scale.x; p.sy = loc->xf.scale.y; p.sz = loc->xf.scale.z;
        // THE SURFACE, WHICH USED TO BE DROPPED ON EVERY SAVE: `(void)mr;` sat here and the
        // material line was simply missing. The token itself must never be written -- it's a
        // process-local intern id (docs/CHUNKS.md 5.1) -- so it goes back out as the NAME it was interned under.
        if (mr && mr->material) p.material = aver_scene_material_name(mr->material);
        // `nocollide` now round-trips. This was hardcoded true, so a placement authored
        // nocollide came back colliding and quietly gained a static body on the next load.
        // Entities created in the editor are absent from the map and keep the true default.
        const auto collideIt = entityCollide_.find(static_cast<u32>(e));
        p.collide = collideIt == entityCollide_.end() ? true : collideIt->second;
        // A SNAPPED PLACEMENT KEEPS ITS OFFSET, not the resolved world height. See the load site
        // for why writing loc->xf.position.z here bakes the terrain into the level.
        //
        // The offset is restored VERBATIM rather than recomputed as (live z - ground): the
        // ground under it may have been sculpted since the load, and re-deriving would silently
        // rewrite an authored number the user never touched. Moving a snapped object in the
        // viewport therefore does not yet move it in the file -- which is the honest behaviour
        // until the editor grows a way to show and edit a snap offset.
        const auto snapIt = entitySnapZ_.find(static_cast<u32>(e));
        if (snapIt != entitySnapZ_.end()) { p.snapToGround = true; p.z = snapIt->second; }
        // THE OUTLINER LABEL, PERSISTED ONLY WHEN IT IS NOT THE ONE makeEntityLabel WOULD HAND
        // BACK ANYWAY. makeEntityLabel (SandboxViewport.cpp) numbers entityLabelBase's word with an
        // ordinal from labelCounts_ -- reproduced here with the same entityLabelBase against
        // shadowLabelCounts instead of the live counter. Calling the real makeEntityLabel to find out would not just read the counter,
        // it would ADVANCE it -- consuming an ordinal that belongs to the next real spawn and
        // leaving every label after this save off by one. shadowLabelCounts is scoped to this
        // save alone and starts at zero, matching what a fresh load of the file being written
        // right now would count up to for this same placement, in this same order.
        //
        // A LEVEL NOBODY RENAMED ANYTHING IN THEREFORE WRITES NO `name` TOKENS AT ALL: every
        // entity's stored label is exactly its shadow default, so the comparison below never
        // fires, and an old save stays byte-identical to a new one of the same scene.
        //
        // THE ONE CASE THIS CAN OVER-WRITE: writeOcworld emits a parented placement's CHILD line
        // depth-first under its parent's BEGIN, which is not necessarily this loop's own
        // levelEntities_ order when a child sits earlier in that list than its parent. A reload
        // then counts ordinals in the file's (parent-first) order, not this pass's, so an
        // untouched label can occasionally fail this comparison anyway. The cost is a spurious but
        // harmless `name` line -- the label shown is unchanged either way -- never a lost rename.
        {
            const std::string base = entityLabelBase(p.material, p.asset);
            const std::string shadowDefault = base + " " + std::to_string(++shadowLabelCounts[base]);
            const auto labelIt = entityLabels_.find(static_cast<u32>(e));
            if (labelIt != entityLabels_.end() && labelIt->second != shadowDefault)
                p.name = labelIt->second;
        }
        w.placements.push_back(std::move(p));
    }

    // CLASS PLACEMENTS GO BACK OUT TOO, AND UNTIL NOW THEY DID NOT: this loop rebuilds a
    // placement from SCENE COMPONENTS, and a `class=` placement has none -- opening a level and
    // saving it DELETED every graph class in it, removing FirstPerson's game mode and targets and
    // leaving no player, with nothing in the log to say why.
    // AND THE TRANSFORM IS REBUILT FROM THE LIVE ENTITY, WHICH IT USED TO NOT BE: this wrote
    // the class placements verbatim, defended by "the editor cannot EDIT a class placement" -- FALSE,
    // since spawnClassPlacements spawns a real, selectable, draggable entity per placement. What
    // didn't happen was the save reading any of it back: move one, save, reload, find it unchanged.
    // PAIRED BY RECORDED INDEX, never by position (see ClassInstance's comment).
    // A DEAD OR MISSING ENTITY FALLS BACK TO THE FILE'S COPY rather than dropping the placement.
    // The pairing and snap rule live in LevelClassSave.hpp, header-only, so a test can drive them
    // with a fake lookup -- SandboxApp is add_executable-only.
    editor::appendClassPlacements(
        level_.classPlacements(), levelClassInstances_,
        [&world](int32_t e, Transform& xf) {
            const scene::Entity ent = static_cast<scene::Entity>(e);
            if (!world.valid(ent)) return false;
            const auto* loc = world.component<scene::CLocal>(ent, scene::kComponentLocal);
            if (!loc) return false;
            xf = loc->xf;
            return true;
        },
        w.placements);

    std::string why;
    if (!fmt::saveOcworld(path, w, &why)) { AVER_WARN("[Level] save failed: {}", why); return false; }
    AVER_INFO("[Level] saved {} placement(s) to {}", w.placements.size(), path);
    // THE DOCUMENT AND THE FILE NOW AGREE. Without this the exit prompt claims unsaved
    // changes forever -- see levelHasUnsavedEdits.
    markLevelSaved();
    // A REAL SAVE RETIRES THE SIDECAR -- but only a save of THE LEVEL. saveLevel is also how the
    // autosave itself and --save-level write, and deleting the recovery file on either would
    // throw away the safety net at the moment it was being created.
    if (path == levelPath_ && autosaveWritten_) clearAutosave();
    return true;
}

// saveLevel's legacy-OCMAP twin -- the exact "start from what the file said, overwrite only what
// the editor genuinely owns" shape saveLevel itself uses against levelHeader_, here against
// legacyMapHeader_ (ROOT, CLIENT, the SURFACE table, GROUND, KILLZ).
bool SandboxApp::saveLevelAsOcmap(const std::string& path) {
    scene::World& world = scene::World::instance();
    fmt::OcMapData m = legacyMapHeader_;
    m.name = levelName_.empty() ? std::string("untitled") : levelName_;

    // THE MARKER IS THE TRUTH WHEN THERE IS ONE -- same reasoning as saveLevel's own SPAWN
    // handling just above, applied to the identical field on the legacy struct.
    {
        Vec3 sp{}; f32 sy = 0.0f;
        if (playerStartTransform(sp, sy)) {
            m.hasSpawn = true;
            m.spawnX = sp.x; m.spawnY = sp.y; m.spawnZ = sp.z; m.spawnYaw = sy;
        }
    }

    for (const scene::Entity e : levelEntities_) {
        if (!world.valid(e)) continue;
        const auto* loc = world.component<scene::CLocal>(e, scene::kComponentLocal);
        if (!loc) continue;
        fmt::OcPlacement p;
        p.asset = world.name(e);
        p.x = loc->xf.position.x; p.y = loc->xf.position.y; p.z = loc->xf.position.z;
        const Vec3 euler = eulerDegFromQuat(loc->xf.rotation);
        p.roll = euler.x; p.pitch = euler.y; p.yaw = euler.z;

        const auto deformIt = entityLegacyDeform_.find(static_cast<u32>(e));
        p.deform = deformIt != entityLegacyDeform_.end() && deformIt->second;
        if (p.deform) {
            const auto matIt = entityLegacyMaterial_.find(static_cast<u32>(e));
            // "default" mirrors parseOcmap's own DEFORM fallback, for an entity this session
            // created rather than loaded: there is no editor UI to author a DEFORM placement yet,
            // but the fallback keeps a future one honest anyway.
            p.material = matIt == entityLegacyMaterial_.end() ? std::string("default") : matIt->second;
            p.scale = 1.0;
            p.surface = -1;
        } else {
            // Legacy PLACE is UNIFORM scale only -- OcPlacement has one `scale` field, not
            // OcWorldPlacement's sx/sy/sz -- so a non-uniform edit made through this editor would
            // lose two axes on this save regardless; .x is as good a choice as any.
            p.scale = loc->xf.scale.x;
            const auto surfIt = entityLegacySurface_.find(static_cast<u32>(e));
            p.surface = surfIt == entityLegacySurface_.end() ? -1 : surfIt->second;
        }
        m.placements.push_back(std::move(p));
    }

    std::string why;
    if (!fmt::saveOcmap(path, m, &why)) { AVER_WARN("[Level] save failed: {}", why); return false; }
    AVER_INFO("[Level] saved {} placement(s) to {} (legacy .ocmap)", m.placements.size(), path);
    markLevelSaved();
    return true;
}

#endif

} // namespace aver
