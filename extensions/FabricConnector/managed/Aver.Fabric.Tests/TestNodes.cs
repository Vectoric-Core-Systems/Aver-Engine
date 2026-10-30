// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The node classes the tests derive: what a converted script looks like (GodotSharp style), with recording virtuals.

using Aver.Fabric;

/// <summary>Records every lifecycle virtual into one shared list, so a test reads the order across a whole tree.</summary>
class Rec : Node
{
    public static readonly List<string> Trail = new();

    public Rec() { }
    public Rec(string name) { Name = name; }

    public override void _EnterTree() => Trail.Add("enter " + Name);
    public override void _Ready() => Trail.Add("ready " + Name);
    public override void _ExitTree() => Trail.Add("exit " + Name);
    public override void _Notification(int what)
    {
        if (what == NotificationParented) Trail.Add("parented " + Name);
        else if (what == NotificationUnparented) Trail.Add("unparented " + Name);
    }
}

class Rec3D : Node3D
{
    public Rec3D() { }
    public Rec3D(string name) { Name = name; }
    public override void _EnterTree() => Rec.Trail.Add("enter " + Name);
    public override void _Ready() => Rec.Trail.Add("ready " + Name);
}

/// <summary>A script with _Process and _PhysicsProcess and the exported values a level sets.</summary>
class Hero : Node3D
{
    public readonly List<double> Frames = new();
    public readonly List<double> Steps = new();
    public int Entered;
    public float SpeedAtEnter = -1f;

    [Export] public float Speed = 5f;
    [Export] public Vector3 StartPos;
    [Export] public string Title = "";
    [Export] public int Lives { get; set; } = 3;
    [Export] public Color Tint = new(1, 1, 1, 1);
    [Export] public Aver.Fabric.Collections.Array Items = new();

    public override void _EnterTree() { Entered++; SpeedAtEnter = Speed; }
    public override void _Process(double delta) => Frames.Add(delta);
    public override void _PhysicsProcess(double delta) => Steps.Add(delta);
}

/// <summary>Overrides nothing: it must not tick at all.</summary>
class Idle : Node { }

class InputNode : Node
{
    public readonly List<string> Seen = new();
    public override void _Input(InputEvent @event) => Seen.Add("input " + @event.AsText());
    public override void _UnhandledInput(InputEvent @event) => Seen.Add("unhandled " + @event.AsText());
    public override void _UnhandledKeyInput(InputEvent @event) => Seen.Add("unhandled key " + @event.AsText());
}

/// <summary>A GodotSharp class with its own signals, as the converter writes them.</summary>
class Emitter : Node
{
    [Signal] public delegate void HitEventHandler(int damage);
    [Signal] public delegate void DiedEventHandler();
    [Signal] public delegate void NamedEventHandler(string who, float amount);

    public new class SignalName : Node.SignalName
    {
        public static readonly StringName Hit = "Hit";
        public static readonly StringName Died = "Died";
        public static readonly StringName Named = "Named";
    }

    public readonly List<string> Got = new();
    public int Last;

    public void OnHit(int damage) { Last = damage; Got.Add("hit " + damage); }
    public void OnDied() => Got.Add("died");
    public void OnNamed(string who, float amount) => Got.Add($"named {who} {amount}");
    public void Throws(int damage) => throw new InvalidOperationException("handler failed");
    public void Second(int damage) => Got.Add("second " + damage);
}

/// <summary>The root of the arena fixture: exported values, and the method its Gate child's scene connection names.</summary>
class ArenaNode : Hero
{
    public readonly List<string> Calls = new();
    public void OnGateHit(int damage) => Calls.Add("gate hit " + damage);
}

/// <summary>Waits on a scene tree timer with `await ToSignal`, the way a converted script does.</summary>
class Awaiter : Node
{
    public async void Wait(double seconds, List<string> log, string tag, bool physics = false, bool always = true)
    {
        await ToSignal(GetTree().CreateTimer(seconds, always, physics), SceneTreeTimer.SignalName.Timeout);
        log.Add(tag + "@" + GetTree().GetFrame());
    }
}

/// <summary>A group member that records calls made through call_group.</summary>
class GroupMember : Node
{
    public static readonly List<string> Trail = new();
    public int Value;
    public void Bump(int by = 1) { Value += by; Trail.Add(Name + " bump " + by); }
    public void Ping() => Trail.Add(Name + " ping");
}

/// <summary>Reads input the way a converted script does and can stop an event.</summary>
class InputProbe : Node
{
    /// <summary>Every probe's lines in the order the events reached them.</summary>
    public static readonly List<string> All = new();

    public readonly List<string> Log = new();
    public bool StopInInput, StopInUnhandled;
    public string Tag = "";

    public override void _Input(InputEvent @event)
    {
        string line = Tag + " input " + @event.AsText();
        Log.Add(line); All.Add(line);
        if (StopInInput) GetViewport()!.SetInputAsHandled();
    }

    public override void _UnhandledInput(InputEvent @event)
    {
        string line = Tag + " unhandled " + @event.AsText();
        Log.Add(line); All.Add(line);
        if (StopInUnhandled) GetViewport()!.SetInputAsHandled();
    }
}
