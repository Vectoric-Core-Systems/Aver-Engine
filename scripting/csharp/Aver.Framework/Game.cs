// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// Static access to the running play session: state, singletons, lookup and world queries.

using Aver.Scene;

namespace Aver.Framework;

/// <summary>The running play session, reached statically from any script. Read-only: the host starts and stops it.</summary>
public static class Game
{
    /// <summary>The world's play state — Editor, Playing or Paused.</summary>
    public static PlayState State => (PlayState)Fw.aver_fw_play_state();

    /// <summary>True while a session is actively ticking.</summary>
    public static bool IsPlaying => State == PlayState.Playing;

    /// <summary>True while a session exists but is frozen.</summary>
    public static bool IsPaused => State == PlayState.Paused;

    /// <summary>True while a session exists at all — Playing or Paused.</summary>
    public static bool HasSession => State != PlayState.Editor;

    /// <summary>Requests that the host exit. A REQUEST, NOT AN IMMEDIATE TERMINATION -- the host
    /// honours it at the next frame boundary, the same way SandboxApp::requestExitChecked already
    /// defers to Engine::requestExit() (Runtime/include/aver/runtime/Engine.hpp) rather than tearing
    /// the process down inline from wherever the request happened. THE LINE AFTER THIS CALL STILL
    /// RUNS, and so does the rest of this frame's OnTick across every other actor -- write a caller
    /// as if this returned void and changed nothing, because until the host's next frame boundary
    /// that is exactly what it did.
    ///
    /// Before this existed there was NO WAY for a shipped game to exit itself: grepping
    /// Quit/RequestExit/request_exit across every scripting/csharp/Aver.* file and every *_abi.h
    /// returned nothing, so a packaged build's only exits were Alt+F4 and the OS killing the window.
    /// A pause menu's "Quit to Desktop" button had nothing to call.
    ///
    /// A REQUEST, NOT A TERMINATION: the host polls this once a frame and shuts down through its own
    /// ordinary path, so the statement after your call to this DOES run, and so does the rest of the
    /// frame. Nothing here calls exit() -- unwinding out of a managed call mid-frame with a device,
    /// an audio stream and a physics world all live is how a clean quit becomes a crash report.</summary>
    public static void Quit() => Fw.aver_fw_set_quit_requested(1);

    /// <summary>Withdraws a quit request made by <see cref="Quit"/>, for a "really quit?" prompt whose
    /// answer was no. Also what an editor host calls between Play sessions: the native flag is
    /// process-wide and <c>aver_fw_begin_play</c> deliberately does not clear it, so a second Play in
    /// the same editor session would otherwise inherit the first one's quit.</summary>
    public static void CancelQuit() => Fw.aver_fw_set_quit_requested(0);

    /// <summary>Whether a quit has been requested and not yet acted on.</summary>
    public static bool QuitRequested => Fw.aver_fw_quit_requested() != 0;

    /// <summary>Frees the OS mouse cursor for as long as the returned scope is alive, then hands
    /// capture back to the host's ordinary policy. Runtime/src/GameApp.cpp hides and confines the
    /// cursor for as long as a session is PLAYING, with no way to get it back -- so a pause menu
    /// could not click its own buttons.
    ///
    /// <code>
    /// using (Game.FreeCursor())
    /// {
    ///     // pause menu runs here; the pointer is visible and unconfined
    /// }   // capture returns to whatever the host would otherwise do
    /// </code>
    ///
    /// A SCOPE RATHER THAN A SETTER, because the native side counts requests instead of holding a
    /// bool, and a count is only correct if every request is paired with exactly one release. A
    /// dialogue box opened over a pause menu is the ordinary case where a bool breaks: the box
    /// closes, sets "captured", and steals the pointer from the menu still underneath it. Disposing
    /// is what pairs them, and `using` is what makes forgetting hard.
    ///
    /// THIS ONLY EVER RELEASES THE CURSOR, never takes it. The host keeps its existing rules about
    /// when capture is refused outright -- an unfocused window, a bounded --frames capture, an
    /// explicit --no-mouse-capture -- and this cannot override those. A test harness that grabbed
    /// the pointer of an unattended machine would be worse than any menu is good.</summary>
    public static CursorScope FreeCursor() => new CursorScope();

    /// <summary>Whether anything currently wants the cursor free -- see <see cref="FreeCursor"/>.
    /// Read this rather than tracking your own bool; with several scopes alive at once, yours would
    /// only ever describe your own.</summary>
    public static bool CursorFreeRequested => Fw.aver_fw_cursor_requested() != 0;

    /// <summary>The lifetime of one "let the player use the mouse" request. See
    /// <see cref="Game.FreeCursor"/>; construct it through that rather than directly.
    ///
    /// A STRUCT, so the common `using (Game.FreeCursor())` allocates nothing on a path a pause menu
    /// may open every frame it is visible.</summary>
    public readonly struct CursorScope : System.IDisposable
    {
        /// <summary>Takes the request. PUBLIC because C# requires a parameterless struct constructor
        /// to be (CS8958), not because direct construction is the intended route -- prefer
        /// <see cref="Game.FreeCursor"/>, which reads as what it does at the call site. Constructing
        /// one directly behaves identically, so the compiler's rule costs nothing here.</summary>
        public CursorScope() => Fw.aver_fw_cursor_request();
        /// <summary>Releases this request; capture returns to the host's ordinary policy once no
        /// request is left outstanding.</summary>
        public void Dispose() => Fw.aver_fw_cursor_release();
    }

    /// <summary>The current GameMode's entity, or <see cref="Entity.None"/> in editor.</summary>
    public static Entity Mode => new(Fw.aver_fw_game_mode());

    /// <summary>The GameInstance entity, or <see cref="Entity.None"/> in editor.</summary>
    public static Entity Instance => new(Fw.aver_fw_game_instance());

    /// <summary>Player 0's controller.</summary>
    public static Entity LocalPlayerController => GetPlayerController(0);

    /// <summary>The controller for a 0-based player index, or <see cref="Entity.None"/>.</summary>
    public static Entity GetPlayerController(int playerIndex = 0) => new(Fw.aver_fw_player_controller(playerIndex));

    /// <summary>The pawn a given player currently possesses, or <see cref="Entity.None"/>.</summary>
    public static Entity GetPlayerPawn(int playerIndex = 0) =>
        new(Fw.aver_fw_controlled_pawn(Fw.aver_fw_player_controller(playerIndex)));

    /// <summary>The current GameMode as <typeparamref name="T"/>, or null.</summary>
    public static T? ModeAs<T>() where T : AverGameMode => Actors.Get<T>(Mode);

    /// <summary>The GameInstance as <typeparamref name="T"/>, or null.</summary>
    public static T? InstanceAs<T>() where T : AverGameInstance => Actors.Get<T>(Instance);

    /// <summary>A player's controller as <typeparamref name="T"/>, or null.</summary>
    public static T? PlayerControllerAs<T>(int playerIndex = 0) where T : AverPlayerController =>
        Actors.Get<T>(GetPlayerController(playerIndex));

    /// <summary>A player's possessed pawn as <typeparamref name="T"/>, or null.</summary>
    public static T? PlayerPawnAs<T>(int playerIndex = 0) where T : AverPawn => Actors.Get<T>(GetPlayerPawn(playerIndex));

    /// <summary>The live entity named <paramref name="name"/> (first match), or <see cref="Entity.None"/>. A linear scan.</summary>
    public static Entity Find(string name) => new(SceneNative.aver_scene_find(name));

    /// <summary>Writes the whole world to <paramref name="path"/> and returns whether it landed.
    ///
    /// A SNAPSHOT, not a diff: every entity, every component, every field. That is what makes a save
    /// survive you editing the level it was taken in, and it is why the file is not small.
    ///
    /// The write is ATOMIC -- a temporary file, then a rename -- so a crash or a full disk part-way
    /// through leaves the PREVIOUS save intact rather than destroying the thing the player asked the
    /// game to keep.
    ///
    /// False when the host installed no save provider (a build with no save support says so rather
    /// than pretending), or when the write itself failed. The reason is logged.</summary>
    public static bool SaveGame(string path) =>
        !string.IsNullOrEmpty(path) && Fw.aver_fw_save_write(path) != 0;

    /// <summary>Replaces the whole world from <paramref name="path"/>.
    ///
    /// EVERYTHING CURRENTLY IN THE WORLD IS DESTROYED FIRST, actors through the path that runs
    /// OnEndPlay. Any Entity handle you were holding is dead afterwards -- re-find what you need by
    /// name. A restored actor begins play AFTER its saved fields are back, so its OnBeginPlay sees
    /// the world the player left rather than the class defaults.
    ///
    /// False when there is no provider, the file is missing or malformed, or the restore failed.
    /// A failed restore leaves the world EMPTY rather than half-populated: a world that looks
    /// playable and is not is worse than one that plainly is not.</summary>
    public static bool LoadGame(string path) =>
        !string.IsNullOrEmpty(path) && Fw.aver_fw_save_load(path) != 0;

    /// <summary>The actor named <paramref name="name"/> as <typeparamref name="T"/>, or null.</summary>
    public static T? FindActor<T>(string name) where T : AverActor => Actors.Get<T>(Find(name));

    /// <summary>Every live actor in the world; plain scene entities excluded. Do not spawn or destroy while enumerating.</summary>
    public static IEnumerable<Entity> AllActors()
    {
        int n = SceneNative.aver_scene_count();
        for (int i = 0; i < n; i++)
        {
            Entity e = new(SceneNative.aver_scene_at(i));
            if (e.IsActor) yield return e;
        }
    }

    /// <summary>Every live actor whose managed instance is a <typeparamref name="T"/>.</summary>
    public static IEnumerable<T> ActorsOf<T>() where T : AverActor
    {
        foreach (Entity e in AllActors())
            if (Actors.Get<T>(e) is { } actor)
                yield return actor;
    }

    /// <summary>Requests a simulated fluid volume centred at <paramref name="centreCm"/> (world-space
    /// centimetres) with the given half-extent, using the solver's OWN RAW knobs -- see
    /// FluidVolume.hpp's own FluidVolumeDesc comment for what each means and its unit. Defaults
    /// (1.0e-4, 0.1, 5, -1) are the same numbers FluidVolumeDesc itself defaults to, so calling this
    /// with none of the four gets the identical solver an unauthored WATER record already gets.
    /// `pressure` below zero asks the solver to derive it from the volume's own size
    /// (fluidPressureFor) rather than use the value passed -- the same sentinel a WATER record's own
    /// unauthored `pressure=` already means.
    ///
    /// NO DENSITY OR VISCOSITY HERE -- see <see cref="SpawnFluidVolumeMaterial"/> for the layer that
    /// takes real fluid values instead of solver knobs. This overload is the raw escape hatch, kept
    /// reachable rather than folded into the other one, exactly as the design brief's own precedence
    /// rule requires: a raw `damping` here and a material on the other overload are two different
    /// requests, never silently merged.
    ///
    /// This is the SAME relay a graph's `COMP id Fluid ...` line spawns through (Aver.Graph's
    /// GraphComponentTree.ApplyKind, "fluid" case) -- calling it directly from a script is the
    /// other authoring path the relay exists for, not a second mechanism.
    ///
    /// QUEUED, NOT SYNCHRONOUS: the bool says a provider accepted the request, not that a volume
    /// now simulates -- see framework_abi.h's own aver_fw_fluid_spawn comment for why it cannot
    /// answer synchronously. Whether it actually spawned is in the host's own log. False when the
    /// host installed no fluid-spawn provider -- a build with no simulated-fluids module linked
    /// says so rather than pretending.</summary>
    public static bool SpawnFluidVolume(Vec3 centreCm, Vec3 halfExtentCm, string name = "unnamed",
                                         float compliance = 1.0e-4f, float damping = 0.1f,
                                         int iterations = 5, float pressure = -1f) =>
        Fw.aver_fw_fluid_spawn(centreCm.X, centreCm.Y, centreCm.Z,
                                halfExtentCm.X, halfExtentCm.Y, halfExtentCm.Z,
                                compliance, damping, iterations, pressure, name) != 0;

    /// <summary>Same request as <see cref="SpawnFluidVolume"/>, plus the MATERIAL layer: real
    /// density (kg/m^3) and viscosity (Pa*s) instead of typing compliance/damping directly -- see
    /// fluids::FluidMaterial's own comment (FluidVolume.hpp) for the honest split between the two
    /// (density real, viscosity a calibrated fit onto `damping`, some things refused outright).
    ///
    /// `materialPreset` ("water"/"lightoil"/"honey"/"lava", case-insensitive) WINS over
    /// <paramref name="densityKgM3"/>/<paramref name="viscosityPaS"/> when non-empty; otherwise
    /// either of those alone still builds a material, the other field taking FluidMaterial's own
    /// struct default (water's own numbers). Leave all three at their defaults (empty preset,
    /// density/viscosity below zero) to spawn with no material at all -- identical to calling
    /// <see cref="SpawnFluidVolume"/>.
    ///
    /// `damping` HERE CAN STILL CONFLICT WITH A MATERIAL: this call forwards both exactly as given
    /// and does not itself decide which wins. Giving a non-default `damping` alongside a preset or
    /// density/viscosity REFUSES the whole spawn -- caught once, downstream, at
    /// fluids::FluidScene::spawn (fluids::fluidResolveMaterial), the one place both this call and a
    /// level's own WATER record converge -- rather than silently picking one here.</summary>
    public static bool SpawnFluidVolumeMaterial(Vec3 centreCm, Vec3 halfExtentCm, string name = "unnamed",
                                                 float compliance = 1.0e-4f, float damping = 0.1f,
                                                 int iterations = 5, float pressure = -1f,
                                                 float densityKgM3 = -1f, float viscosityPaS = -1f,
                                                 string materialPreset = "") =>
        Fw.aver_fw_fluid_spawn_material(centreCm.X, centreCm.Y, centreCm.Z,
                                         halfExtentCm.X, halfExtentCm.Y, halfExtentCm.Z,
                                         compliance, damping, iterations, pressure,
                                         densityKgM3, viscosityPaS, materialPreset, name) != 0;

    /// <summary>Every live entity carrying all of the tag bits in <paramref name="mask"/>.</summary>
    public static IEnumerable<Entity> WithTag(uint mask)
    {
        int n = SceneNative.aver_scene_count();
        for (int i = 0; i < n; i++)
        {
            Entity e = new(SceneNative.aver_scene_at(i));
            if (e.IsValid && e.HasTag(mask)) yield return e;
        }
    }
}
