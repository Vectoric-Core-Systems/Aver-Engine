// Runtime side: the play session, input publishing, the play camera, animation curves/notifies, mouse capture.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

#include "SandboxApp.hpp"
#include "aver/game/GameCamera.hpp"
#include "aver/game/GameInput.hpp"
#include "aver/game/GamePawn.hpp"

#include <cfloat>
// For teleportPawnToCamera: finding and measuring a capsule this file did not create, and a ray
// that can skip it.
#if AVER_MODULE_PHYSICS
#include "aver/physics/physics_character_abi.h"
#include "aver/physics/physics_layers_abi.h"
#endif

namespace aver {
#if AVER_MODULE_FRAMEWORK

namespace {
// The walking default pawn's capsule (see startDefaultPawnWalk), up here because startPlay's Play From Here
// also needs the eye height to stand a FLYING pawn's camera above the picked point.
constexpr f32 kWalkHeight = 180.0f, kWalkRadius = 34.0f, kWalkEye = 160.0f, kWalkSpeed = 450.0f,
              kWalkRunSpeed = 900.0f, kWalkJump = 465.0f;
// Play From Here stands the pawn's feet this far above the picked surface, so a capsule or a pawn's own
// ground snap never starts embedded in the triangle the ray hit.
constexpr f32 kPlayFromHereLiftCm = 5.0f;

// Puts the possessed pawn exactly at the camera, pitch included -- the engine-default-pawn path's
// fallback when there is no Player Start (and, with PlaySpawnAt::CameraLocation, its first choice).
// Not game::placePossessedPawn: that is yaw-only, and a flying spectator has no other node to carry pitch.
void placeDefaultPawnAtCamera(const Vec3& camPos, f32 yaw, f32 pitch) {
    const int32_t pn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
    if (!pn) return;
    const scene::Entity pe = static_cast<scene::Entity>(static_cast<uint32_t>(pn));
    scene::World& pw = scene::World::instance();
    if (!pw.valid(pe)) return;
    pw.setLocalPosition(pe, camPos);
    pw.setLocalRotation(pe, Quat::fromAxisAngle(Vec3{0, 0, 1}, yaw) *
                            Quat::fromAxisAngle(Vec3{0, 1, 0}, -pitch));
}

// Where a STANDING pawn's feet go for a camera at `cam` (teleportPawnToCamera): one eye height below
// it, which is what puts a first-person view back exactly where the editor camera was.
// BUT NEVER UNDER THE FLOOR. A camera hovering knee-high over a street would otherwise put the feet,
// and most of the capsule, beneath the surface -- and level collision is one-sided triangle meshes
// with nothing underneath, so the pawn would fall out of the level. A ray straight down over that
// one eye height finds the surface, and the feet stop kPlayFromHereLiftCm above it.
// `self` is the pawn's own capsule or body (0 for none), which the ray skips: a character has a
// broadphase body a ray can hit, and ejecting leaves the camera INSIDE it, at the pawn's eye -- an
// unfiltered ray would stop there at distance 0 and never reach the floor it is looking for.
// NOR IS ANY OTHER BODY THE CAMERA IS INSIDE a floor. Jolt reports a ray that starts inside a box (a
// trigger volume, a crate) as a hit at distance 0, which would stand the pawn's feet above the camera.
// The ray goes again past that one body; a hit still at the camera after that is not taken -- no floor
// clamp is better than one that lifts the pawn over the view.
// ONE IGNORE SLOT CANNOT SKIP TWO BODIES: with the camera inside the pawn's capsule AND another body, the
// second ray ignores the other body and stops on the pawn. The pawn is then right there, so its current
// feet (`ownFeetZ`, -FLT_MAX for none) are a floor known to hold it.
Vec3 feetBelowCamera(const Vec3& cam, f32 eye, int32_t self, f32 ownFeetZ) {
    Vec3 feet = cam - Vec3{0.0f, 0.0f, eye};
#if AVER_MODULE_PHYSICS
    if (eye <= 0.0f) return feet;
    constexpr f32 kAtCameraCm = 1.0f;
    float hit[3] = {0.0f, 0.0f, 0.0f};
    int32_t h = aver_phys_raycast_ex(cam.x, cam.y, cam.z, 0.0f, 0.0f, -1.0f, eye,
                                     AVER_PHYS_LAYER_MASK_ALL, self, hit, nullptr, nullptr);
    if (h != 0 && hit[2] > cam.z - kAtCameraCm)
        h = aver_phys_raycast_ex(cam.x, cam.y, cam.z, 0.0f, 0.0f, -1.0f, eye,
                                 AVER_PHYS_LAYER_MASK_ALL, h, hit, nullptr, nullptr);
    if (h != 0 && h != self && hit[2] <= cam.z - kAtCameraCm)
        feet.z = std::fmax(feet.z, hit[2] + kPlayFromHereLiftCm);
    else if (h != 0 && h == self)
        feet.z = std::fmax(feet.z, ownFeetZ);
#else
    (void)self; (void)ownFeetZ;
#endif
    return feet;
}
} // namespace

// Starts a play session: finds the user GameMode and optional GameInstance and begins play.
// WITH NO GAMEMODE, falls back to the drone rather than doing nothing: pressing Play used to log a
// warning and return, making a content-only project's button look broken. A REAL GameMode always
// wins; this is only reached when the project declares none.
// Records where everything the LEVEL owns is standing, so Stop can put it back -- a transform
// snapshot, not a reload or full capture (see playWorldSnapshot_).
void SandboxApp::capturePlayWorld() {
    playWorldSnapshot_.clear();
    playWorldCaptured_ = false;
#if AVER_MODULE_SCENE
    scene::World& w = scene::World::instance();
    playWorldSnapshot_.reserve(levelEntities_.size());
    for (const scene::Entity e : levelEntities_) {
        if (!w.valid(e)) continue;
        // THE RAW BIT, deliberately NOT authoredVisible(): this is "what Play found", the exact
        // same convention localTransform(e) just below follows, and an entity H-hid before Play
        // began is one whose bit already read clear before anything here ran. Capturing
        // authoredVisible() instead (true for an H-hidden entity, since editorHidden_ still names
        // it) would make Stop UN-HIDE it -- Play/Stop touching an H-hide it never promised to
        // touch, purely as a side effect of what gameplay did or did not do in between.
        bool vis = true;
        if (const auto* mr = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer))
            vis = (mr->flags & scene::kMeshRendererVisible) != 0;
        playWorldSnapshot_.push_back({e, w.localTransform(e), vis});
    }
    playWorldCaptured_ = true;
    AVER_INFO("[Sandbox] Play: {} level transform(s) recorded; Stop will put them back",
              (u32)playWorldSnapshot_.size());
    // Here, before begin_play spawns the pawn: everything alive now is level content, everything
    // born from here on starts provisional (PlayMobility.hpp).
    playMobility_.begin(w);

    // AND THE AUTHORED PHYSICS BECOMES REAL, here and nowhere else: a CRigidBody is data until
    // PLAY turns it into a Jolt body -- building at LOAD would mean a crate settling while someone
    // was only looking around, the same complaint that got gameplay graphs gated out of Select mode.
    // AFTER the snapshot, deliberately: Stop restores from it, so it must record the level BEFORE
    // physics touched anything.
#if AVER_MODULE_PHYSICS
    editor::syncPhysicsFromScene();
    // THE LEVEL'S CARS, same rule and same place: a `vehicle` placement is a fixed pose until PLAY builds
    // its Jolt vehicle, from where the entity stands NOW (a car the user nudged in the editor drives from
    // its new spot). After the snapshot, so Stop restores the placement and not wherever the car ended
    // up; after playMobility_.begin, because seeding needs a session to seed into.
    if (level_.beginVehicles(vehicles_, content_) > 0) playMobility_.seedMovable(vehicles_.entities());
#endif
#endif
}

// Puts the level back where Play found it.
// LOCAL transforms, because that is what was captured and what physics and gameplay actually
// write. Restoring a WORLD transform onto a child would fight its parent and land it somewhere
// neither meant.
// An entity the session destroyed is simply skipped -- pretending otherwise by recreating an empty one at the right coordinates would be worse than leaving it gone.
void SandboxApp::restorePlayWorld() {
    if (!playWorldCaptured_) return;
#if AVER_MODULE_SCENE
    scene::World& w = scene::World::instance();
    u32 put = 0, lost = 0;
    for (const PlaySavedTransform& t : playWorldSnapshot_) {
        if (!w.valid(t.e)) { ++lost; continue; }
        if (w.setLocalTransform(t.e, t.xf)) ++put;
        // THE RAW BIT BACK, same convention as the transform just above -- not through
        // setAuthoredVisible(), which would also touch editorHidden_ and turn an H-hide from
        // before Play into a permanent, authored one. A graph's Entity.SetVisible call during Play
        // writes this identical bit directly, so undoing whatever it did is this simple.
        if (auto* mr = w.component<scene::CMeshRenderer>(t.e, scene::kComponentMeshRenderer)) {
            if (t.visible) mr->flags |=  scene::kMeshRendererVisible;
            else           mr->flags &= ~scene::kMeshRendererVisible;
        }
    }
    if (lost) AVER_INFO("[Sandbox] Stop: {} level transform(s) restored; {} entity/entities were "
                        "destroyed during play and cannot be", put, lost);
    else      AVER_INFO("[Sandbox] Stop: {} level transform(s) restored", put);
#endif
#if AVER_MODULE_FLUIDS
    // AND THE FLUID, WHICH A TRANSFORM CANNOT PUT BACK: a soft body's shape lives in its
    // vertices, not an entity transform, so a pool left sloshing by a session stayed sloshing
    // through Stop. Re-settling means building it again from the descriptor the level authored,
    // through the deferred drain rather than by spawning here.
    if (water_.respawnLevelVolume())
        AVER_INFO("[Sandbox] Stop: the simulated fluid is being re-settled from its authored "
                  "volume; it rebuilds on the next frame");
#endif
    playWorldSnapshot_.clear();
    playWorldCaptured_ = false;
}

// THE ENGINE'S OWN DEFAULT GAME MODE, PAWN AND CONTROLLER, declared once and reused.
// WHY THE ENGINE AND NOT THE PROJECT: Unreal answers "no GameMode" with classes IT ships, so an
// empty project is playable out of the box. Every previous answer here needed the PROJECT to
// supply something (a .ocgraph, then a DRONE.GRAPH key), leaving an unauthored project unplayable.
// DECLARED NATIVELY through the same C ABI C# uses, so the editor registers these without managed
// code or project content.
// NO PARENT, KIND BITS SET DIRECTLY: parenting to "Pawn"/"GameMode" depends on managed-side base
// rows existing; AVER_FW_CLASS_PAWN/CONTROLLER/GAME_MODE need no ancestor.
// Returns the GameMode class, or 0 if the framework would not take it.
int32_t SandboxApp::engineDefaultGameMode() {
    if (engineDefaultGm_) return engineDefaultGm_;
    const int32_t pawn = aver_fw_class_declare("AverDefaultPawn", nullptr);
    const int32_t ctrl = aver_fw_class_declare("AverDefaultController", nullptr);
    const int32_t gm   = aver_fw_class_declare("AverDefaultGameMode", nullptr);
    if (!pawn || !ctrl || !gm) {
        AVER_WARN("[Sandbox] the framework would not declare the engine's default play classes");
        return 0;
    }
    aver_fw_class_set_flags(pawn, AVER_FW_CLASS_PAWN);
    aver_fw_class_set_flags(ctrl, AVER_FW_CLASS_CONTROLLER);
    aver_fw_class_set_flags(gm,   AVER_FW_CLASS_GAME_MODE);
    aver_fw_class_set_default_pawn(gm, "AverDefaultPawn");
    aver_fw_class_set_player_controller(gm, "AverDefaultController");
    aver_fw_class_seal(pawn); aver_fw_class_seal(ctrl); aver_fw_class_seal(gm);
    engineDefaultGm_ = gm;
    AVER_INFO("[Sandbox] engine default play classes registered "
              "(AverDefaultGameMode -> AverDefaultPawn, AverDefaultController)");
    return gm;
}

// Puts the possessed pawn on the level's Player Start, if it has one. Returns false when there is
// none, so a caller can fall back to whatever it did before.
// ONE HELPER FOR BOTH PLAY PATHS, deliberately: the engine's default pawn and a project's real
// GameMode pawn spawn through completely different code, and "where does the player start" must
// answer the same way for both -- neither did, making a Player Start look like a marker that did
// nothing.
// POSITION AND ROTATION BOTH: AverCharacter used to overwrite yaw with 0 on its first frame,
// silently discarding a Player Start's yaw; Character.cs now seeds its own yaw from this transform.
bool SandboxApp::placePawnAtPlayerStart(const char* who) {
#if AVER_MODULE_SCENE
    Vec3 sp{}; f32 sy = 0.0f;
    if (!playerStartTransform(sp, sy)) return false;
    if (!game::placePossessedPawn(sp, sy)) return false;
    AVER_INFO("[Sandbox] Play: {} spawned at the level's Player Start ({:.0f}, {:.0f}, {:.0f}) "
              "yaw {:.0f}", who, sp.x, sp.y, sp.z, sy);
    return true;
#else
    (void)who; return false;
#endif
}

void SandboxApp::startPlay() {
    // The profiler's GPU average and CPU phases start from nothing: a Play-only cost would
    // otherwise be divided by every edit frame since boot.
    resetPlayProfile(true);
    // PLAY FROM HERE'S ONE-SHOT, taken and cleared before anything below can return early, so it can
    // never outlive the Play press it was set for -- and playSpawnAt_, the persistent choice, is
    // never written. The point is where the pawn's FEET go.
    const std::optional<Vec3> fromHere = playFromHere_;
    playFromHere_.reset();
    // BEFORE anything begins, so what is recorded is the editor's level and not one frame of
    // gameplay's effect on it.
    capturePlayWorld();
#if AVER_MODULE_SCENE
    // Object animation (a placed mesh's transform clip) plays from here until Stop. After the snapshot:
    // its first live tick is what starts moving the placements the snapshot just recorded. The two
    // begin_play failure branches below turn it back off, since nothing would ever call Stop.
    anim::animSystem().setObjectAnimationLive(true);
#endif
    // And the editor's view, for storeLevelView: leaving the level mid-session must remember where
    // the editor camera was, not the pawn's eye the session is driving it from.
    preplayView_ = editor::LevelView{camPos_.x, camPos_.y, camPos_.z, degrees(yaw_), degrees(pitch_), flySpeed_};
    preplayViewValid_ = true;

    // TERRAIN COLLISION, BEFORE A SINGLE PAWN EXISTS TO FALL THROUGH IT.
    //
    // GameLandscape::rebuildCollision had exactly two callers, loadForLevel and save, and its own
    // comment defends that: rebuilding a Jolt heightfield per sculpt tick would cost far more than
    // editing needs. That is right, and it is untouched -- editing still never rebuilds. What it
    // missed is that nothing forced a save on the way in here, so the most ordinary way a user
    // checks terrain work -- sculpt a hill, press Play, walk onto it -- ran against the heights from
    // BEFORE the edit. Walking through your own hill is indistinguishable from "terrain editing does
    // not work", which is how it was reported.
    //
    // Entering Play is the one moment collision must be current, and this is once rather than per
    // stroke. rebuildCollisionIfStale is a single branch when nothing has moved, which is the common
    // Play, so the frequency argument the original comment makes is not weakened by it.
    //
    // HERE, NOT AT THE END: the begin_play calls below can possess a pawn and tick physics, and a
    // rebuild after that point would already be too late for the first frame a player sees.
    //
    // GUARDED ON LANDSCAPE, NOT ON FRAMEWORK. `landscape_` is declared under AVER_MODULE_LANDSCAPE
    // and this function's surrounding region is AVER_MODULE_FRAMEWORK -- two different questions, and
    // the enclosing guard does not answer this one. A landscape-off build would not compile, which
    // the default configuration cannot show because it has landscape on;
    // scripts/module-guard-audit.py caught it, which is the whole reason that tool exists.
#if AVER_MODULE_LANDSCAPE
    landscape_.rebuildCollisionIfStale();
#endif

#if AVER_MODULE_SCRIPTING
    // INPUT SCHEME, before EITHER begin_play call below (the engine's default-pawn fallback and
    // the project's own GameMode both follow this point) so a GameMode's OnBeginPlay -- and
    // whatever pawn/controller it possesses -- already sees the scheme's pushed context.
    // RE-RESOLVED AT EVERY PLAY START, not cached from project open: this is what makes an
    // .ocinput edit saved between two Play presses take effect on the second one, since
    // InputScheme.Load (Aver.Framework, called through ScriptHost::configureInput) re-parses the
    // file fresh every time. See that method's own header comment for the full return contract.
    {
        std::string schemePath = project_.inputScheme.empty()
            ? std::string() : project_.contentDir() + "\\" + project_.inputScheme;
        if (!schemePath.empty() && !fileExists(schemePath)) {
            AVER_WARN("[Sandbox] Play: INPUT.SCHEME '{}' does not exist -- no input scheme will be loaded",
                      schemePath);
            schemePath.clear();
        }
        // <project dir>\Saved\Settings.ini. No createDirectories() call needed here: Settings'
        // own flush creates its target directory itself the first time it writes
        // (modules/settings/src/Settings.cpp's aver_settings_flush), the same way an .ocsave or a
        // .editorprefs write does.
        const std::string settingsPath = project_.dir + "\\Saved\\Settings.ini";
        const int32_t actions = scripts_.configureInput(schemePath, settingsPath);
        if (actions > 0)
            AVER_INFO("[Sandbox] Play: input scheme ready ({} action(s) from '{}')", actions, schemePath);
        else if (actions == -2)
            AVER_WARN("[Sandbox] Play: this build's scripting bridge predates ConfigureInput -- "
                      "rebindable input is unavailable");
        else if (actions < 0)
            AVER_WARN("[Sandbox] Play: input scheme '{}' failed to load", schemePath);
        // actions == 0 (no INPUT.SCHEME, or one that resolved to nothing) is the ordinary case for
        // a content-only or not-yet-authored project and not worth a line on every Play press.
    }
#endif

    const int32_t gm = aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_MODE);
    if (gm == 0) {
        // scene:: is safe here without a further guard: root CMakeLists forces
        // AVER_MODULE_FRAMEWORK off when SCENE is off (CMakeLists.txt:169-171), and this whole
        // function is #if FRAMEWORK.
        // NO GameMode MEANS A PLAIN FLYING CAMERA, what Unreal hands you.
        // It used to SPAWN THE DRONE and possess it: the drone only moves if the project named a
        // graph (nothing could before DRONE.GRAPH), and the camera FOLLOWS a play-started drone as
        // its pawn, so pressing Play with no GameMode snapped your view onto a stationary
        // quadcopter and left you unable to move. That was the whole of "the drone is glitched".
        // A camera has no such failure mode: nothing to spawn, possess, or decline to move. The
        // drone is still there from Window > Drone, something you asked for.
        // THE ENGINE'S DEFAULT TAKES OVER through the SAME begin_play every real session uses -- a
        // GameMode spawns, a controller possesses it, the camera follows because it genuinely is
        // one. The previous version flew the viewport camera directly with an editor-only flag:
        // looked the same, was not -- aver_fw_play_state stayed EDITOR and every "are we playing"
        // gate needed a second case.
        const int32_t dgm = engineDefaultGameMode();
        if (dgm && aver_fw_begin_play(0, dgm)) {
            defaultPawnPlay_ = true;
            // FIRST PERSON, NO EYE OFFSET, NO BOOM: a flying camera wants to be exactly at its
            // pawn and never see it, i.e. first person with zero eye height.
            // SAVED FIRST, AND PUT BACK ON STOP. aver_fw_set_view writes a PROCESS-WIDE static
            // nothing in the framework ever resets, so walking away leaves every LATER session at
            // eye height 0 -- and drivePlayCamera's no-view-entity fallback is `pawnPos + up * eye`,
            // putting a real character with no view node yet at its FEET, inside the floor. Not
            // hypothetical: an uncompiled-scripts project hits this on its first Play.
            aver_fw_view(&savedViewMode_, &savedViewEye_, &savedViewBoom_);
            savedView_ = true;
            aver_fw_set_view(AVER_FW_VIEW_FIRST_PERSON, 0.0f, 0.0f);

            // AND IT STARTS WHERE YOU WERE LOOKING, WHICH IS THE WHOLE OF THE BUG REPORT.
            // The framework spawns a default pawn at the world ORIGIN; in a real level that's
            // often INSIDE something -- pressing Play in ElectricDreams put the camera inside the
            // terrain, reading as "a cube stuck in the screen".
            // Unreal spawns at a PlayerStart, or the editor camera if none -- playSpawnAt_ picks
            // which one is tried FIRST; the other is never consulted for CameraLocation, since the
            // camera is what the user asked for, not merely what is left when there is no marker.
            bool atCamera = playSpawnAt_ == PlaySpawnAt::CameraLocation;
            if (fromHere) {
                // PLAY FROM HERE beats both choices above. A walker's pawn stands ON the point,
                // level-headed, and startDefaultPawnWalk(false) reads that back as its feet and yaw;
                // a flyer is a camera, so it hovers one standing eye height above the feet and keeps
                // the pitch the editor camera had.
                if (defaultPawnWalk_) placeDefaultPawnAtCamera(*fromHere, yaw_, 0.0f);
                else placeDefaultPawnAtCamera(*fromHere + Vec3{0.0f, 0.0f, kWalkEye}, yaw_, pitch_);
                atCamera = false;
            } else if (atCamera)
                placeDefaultPawnAtCamera(camPos_, yaw_, pitch_);
            else if (!placePawnAtPlayerStart("the engine's default pawn")) {
                placeDefaultPawnAtCamera(camPos_, yaw_, pitch_);
                atCamera = true;
            }
            if (defaultPawnWalk_) startDefaultPawnWalk(atCamera);
            AVER_INFO("[Sandbox] Play: no GameMode declared -- possessing the engine's "
                      "AverDefaultPawn ({}, hold RMB to look). Declare an [AverGameMode] class to take over.",
                      walkCapsule_ ? "WASD to walk, Space jumps, Shift runs" : "WASD/QE to fly");
            // Mouse capture matches the toolbar option from the first frame, not just after a
            // viewport click -- the same convention the GameMode path below follows.
            releasedByUser_ = !playGameGetsMouse_;
        } else {
            AVER_WARN("[Sandbox] Play: no GameMode declared and the engine's default could not "
                      "start; the viewport camera is unchanged");
#if AVER_MODULE_SCENE
            anim::animSystem().setObjectAnimationLive(false);
#if AVER_MODULE_PHYSICS
            vehicles_.end();   // built by capturePlayWorld; no Stop will come for it
#endif
#endif
        }
        return;
    }
    const int32_t gi = aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_INSTANCE);  // 0 == none, allowed
    if (aver_fw_begin_play(gi, gm)) {
        AVER_INFO("[Sandbox] Play: begin_play GameMode='{}'{}", aver_fw_class_name(gm),
                  gi ? std::string(" GameInstance='") + aver_fw_class_name(gi) + "'" : std::string());
        // AFTER begin_play, not before: the pawn this moves doesn't exist until the GameMode has
        // spawned and possessed it.
        // CameraLocation IS A FALLBACK NOW, BUT AN ASKED-FOR ONE: playSpawnAt_ is the user's own
        // toolbar choice, not a silent override of where the project's GameMode wanted its pawn --
        // PlayerStart stays the default. Unreal places the pawn at the camera too; a Character
        // then falls to the floor.
        if (fromHere) {
            // Play From Here: the pawn's origin is its feet, exactly as at a Player Start.
            if (game::placePossessedPawn(*fromHere, degrees(yaw_)))
                AVER_INFO("[Sandbox] Play: {} spawned from here ({:.0f}, {:.0f}, {:.0f}) yaw {:.0f}",
                          aver_fw_class_name(gm), fromHere->x, fromHere->y, fromHere->z, degrees(yaw_));
            else
                AVER_WARN("[Sandbox] Play From Here: {} possessed no pawn to place; it stays where the "
                          "GameMode put it", aver_fw_class_name(gm));
        } else if (playSpawnAt_ == PlaySpawnAt::CameraLocation) {
            if (game::placePossessedPawn(camPos_, degrees(yaw_)))
                AVER_INFO("[Sandbox] Play: {} spawned at the camera ({:.0f}, {:.0f}, {:.0f}) yaw {:.0f}",
                          aver_fw_class_name(gm), camPos_.x, camPos_.y, camPos_.z, degrees(yaw_));
        } else {
            placePawnAtPlayerStart(aver_fw_class_name(gm));
        }
        releasedByUser_ = !playGameGetsMouse_;
    } else {
        AVER_WARN("[Sandbox] Play: begin_play was rejected (already playing?)");
#if AVER_MODULE_SCENE
        anim::animSystem().setObjectAnimationLive(false);
#if AVER_MODULE_PHYSICS
        vehicles_.end();   // built by capturePlayWorld; no Stop will come for it
#endif
#endif
    }
}

// THE WALKING DEFAULT PAWN (defaultPawnWalk_): a physics capsule under the possessed default pawn, the same
// character the C# Character component drives (180 cm tall, 34 cm radius, eye 160 cm, jump 465 cm/s) with the
// physics module's own stair stepping (40 cm up). `fromCamera`: the pawn stands where the camera was, so its feet
// go one eye height lower; from a Player Start (or Play From Here) the pawn is already standing at its feet.
void SandboxApp::startDefaultPawnWalk(bool fromCamera) {
#if AVER_MODULE_PHYSICS && AVER_MODULE_SCENE
    const int32_t pn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
    if (!pn) return;
    const scene::Entity pe = static_cast<scene::Entity>(static_cast<uint32_t>(pn));
    scene::World& pw = scene::World::instance();
    if (!pw.valid(pe)) return;
    Vec3 feet = pw.localTransform(pe).position;
    if (fromCamera) feet.z -= kWalkEye;
    else {                                     // a Player Start's facing becomes the view's, level head
        const Vec3 f = pw.localTransform(pe).rotation.rotate(Vec3{1, 0, 0});
        if (f.x * f.x + f.y * f.y > 1e-6f) yaw_ = std::atan2(f.y, f.x);
        pitch_ = 0.0f;
    }
    walkCapsule_ = aver_phys_character_create(kWalkRadius, kWalkHeight, feet.x, feet.y, feet.z + kWalkHeight * 0.5f);
    if (!walkCapsule_) {
        AVER_WARN("[Sandbox] Play: the physics world would not create a walking capsule; the default pawn flies");
        return;
    }
    pw.setLocalPosition(pe, feet);
    pw.setLocalRotation(pe, Quat::fromAxisAngle(Vec3{0, 0, 1}, yaw_) * Quat::fromAxisAngle(Vec3{0, 1, 0}, -pitch_));
    aver_fw_set_view(AVER_FW_VIEW_FIRST_PERSON, kWalkEye, 0.0f);
    AVER_INFO("[Sandbox] Play: the default pawn WALKS (capsule {:.0f} x {:.0f} cm, eye {:.0f} cm) from ({:.0f}, {:.0f}, {:.0f})",
              kWalkRadius, kWalkHeight, kWalkEye, feet.x, feet.y, feet.z);
#else
    (void)fromCamera;
#endif
}

// One frame of walking: WASD along the view's yaw (the synthetic --play-test keys count too), Shift runs, Space
// jumps when grounded; the vertical velocity stays the simulation's (gravity). The pawn follows the capsule, feet
// at its bottom, turned by the same yaw/pitch the flying pawn uses, so the first-person view looks where you do.
void SandboxApp::driveDefaultPawnWalk(const Vec3& fwd, const Vec3& right) {
#if AVER_MODULE_PHYSICS && AVER_MODULE_SCENE
    if (!walkCapsule_) return;
    const int32_t pn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
    if (!pn) return;
    const scene::Entity pe = static_cast<scene::Entity>(static_cast<uint32_t>(pn));
    scene::World& pw = scene::World::instance();
    if (!pw.valid(pe)) return;
#if AVER_WITH_IMGUI
#define AVER_WALK_KEY(ik, fk) (ImGui::IsKeyDown(ik) || aver_fw_input_key(fk) != 0)
    const bool jumpKey = ImGui::IsKeyPressed(ImGuiKey_Space, false) || aver_fw_input_key_pressed(AVER_FW_KEY_SPACE) != 0;
#else
#define AVER_WALK_KEY(ik, fk) (aver_fw_input_key(fk) != 0)
    const bool jumpKey = aver_fw_input_key_pressed(AVER_FW_KEY_SPACE) != 0;
#endif
    const Vec3 fh = Vec3{fwd.x, fwd.y, 0.0f}.getSafeNormal();
    const Vec3 rh = Vec3{right.x, right.y, 0.0f}.getSafeNormal();
    Vec3 wish{0, 0, 0};
    if (AVER_WALK_KEY(ImGuiKey_W, AVER_FW_KEY_W)) wish += fh;
    if (AVER_WALK_KEY(ImGuiKey_S, AVER_FW_KEY_S)) wish -= fh;
    if (AVER_WALK_KEY(ImGuiKey_D, AVER_FW_KEY_D)) wish += rh;
    if (AVER_WALK_KEY(ImGuiKey_A, AVER_FW_KEY_A)) wish -= rh;
    const f32 speed = AVER_WALK_KEY(ImGuiKey_LeftShift, AVER_FW_KEY_LSHIFT) ? kWalkRunSpeed : kWalkSpeed;
#undef AVER_WALK_KEY
    wish = wish.getSafeNormal() * speed;
    float v[3] = {0.0f, 0.0f, 0.0f};
    aver_phys_character_velocity(walkCapsule_, v);
    f32 vz = v[2];
    if (jumpKey && aver_phys_character_grounded(walkCapsule_) != 0) vz = kWalkJump;
    aver_phys_character_set_velocity(walkCapsule_, wish.x, wish.y, vz);
    float p[3];
    if (aver_phys_character_position(walkCapsule_, p) != 0)
        pw.setLocalPosition(pe, Vec3{p[0], p[1], p[2] - kWalkHeight * 0.5f});
    pw.setLocalRotation(pe, Quat::fromAxisAngle(Vec3{0, 0, 1}, yaw_) * Quat::fromAxisAngle(Vec3{0, 1, 0}, -pitch_));
#else
    (void)fwd; (void)right;
#endif
}

// True when Play is standing in a drone because the project declares no GameMode. Not a real
// play session -- aver_fw_begin_play never ran -- so aver_fw_play_state() knows nothing about it
// and every place that gates on "are we playing" has to ask this too.
bool SandboxApp::dronePlayActive() const { return droneStartedByPlay_; }

// True while Play is standing in as a plain camera because the project declares no GameMode.
// Not a play session -- begin_play never ran -- so everything that gates on "are we playing"
// has to ask this too, exactly as it already had to ask dronePlayActive().
bool SandboxApp::spectatorPlayActive() const { return defaultPawnPlay_; }

// IS ANY KIND OF PLAY RUNNING -- the question editor chrome actually wants to ask. Three ways to
// be playing, only one a play SESSION: a real GameMode session, Play standing in a drone, and Play
// standing in the engine's spectator pawn -- the two stand-ins never call aver_fw_begin_play, so
// playSessionActive() alone is right one case in three.
// WHY IT MATTERS HERE: chrome must vanish for all three, since all three mean "looking at the game
// now". Two call sites already spelled the disjunction out by hand; one name stops them drifting.
bool SandboxApp::anyPlayActive() const {
    return playSessionActive() || dronePlayActive() || spectatorPlayActive();
}

// Ends whichever kind of play is running. Both kinds, deliberately: a drone the USER switched on
// from Window > Drone is left alone, because stopping play should not take down something the
// user started for their own reasons and never asked play to own.
void SandboxApp::stopPlay() {
    // The GPU average restarts so the edit frames after Stop are not blended with the session's --
    // except while a --gpu-timing dump is still to come (gpuTimingCheck fires at --frames minus 2,
    // which for --play-test with a larger --frames is AFTER this Stop): restarting here would leave the
    // dump averaging only the edit frames after it, and the Play passes the run exists to attribute
    // would be gone. startPlay's restart already made the average Play-only up to this point.
    const bool gpuDumpPending = gpuTiming_ && maxFrames_ != 0 && !gpuTimingDone_;
    if (!gpuDumpPending) resetPlayProfile(false);
    closePlayWindow();
    // Cleared unconditionally, before anything else: a stale ejected/frame-step flag must never
    // survive into the next Play press, however this session ends.
    playEjected_ = false;
    playFrameStepPending_ = false;
    preplayViewValid_ = false;
    // PHYSICS FIRST, because the handles live IN the components: an entity destroyed by the
    // session takes its CRigidBody (and the body handle inside it) with it, and a body whose
    // handle is gone can never be removed. Tearing down here first is what keeps Play/Stop from leaking a Jolt body per destroyed entity.
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS
    editor::clearPhysicsFromScene();
    // THE CARS' BODIES TOO, and before restorePlayWorld below puts the placements back: a vehicle's
    // postPhysics writes its entity every frame, and ending it is what stops that writing. A system
    // that was never begun does nothing here.
    vehicles_.end();
#endif
    if (droneStartedByPlay_) {
        setDroneEnabled(false);
        droneStartedByPlay_ = false;
        AVER_INFO("[Sandbox] Stop: default drone stopped");
    }
    if (defaultPawnPlay_) {
        defaultPawnPlay_ = false;
        AVER_INFO("[Sandbox] Stop: engine default pawn released");
    }
#if AVER_MODULE_PHYSICS
    if (walkCapsule_) {
        aver_phys_character_destroy(walkCapsule_);
        walkCapsule_ = 0;
    }
#endif
    // Restored whether or not the default pawn was what ended, so a view this editor changed can
    // never outlive the session that changed it.
    if (savedView_) {
        savedView_ = false;
        aver_fw_set_view(savedViewMode_, savedViewEye_, savedViewBoom_);
    }
    if (aver_fw_play_state() != AVER_FW_PLAY_EDITOR) {
        aver_fw_end_play();
        AVER_INFO("[Sandbox] Stop: play session ended");
    }
    // AFTER the session ends, not before: end_play is what stops gameplay writing transforms,
    // and restoring while it can still move things would put the level back and then let the
    // last tick shove it again.
#if AVER_MODULE_SCENE
    // Object animation stops BEFORE the restore, for the same reason: not live is what drops its
    // captured bases and stops it writing CLocal, so the restore below is the last word.
    anim::animSystem().setObjectAnimationLive(false);
#endif
    restorePlayWorld();
#if AVER_MODULE_SCENE
    // Then the animators' authored clocks and the animated bodies, both of which need the level
    // already back where Play found it.
    restoreAnimatedEntities();
    // Ended AFTER the restore, so the frame that puts the level back is judged by editor rules:
    // every restored entity rejoins the GI bake in one rebuild.
    if (playMobility_.active()) {
        AVER_INFO("[Sandbox] Stop: {} entity/entities moved during play and were kept out of the "
                  "GI bake; they rejoin it now", playMobility_.movableCount());
        playMobility_.end();
    }
#endif
}

// Publishes this frame's keyboard and mouse into the framework, for the C# Input class -- by
// filling in the editor's policy and handing it to the SHARED publisher, which is now the only
// thing in the tree that turns accumulated input into aver_fw_* calls.
//
// WHAT USED TO BE HERE was a second implementation of game::publishInput: 47 hand-enumerated
// ImGui::IsKeyDown reads filling the named AVER_FW_KEY_* slots, sitting beside a loop filling the
// raw-VK twin from input_. Two different sources for the two halves of the same physical key, in
// the one host anybody actually plays a project in. That was survivable while nothing read both;
// the .ocinput scheme system (commit 02bf9fac) reads the raw contract back through EnhancedInput,
// which made it a second consumer's problem. InputPublishPolicy's own comment in
// Runtime/include/aver/game/GameInput.hpp is the long version -- those fields exist because of
// this function.
//
// AND IT IS A WIDENING, NOT A NO-OP, which is the one thing about this worth watching for. The 47
// slots themselves are the same 47 either way -- that hand-written list and frameworkKeyFromVk
// (aver/framework/InputKeys.hpp) happen to cover the identical set -- but the publisher loops the
// whole VK table into them instead of naming ImGui keys one by one, and the table is deliberately
// UNSIDED where the list was not: it maps VK_SHIFT / VK_CONTROL / VK_MENU, because WM_KEYDOWN
// delivers the unsided code unless the receiver does the extended-key dance, while the list asked
// ImGui for ImGuiKey_LeftShift / LeftCtrl / LeftAlt specifically. So RIGHT shift, ctrl and alt now
// reach gameplay through the LSHIFT/LCTRL/LALT slots in the editor, where before they reached
// nothing, whenever own_.keyboardToGame is true -- and anything added to that table later arrives
// in both hosts at once rather than in the runtime alone. That is the point of the convergence
// rather than a side effect of it: a project that behaves differently under Play than it does in a
// shipped build is the defect this whole slice exists to remove. It is still a behaviour change
// for anyone playing in the editor with the right-hand modifiers.
void SandboxApp::pushInput(bool uiActive) {
    // NO aver_fw_input_new_frame() AND NO EARLY RETURN LEFT IN HERE, and both deletions are the
    // same fact: publishInput calls new_frame itself and then writes EVERY slot -- named, raw VK,
    // mouse and pad -- with an explicit value whatever the policy decided. new_frame rolls cur
    // into prev WITHOUT clearing cur[], so any path that skips a slot republishes last frame's
    // answer for as long as it keeps skipping while aver_fw_tick runs: hold fire, press the
    // release-mouse chord, and the weapon keeps firing with the mouse untouched.
    // THE HEADLESS RETURN WENT WITH THE ImGui READS THAT NEEDED IT. It guarded a GetIO() that
    // would dereference a null GImGui in a run that never called uiInit(); nothing below reads
    // ImGui outside the chord block, which keeps its own uiActive test. Publishing anyway is also
    // the more correct answer, not merely a safe one: own_ is left all-false when ownership was
    // never resolved, and "nothing owns the device" is exactly what a headless run means.
    // EJECTED IS ITS OWN CLAUSE, not folded into releasedByUser_: the pawn must not walk while the
    // editor camera flies, whether or not the player separately released the mouse.
    const bool suppressed = !uiActive || playEjected() || (releasedByUser_ && playSessionActive());

    game::InputPublishPolicy policy;
    // THE MASTER SWITCH IS `suppressed`, AND THE TWO PER-DEVICE FIELDS BELOW ALREADY CONTAIN IT --
    // resolveInputOwnership derives both from the identical term (sandbox/src/InputOwnership.cpp:
    // 15-21). So this is not a second, subtly different gate laid over the keyboard and the mouse;
    // for them it is a restatement, and what it actually decides on its own is the pad and the
    // all-slots release.
    policy.focused        = !suppressed;
    // READ, NOT RE-DERIVED: own_ was resolved once this frame, before this call, by
    // resolveInputOwnership -- the whole reason that function exists is that this question used to
    // have eleven slightly different spellings (sandbox/src/InputOwnership.hpp).
    policy.keyboardToGame = own_.keyboardToGame;
    policy.mouseToGame    = own_.mouseToGame;
    // TRUE, AND NOT own_.keyboardToGame: the pad's gate is `suppressed` alone, which policy.focused
    // just above already is. An ImGui text field steals a keystroke, never a controller button, so
    // the uiWantsKeyboard clause folded into own_.keyboardToGame has nothing to say about a
    // gamepad. publishInput calls publishGamepad itself from this field, which is why the direct
    // game::publishGamepad(!suppressed) that used to close this function is gone -- leaving it
    // would poll the device twice a frame and publish the second answer over the first.
    policy.gamepadActive  = true;
    policy.captured       = mouse_.captured();
    policy.capturedDx     = mouse_.dx();
    policy.capturedDy     = mouse_.dy();

    // THE WHEEL NOW COMES FROM input_ EVEN WHILE THE MOUSE IS CAPTURED, the second behaviour this
    // rewrite deliberately changes rather than merely relocates. The captured branch that stood
    // here read io.MouseWheel -- a different source from the uncaptured branch immediately beside
    // it, and the frame-late one at that, since ImGui merges queued wheel events at NewFrame
    // and onUpdate runs before it (the fly block's own note, sandbox/src/SandboxApp.cpp:2165-2182,
    // spells out why every io.MouseWheel read from onUpdate is zero). Capture confines and hides
    // the cursor and has no opinion at all about the wheel, which is what GameInput.cpp's comment
    // on policy.captured already states. SCOPE, so nobody has to discover it: the wheel during a
    // CAPTURED Play session inside the editor, and nothing else -- the fly camera's speed wheel and
    // --wheel-speed-test read input_ directly and are untouched.

    // THE RECAPTURE CLICK IS EATEN, and only that one button: eating the whole mouse would take
    // LOOK with it, costing the ability to turn until you let go -- a worse bug than the one being
    // fixed. CONSUMED AGAINST input_ NOW rather than ImGui::IsMouseDown(0), because the latch has
    // to be cleared by the same stream that publishes the button; a latch tested against one
    // source and applied to another can hold a frame past the release, or release a frame early.
    // Its SET site is still ImGui's click edge (sandbox/src/SandboxApp.cpp:2324-2331), which is
    // fine while both readers see one Win32 stream (--input-source-test asserts exactly that) but
    // is the next thing to move.
    if (eatRecaptureClick_ && !input_.mouseHeld(0)) eatRecaptureClick_ = false;
    policy.eaten[AVER_FW_KEY_MOUSE_LEFT] = eatRecaptureClick_;

#if AVER_WITH_IMGUI
    // THE DRAWER CHORDS STAY HERE AND STAY ImGui-SOURCED. keybinds_.pressed takes an ImGuiIO&, and
    // ImGuiKey and CommandId must not cross into the library -- the same line InputOwnership.hpp
    // already draws. What crosses is only the RESULT, as framework key ids: the key that opened a
    // panel must not also make the pawn jump, and publishInput still publishes an explicit release
    // into an eaten slot rather than skipping it.
    // uiActive GUARDS THE GetIO() AND NOTHING ELSE now. A run that never called uiInit() has no
    // ImGui context -- GImGui is null and GetIO() faults on the first member read -- and
    // `suppressed` is already true there anyway, so there is no chord to resolve either way.
    if (uiActive) {
        ImGuiIO& io = ImGui::GetIO();
        const bool chordSpace = !suppressed && keybinds_.pressed(editor::CommandId::DrawerToggleContent, io);
        const bool chordEsc   = !suppressed && drawer_ != Drawer::None &&
                                keybinds_.pressed(editor::CommandId::DrawerDismiss, io);
        policy.eaten[AVER_FW_KEY_SPACE]  = chordSpace;
        policy.eaten[AVER_FW_KEY_LCTRL]  = chordSpace;
        policy.eaten[AVER_FW_KEY_ESCAPE] = chordEsc;
    }
#endif

    game::publishInput(input_, policy);
}

// While playing, drives the view camera from the pawn's published view node, falling back to the
// pawn's own matrix. Row 3 is the position, row 0 the forward (+X) axis.
void SandboxApp::drivePlayCamera() {
    // THE DRONE STANDS IN AS THE PAWN WHEN THERE IS NO GameMode: the drone fallback never calls
    // aver_fw_begin_play, so without this the camera would sit still while the drone flies.
    firstPersonPawn_ = game::drivePlayCamera(camPos_, yaw_, pitch_,
                                              dronePlayActive() ? droneEntity_ : scene::kInvalidEntity);
}

#endif

#if AVER_MODULE_SCENE
// STOP, after restorePlayWorld: every animated entity's CAnimator goes back to its authored time, speed
// and flags (Play advanced the clock, and the next Play must start from t0), and its kinematic body goes
// back to the transform just restored. A kinematic body's origin is its entity's pivot (that is what
// driveKinematicBodies relies on), so the entity's world transform is the body's pose; the velocities
// MoveKinematic left on it are zeroed so the first physics step of the next Play does not move it.
void SandboxApp::restoreAnimatedEntities() {
    scene::World& w = scene::World::instance();
    for (const auto& kv : entityAnim_) {
        const scene::Entity e = static_cast<scene::Entity>(kv.first);
        if (!w.valid(e)) continue;
        const EntityAnim authored = kv.second;   // a copy: writeEntityAnim assigns into this very map
        writeEntityAnim(e, authored);
#if AVER_MODULE_PHYSICS
        const auto body = entityBodies_.find(kv.first);
        if (body == entityBodies_.end() ||
            aver_phys_body_motion_type(body->second) != AVER_PHYS_MOTION_KINEMATIC) continue;
        const Transform xf = worldTransformOf(w, e);
        aver_phys_body_set_position(body->second, xf.position.x, xf.position.y, xf.position.z);
        aver_phys_body_set_rotation(body->second, xf.rotation.x, xf.rotation.y, xf.rotation.z, xf.rotation.w);
        aver_phys_body_set_velocity(body->second, 0.0f, 0.0f, 0.0f);
        aver_phys_body_set_angular_velocity(body->second, 0.0f, 0.0f, 0.0f);
#endif
    }
}

#if AVER_MODULE_PHYSICS
// PLAY, after the anim tick: hands the host's animated bodies -- every entity with an EntityAnim whose body
// is kinematic -- to world::driveKinematicBodies, which moves each to where its entity now is. Built from
// entityAnim_ and entityBodies_ rather than the loaded LevelInstance's animatedBodies, because a body
// remade by an edit or a Stop has a new handle that list would still name the old one of; a body that
// stayed static (see rebuildEntityBody) is left out, as instantiate leaves it out of its own list.
// Nothing is driven while Play is paused: the physics world is not stepping, and the held clips have
// not moved the entities anyway.
//
// THE LIST IS BUILT WHEN IT CAN HAVE CHANGED, NOT EVERY FRAME: building it is a hash lookup into the
// level's body table and a locked physics query per animated entity (691 in NeonDistrict), to produce
// the same list. What it is made from changes in three ways, and each is caught. Entities gaining or
// losing an animation or a body change the two table sizes. A body remade or dropped under the same
// sizes bumps colliderRev_ (rebuildEntityBody, destroyEntity). And a body that physics reports gone or
// no longer kinematic (a script, a path that bumps neither) makes the drive itself say so. Any frame
// not driving drops the list, so every Play starts from a fresh one.
void SandboxApp::driveAnimatedBodies(f32 dt) {
    if (entityAnim_.empty() || !anim::animSystem().objectAnimationLive() ||
        anim::animSystem().objectAnimationPaused()) {
        animatedBodiesBuilt_ = false;
        return;
    }
    if (!animatedBodiesBuilt_ || animatedBodiesFromAnim_ != entityAnim_.size() ||
        animatedBodiesFromBodies_ != entityBodies_.size() || animatedBodiesFromRev_ != colliderRev_) {
        animatedBodies_.clear();
        for (const auto& kv : entityAnim_) {
            const auto body = entityBodies_.find(kv.first);
            if (body != entityBodies_.end() &&
                aver_phys_body_motion_type(body->second) == AVER_PHYS_MOTION_KINEMATIC)
                animatedBodies_.push_back({static_cast<scene::Entity>(kv.first), body->second});
        }
        animatedBodiesBuilt_ = true;
        animatedBodiesFromAnim_ = entityAnim_.size();
        animatedBodiesFromBodies_ = entityBodies_.size();
        animatedBodiesFromRev_ = colliderRev_;
    }
    if (world::driveKinematicBodies(scene::World::instance(), animatedBodies_, dt) != 0)
        animatedBodiesBuilt_ = false;
}
#endif
#endif

// Aspect comes from the viewport rect (the dockspace's central node), not the whole window.
f32 SandboxApp::viewAspect() const { return vpH_ > 0.5f ? vpW_ / vpH_ : 1.777f; }

bool SandboxApp::inViewport(f32 mx, f32 my) const { return mx >= vpX_ && mx < vpX_+vpW_ && my >= vpY_ && my < vpY_+vpH_; }

// The camera's forward axis, built from yaw and pitch.
Vec3 SandboxApp::camForward() const {
    return game::cameraForward(yaw_, pitch_);
}

#if AVER_MODULE_SCRIPTING
// THE ANIMATION-NOTIFY WIRE: a clip crosses a marker, the marker names a graph event, the entity
// playing the clip raises it. Each part belongs to a different module, and this is the only place
// they meet -- why the anim module takes a function pointer instead of knowing what a graph is.
// A MISSING HANDLER IS NOT AN ERROR HERE: graphFire's false cases aren't worth a line per frame --
// the managed router already logs each once per (entity, event).
// Answers the framework's relayed curve query out of the animation system.
 i32 SandboxApp::animCurve(i32 entity, i64 nameHash, f32* outValue, void*) {
    // SCRIPTING DOES NOT IMPLY THE SCENE, unlike FRAMEWORK/PARTICLES/SYNAPSE_SCENE above: this
    // function's own signature stays scene-free (an i32, not a scene::Entity, is exactly what
    // makes that possible), but the LOOKUP needs a real scene::Entity to hand to
    // anim::animSystem() -- and Aver.Anim.Scene is itself only added under
    // if(AVER_MODULE_SCENE) (root CMakeLists.txt:362-366), so a scene-off tree has neither the
    // type nor the module to ask. The host keeps answering "no such curve" rather than losing the
    // callback slot the scripting bridge already wired up.
#if AVER_MODULE_SCENE
    f32 v = 0.0f;
    if (!anim::animSystem().curveValue(static_cast<scene::Entity>(entity),
                                       static_cast<u64>(nameHash), v)) return 0;
    *outValue = v;
    return 1;
#else
    (void)entity; (void)nameHash; (void)outValue;
    return 0;
#endif
}

#endif

#if AVER_MODULE_SCRIPTING
#if AVER_MODULE_SYNAPSE_SCENE
// Answers the framework's relayed Synapse steering-target query -- same shape as animCurve
// immediately above, installed the same way.
 i32 SandboxApp::synapseTarget(i32 entity, f32* outX, f32* outY, f32* outZ, void*) {
    scene::World& w = scene::World::instance();
    const auto* a = w.component<synapse::CSynapseAgent>(static_cast<scene::Entity>(entity),
                                                         synapse::agentSystem().componentType());
    if (!a || a->status != static_cast<i32>(synapse::AgentStatus::Pathing)) return 0;
    *outX = a->targetXCm;
    *outY = a->targetYCm;
    *outZ = a->targetZCm;
    return 1;
}

#endif
#endif

#if AVER_MODULE_SCRIPTING
#if AVER_MODULE_SYNAPSE_SCENE
#if AVER_MODULE_FRAMEWORK
// PerceptionSystem's own TargetResolverFn -- who agents should perceive. Same one-liner as
// GameApp::synapseTargetResolver.
 scene::Entity SandboxApp::synapseTargetResolver(void*) {
    return static_cast<scene::Entity>(aver_fw_controlled_pawn(aver_fw_player_controller(0)));
}

#endif
#endif
#endif

#if AVER_MODULE_SCRIPTING
#if AVER_MODULE_SYNAPSE_SCENE
// Answers the framework's relayed Synapse perception query -- same shape as synapseTarget
// above, installed the same way.
 i32 SandboxApp::synapsePerception(i32 entity, i32* outCanSee, i32* outLastTarget,
                             f32* outTimeSinceSeen, void*) {
    scene::World& w = scene::World::instance();
    const auto* p = w.component<synapse::CSynapsePerception>(
        static_cast<scene::Entity>(entity), synapse::perceptionSystem().componentType());
    if (!p) return 0;
    *outCanSee = p->canSeeTarget;
    *outLastTarget = p->lastKnownTargetEntity;
    *outTimeSinceSeen = p->timeSinceSeenSec;
    return 1;
}

#endif
#endif

// SCENE AS WELL AS SCRIPTING, because the PARAMETER decides this one: scene::Entity comes from
// Aver.Scene, so a scripting-on/scene-off tree could not compile this signature however willing the
// scripting host was. The declaration in SandboxApp.hpp carries both terms for the same reason, and
// the two disagreeing is what the module matrix's scene-off row found.
#if AVER_MODULE_SCRIPTING && AVER_MODULE_SCENE
 void SandboxApp::animNotify(scene::Entity e, const char* name, void* user) {
    auto* self = static_cast<SandboxApp*>(user);
    if (!self || !name) return;
    self->scripts_.graphFire(static_cast<i32>(e), name);
}

#endif

// Gives the mouse to the game or hands it back. ShowCursor is a counter, so each call is paired.
void SandboxApp::setMouseCaptured(bool on) {
    mouse_.set(on, captureWindow(), "Sandbox", " (Shift+F1 to release)");
}

// Measures one frame of captured mouse movement, then re-centres for the next.
void SandboxApp::pollCapturedMouse() {
    mouse_.poll(captureWindow());
}

// The scene renders in the top-left of the present image at the play window's size (clamped to the
// editor window, which the present images are sized from); the device mirrors that rect.
void SandboxApp::openPlayWindow(Engine& e) {
    rhi::IDevice* dev = e.device();
    if (!dev || !window_ || playWindow_) return;
    const f32 dpi = window_->dpiScale() > 0.0f ? window_->dpiScale() : 1.0f;
    WindowDesc d;
    d.title = "Aver Engine - Play (Esc to stop)";
    d.width  = static_cast<u32>(std::fmax(320.0f, vpW_) / dpi);   // logical; create() applies the DPI
    d.height = static_cast<u32>(std::fmax(180.0f, vpH_) / dpi);
    d.quitOnDestroy = false;
    auto w = std::make_unique<Window>();
    if (!w->create(d)) {
        AVER_WARN("[Sandbox] Play in New Window: the window could not be created; playing in the viewport");
        return;
    }
    const u32 pw = std::min(w->width(), window_->width());
    const u32 ph = std::min(w->height(), window_->height());
    if (!dev->setMirrorWindow(w->nativeHandle(), pw, ph)) {
        AVER_WARN("[Sandbox] Play in New Window needs the D3D12 renderer; playing in the viewport instead");
        w->destroy();
        return;
    }
    w->setEventCallback(&sandboxWindowEvent, &input_);
    mouse_.set(false, window_, "Sandbox", "");   // re-captured against the new window next frame
    playWindow_ = std::move(w);
    playWindowDevice_ = dev;
    playWindowW_ = pw;
    playWindowH_ = ph;
    AVER_INFO("[Sandbox] Play in New Window: {}x{}", pw, ph);
}

void SandboxApp::closePlayWindow() {
    if (!playWindow_) return;
    mouse_.set(false, playWindow_.get(), "Sandbox", "");
    if (playWindowDevice_) playWindowDevice_->setMirrorWindow(nullptr, 0, 0);
    playWindow_->destroy();
    playWindow_.reset();
    playWindowDevice_ = nullptr;
    if (window_) window_->focus();
}

void SandboxApp::updatePlayWindow(Engine& e) {
    if (!playWindow_) return;
    if (playWindow_->shouldClose()) { stopPlay(); return; }
    if (!anyPlayActive()) { closePlayWindow(); return; }
    if (!playWindow_->inModalResize() && e.device() && window_) {
        const u32 pw = std::min(playWindow_->width(), window_->width());
        const u32 ph = std::min(playWindow_->height(), window_->height());
        if (pw > 0 && ph > 0 && (pw != playWindowW_ || ph != playWindowH_) &&
            e.device()->setMirrorWindow(playWindow_->nativeHandle(), pw, ph)) {
            playWindowW_ = pw;
            playWindowH_ = ph;
        }
    }
    vpX_ = 0.0f;
    vpY_ = 0.0f;
    vpW_ = static_cast<f32>(playWindowW_);
    vpH_ = static_cast<f32>(playWindowH_);
}

// True while a game is playing, in a build with or without the framework.
bool SandboxApp::playSessionActive() const {
#if AVER_MODULE_FRAMEWORK
    return aver_fw_play_state() != AVER_FW_PLAY_EDITOR;
#else
    return false;
#endif
}

// Starts `m` (refused while any play is active, like the Play button disabling itself) and
// remembers it as the main toolbar button's mode. Declared unguarded so the toolbar compiles in
// every configuration; a framework-less build has no session to start.
void SandboxApp::launchPlay(Engine& e, PlayMode m) {
#if AVER_MODULE_FRAMEWORK
    if (anyPlayActive()) return;
    playMode_ = m;
    AVER_INFO("[Sandbox] Play: {}", m == PlayMode::SelectedViewport ? "Selected Viewport" :
                                     m == PlayMode::Simulate ? "Simulate" :
                                     m == PlayMode::NewWindow ? "New Window" : "Standalone");
    switch (m) {
    case PlayMode::SelectedViewport:
        startPlay();
        break;
    case PlayMode::NewWindow:
        startPlay();
        if (anyPlayActive()) openPlayWindow(e);
        break;
    case PlayMode::Simulate:
        startPlay();
        // Unreal's Simulate starts ejected: gameplay and physics tick, but the editor keeps the
        // camera, input and tools -- F8 possesses.
        if (playSessionActive()) playEjected_ = true;
        break;
    case PlayMode::Standalone:
        launchInRuntime(e);
        break;
    }
#else
    (void)e; (void)m;
#endif
}

// PLAY FROM HERE (viewport right-click): starts a viewport Play with the pawn's feet on `surface` (the
// point the ray hit) lifted a few cm, facing the editor camera's yaw. Always the Selected Viewport
// mode -- Simulate possesses nobody and Standalone is another process, neither has a pawn to put
// anywhere -- and playMode_ is deliberately NOT written, so the main Play button keeps repeating what
// the user chose. startPlay consumes playFromHere_ (one shot), and playSpawnAt_ is never touched.
// No Engine& needed: only Standalone reaches into it, and this is never Standalone.
void SandboxApp::playFromHere(const Vec3& surface) {
#if AVER_MODULE_FRAMEWORK
    if (anyPlayActive()) return;
    playFromHere_ = surface + Vec3{0.0f, 0.0f, kPlayFromHereLiftCm};
    AVER_INFO("[Sandbox] Play: from here ({:.0f}, {:.0f}, {:.0f}), yaw {:.0f}",
              playFromHere_->x, playFromHere_->y, playFromHere_->z, degrees(yaw_));
    startPlay();
    playFromHere_.reset();   // startPlay already took it; this only matters if it ever returns before doing so
#else
    (void)surface;
#endif
}

// F8. Only meaningful in a real framework session (playSessionActive()); a no-op otherwise.
void SandboxApp::togglePlayEject() {
#if AVER_MODULE_FRAMEWORK
    if (!playSessionActive()) return;
    playEjected_ = !playEjected_;
    // Possessing again hands the mouse back to the game, same as a fresh session start.
    if (!playEjected_) releasedByUser_ = false;
#endif
}

// Shift+F while ejected (CommandId::PlayPawnToCamera). The reason to eject in a large level is to fly
// somewhere else; this brings the possessed pawn along, so F8 resumes play THERE instead of snapping
// the view back to wherever the pawn was left. Placed by the same rule as Play's "spawn at the
// camera": a flying default pawn IS the camera, pitch and all; anything that stands gets its feet one
// eye height below it (feetBelowCamera), so a first-person view comes back where the camera was.
//
// THE CAPSULE MOVES TOO, AND THAT IS THE WHOLE DIFFICULTY. A walking pawn's position belongs to its
// physics character, and whoever drives it copies the capsule back onto the entity every tick
// (driveDefaultPawnWalk here, Character.SyncFromSimulation in C#) -- an entity moved on its own is
// back where it started one frame later. The default pawn's capsule is walkCapsule_. A project pawn's
// is a private field of its managed AverCharacter, so it is found by the entity stamp that class puts
// on it (aver_phys_character_of_entity) and measured (aver_phys_character_shape), since a character's
// position is its capsule's CENTRE and only the owner knew the height.
//
// AND THE FACING: the camera's yaw on the pawn, its pitch on the pawn's view node when it has one (a
// character's head). An AverCharacter keeps both privately and used to write its old facing straight
// back; Character.Drive now takes a facing it did not write as given, so it turns to the camera too.
void SandboxApp::teleportPawnToCamera() {
#if AVER_MODULE_FRAMEWORK
    if (!playEjected()) return;
    const int32_t pn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
    const scene::Entity pe = static_cast<scene::Entity>(static_cast<uint32_t>(pn));
    scene::World& pw = scene::World::instance();
    if (!pn || !pw.valid(pe)) {
        AVER_WARN("[Sandbox] Pawn to Camera: this session has no possessed pawn to move");
#if AVER_WITH_IMGUI
        notifyOutcome(editor::NotifySeverity::Warning, "No pawn to move",
                      "This play session has no possessed pawn.");
#endif
        return;
    }

    Vec3 feet = camPos_;
    const char* what = "the flying default pawn, at the camera itself";
    if (defaultPawnPlay_ && !walkCapsule_) {
        placeDefaultPawnAtCamera(camPos_, yaw_, pitch_);
    } else if (defaultPawnPlay_) {
        what = "the walking default pawn and its capsule";
        f32 ownFeetZ = -FLT_MAX;
#if AVER_MODULE_PHYSICS
        float wp[3];
        if (aver_phys_character_position(walkCapsule_, wp) != 0) ownFeetZ = wp[2] - kWalkHeight * 0.5f;
#endif
        feet = feetBelowCamera(camPos_, kWalkEye, walkCapsule_, ownFeetZ);
#if AVER_MODULE_PHYSICS
        aver_phys_character_set_position(walkCapsule_, feet.x, feet.y, feet.z + kWalkHeight * 0.5f);
        // Stopped dead: a capsule keeps the last velocity it was given for as long as nothing drives
        // it, and nothing does while ejected -- it would walk off from where it was just put.
        aver_phys_character_set_velocity(walkCapsule_, 0.0f, 0.0f, 0.0f);
#endif
        // The same two writes driveDefaultPawnWalk makes once possessed, so the pawn is already
        // where and how that function will next put it.
        placeDefaultPawnAtCamera(feet, yaw_, pitch_);
    } else {
        what = "the pawn";
        // The eye height the game camera itself puts this pawn's view at (aver_fw_view: what a
        // character publishes every tick it drives, the framework's default otherwise; GameCamera.hpp
        // stands the view at pawn + up * eye), so feet = camera - eye is the inverse of where F8 will
        // put the view back.
        int32_t viewMode = 0; float eye = 0.0f, boom = 0.0f;
        aver_fw_view(&viewMode, &eye, &boom);
        int32_t self = 0;
        // The pawn's own feet, for feetBelowCamera's camera-inside-the-pawn case: its entity origin.
        const f32 ownFeetZ = pw.localTransform(pe).position.z;
#if AVER_MODULE_PHYSICS
        const int32_t capsule = aver_phys_character_of_entity(pn);
        const auto* rb = pw.component<scene::CRigidBody>(pe, scene::kComponentRigidBody);
        const int32_t body = (!capsule && rb) ? rb->body : 0;
        self = capsule ? capsule : body;
        // Neither found: only the entity moves. That sticks for a pawn nothing else positions, and is
        // undone at the next tick by one whose own script copies some other body back onto it -- which
        // this cannot see, so the log says which case it was rather than claiming success either way.
        if (!self) what = "the pawn (no physics capsule or body found for it: if its own script positions it, the move will not stay)";
#endif
        feet = feetBelowCamera(camPos_, eye, self, ownFeetZ);
#if AVER_MODULE_PHYSICS
        if (capsule) {
            what = "the pawn and its character capsule";
            float radius = 0.0f, height = kWalkHeight;
            aver_phys_character_shape(capsule, &radius, &height);
            aver_phys_character_set_position(capsule, feet.x, feet.y, feet.z + height * 0.5f);
            aver_phys_character_set_velocity(capsule, 0.0f, 0.0f, 0.0f);
        } else if (body) {
            // A pawn that IS a rigid body (a level entity with a CRigidBody, possessed): the body is
            // built at its entity's own position (PhysicsSceneSync.cpp), so it goes to the same
            // point, upright and at rest, facing the camera's yaw.
            what = "the pawn and its rigid body";
            const Quat q = Quat::fromAxisAngle(Vec3{0, 0, 1}, yaw_);
            aver_phys_body_set_position(body, feet.x, feet.y, feet.z);
            aver_phys_body_set_rotation(body, q.x, q.y, q.z, q.w);
            aver_phys_body_set_velocity(body, 0.0f, 0.0f, 0.0f);
            aver_phys_body_set_angular_velocity(body, 0.0f, 0.0f, 0.0f);
        }
#endif
        game::placePossessedPawn(feet, degrees(yaw_));
        // The pitch, on the pawn's head: the view node it publishes, when that node really hangs off
        // this pawn (the published id is process-wide and can outlive whoever set it). Written the way
        // Character.ApplyLookRotation writes it, Rot(0, -pitch, 0), which Drive then reads back.
        const int32_t vn = aver_fw_view_entity();
        const scene::Entity ve = static_cast<scene::Entity>(static_cast<uint32_t>(vn));
        if (vn && pw.valid(ve) && pw.parent(ve) == pe)
            pw.setLocalRotation(ve, Quat::fromAxisAngle(Vec3{0, 1, 0}, -pitch_));
    }
    AVER_INFO("[Sandbox] Pawn to Camera: moved {} to ({:.0f}, {:.0f}, {:.0f}) yaw {:.0f}", what,
              feet.x, feet.y, feet.z, degrees(yaw_));
#if AVER_WITH_IMGUI
    // Named only when it has a chord: a cleared binding would read "none possesses it there".
    const editor::Chord& eject = keybinds_.chordFor(editor::CommandId::PlayEject);
    notifyOutcome(editor::NotifySeverity::Info, "Pawn moved to the camera",
                  eject.isBound() ? editor::chordToString(eject) + " possesses it there."
                                  : std::string("Possess it to resume play there."));
#endif
#endif
}

// Frame Skip: only while the session is actually paused, so a stray press mid-play cannot pause
// it for one tick and resume it a frame later than the button shows.
void SandboxApp::requestPlayFrameStep() {
#if AVER_MODULE_FRAMEWORK
    if (aver_fw_play_state() == AVER_FW_PLAY_PAUSED) playFrameStepPending_ = true;
#endif
}

// True while a real session runs ejected. playSessionActive() already reads false without the
// framework, so this needs no guard of its own.
bool SandboxApp::playEjected() const { return playEjected_ && playSessionActive(); }

// True while controller 0 possesses a pawn that is a live entity -- what Pawn to Camera needs, asked
// before its chord may take Shift+F from Frame Selected (with no pawn, Shift+F frames as it always did).
bool SandboxApp::hasPossessedPawn() const {
#if AVER_MODULE_FRAMEWORK
    const int32_t pn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
    return pn != 0 && scene::World::instance().valid(static_cast<scene::Entity>(static_cast<uint32_t>(pn)));
#else
    return false;
#endif
}

} // namespace aver
