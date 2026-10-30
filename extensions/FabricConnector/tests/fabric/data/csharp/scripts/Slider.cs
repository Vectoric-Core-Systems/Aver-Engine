using Godot;

// Tween: a property tween with a curve, a chained callback, and a parallel one.
public partial class Slider : Node3D
{
    [Signal] public delegate void ArrivedEventHandler();

    [Export] public Vector3 Offset = new Vector3(0, 2, 0);
    [Export] public float Duration = 0.5f;

    private Vector3 _start;
    public bool HasArrived;

    public override void _Ready()
    {
        _start = Position;
    }

    public void Slide()
    {
        Tween tween = CreateTween();
        tween.TweenProperty(this, "position", _start + Offset, Duration)
             .SetTrans(Tween.TransitionType.Sine)
             .SetEase(Tween.EaseType.Out);
        tween.TweenCallback(Callable.From(OnArrived));
    }

    public void SlideBoth()
    {
        Tween tween = GetTree().CreateTween().SetParallel(true);
        tween.TweenProperty(this, "position:y", _start.Y + Offset.Y, Duration);
        tween.TweenProperty(this, "scale", Vector3.One * 2.0f, Duration);
        tween.Chain().TweenCallback(Callable.From(OnArrived));
    }

    private void OnArrived()
    {
        HasArrived = true;
        EmitSignal(SignalName.Arrived);
    }
}
