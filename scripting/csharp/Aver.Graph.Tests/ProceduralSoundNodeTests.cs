// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the real-time sound-graph nodes (docs/AUDIO.md section 13): the four rows in GraphProceduralSoundNodeTable.cs, every
// method on ProceduralSoundGraph. Bare-process scope like AudioControlNodeTests: the wiring the table owns (pins, defaults, the method
// each row names, valid IL, the compiler split), not what the mixer does -- that is tests/audio/SoundGraphTest and AudioAbiTest.
// With no audio device every call answers false with voice 0, which the last test checks where the native library loads.
using System;
using System.Linq;
using Aver.Framework;
using Aver.Graph;
using Row = NodeRowKit.Row;

[AutoRun]
static class ProceduralSoundNodeTests
{
    private static readonly Row[] Rows =
    {
        new("an_playsoundgraph", GenericKind.Exec, new[] { "exec:exec:in", "volume:float:in", "looping:bool:in", "bus:int:in", "seed:int:in", "then:exec:out", "voice:int:out", "success:bool:out" }, new[] { "volume=1" }),
        new("an_playsoundgraphat", GenericKind.Exec, new[] { "exec:exec:in", "x:float:in", "y:float:in", "z:float:in", "volume:float:in", "looping:bool:in", "bus:int:in", "innerCm:float:in", "outerCm:float:in", "seed:int:in", "then:exec:out", "voice:int:out", "success:bool:out" }, new[] { "volume=1", "innerCm=200", "outerCm=2000" }),
        new("an_setsoundgraphparam", GenericKind.Exec, new[] { "exec:exec:in", "voice:int:in", "value:float:in", "then:exec:out", "success:bool:out" }),
        new("an_noteoffsoundgraph", GenericKind.Exec, new[] { "exec:exec:in", "voice:int:in", "then:exec:out", "success:bool:out" }),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("procedural sound", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("procedural sound", Rows);
        failures += NodeRowKit.CheckIlValid("procedural sound", types);
        failures += NodeRowKit.CheckCompilerSplit("procedural sound", types);
        failures += TestPlaySetNoteOffChain();
        failures += TestSilenceIsNotAFailure();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the sound-graph rows are exactly the designed set of 4, all exec (each has a side effect)");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "ProceduralSoundGraph").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Any(r => r.Kind != GenericKind.Exec)) return Fail("a play, a write and a note-off are all exec nodes");
        Console.WriteLine("  PASS");
        return 0;
    }

    // Play's voice pin feeds the input write and the note-off, and the audio-control nodes accept the same handle.
    private static int TestPlaySetNoteOffChain()
    {
        Console.WriteLine("Test: Play Sound Graph -> its voice pin feeds Set Sound Graph Input, Set Voice Volume and Note Off Sound Graph");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE pg AN_PlaySoundGraphAt sound=sfx/engine.ocsnd\nNODE sp AN_SetSoundGraphParam param=rpm\nNODE vv AN_SetVoiceVolume\nNODE no AN_NoteOffSoundGraph\n" +
                            "NODE p2 AN_PlaySoundGraph sound=sfx/engine.ocsnd\n" +
                            "LINK tick.exec pg.exec\nLINK pg.then sp.exec\nLINK sp.then vv.exec\nLINK vv.then no.exec\nLINK no.then p2.exec\n" +
                            "LINK pg.voice sp.voice\nLINK pg.voice vv.voice\nLINK pg.voice no.voice\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }

    // Without a device (or without the audio DLL) the surface is inert: no exception, voice 0.
    private static int TestSilenceIsNotAFailure()
    {
        Console.WriteLine("Test: with no audio device the sound-graph calls answer false with voice 0 (skipped when the audio DLL is absent)");
        try
        {
            if (Audio.Ready) { Console.WriteLine("  SKIP: a device is open in this process"); return 0; }
            if (ProceduralSoundGraph.PlaySoundGraphForGraph("sfx/none.ocsnd", 1f, false, 0, 0, out int v) || v != 0) return Fail("a play with no device must answer false and voice 0");
            if (ProceduralSoundGraph.PlaySoundGraphAtForGraph("sfx/none.ocsnd", 0, 0, 0, 1f, false, 0, 200f, 2000f, 0, out v) || v != 0) return Fail("a positioned play with no device must answer false and voice 0");
            if (ProceduralSoundGraph.SetSoundGraphParamForGraph(0, "rpm", 1f)) return Fail("a write to voice 0 must answer false");
            if (ProceduralSoundGraph.NoteOffSoundGraphForGraph(0)) return Fail("a note-off to voice 0 must answer false");
            if (Audio.LoadGraph("sfx/none.ocsnd").IsValid || new Voice(0).IsGraphVoice) return Fail("a closed mixer loads no graph and has no graph voice");
            Console.WriteLine("  PASS");
        }
        catch (Exception e) when (NodeRowKit.NativeMissing(e))
        {
            Console.WriteLine("  SKIP: audio library not loadable in this process");
        }
        return 0;
    }
}
