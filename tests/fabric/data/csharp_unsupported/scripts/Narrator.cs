using Godot;

// Uses Godot classes and members Aver.Fabric does not have yet: each must come out as a precise // FABRIC: line and a report entry.
public partial class Narrator : CharacterBody3D
{
    [Export] public XROrigin3D Voice;
    [Export] public int Lines = 3;

    private XRCamera3D _agent;

    public override void _Ready()
    {
        _agent = GetNode<XRCamera3D>("Agent");
        Voice.Play();
    }

    public override void _PhysicsProcess(double delta)
    {
        MoveAndSlide();
        ApplyFloorSnap();
        Lines++;
    }
}
