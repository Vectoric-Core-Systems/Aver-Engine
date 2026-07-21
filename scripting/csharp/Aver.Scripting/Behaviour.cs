namespace Aver.Scripting;

/// <summary>
/// Base class for a gameplay script. Derive from it, override what you need, and the engine's
/// scripting host finds it by reflection when your assembly is loaded.
/// </summary>
/// <remarks>
/// <para>
/// <b>Why a base class rather than an attribute.</b> Both were on the table. The base class wins
/// on the one thing that matters here: the compiler checks the hooks. An attribute-driven design
/// (<c>[AverScript]</c> on a class, hooks found by name) lets a misspelt <c>OnUpate</c> compile
/// cleanly and then simply never run — a failure with no error message anywhere, which is the
/// worst kind for a scripting layer aimed at people who are not engine developers. Discovery also
/// collapses to a single <c>IsAssignableFrom</c> test instead of an attribute scan plus a method
/// search plus a signature check.
/// </para>
/// <para>
/// The cost is C#'s single inheritance. That is acceptable: a behaviour is a leaf type by nature,
/// and shared logic belongs in a component or a plain class the behaviour holds, not in a base
/// it inherits from.
/// </para>
/// <para>
/// A behaviour needs a public parameterless constructor — the host constructs it, so there is
/// nothing to pass. All three hooks are called on the engine's main thread, inside the frame loop.
/// </para>
/// <para>
/// <b>Exceptions never cross back into the engine.</b> If a hook throws, the host logs it and
/// disables that behaviour for the rest of the session. The process is not torn down and the other
/// behaviours keep running.
/// </para>
/// </remarks>
public abstract class AverBehaviour
{
    /// <summary>Called once, immediately after the behaviour is constructed.</summary>
    public virtual void OnStart() { }

    /// <summary>Called once per frame. <paramref name="dt"/> is seconds since the last frame.</summary>
    public virtual void OnUpdate(float dt) { }

    /// <summary>Called once as the host shuts down, or when the assembly is about to be unloaded.</summary>
    public virtual void OnShutdown() { }
}
