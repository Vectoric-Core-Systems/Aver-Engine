// Runtime side: the play session, input publishing, the play camera, animation curves/notifies, mouse capture.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

#include "SandboxApp.hpp"
#include "aver/platform/Gamepad.hpp"

namespace aver {
#if AVER_MODULE_FRAMEWORK
namespace {
// GamepadState's arrays are laid out in the ABI's own documented order (Gamepad.hpp's own comment
// spells the order out without including this header) precisely so pushInput's loops below can walk
// it index-for-index instead of a name-by-name switch. Pinned here, the same guard
// Runtime/src/GameInput.cpp carries for its own copy of this same loop -- see that file for why a
// silent count drift is worse than a compile error.
static_assert(GamepadState::kButtonCount == static_cast<usize>(AVER_FW_GAMEPAD_BUTTON_COUNT),
              "GamepadState::buttons and AVER_FW_GAMEPAD_* have drifted apart");
static_assert(GamepadState::kAxisCount == static_cast<usize>(AVER_FW_GAMEPAD_AXIS_COUNT),
              "GamepadState::axes and AVER_FW_GAMEPAD_AXIS_* have drifted apart");
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
    for (const scene::Entity e : levelEntities_)
        if (w.valid(e)) playWorldSnapshot_.push_back({e, w.localTransform(e)});
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
    const int32_t pn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
    if (!pn) return false;
    const scene::Entity pe = static_cast<scene::Entity>(static_cast<uint32_t>(pn));
    scene::World& pw = scene::World::instance();
    if (!pw.valid(pe)) return false;
    pw.setLocalPosition(pe, sp);
    pw.setLocalRotation(pe, quatFromEulerDeg(Vec3{0.0f, 0.0f, sy}));
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
    else if (mouseCaptured_) aver_fw_input_set_mouse(captureDx_, captureDy_, io.MouseWheel);
    // THE UNCAPTURED BRANCH IS THE EDITOR'S NORMAL ONE (mouseCaptured_ is false outside Play),
    // so gameplay saw the same frame-late mouse the camera did. input_.mouseDX/DY and
    // input_.wheel() are the correctly-phased sources during onUpdate -- see the look block.
    else aver_fw_input_set_mouse(m ? static_cast<f32>(input_.mouseDX()) : 0.0f,
                                 m ? static_cast<f32>(input_.mouseDY()) : 0.0f,
                                 m ? input_.wheel() : 0.0f);

    // ---- GAMEPAD, published pad 0 only -- the ABI itself accepts nothing else (framework_abi.h:
    // "pad is fixed at 0 for every call"). Gated on `suppressed`, not `kb`: an ImGui text field
    // steals a keystroke, never a controller button, so the extra uiWantsKeyboard clause folded
    // into `kb` has nothing to do with a gamepad -- `suppressed` alone (window unfocused, or the
    // player released the mouse mid-session) is the whole of when the game does not own this
    // device. Polled unconditionally, same as input_ is read unconditionally above, so
    // Aver.Platform's own hotplug/re-probe throttle (Gamepad.hpp) keeps ticking through a
    // suppressed stretch instead of resetting cold when it ends.
    GamepadState pad{};
    pollGamepads(&pad, 1);
    for (int32_t b = 0; b < AVER_FW_GAMEPAD_BUTTON_COUNT; ++b)
        aver_fw_input_set_gamepad_button(0, b, (!suppressed && pad.buttons[b]) ? 1 : 0);
    for (int32_t a = 0; a < AVER_FW_GAMEPAD_AXIS_COUNT; ++a)
        aver_fw_input_set_gamepad_axis(0, a, !suppressed ? pad.axes[a] : 0.0f);
#endif
}

// While playing, drives the view camera from the pawn's published view node, falling back to the
// pawn's own matrix. Row 3 is the position, row 0 the forward (+X) axis.
void SandboxApp::drivePlayCamera() {
    scene::World& w = scene::World::instance();
    scene::Entity e = scene::kInvalidEntity;

    // Reset every call, unconditionally, ahead of every early return below -- see firstPersonPawn_'s
    // own declaration for why a value left over from a previous call is not merely wrong but
    // dangerous (a reused entity handle picking this frame's edit-mode selection back up).
    firstPersonPawn_ = scene::kInvalidEntity;

    if (aver_fw_play_state() == AVER_FW_PLAY_PLAYING) {
        const int32_t pawn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
        if (pawn == 0) return;
        e = static_cast<scene::Entity>(static_cast<uint32_t>(pawn));
    } else if (dronePlayActive() && droneEntity_ != scene::kInvalidEntity) {
        // THE DRONE IS THE PAWN WHEN THERE IS NO GameMode, so the camera follows it like one.
        // It never was before: gated on aver_fw_controlled_pawn, and the drone fallback never
        // calls aver_fw_begin_play, so this returned 0 immediately -- the drone flew and the
        // camera sat wherever it was left, with no possession and no follow.
        // Treated as the possessed pawn HERE rather than through aver_fw_begin_play, since real
        // possession needs a PlayerController and a pawn CLASS, which the fallback exists for
        // projects declaring neither.
        e = droneEntity_;
    } else {
        return;
    }
    if (!w.valid(e)) return;

    int32_t mode = AVER_FW_VIEW_THIRD_PERSON; float eye = 160.0f, boom = 450.0f;
    aver_fw_view(&mode, &eye, &boom);

    // ONLY first person hides anything. Set from `e` (the pawn itself), not `ve`/the view node
    // below: a body mesh's COMP hangs off the pawn's own entity or an ancestor chain that ends
    // there, never off the camera transform, so owner-hide has to compare against the same entity the hierarchy roots at.
    firstPersonPawn_ = (mode == AVER_FW_VIEW_FIRST_PERSON) ? e : scene::kInvalidEntity;

    // Prefer the view node; fall back to the pawn if it has not published one.
    const int32_t viewId = aver_fw_view_entity();
    const scene::Entity ve = static_cast<scene::Entity>(static_cast<uint32_t>(viewId));
    const bool haveView = viewId != 0 && w.valid(ve);
    const Mat4& vm = haveView ? w.worldMatrix(ve) : w.worldMatrix(e);
    const Mat4& pm = w.worldMatrix(e);

    const Vec3 headPos{vm.m[3][0], vm.m[3][1], vm.m[3][2]};
    const Vec3 headFwd = Vec3{vm.m[0][0], vm.m[0][1], vm.m[0][2]}.getSafeNormal();
    const Vec3 pawnPos{pm.m[3][0], pm.m[3][1], pm.m[3][2]};
    const Vec3 pawnFwd = Vec3{pm.m[0][0], pm.m[0][1], pm.m[0][2]}.getSafeNormal();
    const Vec3 up{0, 0, 1};

    Vec3 look;
    if (mode == AVER_FW_VIEW_FIRST_PERSON) {
        camPos_ = haveView ? headPos : pawnPos + up * eye;
        look    = headFwd;
    } else {
        const Vec3 pivot = haveView ? headPos : pawnPos + up * eye;
        const Vec3 armDir = haveView ? headFwd : pawnFwd;
        camPos_ = pivot - armDir * boom;
        look    = (pivot - camPos_).getSafeNormal();
    }
    // camForward() composes {cosP cosY, cosP sinY, sinP}; invert the look direction to yaw/pitch.
    yaw_   = std::atan2(look.y, look.x);
    pitch_ = std::asin(std::fmax(-1.0f, std::fmin(1.0f, look.z)));
}

#endif

// Aspect comes from the viewport rect (the dockspace's central node), not the whole window.
f32 SandboxApp::viewAspect() const { return vpH_ > 0.5f ? vpW_ / vpH_ : 1.777f; }

bool SandboxApp::inViewport(f32 mx, f32 my) const { return mx >= vpX_ && mx < vpX_+vpW_ && my >= vpY_ && my < vpY_+vpH_; }

// The camera's forward axis, built from yaw and pitch.
Vec3 SandboxApp::camForward() const {
    return Vec3{ std::cos(pitch_)*std::cos(yaw_), std::cos(pitch_)*std::sin(yaw_), std::sin(pitch_) };
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
#if defined(_WIN32)
    if (on == mouseCaptured_) return;
    mouseCaptured_ = on;
    if (on) {
        ShowCursor(FALSE);
        warpToAnchor();
    } else {
        ShowCursor(TRUE);
        ClipCursor(nullptr);
    }
    AVER_INFO("[Sandbox] mouse {} the game{}", on ? "captured by" : "released from",
              on ? " (Shift+F1 to release)" : "");
#else
    mouseCaptured_ = on;
#endif
}

#if defined(_WIN32)
// Parks the cursor at the centre of the window, remembers where that was, and confines it there.
void SandboxApp::warpToAnchor() {
    HWND hwnd = window_ ? static_cast<HWND>(window_->nativeHandle()) : nullptr;
    if (!hwnd) return;
    // A WINDOW THAT IS NOT FOREGROUND HAS NO BUSINESS MOVING THE POINTER: SetCursorPos/ClipCursor
    // below are global and would drag the cursor to this window's centre while the user works
    // elsewhere. Windows ignores ClipCursor from a background window anyway, so only the cursor
    // theft is lost. Anchoring to where the pointer actually IS keeps the delta honest on refocus.
    if (::GetForegroundWindow() != hwnd) {
        POINT q{};
        if (GetCursorPos(&q)) { captureAnchorX_ = q.x; captureAnchorY_ = q.y; }
        return;
    }
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return;
    POINT c{ (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
    ClientToScreen(hwnd, &c);
    captureAnchorX_ = c.x; captureAnchorY_ = c.y;
    SetCursorPos(c.x, c.y);
    RECT screen{};
    POINT tl{ rc.left, rc.top }, br{ rc.right, rc.bottom };
    ClientToScreen(hwnd, &tl); ClientToScreen(hwnd, &br);
    screen.left = tl.x; screen.top = tl.y; screen.right = br.x; screen.bottom = br.y;
    ClipCursor(&screen);
}

// Measures one frame of captured mouse movement, then re-centres for the next.
void SandboxApp::pollCapturedMouse() {
    captureDx_ = captureDy_ = 0.0f;
    if (!mouseCaptured_) return;
    HWND fg = window_ ? static_cast<HWND>(window_->nativeHandle()) : nullptr;
    // A STALE ANCHOR IS A VIEW SNAP, and losing focus is how the anchor goes stale: Windows drops
    // ClipCursor confinement the moment a window stops being foreground, and nothing re-captures on
    // the way back (setMouseCaptured() no-ops if the state hasn't changed), so the next
    // GetCursorPos() would measure against a pre-alt-tab anchor and hand the framework one
    // enormous delta -- the camera whips round exactly once, the frame focus returns.
    // RE-ANCHOR AND REPORT ZERO: one frame of no look input on refocus is imperceptible; a spin
    // is not.
    // ANCHOR TO WHERE THE CURSOR IS, NOT warpToAnchor(): that calls SetCursorPos, and dragging the
    // pointer to this window's centre every frame while the user works elsewhere is a worse bug --
    // it would also steal the cursor during a bounded --frames run with a play session up.
    if (fg && ::GetForegroundWindow() != fg) {
        POINT q{};
        if (GetCursorPos(&q)) { captureAnchorX_ = q.x; captureAnchorY_ = q.y; }
        return;
    }
    POINT p{};
    if (!GetCursorPos(&p)) return;
    captureDx_ = static_cast<f32>(p.x - captureAnchorX_);
    captureDy_ = static_cast<f32>(p.y - captureAnchorY_);
    warpToAnchor();
}

#endif

#if defined(_WIN32)
#else
void SandboxApp::warpToAnchor() {}

void SandboxApp::pollCapturedMouse() { captureDx_ = captureDy_ = 0.0f; }

#endif

// True while a game is playing, in a build with or without the framework.
bool SandboxApp::playSessionActive() const {
#if AVER_MODULE_FRAMEWORK
    return aver_fw_play_state() != AVER_FW_PLAY_EDITOR;
#else
    return false;
#endif
}

} // namespace aver
