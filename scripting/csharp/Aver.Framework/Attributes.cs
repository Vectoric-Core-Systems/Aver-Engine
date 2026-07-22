namespace Aver.Framework;

// The attributes carry DATA only — a name, a parent, a range, a "the editor owns this" marker. They
// deliberately never name a hook. A hook is a virtual override, so the COMPILER checks it: a misspelt
// OnTick is a build error, not a method that silently never runs. That is the settled reasoning behind
// AverBehaviour (Aver.Scripting/Behaviour.cs), and it applies unchanged here. An attribute that named a
// hook would throw that guarantee away.

/// <summary>
/// Registers the type it decorates as a gameplay class, and names its parent class. A type deriving a
/// framework base type is discovered by <c>IsAssignableFrom</c>; this attribute supplies the two facts
/// discovery cannot read off the C# type — the registry NAME (which need not match the C# identifier,
/// e.g. <c>BP_Spinner</c>) and the PARENT class, which is a string because the parent may be a native
/// class (<c>StaticMeshActor</c>) or another script class not yet loaded.
/// </summary>
/// <remarks>
/// <c>Inherited = false</c>: a subclass is its own class with its own row, never silently inheriting the
/// base's registered name. The parent link is resolved by name at seal time, so two script classes never
/// take a compile-time reference to one another.
/// </remarks>
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class AverClassAttribute : Attribute
{
    public AverClassAttribute(string name) => Name = name;

    /// <summary>The registry name. This is what <c>[AverGameMode]</c> links and what a level file stores.</summary>
    public string Name { get; }

    /// <summary>The parent class name, resolved at seal. Defaults to the root gameplay actor.</summary>
    public string Parent { get; init; } = "Actor";
}

/// <summary>
/// Registers a GameMode class and names, BY STRING, the classes it hands out: the pawn every joining
/// player is given and the controller that possesses it. Strings, not <c>typeof</c>, so a GameMode in
/// one assembly can name a pawn in another without a compile-time reference — the same late-bound rule
/// the whole class graph uses. Resolved at seal via <c>aver_fw_class_set_default_pawn</c> /
/// <c>aver_fw_class_set_player_controller</c>.
/// </summary>
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class AverGameModeAttribute : Attribute
{
    public AverGameModeAttribute(string name) => Name = name;

    public string Name { get; }
    public string DefaultPawnClass { get; init; } = "Pawn";
    public string PlayerControllerClass { get; init; } = "PlayerController";
}

/// <summary>
/// Marks a field as a class-default-with-per-instance-override. The value lives in NATIVE storage, which
/// is why it appears in Details, serialises into the world, survives Play/Stop, and survives a hot
/// reload (the reload parks it and pushes it back). Read it as "this belongs to the engine", not as
/// "show this in the inspector".
/// </summary>
/// <remarks>
/// The contrast is the whole point: a plain (non-<c>[Editable]</c>) field is ordinary managed state on
/// the instance, so it restarts from its initialiser after a reload. <see cref="Min"/>/<see cref="Max"/>
/// clamp a numeric field in the inspector and on deserialisation; the defaults are the open interval, so
/// an un-ranged field is simply unclamped.
/// </remarks>
[AttributeUsage(AttributeTargets.Field)]
public sealed class EditableAttribute : Attribute
{
    public float Min { get; init; } = float.NegativeInfinity;
    public float Max { get; init; } = float.PositiveInfinity;
}

/// <summary>
/// Marks a property in the editor-owned generated region as one model slot inside an actor. Its presence
/// tells the inspector "this is a placed model the viewport gizmo drives" and warns a human off
/// hand-assigning it — the <c>set</c> is <c>private</c> and only the generated <c>BuildModels</c> writes
/// it. The model's stable identity is NOT on this attribute; it is the <c>ObjectId</c> literal in the
/// matching <see cref="ActorBuilder.Place"/> call, the single source the save-time rewriter keys on.
/// </summary>
/// <remarks>
/// <b>Why a marker at all, if the id lives in the Place call?</b> Discovery of model slots for the
/// Details panel and the outliner is by attribute, not by parsing method bodies — the same
/// reflection-over-text-parsing choice made everywhere else in this surface. Target is
/// <see cref="AttributeTargets.Property"/> because a slot is exposed as a typed
/// <see cref="ModelHandle"/> property hand code can read; it carries no fields, so a future field
/// addition is a non-breaking change.
/// </remarks>
[AttributeUsage(AttributeTargets.Property)]
public sealed class ModelAttribute : Attribute
{
}
