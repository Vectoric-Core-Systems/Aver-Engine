// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// GDScript to C#: the entry point. The result is C# text that goes through the same compile, generate and report steps as GodotSharp.

namespace Aver.Fabric.Convert.GdScript;

/// <summary>One converted .gd file, before the C# pipeline has compiled it.</summary>
internal sealed class GdConvertedFile
{
    public string ResPath = "";
    public string Output = "";
    public string ClassName = "";
    public string Text = "";
    public List<EmitNote> Notes = new();
    public List<ParseError> ParseErrors = new();
    public int Functions, Signals, Members;
}

internal static class GdScriptConverter
{
    /// <summary>Converts the scripts as one project: every class sees the others.</summary>
    public static List<GdConvertedFile> Convert(IReadOnlyList<GdSource> sources, ApiModel? api, string? godotProjectDir, SceneIndex? scenes = null)
    {
        ProjectModel model = ProjectModel.Build(sources, api, godotProjectDir);
        model.Scenes = scenes;
        var emitter = new GdEmitter(model);
        emitter.CollectParamHints(model.Scripts);
        var result = new List<GdConvertedFile>();
        foreach (ScriptInfo s in model.Scripts)
        {
            string text;
            bool failed = false;
            try { text = emitter.EmitScript(s); }
            catch (Exception ex) when (ex is not OutOfMemoryException)
            {
                text = $"// FABRIC: the converter failed on this script: {ex.GetType().Name}: {ex.Message.Replace('\n', ' ')}\n" +
                       $"public partial class {s.CsName} : GodotObject {{ }}\n";
                failed = true;
            }
            var file = new GdConvertedFile
            {
                ResPath = s.ResPath, Output = s.Output, ClassName = s.CsName, Text = text, Notes = emitter.Notes.ToList(),
                ParseErrors = s.Parse?.Errors ?? new List<ParseError>(),
                Functions = s.Members.Count(m => m.Kind == MK.Func), Signals = s.Members.Count(m => m.Kind == MK.Signal),
                Members = s.Members.Count(m => m.Kind is MK.Var or MK.Const),
            };
            if (failed)
                file.Notes.Add(new EmitNote { Tag = 0, Type = "converter", Rating = Rating.Unsupported, Reason = "the converter failed on this script (see the first line of the output)" });
            result.Add(file);
        }
        return result;
    }
}
