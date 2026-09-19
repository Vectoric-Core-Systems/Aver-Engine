// Runtime side: the play session, input publishing, the play camera, animation curves/notifies, mouse capture.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

#include "SandboxApp.hpp"
#include "aver/game/GameCamera.hpp"
#include "aver/game/GameInput.hpp"
#include "aver/game/GamePawn.hpp"

namespace aver {
#if AVER_MODULE_FRAMEWORK

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

    // AND THE AUTHORED PHYSICS BECOMES REAL, here and nowhere else: a CRigidBody is data until
    // PLAY turns it into a Jolt body -- building at LOAD would mean a crate settling while someone
    // was only looking around, the same complaint that got gameplay graphs gated out of Select mode.
    // AFTER the snapshot, deliberately: Stop restores from it, so it must record the level BEFORE
    // physics touched anything.
#if AVER_MODULE_PHYSICS
    editor::syncPhysicsFromScene();
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
    // BEFORE anything begins, so what is recorded is the editor's level and not one frame of
    // gameplay's effect on it.
    capturePlayWorld();

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
            // Unreal spawns at a PlayerStart, or the editor camera if none. There was no
            // PlayerStart concept here then, so this kept you where you were.
            // THE PLAYER START FIRST, THE CAMERA SECOND -- the half this could not do when
            // written: the marker exists now, and playerStartTransform resolves it or the level's
            // SPAWN record, keeping the camera fallback for a level with no marker.
            if (!placePawnAtPlayerStart("the engine's default pawn")) {
                const int32_t pn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
                if (pn) {
                    const scene::Entity pe = static_cast<scene::Entity>(static_cast<uint32_t>(pn));
                    scene::World& pw = scene::World::instance();
                    if (pw.valid(pe)) {
                        pw.setLocalPosition(pe, camPos_);
                        pw.setLocalRotation(pe, Quat::fromAxisAngle(Vec3{0, 0, 1}, yaw_) *
                                                Quat::fromAxisAngle(Vec3{0, 1, 0}, -pitch_));
                    }
                }
            }
            AVER_INFO("[Sandbox] Play: no GameMode declared -- possessing the engine's "
                      "AverDefaultPawn (WASD/QE to fly, hold RMB to look). Declare an "
                      "[AverGameMode] class to take over.");
        } else {
            AVER_WARN("[Sandbox] Play: no GameMode declared and the engine's default could not "
                      "start; the viewport camera is unchanged");
        }
        return;
    }
    const int32_t gi = aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_INSTANCE);  // 0 == none, allowed
    if (aver_fw_begin_play(gi, gm)) {
        AVER_INFO("[Sandbox] Play: begin_play GameMode='{}'{}", aver_fw_class_name(gm),
                  gi ? std::string(" GameInstance='") + aver_fw_class_name(gi) + "'" : std::string());
        // AFTER begin_play, not before: the pawn this moves doesn't exist until the GameMode has
        // spawned and possessed it.
        // NO FALLBACK HERE, deliberately -- unlike the default-pawn path, a project's own pawn
        // spawns where its GameMode chooses, a decision the project made; silently relocating it
        // to wherever someone was looking would override that every time Play is pressed.
        placePawnAtPlayerStart(aver_fw_class_name(gm));
    } else {
        AVER_WARN("[Sandbox] Play: begin_play was rejected (already playing?)");
    }
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
    // PHYSICS FIRST, because the handles live IN the components: an entity destroyed by the
    // session takes its CRigidBody (and the body handle inside it) with it, and a body whose
    // handle is gone can never be removed. Tearing down here first is what keeps Play/Stop from leaking a Jolt body per destroyed entity.
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS
    editor::clearPhysicsFromScene();
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
    restorePlayWorld();
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
    const bool suppressed = !uiActive || (releasedByUser_ && playSessionActive());

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
    f32 v = 0.0f;
    if (!anim::animSystem().curveValue(static_cast<scene::Entity>(entity),
                                       static_cast<u64>(nameHash), v)) return 0;
    *outValue = v;
    return 1;
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

#if AVER_MODULE_SCRIPTING
 void SandboxApp::animNotify(scene::Entity e, const char* name, void* user) {
    auto* self = static_cast<SandboxApp*>(user);
    if (!self || !name) return;
    self->scripts_.graphFire(static_cast<i32>(e), name);
}

#endif

// Gives the mouse to the game or hands it back. ShowCursor is a counter, so each call is paired.
void SandboxApp::setMouseCaptured(bool on) {
    mouse_.set(on, window_, "Sandbox", " (Shift+F1 to release)");
}

// Measures one frame of captured mouse movement, then re-centres for the next.
void SandboxApp::pollCapturedMouse() {
    mouse_.poll(window_);
}

// True while a game is playing, in a build with or without the framework.
bool SandboxApp::playSessionActive() const {
#if AVER_MODULE_FRAMEWORK
    return aver_fw_play_state() != AVER_FW_PLAY_EDITOR;
#else
    return false;
#endif
}

} // namespace aver
