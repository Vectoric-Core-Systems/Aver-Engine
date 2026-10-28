using Godot;
using Godot.Collections;

// A node with exported fields and its own signals, connected and emitted in the usual ways.
public partial class Door : Node3D
{
    [Signal] public delegate void OpenedEventHandler(string by);
    [Signal] public delegate void DamagedEventHandler(int amount, Node3D source);
    [Signal] public delegate void BrokenEventHandler();

    [ExportCategory("Door")]
    [ExportGroup("Stats")]
    [Export] public int Health = 100;
    [Export] public string Label = "Door";
    [Export(PropertyHint.Enum, "Wood,Iron,Glass")] public int Material = 0;

    [ExportSubgroup("Lists")]
    [Export] public Godot.Collections.Array<string> Tags = new();
    [Export] public Dictionary Notes = new();
    [Export] public PackedScene Debris;
    [Export] public NodePath TargetPath;
    [Export] public Color Tint = new Color(1, 0.5f, 0.25f);

    public bool IsOpen { get; private set; }
    public int OpenedCount;
    public string LastOpenedBy = "";

    public override void _Ready()
    {
        // The C# event the source generator would have made, and a Connect by name.
        Opened += OnOpened;
        Connect(SignalName.Damaged, Callable.From<int, Node3D>(OnDamaged));
        Broken += () => GD.Print("broken");
    }

    public void Open(string by)
    {
        if (IsOpen) return;
        IsOpen = true;
        EmitSignal(SignalName.Opened, by);
    }

    public void Hit(int amount)
    {
        Health -= amount;
        EmitSignalDamaged(amount, null);
        if (Health <= 0)
        {
            EmitSignal(SignalName.Broken);
        }
    }

    private void OnOpened(string by)
    {
        OpenedCount++;
        LastOpenedBy = by;
    }

    private void OnDamaged(int amount, Node3D source)
    {
        GD.Print($"{Label} took {amount}");
    }
}
