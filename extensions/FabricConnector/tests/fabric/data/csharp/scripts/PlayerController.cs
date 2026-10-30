using Godot;
using System;

namespace CSharpGame;

// The template Godot gives a first-person CharacterBody3D.
public partial class PlayerController : CharacterBody3D
{
    [Export] public float Speed { get; set; } = 5.0f;
    [Export(PropertyHint.Range, "0,20,0.5")] public float JumpVelocity = 4.5f;
    [Export] public Node3D CameraPivot;
    [Export(PropertyHint.Layers3DPhysics)] public uint InteractMask = 1;

    private float _gravity = 9.8f;

    public override void _Ready()
    {
        GD.Print("player ready at ", GlobalPosition);
    }

    public override void _PhysicsProcess(double delta)
    {
        Vector3 velocity = Velocity;

        if (!IsOnFloor())
        {
            velocity.Y -= _gravity * (float)delta;
        }

        if (Input.IsActionJustPressed("jump") && IsOnFloor())
        {
            velocity.Y = JumpVelocity;
        }

        Vector2 inputDir = Input.GetVector("move_left", "move_right", "move_forward", "move_back");
        Vector3 direction = (Transform.Basis * new Vector3(inputDir.X, 0, inputDir.Y)).Normalized();
        if (direction != Vector3.Zero)
        {
            velocity.X = direction.X * Speed;
            velocity.Z = direction.Z * Speed;
        }
        else
        {
            velocity.X = Mathf.MoveToward(Velocity.X, 0, Speed);
            velocity.Z = Mathf.MoveToward(Velocity.Z, 0, Speed);
        }

        Velocity = velocity;
        MoveAndSlide();

        if (CameraPivot != null)
        {
            CameraPivot.GlobalPosition = GlobalPosition + new Vector3(0, 1.6f, 0);
        }
    }

    public override void _UnhandledInput(InputEvent @event)
    {
        if (@event is InputEventKey key && key.Pressed && key.Keycode == Key.Escape)
        {
            GetTree().Quit();
        }
    }
}
