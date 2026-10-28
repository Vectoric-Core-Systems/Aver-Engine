// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// A reader for the part of the .ocworld level format that carries a converted Godot scene: PLACE / PLACEG / CHILD / CHILDG lines with
// BEGIN / END nesting, and the COMPONENT Props and COMPONENT Groups records on them (docs/plans/fabric/G3_RUNTIME.md section 1).
// It is the runtime's own small reader (the engine's full one is C++); every other record of a level is skipped.

using System.Globalization;
using System.Text;
using Aver.Scene;

namespace Aver.Fabric;

/// <summary>One placement of a converted scene: a node with its class, local transform, properties and children.</summary>
internal sealed class SceneNodeData
{
    public string Asset = "none", Class = "", Name = "";
    public double X, Y, Z, Yaw, Pitch, Roll, Sx = 1, Sy = 1, Sz = 1;
    public readonly List<(string Key, string Text)> Props = new();
    public readonly List<string> Groups = new();
    public readonly List<SceneNodeData> Children = new();

    /// <summary>An autoload: the project keeps it across scenes, so it is not part of any scene's own tree.</summary>
    public bool Persistent => Props.Exists(p => p.Key == "_persistent" && p.Text is "1" or "true");

    /// <summary>The placement's transform as a Godot transform (the line is Aver space: centimetres, yaw/pitch/roll in degrees).</summary>
    public Transform3D LocalTransform() =>
        FabricSpace.FromAver(new Vec3((float)X, (float)Y, (float)Z), new Rot((float)Yaw, (float)Pitch, (float)Roll).ToQuat(), new Vec3((float)Sx, (float)Sy, (float)Sz));
}

internal static class OcWorldScene
{
    /// <summary>The top-level placements of a level, with their children. Throws FormatException on a file that is not a level or has
    /// unbalanced BEGIN/END.</summary>
    internal static List<SceneNodeData> Parse(string text)
    {
        var roots = new List<SceneNodeData>();
        var scope = new Stack<SceneNodeData>();
        SceneNodeData? last = null;
        bool header = false;
        int lineNo = 0;
        foreach (string raw in text.Split('\n'))
        {
            lineNo++;
            string line = raw;
            int hash = line.IndexOf('#');
            if (hash >= 0) line = line[..hash];
            line = line.Trim();
            if (line.EndsWith(';')) line = line[..^1].TrimEnd();
            if (line.Length == 0) continue;
            string[] t = line.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            string key = t[0].ToUpperInvariant();
            switch (key)
            {
                case "OCWORLD":
                case "OCMAP":
                    header = true;
                    break;
                case "BEGIN":
                    if (last == null) throw new FormatException($"line {lineNo}: BEGIN with no placement before it");
                    scope.Push(last);
                    break;
                case "END":
                    if (scope.Count == 0) throw new FormatException($"line {lineNo}: END with no matching BEGIN");
                    scope.Pop();
                    break;
                case "PLACE":
                case "PLACEG":
                case "CHILD":
                case "CHILDG":
                {
                    bool child = key.StartsWith("CHILD", StringComparison.Ordinal);
                    bool g = key.EndsWith('G');
                    if (child && scope.Count == 0) throw new FormatException($"line {lineNo}: CHILD outside any BEGIN/END scope");
                    var d = new SceneNodeData { Asset = t.Length > 1 ? t[1] : "none" };
                    d.X = Num(t, 2); d.Y = Num(t, 3); d.Z = Num(t, 4);
                    d.Yaw = Num(t, 5); d.Pitch = Num(t, 6); d.Roll = Num(t, 7);
                    int next;
                    if (g)
                    {
                        d.Sx = Num(t, 8, 1); d.Sy = Num(t, 9, 1); d.Sz = Num(t, 10, 1);
                        next = 11;
                    }
                    else
                    {
                        double s = Num(t, 8, 1);
                        d.Sx = d.Sy = d.Sz = s == 0 ? 1 : s;
                        next = 9;
                    }
                    for (int i = next; i < t.Length; i++)
                    {
                        if (t[i].Equals("class", StringComparison.OrdinalIgnoreCase) && i + 1 < t.Length) d.Class = t[++i];
                        else if (t[i].Equals("name", StringComparison.OrdinalIgnoreCase) && i + 1 < t.Length) d.Name = PercentDecode(t[++i]);
                    }
                    if (child) scope.Peek().Children.Add(d); else roots.Add(d);
                    last = d;
                    break;
                }
                case "COMPONENT":
                    if (last == null || t.Length < 2) break;
                    if (t[1].Equals("Props", StringComparison.OrdinalIgnoreCase))
                    {
                        for (int i = 2; i < t.Length; i++)
                        {
                            int eq = t[i].IndexOf('=');
                            string k = PercentDecode(eq < 0 ? t[i] : t[i][..eq]).ToLowerInvariant();
                            if (k.Length == 0) continue;
                            last.Props.Add((k, eq < 0 ? "1" : PercentDecode(t[i][(eq + 1)..])));
                        }
                    }
                    else if (t[1].Equals("Groups", StringComparison.OrdinalIgnoreCase))
                    {
                        for (int i = 2; i < t.Length; i++)
                        {
                            int eq = t[i].IndexOf('=');
                            if (eq < 0 || !t[i][..eq].Equals("names", StringComparison.OrdinalIgnoreCase)) continue;
                            foreach (string name in PercentDecode(t[i][(eq + 1)..]).Split(',', StringSplitOptions.RemoveEmptyEntries)) last.Groups.Add(name);
                        }
                    }
                    break;
            }
        }
        if (scope.Count > 0) throw new FormatException($"unbalanced BEGIN/END: {scope.Count} scope(s) open at the end of the file");
        if (!header) throw new FormatException("not a level file (no OCWORLD header line)");
        return roots;
    }

    private static double Num(string[] t, int i, double fallback = 0) =>
        i < t.Length && double.TryParse(t[i], NumberStyles.Float, CultureInfo.InvariantCulture, out double v) ? v : fallback;

    // The writer percent-encodes whitespace, '%', quotes, '#' and ';' so a value stays one token. A '%' not followed by two hex digits is kept.
    internal static string PercentDecode(string s)
    {
        if (s.IndexOf('%') < 0) return s;
        var bytes = new List<byte>(s.Length);
        for (int i = 0; i < s.Length; i++)
        {
            if (s[i] == '%' && i + 2 < s.Length && Uri.IsHexDigit(s[i + 1]) && Uri.IsHexDigit(s[i + 2]))
            {
                bytes.Add(Convert.ToByte(s.Substring(i + 1, 2), 16));
                i += 2;
            }
            else
            {
                int len = char.IsHighSurrogate(s[i]) && i + 1 < s.Length ? 2 : 1;
                bytes.AddRange(Encoding.UTF8.GetBytes(s.Substring(i, len)));
                i += len - 1;
            }
        }
        return Encoding.UTF8.GetString(bytes.ToArray());
    }
}
