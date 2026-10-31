// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// What a vehicle demo's scripts needed to compile: a member that only a script derived from the declared class has (`var town: Node3D`, `town.mood`),
// the size of a script enum, Tween.TRANS_* / EASE_* constants, RenderingServer.frame_post_draw, Input.vibrate_handheld and Light3D.shadow_reverse_cull_face.

using System.Reflection;
using Aver.Fabric;
using Aver.Fabric.Convert;

static class ScriptSubtypeTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("convert: a member only a derived script has is reached through that script (variable, call, enum through an instance)", ConvertSubtypeMember);
        T.Run("convert: two unrelated scripts with one member name are not guessed between", ConvertSubtypeAmbiguous);
        T.Run("convert: Enum.size() is the number of names", ConvertEnumSize);
        T.Run("convert: Tween.TRANS_* and EASE_* are the C# enums", ConvertTweenConstants);
        T.Run("convert: await RenderingServer.frame_post_draw, Input.vibrate_handheld and shadow_reverse_cull_face compile", ConvertEngineGaps);
        T.Run("runtime: frame_post_draw comes after the frame that was drawn, not before", PostDrawOrder);
        T.Run("runtime: vibrate_handheld does nothing, shadow_reverse_cull_face is kept", KeptApi);
    });

    private static ConversionResult Conv(IReadOnlyList<ScriptSource> sources) =>
        ScriptConverter.Convert(sources, new ConvertSettings { RefsDir = AppContext.BaseDirectory, KeepAssembly = true, ExtraSources = System.Array.Empty<(string, string)>() });

    private static ScriptSource Gd(string resPath, string cls, string text) =>
        new() { ResPath = resPath, Text = text, Kind = "gdscript", ClassName = cls, Output = "Content/Scripts/" + cls + ".cs" };

    private static string Errors(ConversionResult r) => string.Join("; ", r.Errors.Take(4).Select(e => e.ToString())) + string.Join("; ", r.GeneralErrors);

    private static string Cs(ConversionResult r, string cls) => r.Scripts.First(s => s.ClassName == cls).Text;

    private static bool Has(string text, params string[] parts) => parts.All(text.Contains);

    // ---- a member only a derived script has ----

    private const string TownScript =
        "extends Node3D\n\nenum Mood { SUNRISE, DAY, SUNSET, NIGHT }\n\nvar mood := Mood.DAY\nvar calls := 0\nvar flag := false\n\n" +
        "func setup(car, back, sdfgi):\n\tcalls += 1\n\tflag = sdfgi\n";

    private const string MenuScript =
        "extends Control\n\nvar town: Node3D = null\n\nfunc pick(night: bool):\n\tif night:\n\t\ttown.mood = town.Mood.NIGHT\n\ttown.setup(null, null, night)\n";

    private static void ConvertSubtypeMember()
    {
        ConversionResult r = Conv(new[] { Gd("res://town.gd", "Town", TownScript), Gd("res://menu.gd", "Menu", MenuScript) });
        T.Check("both scripts compile", r.Success, Errors(r));
        if (!r.Success) return;
        string cs = Cs(r, "Menu");
        T.Check("town.mood is read through Town, the one script with a mood, and town.Mood.NIGHT is its enum", cs.Contains("((Town)town).mood = Town.Mood.Night"), cs);
        T.Check("town.setup(...) is a typed call on Town, not a call by name", cs.Contains("((Town)town).Setup(") && !cs.Contains(".Call(\"setup\""), cs);
        T.Check("nothing is read by name", !cs.Contains("GetMember") && !cs.Contains(".Get(\"mood\")"), cs);

        Assembly asm = Assembly.Load(r.Assembly!);
        Type townType = asm.GetType("Town")!, menuType = asm.GetType("Menu")!;
        var town = (Node3D)Activator.CreateInstance(townType)!;
        var menu = (Control)Activator.CreateInstance(menuType)!;
        menuType.GetField("town")!.SetValue(menu, town);
        menuType.GetMethod("Pick")!.Invoke(menu, new object[] { true });
        T.Check("run: the enum value reached the node and setup ran with its arguments",
                Convert.ToInt32(townType.GetProperty("mood")?.GetValue(town) ?? townType.GetField("mood")?.GetValue(town)) == 3 && Convert.ToInt32(townType.GetField("Calls")!.GetValue(town)) == 1 &&
                (bool)townType.GetField("Flag")!.GetValue(town)!);
    }

    private static void ConvertSubtypeAmbiguous()
    {
        const string a = "extends Node3D\n\nvar speed := 1.0\n";
        const string b = "extends Node3D\n\nvar speed := 2.0\n";
        const string user = "extends Node\n\nvar thing: Node3D\n\nfunc f():\n\treturn thing.speed\n";
        ConversionResult r = Conv(new[] { Gd("res://a.gd", "A", a), Gd("res://b.gd", "B", b), Gd("res://user.gd", "User", user) });
        string cs = Cs(r, "User");
        T.Check("with A.speed and B.speed unrelated, Thing is not guessed to be either", !cs.Contains("((A)") && !cs.Contains("((B)"), cs);
        T.Check("and the converter says the member is missing", r.Scripts.First(s => s.ClassName == "User").Items.Any(i => i.Property == "speed"), cs);
    }

    // ---- enum size ----

    private static void ConvertEnumSize()
    {
        const string script =
            "extends Node\n\nenum Unit { MS, KMH, MPH }\nenum Alias { A = 1, B = 1, C = 2 }\n\nvar unit := Unit.MS\n\n" +
            "func next():\n\tunit = ((unit + 1) % Unit.size()) as Unit\n\treturn unit\n\nfunc count():\n\treturn Alias.size() + Unit.size()\n";
        ConversionResult r = Conv(new[] { Gd("res://units.gd", "Units", script) });
        T.Check("the script compiles", r.Success, Errors(r));
        if (!r.Success) return;
        string cs = Cs(r, "Units");
        T.Check("size() is the number of names, as a constant (no .Size member)", !cs.Contains(".Size(") && !cs.Contains(".Size)"), cs);
        Type units = Assembly.Load(r.Assembly!).GetType("Units")!;
        object o = Activator.CreateInstance(units)!;
        MethodInfo next = units.GetMethod("Next")!;
        next.Invoke(o, null);
        next.Invoke(o, null);
        object? wrapped = next.Invoke(o, null);
        T.Check("run: the unit steps 1, 2 and wraps to 0 at the size", Convert.ToInt32(wrapped) == 0);
        T.Check("run: a duplicate value still counts as a name (3 + 3)", Convert.ToInt32(units.GetMethod("Count")!.Invoke(o, null)) == 6);
    }

    // ---- API gaps ----

    private static void ConvertTweenConstants()
    {
        const string script =
            "extends Node3D\n\nfunc go():\n\tvar t := create_tween()\n\tt.tween_property(self, \"position:y\", 2.0, 0.5).set_trans(Tween.TRANS_SINE).set_ease(Tween.EASE_IN_OUT)\n" +
            "\tt.tween_property(self, \"position:y\", 0.0, 0.5).set_trans(Tween.TRANS_ELASTIC).set_ease(Tween.EASE_OUT_IN)\n\tt.set_trans(Tween.TRANS_BACK)\n\tt.set_ease(Tween.EASE_OUT)\n" +
            "\tt.set_process_mode(Tween.TWEEN_PROCESS_PHYSICS)\n\tt.set_pause_mode(Tween.TWEEN_PAUSE_PROCESS)\n";
        ConversionResult r = Conv(new[] { Gd("res://tw.gd", "Tw", script) });
        T.Check("the script compiles", r.Success, Errors(r));
        if (!r.Success) return;
        string cs = Cs(r, "Tw");
        T.Check("TRANS_SINE and EASE_IN_OUT are Tween.TransitionType.Sine and Tween.EaseType.InOut", Has(cs, "SetTrans(Tween.TransitionType.Sine)", "SetEase(Tween.EaseType.InOut)"), cs);
        T.Check("the multi-word ones too (ELASTIC, OUT_IN, BACK, OUT)", Has(cs, "Tween.TransitionType.Elastic", "Tween.EaseType.OutIn", "Tween.TransitionType.Back", "Tween.EaseType.Out)"), cs);
        T.Check("and the process and pause modes (TWEEN_PROCESS_PHYSICS, TWEEN_PAUSE_PROCESS)", Has(cs, "Tween.TweenProcessMode.Physics", "Tween.TweenPauseMode.Process"), cs);
    }

    private static void ConvertEngineGaps()
    {
        const string script =
            "extends Node3D\n\nfunc go(l: Light3D):\n\tawait RenderingServer.frame_post_draw\n\tInput.vibrate_handheld(100)\n\tl.shadow_reverse_cull_face = true\n" +
            "\tRenderingServer.frame_pre_draw.connect(_on_pre)\n\nfunc _on_pre():\n\tpass\n";
        ConversionResult r = Conv(new[] { Gd("res://gaps.gd", "Gaps", script) });
        T.Check("the script compiles", r.Success, Errors(r));
        if (!r.Success) return;
        string cs = Cs(r, "Gaps");
        T.Check("the await waits on the singleton's signal", cs.Contains("RenderingServer.Singleton, RenderingServer.SignalName.FramePostDraw"), cs);
        T.Check("vibrate_handheld and the shadow flag are typed members", Has(cs, "Input.VibrateHandheld(100)", "l.ShadowReverseCullFace = true"), cs);
    }

    // ---- the runtime ----

    private static void PostDrawOrder()
    {
        int post = 0, pre = 0;
        Action countPost = () => post++;
        Action countPre = () => pre++;
        RenderingServer.FramePostDraw += countPost;
        RenderingServer.FramePreDraw += countPre;
        T.Frame();
        T.Check("frame 1: pre_draw came at its end, post_draw has no frame to follow yet", pre == 1 && post == 0);
        int late = 0;
        RenderingServer.FramePostDraw += () => late++;   // made after the frame ended: its next draw is frame 2's
        T.Frame();
        T.Check("frame 2: post_draw for frame 1's draw reaches the early connection only", pre == 2 && post == 1 && late == 0);
        T.Frame();
        T.Check("frame 3: and now the late one, for the draw of frame 2", post == 2 && late == 1);
        RenderingServer.FramePostDraw -= countPost;
        RenderingServer.FramePreDraw -= countPre;
        T.Frame();
        T.Check("a disconnected handler hears nothing more", post == 2 && pre == 3);

        SignalAwaiter aw = new(RenderingServer.Singleton, RenderingServer.SignalName.FramePostDraw);
        T.Frame();
        T.Check("an await made between frames is not finished by the draw already past", !aw.IsCompleted);
        T.Frame();
        T.Check("it is finished by the next frame's draw", aw.IsCompleted);
    }

    private static void KeptApi()
    {
        Input.VibrateHandheld(100);
        Input.VibrateHandheld();
        var light = new OmniLight3D();
        T.Check("shadow_reverse_cull_face is false until set, and kept", !light.ShadowReverseCullFace && (light.ShadowReverseCullFace = true));
        light.Set("shadow_reverse_cull_face", false);
        T.Check("and it is a property by name too, as a scene sets it", !light.ShadowReverseCullFace);
    }
}
