// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the audio-control nodes (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md section 2.2): the 11 rows in
// GraphAudioControlNodeTable.cs, every method on AudioControlGraph. Bare-process scope like MorphNodeTests: the wiring the
// table owns (pins, defaults, the method each row names, valid IL, the compiler split), not what the mixer does. With no audio
// device every write answers false and every read 0, which the last test checks where the native library loads.
using System;
using System.Linq;
using Aver.Framework;
using Aver.Graph;
using Row = NodeRowKit.Row;

[AutoRun]
static class AudioControlNodeTests
{
    private static readonly Row[] Rows =
    {
        new("an_setmastervolume", GenericKind.Exec, new[] { "exec:exec:in", "volume:float:in", "then:exec:out", "success:bool:out" }, new[] { "volume=1" }),
        new("an_getmastervolume", GenericKind.Pure, new[] { "volume:float:out" }),
        new("an_getbusvolume", GenericKind.Pure, new[] { "bus:int:in", "volume:float:out" }),
        new("an_stopallsounds", GenericKind.Exec, new[] { "exec:exec:in", "then:exec:out", "success:bool:out" }),
        new("an_setvoicevolume", GenericKind.Exec, new[] { "exec:exec:in", "voice:int:in", "volume:float:in", "then:exec:out", "success:bool:out" }),
        new("an_setvoicepitch", GenericKind.Exec, new[] { "exec:exec:in", "voice:int:in", "pitch:float:in", "then:exec:out", "success:bool:out" }, new[] { "pitch=1" }),
        new("an_setvoiceposition", GenericKind.Exec, new[] { "exec:exec:in", "voice:int:in", "x:float:in", "y:float:in", "z:float:in", "then:exec:out", "success:bool:out" }),
        new("an_isvoiceplaying", GenericKind.Pure, new[] { "voice:int:in", "playing:bool:out" }),
        new("an_stopvoice", GenericKind.Exec, new[] { "exec:exec:in", "voice:int:in", "then:exec:out", "success:bool:out" }),
        new("an_setocclusioncurve", GenericKind.Exec, new[] { "exec:exec:in", "minCutoffHz:float:in", "minVolume:float:in", "then:exec:out", "success:bool:out" }, new[] { "minCutoffHz=800", "minVolume=0.25" }),
        new("an_getaudiostats", GenericKind.Pure, new[] { "activeVoices:int:out", "activeStreams:int:out", "underruns:int:out" }),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("audio control", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("audio control", Rows);
        failures += NodeRowKit.CheckIlValid("audio control", types);
        failures += NodeRowKit.CheckCompilerSplit("audio control", types);
        failures += TestVoiceControlChain();
        failures += TestReadsFeedLaterNodes();
        failures += TestSilenceIsNotAFailure();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the audio-control rows are exactly the designed set of 11, 4 pure reads and 7 exec writes");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "AudioControlGraph").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 11) return Fail($"expected 11 rows, test lists {Rows.Length}");
        var pure = Rows.Where(r => r.Kind == GenericKind.Pure).Select(r => r.Type).OrderBy(x => x).ToArray();
        if (!pure.SequenceEqual(new[] { "an_getaudiostats", "an_getbusvolume", "an_getmastervolume", "an_isvoiceplaying" })) return Fail("only the four reads may be pure");
        Console.WriteLine("  PASS");
        return 0;
    }

    // The voice handle comes from a stream play node's voice pin and steers the rest.
    private static int TestVoiceControlChain()
    {
        Console.WriteLine("Test: Play Stream -> its voice pin feeds Set Voice Volume, Pitch, Position and Stop Voice; master and occlusion writes chain on");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE ps AN_PlayStreamAt sound=sfx/engine.ogg\nNODE vv AN_SetVoiceVolume\nNODE vp AN_SetVoicePitch\nNODE vo AN_SetVoicePosition\n" +
                            "NODE sv AN_StopVoice\nNODE mv AN_SetMasterVolume\nNODE oc AN_SetOcclusionCurve\nNODE sa AN_StopAllSounds\n" +
                            "LINK tick.exec ps.exec\nLINK ps.then vv.exec\nLINK vv.then vp.exec\nLINK vp.then vo.exec\nLINK vo.then sv.exec\nLINK sv.then mv.exec\nLINK mv.then oc.exec\nLINK oc.then sa.exec\n" +
                            "LINK ps.voice vv.voice\nLINK ps.voice vp.voice\nLINK ps.voice vo.voice\nLINK ps.voice sv.voice\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }

    // Playing gates a branch; the volume and the stats feed prints.
    private static int TestReadsFeedLaterNodes()
    {
        Console.WriteLine("Test: Is Voice Playing gates a Branch; master volume, bus volume and the stats feed prints");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE ps AN_PlayMusic sound=music/theme.ogg\nNODE ip AN_IsVoicePlaying\nNODE br Branch\nNODE mv AN_GetMasterVolume\nNODE bv AN_GetBusVolume\n" +
                            "NODE st AN_GetAudioStats\nNODE pa Print\nNODE pb Print\nNODE pc PrintInt\n" +
                            "LINK tick.exec ps.exec\nLINK ps.then br.exec\nLINK ps.voice ip.voice\nLINK ip.playing br.cond\nLINK br.true pa.exec\nLINK mv.volume pa.value\nLINK pa.then pb.exec\nLINK bv.volume pb.value\n" +
                            "LINK pb.then pc.exec\nLINK st.activeVoices pc.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }

    // Without a device (or without the audio DLL) the control surface is inert: no exception, no sound.
    private static int TestSilenceIsNotAFailure()
    {
        Console.WriteLine("Test: with no audio device Audio.Ready is false and the volumes read 0 (skipped when the audio DLL is absent)");
        try
        {
            if (Audio.Ready) { Console.WriteLine("  SKIP: a device is open in this process"); return 0; }
            if (Audio.MasterVolume != 0f || Audio.ActiveVoices != 0 || Audio.ActiveStreams != 0) return Fail("a closed mixer must read 0");
            Console.WriteLine("  PASS");
        }
        catch (Exception e) when (NodeRowKit.NativeMissing(e))
        {
            Console.WriteLine("  SKIP: audio library not loadable in this process");
        }
        return 0;
    }
}
