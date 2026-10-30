using Godot;
using System;
using System.Threading.Tasks;

// await on a SceneTreeTimer, a Timer node and a C# Task helper.
public partial class Countdown : Node
{
    [Signal] public delegate void FinishedEventHandler(double elapsed);

    [Export] public double Seconds = 1.0;
    public bool Done;
    public int Ticks;

    private Timer _ticker;

    public override async void _Ready()
    {
        _ticker = new Timer();
        _ticker.WaitTime = 0.25;
        _ticker.OneShot = false;
        _ticker.Timeout += () => Ticks++;
        AddChild(_ticker);
        _ticker.Start();

        await ToSignal(GetTree().CreateTimer(Seconds), SceneTreeTimer.SignalName.Timeout);
        Done = true;
        _ticker.Stop();
        EmitSignal(SignalName.Finished, Seconds);
    }

    public async Task WaitFrames(int n)
    {
        for (int i = 0; i < n; i++)
        {
            await ToSignal(GetTree(), SceneTree.SignalName.ProcessFrame);
        }
    }
}
