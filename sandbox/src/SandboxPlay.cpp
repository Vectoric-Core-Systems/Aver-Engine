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

// Publishes this frame's keyboard and mouse into the framework, for the C# Input class.
// Suppressed while ImGui wants the input, and the editor's drawer chord wins over gameplay.
void SandboxApp::pushInput(bool uiActive) {
    aver_fw_input_new_frame();
#if AVER_WITH_IMGUI
    // NEVER RETURN AFTER new_frame(). aver_fw_input_new_frame rolls cur into prev and zeroes the
    // mouse -- it does NOT clear cur[], so an early return here means "publish LAST frame's answer
    // forever" while aver_fw_tick keeps running: hold fire, press the release-mouse chord, and the
    // weapon keeps firing with the mouse untouched.
    // THE ENGINE ALREADY LEARNED THIS ONCE, on the other host: GameInput.cpp's publishInput says
    // it in full ("the first version... assumed new_frame clears the key state. IT DOES NOT").
    // A BOOL, NOT A RETURN, so every set_key call still runs and publishes an explicit release --
    // suppression folds into the SAME kb/m terms already read, so no slot can be left behind.
    // READ, NOT RE-DERIVED: own_ was resolved once this frame by resolveInputOwnership.
    // `suppressed` survives only because the mouse branch below still needs "publish a zero" apart
    // from "publish the captured delta".
    const bool suppressed = !uiActive || (releasedByUser_ && playSessionActive());
    // NOT THE EARLY RETURN THE COMMENT ABOVE FORBIDS, and the difference is which condition.
    // The one that was rightly deleted was the mid-session RELEASE, where cur[] holds live values
    // that must still be published down or every held key latches forever. uiActive is a
    // process-lifetime property -- set once in uiInit, cleared in uiShutdown -- so when it is
    // false nothing here has ever published a 1 and there is nothing to release. Without this,
    // GetIO() below dereferences a null GImGui in any headless run.
    if (!uiActive) return;
    ImGuiIO& io = ImGui::GetIO();
    const bool kb = own_.keyboardToGame;
    const bool chordSpace = !suppressed && keybinds_.pressed(editor::CommandId::DrawerToggleContent, io);
    const bool chordEsc   = !suppressed && drawer_ != Drawer::None && keybinds_.pressed(editor::CommandId::DrawerDismiss, io);
    for (int i = 0; i < 26; ++i) aver_fw_input_set_key(AVER_FW_KEY_A + i, kb && ImGui::IsKeyDown((ImGuiKey)(ImGuiKey_A + i)));
    for (int i = 0; i < 10; ++i) aver_fw_input_set_key(AVER_FW_KEY_0 + i, kb && ImGui::IsKeyDown((ImGuiKey)(ImGuiKey_0 + i)));
    aver_fw_input_set_key(AVER_FW_KEY_SPACE,  kb && !chordSpace && ImGui::IsKeyDown(ImGuiKey_Space));
    aver_fw_input_set_key(AVER_FW_KEY_LSHIFT, kb && ImGui::IsKeyDown(ImGuiKey_LeftShift));
    aver_fw_input_set_key(AVER_FW_KEY_LCTRL,  kb && !chordSpace && ImGui::IsKeyDown(ImGuiKey_LeftCtrl));
    aver_fw_input_set_key(AVER_FW_KEY_LALT,   kb && ImGui::IsKeyDown(ImGuiKey_LeftAlt));
    aver_fw_input_set_key(AVER_FW_KEY_ENTER,  kb && ImGui::IsKeyDown(ImGuiKey_Enter));
    aver_fw_input_set_key(AVER_FW_KEY_ESCAPE, kb && !chordEsc && ImGui::IsKeyDown(ImGuiKey_Escape));
    aver_fw_input_set_key(AVER_FW_KEY_TAB,    kb && ImGui::IsKeyDown(ImGuiKey_Tab));
    aver_fw_input_set_key(AVER_FW_KEY_LEFT,   kb && ImGui::IsKeyDown(ImGuiKey_LeftArrow));
    aver_fw_input_set_key(AVER_FW_KEY_RIGHT,  kb && ImGui::IsKeyDown(ImGuiKey_RightArrow));
    aver_fw_input_set_key(AVER_FW_KEY_UP,     kb && ImGui::IsKeyDown(ImGuiKey_UpArrow));
    aver_fw_input_set_key(AVER_FW_KEY_DOWN,   kb && ImGui::IsKeyDown(ImGuiKey_DownArrow));
    // ---- THE RAW-VK TWIN, published from the editor's own accumulator ----
    // The 46-slot AVER_FW_KEY_* enum cannot reach an F-key, numpad or OEM key, and cannot be
    // renumbered (the InputKey graph node takes a literal integer). aver_fw_input_set_vk is the
    // additive answer.
    // IT READS input_, NOT ImGui: ImGui's key enum isn't Win32's, and a 256-entry reverse table
    // would rot. input_ is fed straight from the Win32 stream, already keyed by virtual key.
    // Gated on the same own_.keyboardToGame as everything above, publishing UP rather than
    // leaving anything latched.
    for (int32_t vk = 0; vk < AVER_FW_VK_COUNT; ++vk)
        aver_fw_input_set_vk(vk, (kb && input_.keyHeld(vk)) ? 1 : 0);

    // THE RECAPTURE CLICK IS EATEN, and only that one button: suppressing all of `m` would take
    // mouse LOOK with it, costing the ability to turn until you let go -- a worse bug than the one
    // being fixed. Cleared the moment the button is released, so a genuine second click fires normally.
    if (eatRecaptureClick_ && !ImGui::IsMouseDown(0)) eatRecaptureClick_ = false;
    const bool m = own_.mouseToGame;
    aver_fw_input_set_key(AVER_FW_KEY_MOUSE_LEFT,   m && !eatRecaptureClick_ && ImGui::IsMouseDown(0));
    aver_fw_input_set_key(AVER_FW_KEY_MOUSE_RIGHT,  m && ImGui::IsMouseDown(1));
    aver_fw_input_set_key(AVER_FW_KEY_MOUSE_MIDDLE, m && ImGui::IsMouseDown(2));
    // Suppressed publishes an explicit zero rather than falling through to the captured branch:
    // new_frame() already zeroes the delta, but saying so here keeps this function's contract one
    // sentence long -- every slot is written, every frame, whatever the gate decided.
    if (suppressed) aver_fw_input_set_mouse(0.0f, 0.0f, 0.0f);
    else if (mouse_.captured()) aver_fw_input_set_mouse(mouse_.dx(), mouse_.dy(), io.MouseWheel);
    // THE UNCAPTURED BRANCH IS THE EDITOR'S NORMAL ONE (mouse_.captured() is false outside Play),
    // so gameplay saw the same frame-late mouse the camera did. input_.mouseDX/DY and
    // input_.wheel() are the correctly-phased sources during onUpdate -- see the look block.
    else aver_fw_input_set_mouse(m ? static_cast<f32>(input_.mouseDX()) : 0.0f,
                                 m ? static_cast<f32>(input_.mouseDY()) : 0.0f,
                                 m ? input_.wheel() : 0.0f);

    // Gated on `suppressed`, not `kb`: an ImGui text field steals a keystroke, never a controller
    // button, so the extra uiWantsKeyboard clause folded into `kb` has nothing to do with a
    // gamepad -- `suppressed` alone (window unfocused, or the player released the mouse
    // mid-session) is the whole of when the game does not own this device. See
    // game::publishGamepad's own comment for why it still polls unconditionally.
    game::publishGamepad(!suppressed);
#endif
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
