namespace Aver.UI;

/// <summary>
/// Marks a class the engine should discover as a HUD: something that draws through <see cref="Hud"/>
/// and can be shown on its own, without a game running.
/// </summary>
/// <remarks>
/// <para>
/// WHY AN ATTRIBUTE IS NEEDED AT ALL. A HUD is not an actor. It has no transform, no components and
/// no place in the world, so none of the discovery the gameplay layer uses applies to it — there is
/// no base type to test with <c>IsAssignableFrom</c> and no class registry row to look it up in. Left
/// as a plain static class, as SkyForge's <c>PlayerHud</c> is, a HUD is reachable only from the one
/// call site that happens to know its name. Nothing else can find it, and an editor certainly cannot.
/// </para>
/// <para>
/// WHAT IT PROMISES, and it is a short list on purpose: a public parameterless constructor, and a
/// <c>public void Draw(float dt)</c>. The bridge constructs one instance per marked class at load and
/// calls <c>Draw</c> when asked. Nothing else about the type is assumed.
/// </para>
/// <para>
/// THE HOOK IS NOT NAMED BY THE ATTRIBUTE, which is the same rule <c>Aver.Framework/Attributes.cs</c>
/// settles for gameplay: an attribute carries DATA, never the name of a method. Here the data is the
/// display name. <c>Draw</c> is found by signature and a type that lacks it is reported at load with
/// its own name in the message, rather than silently never drawing.
/// </para>
/// <para>
/// WHY <c>Draw(float dt)</c> AND NOTHING MORE. A HUD that took its game's state as parameters — as
/// <c>PlayerHud.Draw(dt, shots, hits, recoil, cooldown)</c> does — cannot be called by anything that
/// does not already know that game, which is precisely the editor's position. State belongs on the
/// instance, written by gameplay and read by <c>Draw</c>; then the same method serves the game, which
/// has set the fields, and the editor, which has not and gets whatever the fields default to.
/// </para>
/// </remarks>
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class AverHudAttribute : Attribute
{
    /// <param name="name">
    /// What to call it in the editor. Free text — it names a thing a person picks from a list, not a
    /// registry key anything resolves by, so unlike <c>[AverClass]</c> it carries no lookup duty.
    /// </param>
    public AverHudAttribute(string name) => Name = name;

    /// <summary>The display name.</summary>
    public string Name { get; }

    /// <summary>
    /// Draw this one when the editor has no better reason to choose. Ties break on declaration order;
    /// a project with none marked simply has no default, which is not an error.
    /// </summary>
    public bool Default { get; init; }
}
