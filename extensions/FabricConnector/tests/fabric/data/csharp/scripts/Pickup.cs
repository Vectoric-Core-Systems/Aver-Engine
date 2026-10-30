#if GODOT
using Godot;
#endif
using System;

// Attributes, qualified Godot names, an #if GODOT block and an RPC with options.
[Tool]
[GlobalClass, Icon("res://icons/pickup.svg")]
public partial class Pickup : Area3D
{
    [Export] public int Value = 1;
    public Godot.Vector3 Spin = new Godot.Vector3(0, 1, 0);
    public global::Godot.Collections.Array<Godot.Node3D> Seen = new();
    public int Collected;

    public override void _Ready()
    {
        BodyEntered += OnBodyEntered;
#if TOOLS
        GD.Print("editor only: never compiled for a game");
#endif
    }

    public override void _Process(double delta)
    {
        RotateY(Spin.Y * (float)delta);
    }

    [Rpc(MultiplayerApi.RpcMode.AnyPeer, CallLocal = true)]
    public void Collect(Node3D by)
    {
        Collected += Value;
        Seen.Add(by);
        QueueFree();
    }

    private void OnBodyEntered(Node3D body)
    {
        Collect(body);
    }
}
