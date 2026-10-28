using Godot;

public partial class Door : Node3D
{
    [Signal] public delegate void OpenedEventHandler(string by);

    [Export] public int Health = 100;
    [Export] public string Label = "Door";

    public bool IsOpen { get; private set; }

    public void Open(string by)
    {
        if (IsOpen) return;
        IsOpen = true;
        EmitSignal(SignalName.Opened, by);
    }
}
