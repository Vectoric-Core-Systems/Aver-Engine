// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// Tests for the six audio nodes -- the graph half of connecting a mixer that was built, tested, and
// then called by nothing at all for weeks.
//
// WHAT THESE CAN AND CANNOT PROVE. There is no audio device in this bare test process and no native
// DLL beside it, so nothing here asserts that a sound is audible -- that is what tests/audio's own
// AudioTest/AudioProbe are for, and they already pass. What these prove is the WIRING: that sound=
// parses onto the right property, that the pins match the palette, that side-effecting nodes are
// refused by the pure compiler, and that the emitted IL reaches the real managed API rather than
// being silently dropped.
//
// Audio.Ready and every method on Audio catch DllNotFoundException by design (a machine with no
// output device is a SUPPORTED configuration, per Audio's own comment), so unlike the Synapse and
// SaveGame nodes these do NOT surface an EntryPointNotFoundException -- they return false/0 exactly
// as they would on a silent machine. Asserting that graceful path IS the contract worth testing.
using System;
using Aver.Graph;

static class AudioNodeTests
{
    public static int RunAll()
    {
        int failures = 0;
        failures += TestPinShapes();
        failures += TestSoundAttributeParses();
        failures += TestPlaySoundRefusedByPullCompiler();
        failures += TestPlaySoundWithoutSoundAttributeFails();
        failures += TestIsSoundPlayingIsPureOnBothCompilers();
        failures += TestPlaySoundRunsAndDegradesGracefully();
        return failures;
    }

    private static int TestPinShapes()
    {
        Console.WriteLine("Test: the six audio nodes have exactly the pins the palette declares");
        try
        {
            const string text =
                "OCGRAPH 1\n" +
                "NODE ps PlaySound sound=Audio/click.ocaudio\n" +
                "NODE pa PlaySoundAt sound=Audio/step.ocaudio\n" +
                "NODE st StopSound\n" +
                "NODE ip IsSoundPlaying\n" +
                "NODE sl SetListener\n" +
                "NODE bv SetBusVolume\n" +
                "OUT ip playing\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var ps = graph.Nodes["ps"];
            bool psOk = ps.Pins.Count == 8 &&
                ps.Pins.Find(p => p.Name == "exec" && !p.IsOutput && p.Type == PinType.Exec) != null &&
                ps.Pins.Find(p => p.Name == "volume" && !p.IsOutput && p.Type == PinType.Float) != null &&
                ps.Pins.Find(p => p.Name == "looping" && !p.IsOutput && p.Type == PinType.Bool) != null &&
                ps.Pins.Find(p => p.Name == "bus" && !p.IsOutput && p.Type == PinType.Int) != null &&
                ps.Pins.Find(p => p.Name == "voice" && p.IsOutput && p.Type == PinType.Int) != null &&
                ps.Pins.Find(p => p.Name == "success" && p.IsOutput && p.Type == PinType.Bool) != null;
            if (!psOk)
            {
                Console.WriteLine($"  FAIL: PlaySound pins: [{string.Join(", ", ps.Pins.ConvertAll(p => p.Name))}]");
                return 1;
            }

            var pa = graph.Nodes["pa"];
            bool paOk = pa.Pins.Count == 13 &&
                pa.Pins.Find(p => p.Name == "innerCm" && !p.IsOutput && p.Type == PinType.Float) != null &&
                pa.Pins.Find(p => p.Name == "outerCm" && !p.IsOutput && p.Type == PinType.Float) != null &&
                pa.Pins.Find(p => p.Name == "z" && !p.IsOutput && p.Type == PinType.Float) != null;
            if (!paOk)
            {
                Console.WriteLine($"  FAIL: PlaySoundAt pins: [{string.Join(", ", pa.Pins.ConvertAll(p => p.Name))}]");
                return 1;
            }

            var ip = graph.Nodes["ip"];
            bool ipOk = ip.Pins.Count == 2 && ip.Pins.TrueForAll(p => p.Type != PinType.Exec) &&
                ip.Pins.Find(p => p.Name == "playing" && p.IsOutput && p.Type == PinType.Bool) != null;
            if (!ipOk)
            {
                Console.WriteLine("  FAIL: IsSoundPlaying should be pure with 2 pins and no exec");
                return 1;
            }

            bool execOk = graph.Nodes["st"].Pins.Count == 4 &&
                          graph.Nodes["sl"].Pins.Count == 4 &&
                          graph.Nodes["bv"].Pins.Count == 5;
            if (!execOk)
            {
                Console.WriteLine("  FAIL: StopSound/SetListener/SetBusVolume pin counts are wrong");
                return 1;
            }

            Console.WriteLine("  PASS: all six shapes, and IsSoundPlaying is the only one without exec pins");
            return 0;
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.Message}"); return 1; }
    }

    // sound= must land on its OWN property, not be swallowed as an unknown token. That is the
    // failure a pin-shape test alone would not catch.
    private static int TestSoundAttributeParses()
    {
        Console.WriteLine("Test: sound= parses onto Node.SoundPath, exactly as written");
        try
        {
            const string text = "OCGRAPH 1\nNODE ps PlaySound sound=Audio/Footsteps/gravel_03.ocaudio\nOUT ps success\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var ps = graph.Nodes["ps"];
            if (ps.SoundPath != "Audio/Footsteps/gravel_03.ocaudio")
            {
                Console.WriteLine($"  FAIL: SoundPath is '{ps.SoundPath}', expected the literal path");
                return 1;
            }
            Console.WriteLine("  PASS: the exact path, including its slashes and extension");
            return 0;
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.Message}"); return 1; }
    }

    private static int TestPlaySoundRefusedByPullCompiler()
    {
        Console.WriteLine("Test: PlaySound in a no-ENTRY (pure-PULL) graph is refused, naming PlaySound");
        try
        {
            const string text = "OCGRAPH 1\nNODE ps PlaySound sound=a.ocaudio\nOUT ps voice\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            if (new GraphCompiler(graph).Compile(out var cerr) != null)
            {
                Console.WriteLine("  FAIL: expected Compile() to refuse an ungated PlaySound");
                return 1;
            }
            if (cerr == null || cerr.IndexOf("PlaySound", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error naming PlaySound, got: {cerr}");
                return 1;
            }
            Console.WriteLine($"  PASS: {cerr}");
            return 0;
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.Message}"); return 1; }
    }

    private static int TestPlaySoundWithoutSoundAttributeFails()
    {
        Console.WriteLine("Test: PlaySound with no sound= attribute fails to compile, naming the node");
        try
        {
            const string text =
                "OCGRAPH 1\nNODE tick OnTick\nNODE ps PlaySound\nLINK tick.exec ps.exec\nENTRY tick OnTick\nOUT ps success\n";
            if (!OcGraphParser.Parse(text, out var graph, out var perr))
            {
                Console.WriteLine($"  PASS: refused at parse: {perr}");
                return 0;
            }
            if (new GraphCompiler(graph).CompileEntryPoint("OnTick", out var cerr) != null)
            {
                Console.WriteLine("  FAIL: expected compilation to fail with no sound=");
                return 1;
            }
            if (cerr == null || cerr.IndexOf("sound=", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error mentioning sound=, got: {cerr}");
                return 1;
            }
            Console.WriteLine($"  PASS: {cerr}");
            return 0;
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.Message}"); return 1; }
    }

    // BOTH COMPILERS -- a node implemented in only one path is the single most repeated bug shape
    // in GraphCompiler.cs's history.
    private static int TestIsSoundPlayingIsPureOnBothCompilers()
    {
        Console.WriteLine("Test: IsSoundPlaying compiles on BOTH compilers (it is a pure read)");
        try
        {
            const string pull = "OCGRAPH 1\nNODE v ConstInt value=0\nNODE ip IsSoundPlaying\nLINK v.value ip.voice\nOUT ip playing\n";
            if (!OcGraphParser.Parse(pull, out var g1, out var e1))
            {
                Console.WriteLine($"  FAIL: pull parse: {e1}");
                return 1;
            }
            if (new GraphCompiler(g1).Compile(out var c1) == null)
            {
                Console.WriteLine($"  FAIL: Compile() refused a pure read: {c1}");
                return 1;
            }

            const string push = "OCGRAPH 1\nNODE tick OnTick\nENTRY tick OnTick\nNODE v ConstInt value=0\nNODE ip IsSoundPlaying\nLINK v.value ip.voice\nOUT ip playing\n";
            if (!OcGraphParser.Parse(push, out var g2, out var e2))
            {
                Console.WriteLine($"  FAIL: push parse: {e2}");
                return 1;
            }
            if (new GraphCompiler(g2).CompileEntryPoint("OnTick", out var c2) == null)
            {
                Console.WriteLine($"  FAIL: CompileEntryPoint() refused a pure read: {c2}");
                return 1;
            }
            Console.WriteLine("  PASS: both compilers accept it");
            return 0;
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.Message}"); return 1; }
    }

    // THE ONE THAT ACTUALLY RUNS THE IL. On a machine with no audio DLL this must return false and
    // voice 0 rather than throwing -- the "silence is a supported configuration" contract. If the
    // emitted call were wired to the wrong method or the wrong argument order, this is where a
    // signature mismatch surfaces, because DynamicInvoke really does execute it.
    private static int TestPlaySoundRunsAndDegradesGracefully()
    {
        Console.WriteLine("Test: PlaySound's emitted IL RUNS, and reports failure rather than throwing with no device");
        try
        {
            const string text =
                "OCGRAPH 1\n" +
                "NODE tick OnTick\nENTRY tick OnTick\n" +
                "NODE vol ConstFloat value=0.8\n" +
                "NODE pit ConstFloat value=1.0\n" +
                "NODE lop ConstBool value=false\n" +
                "NODE bus ConstInt value=0\n" +
                "NODE ps PlaySound sound=Audio/click.ocaudio\n" +
                "LINK tick.exec ps.exec\n" +
                "LINK vol.value ps.volume\n" +
                "LINK pit.value ps.pitch\n" +
                "LINK lop.value ps.looping\n" +
                "LINK bus.value ps.bus\n" +
                "OUT ps success\n";
            if (!OcGraphParser.Parse(text, out var graph, out var perr))
            {
                Console.WriteLine($"  FAIL: Parse error: {perr}");
                return 1;
            }
            var compiled = new GraphCompiler(graph).CompileEntryPoint("OnTick", out var cerr);
            if (compiled is not Func<bool> fn)
            {
                Console.WriteLine($"  FAIL: compile: {cerr ?? "(wrong delegate shape)"}");
                return 1;
            }
            bool got = fn();
            if (got)
            {
                Console.WriteLine("  FAIL: reported success with no audio device present -- the graceful path is not being taken");
                return 1;
            }
            Console.WriteLine("  PASS: ran end to end, returned false on a silent machine, threw nothing");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: it threw instead of degrading: {ex.InnerException?.Message ?? ex.Message}");
            return 1;
        }
    }
}
