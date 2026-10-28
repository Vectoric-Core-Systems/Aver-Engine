// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Shared harness: assertions, approximate comparison and a clean slate (managed node host, fresh tree, captured log) per test.

using Aver.Fabric;
using Aver.Scripting;

static class T
{
    public static readonly List<(Log.Level Level, string Message)> Logs = new();
    private static int s_failures;
    private static int s_checks;

    public static int Checks => s_checks;

    /// <summary>A clean slate: the managed node host, a new scene tree, an empty deferred queue and a captured log.</summary>
    public static void Reset()
    {
        NodeHost.Current = new ManagedNodeHost();
        SceneTree.Reset();
        Aver.Framework.GameThread.Reset();
        Logs.Clear();
        Rec.Trail.Clear();
        Log.SetSink((l, m) => Logs.Add((l, m)));
        GD.PrintSink = null;
        GD.ErrorSink = null;
        ResourceLoader.ClearCache();
        ResourceLoader.Provider = null;
        ResourceLoader.ContentRootOverride = null;
    }

    /// <summary>Runs one frame of the managed loop: <paramref name="physicsSteps"/> fixed steps, then the process pass.</summary>
    public static void Frame(double dt = 1.0 / 60.0, int physicsSteps = 0) => SceneTree.Instance.RunManagedFrame(dt, physicsSteps);

    public static void Frames(int count, double dt, int physicsStepsPerFrame = 0)
    {
        for (int i = 0; i < count; i++) Frame(dt, physicsStepsPerFrame);
    }

    /// <summary>The folder of the hand-written Godot-text and level fixtures (tests/fabric/data), found above the test's output folder.</summary>
    public static string DataDir()
    {
        for (DirectoryInfo? d = new DirectoryInfo(AppContext.BaseDirectory); d != null; d = d.Parent)
        {
            string p = Path.Combine(d.FullName, "tests", "fabric", "data");
            if (Directory.Exists(p)) return p;
        }
        throw new DirectoryNotFoundException("tests/fabric/data was not found above " + AppContext.BaseDirectory);
    }

    public static bool Near(float a, float b, float eps = 1e-4f) => MathF.Abs(a - b) <= eps;
    public static bool Near(Vector3 a, Vector3 b, float eps = 1e-4f) => Near(a.X, b.X, eps) && Near(a.Y, b.Y, eps) && Near(a.Z, b.Z, eps);
    public static bool Near(Quaternion a, Quaternion b, float eps = 1e-4f) =>
        (Near(a.X, b.X, eps) && Near(a.Y, b.Y, eps) && Near(a.Z, b.Z, eps) && Near(a.W, b.W, eps)) ||
        (Near(a.X, -b.X, eps) && Near(a.Y, -b.Y, eps) && Near(a.Z, -b.Z, eps) && Near(a.W, -b.W, eps));

    public static int ErrorCount(string fragment = "") => Logs.Count(l => l.Level == Log.Level.Error && l.Message.Contains(fragment));

    public static void Check(string what, bool ok, string detail = "")
    {
        s_checks++;
        if (ok) { Console.WriteLine($"  PASS: {what}"); return; }
        Console.WriteLine($"  FAIL: {what}{(detail.Length > 0 ? " -- " + detail : "")}");
        s_failures++;
    }

    public static void CheckVec(string what, Vector3 expected, Vector3 actual, float eps = 1e-4f) =>
        Check(what, Near(expected, actual, eps), $"expected {expected}, got {actual}");

    public static void CheckNum(string what, float expected, float actual, float eps = 1e-4f) =>
        Check(what, Near(expected, actual, eps), $"expected {expected}, got {actual}");

    public static void CheckSeq<TItem>(string what, IEnumerable<TItem> expected, IEnumerable<TItem> actual) =>
        Check(what, expected.SequenceEqual(actual), $"expected [{string.Join(", ", expected)}], got [{string.Join(", ", actual)}]");

    public static void Run(string name, Action test)
    {
        Console.WriteLine($"Test: {name}");
        Reset();
        try { test(); }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: threw {ex.GetType().Name}: {ex.Message}\n{ex.StackTrace}");
            s_failures++;
        }
    }

    /// <summary>Runs a file's tests and returns how many checks failed in them.</summary>
    public static int Suite(Action tests)
    {
        int before = s_failures;
        tests();
        return s_failures - before;
    }
}
