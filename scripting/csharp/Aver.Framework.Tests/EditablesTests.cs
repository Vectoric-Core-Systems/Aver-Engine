// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// EditablesTests: [Editable] fields as text -- found by any case, parsed per type, clamped, and formatted back
// the way a level stores them. Reflection only; no host.

using System.Reflection;
using Aver.Framework;
using Aver.Scene;

static class EditablesTests
{
    private enum Mood { Calm, Angry }

    private class Base { [Editable] public int Lives = 3; }

    private sealed class Door : Base
    {
        [Editable(Min = 0f, Max = 10f)] public float Speed = 1f;
        [Editable] public string Label = "";
        [Editable] public bool Locked;
        [Editable] public Mood Temper;
        [Editable] public Vec3 Offset;
        [Editable] private double _secret = 0.5;
        public float NotEditable = 9f;
        internal double Secret => _secret;
    }

    public static int RunAll() => T.Suite(() =>
    {
        T.Run("editables: every [Editable] field, base class first, and none other", Listing);
        T.Run("editables: each type parses from the text a level stores", Parsing);
        T.Run("editables: numbers clamp to Min/Max, bad text is refused", ClampAndRefuse);
        T.Run("editables: format is what parse reads back", RoundTrip);
    });

    private static void Listing()
    {
        string[] names = Editables.Of(typeof(Door)).Select(f => f.Name).ToArray();
        T.Check("seven fields, the base class's first", names.Length == 7 && names[0] == "Lives");
        T.Check("a field without the attribute is not listed", !names.Contains("NotEditable"));
        T.Check("found by any case", Editables.Find(typeof(Door), "speed")?.Name == "Speed");
    }

    private static void Parsing()
    {
        var d = new Door();
        bool Set(string name, string text) => Editables.TrySet(d, Editables.Find(typeof(Door), name)!, text, out _);
        T.Check("float", Set("speed", "2.5") && d.Speed == 2.5f);
        T.Check("int on the base class", Set("lives", "7") && d.Lives == 7);
        T.Check("string kept as written", Set("label", "front door") && d.Label == "front door");
        T.Check("bool from true and 1", Set("locked", "true") && d.Locked && Set("locked", "0") && !d.Locked);
        T.Check("enum by name, any case", Set("temper", "angry") && d.Temper == Mood.Angry);
        T.Check("Vec3 from spaces or commas", Set("offset", "1 2,3") && d.Offset.X == 1f && d.Offset.Y == 2f && d.Offset.Z == 3f);
        T.Check("a private field", Set("_secret", "0.25") && d.Secret == 0.25);
    }

    private static void ClampAndRefuse()
    {
        var d = new Door();
        FieldInfo speed = Editables.Find(typeof(Door), "Speed")!;
        T.Check("above Max clamps", Editables.TrySet(d, speed, "50", out _) && d.Speed == 10f);
        T.Check("below Min clamps", Editables.TrySet(d, speed, "-3", out _) && d.Speed == 0f);
        T.Check("not a number is refused, the field untouched",
                !Editables.TrySet(d, speed, "fast", out string why) && d.Speed == 0f && why.Length > 0);
        T.Check("a Vec3 needs three numbers", !Editables.TrySet(d, Editables.Find(typeof(Door), "Offset")!, "1 2", out _));
    }

    private static void RoundTrip()
    {
        var d = new Door { Speed = 0.1f, Offset = new Vec3(1.5f, -2f, 1e-3f), Temper = Mood.Angry, Locked = true };
        var e = new Door();
        foreach (FieldInfo f in Editables.Of(typeof(Door)))
            Editables.TrySet(e, f, Editables.Format(f.GetValue(d)), out _);
        T.Check("every field survives format then parse",
                e.Speed == d.Speed && e.Offset.X == 1.5f && e.Offset.Z == 1e-3f && e.Temper == Mood.Angry && e.Locked);
    }
}
