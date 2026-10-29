// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Shared enums and small value types of Godot's core API.

namespace Aver.Fabric;

/// <summary>Godot's Error codes (the numbering is Godot's).</summary>
public enum Error
{
    Ok = 0, Failed = 1, Unavailable = 2, Unconfigured = 3, Unauthorized = 4, ParameterRangeError = 5, OutOfMemory = 6,
    FileNotFound = 7, FileBadDrive = 8, FileBadPath = 9, FileNoPermission = 10, FileAlreadyInUse = 11, FileCantOpen = 12,
    FileCantWrite = 13, FileCantRead = 14, FileUnrecognized = 15, FileCorrupt = 16, FileMissingDependencies = 17, FileEof = 18,
    CantOpen = 19, CantCreate = 20, QueryFailed = 21, AlreadyInUse = 22, Locked = 23, Timeout = 24, CantConnect = 25,
    CantResolve = 26, ConnectionError = 27, CantAcquireResource = 28, CantFork = 29, InvalidData = 30, InvalidParameter = 31,
    AlreadyExists = 32, DoesNotExist = 33, DatabaseCantRead = 34, DatabaseCantWrite = 35, CompilationFailed = 36,
    MethodNotFound = 37, LinkFailed = 38, ScriptFailed = 39, CyclicLink = 40, InvalidDeclaration = 41, DuplicateSymbol = 42,
    ParseError = 43, Busy = 44, Skip = 45, Help = 46, Bug = 47, PrinterOnFire = 48,
}

/// <summary>Horizontal alignment, vertical alignment and the other small enums scripts name often.</summary>
public enum Side { Left = 0, Top = 1, Right = 2, Bottom = 3 }

public enum Corner { TopLeft = 0, TopRight = 1, BottomRight = 2, BottomLeft = 3 }

public enum Orientation { Vertical = 1, Horizontal = 0 }

public enum HorizontalAlignment { Left = 0, Center = 1, Right = 2, Fill = 3 }

public enum VerticalAlignment { Top = 0, Center = 1, Bottom = 2, Fill = 3 }

/// <summary>A resource id, as an opaque 64-bit number (Godot's Rid).</summary>
public readonly struct Rid : IEquatable<Rid>
{
    public ulong Id { get; }
    public Rid(ulong id) => Id = id;
    public bool IsValid => Id != 0;
    public bool Equals(Rid other) => Id == other.Id;
    public override bool Equals(object? obj) => obj is Rid r && Equals(r);
    public override int GetHashCode() => Id.GetHashCode();
    public static bool operator ==(Rid l, Rid r) => l.Id == r.Id;
    public static bool operator !=(Rid l, Rid r) => l.Id != r.Id;
    public override string ToString() => $"RID({Id})";
}

/// <summary>Marks a delegate as a signal: <c>[Signal] public delegate void HitEventHandler(int damage);</c> declares signal "Hit".</summary>
[AttributeUsage(AttributeTargets.Delegate)]
public sealed class SignalAttribute : Attribute { }

/// <summary>Marks a field or property whose value a scene or the inspector sets (Godot's @export).</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public sealed class ExportAttribute : Attribute
{
    public PropertyHint Hint { get; }
    public string HintString { get; }
    public ExportAttribute() { Hint = PropertyHint.None; HintString = ""; }
    public ExportAttribute(PropertyHint hint, string hintString = "") { Hint = hint; HintString = hintString; }
}

[AttributeUsage(AttributeTargets.Class | AttributeTargets.Field | AttributeTargets.Property)]
public sealed class ExportGroupAttribute : Attribute
{
    public string Name { get; }
    public string Prefix { get; }
    public ExportGroupAttribute(string name, string prefix = "") { Name = name; Prefix = prefix; }
}

[AttributeUsage(AttributeTargets.Class | AttributeTargets.Field | AttributeTargets.Property)]
public sealed class ExportSubgroupAttribute : Attribute
{
    public string Name { get; }
    public string Prefix { get; }
    public ExportSubgroupAttribute(string name, string prefix = "") { Name = name; Prefix = prefix; }
}

[AttributeUsage(AttributeTargets.Class | AttributeTargets.Field | AttributeTargets.Property)]
public sealed class ExportCategoryAttribute : Attribute
{
    public string Name { get; }
    public ExportCategoryAttribute(string name) => Name = name;
}

/// <summary>Registers a script class by name for the editor's Create dialog (no runtime effect).</summary>
[AttributeUsage(AttributeTargets.Class)]
public sealed class GlobalClassAttribute : Attribute { }

/// <summary>The icon the Godot editor shows for a global class (accepted, no runtime effect).</summary>
[AttributeUsage(AttributeTargets.Class)]
public sealed class IconAttribute : Attribute
{
    public string Path { get; }
    public IconAttribute(string path) => Path = path;
}

/// <summary>Runs a script in the editor (no runtime effect).</summary>
[AttributeUsage(AttributeTargets.Class)]
public sealed class ToolAttribute : Attribute { }

/// <summary>Marks a method as an RPC (G6 NET2): GodotSharp's attribute. The mode is who may call it, <c>CallLocal</c> also runs it on the caller,
/// <c>TransferMode</c> and <c>TransferChannel</c> choose how the packet travels. The defaults are GodotSharp's: authority, call remote, reliable, channel 0.</summary>
[AttributeUsage(AttributeTargets.Method)]
public sealed class RpcAttribute : Attribute
{
    public RpcAttribute() { }
    public RpcAttribute(MultiplayerApi.RpcMode mode) { Mode = mode; }

    /// <summary>The form an earlier conversion wrote: the mode by name ("AnyPeer", "authority", "any_peer", "disabled").</summary>
    public RpcAttribute(string mode) { Mode = ParseMode(mode); }

    public MultiplayerApi.RpcMode Mode { get; } = MultiplayerApi.RpcMode.Authority;
    public bool CallLocal { get; set; }
    public MultiplayerPeer.TransferModeEnum TransferMode { get; set; } = MultiplayerPeer.TransferModeEnum.Reliable;
    public int TransferChannel { get; set; }

    internal static MultiplayerApi.RpcMode ParseMode(string mode) => mode.Replace("_", "").ToLowerInvariant() switch
    {
        "anypeer" => MultiplayerApi.RpcMode.AnyPeer,
        "disabled" => MultiplayerApi.RpcMode.Disabled,
        _ => MultiplayerApi.RpcMode.Authority,
    };
}

/// <summary>How an exported property is edited (Godot's PropertyHint, the values scripts name).</summary>
public enum PropertyHint
{
    None = 0, Range = 1, Enum = 2, EnumSuggestion = 3, ExpRange = 4, Easing = 5, Link = 6, Flags = 7, Layers2DRender = 8,
    Layers2DPhysics = 9, Layers2DNavigation = 10, Layers3DRender = 11, Layers3DPhysics = 12, Layers3DNavigation = 13,
    File = 14, Dir = 15, GlobalFile = 16, GlobalDir = 17, ResourceType = 18, MultilineText = 19, Expression = 20,
    PlaceholderText = 21, ColorNoAlpha = 22, ObjectId = 23, TypeString = 24, NodePathToEditedNode = 25, ObjectTooBig = 26,
    NodePathValidTypes = 27, SaveFile = 28, GlobalSaveFile = 29, IntIsObjectid = 30, IntIsPointer = 31, ArrayType = 32,
    Locale = 33, LocalizableString = 34, NodeType = 35, HideQuaternionEdit = 36, Password = 37, ToolButton = 39,
}
