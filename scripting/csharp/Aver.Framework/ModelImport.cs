// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Importing a model file (FBX, glTF/GLB) into project Content from a script. Import is an editor-time act:
// only the editor installs a provider (aver_fw_set_import_provider), so in a packaged game every call here
// answers "unavailable" instead of throwing. Design: docs/plans/wave9/FBX_IMPORT.md 2.10, docs/FBX_IMPORT.md.

using System.Globalization;
using System.Text;

namespace Aver.Framework;

/// <summary>What an import wrote. <see cref="Message"/> says why when <see cref="Ok"/> is false.</summary>
public readonly record struct ModelImportResult(bool Ok, string Message, int Meshes, int Skeletons, int Clips, int Materials, int Textures);

/// <summary>How the importer flags a clip as carrying root motion (FBX only).</summary>
public enum RootMotionImport
{
    /// <summary>Never flag a clip.</summary>
    Off = 0,
    /// <summary>Flag a clip whose root bone travels more than 10 cm in the plane.</summary>
    Auto = 1,
    /// <summary>Flag every clip that has a track on the root bone.</summary>
    All = 2,
}

/// <summary>The FBX import options a script can set; the rest keep the editor's defaults. Ignored for glTF.</summary>
public sealed class ModelImportOptions
{
    /// <summary>Centimetres per metre: the file is converted to metres, then multiplied by this.</summary>
    public float Scale { get; set; } = 100f;
    /// <summary>Greater than 0 ignores the file's own unit and treats one file unit as this many metres.</summary>
    public float UnitMeters { get; set; } = 0f;
    /// <summary>Import the geometry.</summary>
    public bool Meshes { get; set; } = true;
    /// <summary>Import skeletons and skin weights.</summary>
    public bool Skeletons { get; set; } = true;
    /// <summary>Import animation clips.</summary>
    public bool Animations { get; set; } = true;
    /// <summary>Import materials and their textures.</summary>
    public bool Materials { get; set; } = true;
    /// <summary>Import blend shapes (reported, not carried, until morph targets land).</summary>
    public bool BlendShapes { get; set; } = true;
    /// <summary>Which clips are flagged as root motion.</summary>
    public RootMotionImport RootMotion { get; set; } = RootMotionImport.Auto;
    /// <summary>Resample rate for non-linear curves.</summary>
    public int BakeFps { get; set; } = 30;
    /// <summary>Let the importer drop keys a straight line reproduces.</summary>
    public bool ReduceKeys { get; set; } = true;

    // The importer's option text (fbxOptionsToText's format); a key left out keeps the editor's default.
    internal string ToText()
    {
        static string B(bool v) => v ? "1" : "0";
        var inv = CultureInfo.InvariantCulture;
        var sb = new StringBuilder();
        sb.Append("scale=").Append(Scale.ToString("R", inv));
        sb.Append(";unitMeters=").Append(UnitMeters.ToString("R", inv));
        sb.Append(";meshes=").Append(B(Meshes));
        sb.Append(";skeletons=").Append(B(Skeletons));
        sb.Append(";animations=").Append(B(Animations));
        sb.Append(";materials=").Append(B(Materials));
        sb.Append(";blendShapes=").Append(B(BlendShapes));
        sb.Append(";rootMotion=").Append(RootMotion switch { RootMotionImport.Off => "off", RootMotionImport.All => "all", _ => "auto" });
        sb.Append(";bakeFps=").Append(Math.Clamp(BakeFps, 1, 240).ToString(inv));
        sb.Append(";reduceKeys=").Append(B(ReduceKeys));
        return sb.ToString();
    }
}

/// <summary>Import a model file into the project's Content folder. Only an editor host can; see <see cref="IsAvailable"/>.</summary>
public static class ModelImport
{
    /// <summary>True when the host can import (the editor). False in a packaged game.</summary>
    public static bool IsAvailable
    {
        get
        {
            try { return FwImport.aver_fw_import_available() != 0; }
            catch (DllNotFoundException) { return false; }
            catch (EntryPointNotFoundException) { return false; }
        }
    }

    /// <summary>Imports <paramref name="source"/> (an absolute path, or one relative to the project folder) into
    /// the Content-relative folder <paramref name="destFolder"/>. Runs synchronously. With <paramref name="overwrite"/>
    /// the files it replaces go to the recycle bin. Never throws for a refusal: read <see cref="ModelImportResult.Ok"/>.</summary>
    public static ModelImportResult Import(string source, string destFolder, ModelImportOptions? options = null, bool overwrite = false)
    {
        if (string.IsNullOrEmpty(source)) return Fail("no source file was given");
        var message = new byte[1024];
        var counts = new int[5];
        int ok;
        try
        {
            ok = FwImport.aver_fw_import_model(source, destFolder ?? string.Empty, options?.ToText() ?? string.Empty,
                                               overwrite ? 1 : 0, message, message.Length, counts);
        }
        catch (DllNotFoundException) { return Fail("model import is not available in this host"); }
        catch (EntryPointNotFoundException) { return Fail("model import is not available in this host"); }
        int len = Array.IndexOf(message, (byte)0);
        string text = Encoding.UTF8.GetString(message, 0, len < 0 ? message.Length : len);
        return new ModelImportResult(ok != 0, text, counts[0], counts[1], counts[2], counts[3], counts[4]);
    }

    private static ModelImportResult Fail(string why) => new(false, why, 0, 0, 0, 0, 0);
}

/// <summary>What the Import Model and Is Model Import Available graph nodes call: primitive-only signatures
/// the graph compiler's emitted IL needs.</summary>
public static class ModelImportGraph
{
    /// <summary>Import Model node: imports <paramref name="source"/> into <paramref name="dest"/> (Content-relative).
    /// A <paramref name="scale"/> of 0 or less (an unwired pin) means the default, 100.</summary>
    public static bool ImportModelForGraph(string source, string dest, bool overwrite, float scale,
                                           out int meshes, out int skeletons, out int clips)
    {
        var options = new ModelImportOptions { Scale = scale > 0f ? scale : 100f };
        ModelImportResult r = ModelImport.Import(source, dest, options, overwrite);
        meshes = r.Meshes;
        skeletons = r.Skeletons;
        clips = r.Clips;
        return r.Ok;
    }

    /// <summary>Is Model Import Available node.</summary>
    public static bool IsModelImportAvailableForGraph() => ModelImport.IsAvailable;
}
