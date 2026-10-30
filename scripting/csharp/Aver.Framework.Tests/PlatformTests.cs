// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// PlatformTests: res:// and user:// paths, the manifest's settings, and ConfigFile's text round trip. No host.

using Aver.Framework;
using Aver.Scene;

static class PlatformTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("paths: res:// and user:// resolve under their roots, other paths pass through", PathsResolve);
        T.Run("project settings: KEY value lines, any case, comments and the header skipped", ProjectSettingsParse);
        T.Run("config file: every kind of value survives text and back", ConfigRoundTrip);
        T.Run("config file: sections and keys can be listed and erased; bad text changes nothing", ConfigEdits);
    });

    private static void PathsResolve()
    {
        string content = Path.Combine("C:", "Game", "Content"), user = Path.Combine("C:", "Users", "p", "Game");
        T.Check("res://", Paths.Globalize("res://Maps/a.ocworld", content, user) == Path.Combine(content, "Maps", "a.ocworld"));
        T.Check("user://", Paths.Globalize("user://save/1.cfg", content, user) == Path.Combine(user, "save", "1.cfg"));
        T.Check("any other path is unchanged", Paths.Globalize("notes/x.txt", content, user) == "notes/x.txt");
    }

    private static void ProjectSettingsParse()
    {
        var d = ProjectSettings.Parse(new[] { "OCPROJECT 1", "# a comment", "NAME My Game", "  STARTMAP Maps/A.ocworld  ", "FLAG" });
        T.Check("three keys", d.Count == 3);
        T.Check("a value keeps its spaces", d["name"] == "My Game");
        T.Check("trimmed", d["STARTMAP"] == "Maps/A.ocworld");
        T.Check("a bare key is empty", d["Flag"] == "");
    }

    private static void ConfigRoundTrip()
    {
        var c = new ConfigFile();
        c.SetValue("player", "name", "Ada \"the\" one\nline two");
        c.SetValue("player", "level", 7);
        c.SetValue("player", "speed", 2.5f);
        c.SetValue("player", "whole", 3.0);
        c.SetValue("player", "alive", true);
        c.SetValue("world", "spawn", new Vec3(1.5f, -2f, 0.25f));
        var d = new ConfigFile();
        T.Check("the text parses", d.Parse(c.ToText()));
        T.Check("string with quotes and a newline", d.GetValue("player", "name") as string == "Ada \"the\" one\nline two");
        T.Check("int as long", d.GetValue("player", "level") is long l && l == 7 && d.GetValue("player", "level", 0) == 7);
        T.Check("float as double", d.GetValue("player", "speed") is double s && s == 2.5);
        T.Check("a whole double stays a double", d.GetValue("player", "whole") is double w && w == 3.0);
        T.Check("bool", d.GetValue("player", "alive", false));
        T.Check("Vec3", d.GetValue("world", "spawn") is Vec3 v && v.X == 1.5f && v.Y == -2f && v.Z == 0.25f);
        T.Check("a missing key gives the fallback", d.GetValue("player", "none", 42) == 42);
        T.Check("INI-style layout", c.ToText().StartsWith("[player]\n\nname=\"") && c.ToText().Contains("\nlevel=7\n"));
    }

    private static void ConfigEdits()
    {
        var c = new ConfigFile();
        c.SetValue("a", "x", 1);
        c.SetValue("a", "y", 2);
        c.SetValue("b", "z", 3);
        T.Check("sections in order", c.GetSections().SequenceEqual(new[] { "a", "b" }));
        T.Check("keys in order", c.GetSectionKeys("a").SequenceEqual(new[] { "x", "y" }));
        c.SetValue("a", "x", null);
        T.Check("a null value erases the key", !c.HasSectionKey("a", "x") && c.HasSectionKey("a", "y"));
        c.EraseSection("b");
        T.Check("a section can be erased", !c.HasSection("b"));
        T.Check("bad text is refused", !c.Parse("[a]\nbroken line\n"));
        T.Check("and leaves the contents alone", c.HasSectionKey("a", "y"));
    }
}
