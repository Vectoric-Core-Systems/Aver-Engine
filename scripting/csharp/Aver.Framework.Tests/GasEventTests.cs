// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The bus events of docs/GAMEPLAY_ABILITIES.md: names, sender and target, argument order, listener gating.

using Aver.Framework;

static class GasEventTests
{
    private const string Defs =
        "EFFECT Fx.Dmg\n  POLICY instant\n  MOD Health add -10\n" +
        "EFFECT Fx.Buff\n  POLICY duration 5\n  GRANTS Tag.EvBuff\n  CUE Cue.Test\n  MOD Speed add 1\n" +
        "EFFECT Fx.Tick\n  POLICY duration 3\n  PERIOD 1\n  CUE Cue.Tick\n  MOD Speed add -1\n" +
        "ABILITY Ab.Ev\n  TAGS Ability.Ev\n" +
        "ABILITY Ab.Needy\n  REQUIRES Need.Tag\n";

    public static int RunAll() => T.Suite(() =>
    {
        T.Run("events: AttributeChanged carries id, new and old, from the instigator to the owner", AttributeChanged);
        T.Run("events: the specific name is raised only when something listens", ListenerGating);
        T.Run("events: TagChanged carries id, presence and count", TagChanged);
        T.Run("events: EffectApplied and EffectRemoved carry effect, handle and stacks", EffectEvents);
        T.Run("events: Cue phases 1 added, 2 executed, 0 removed", Cues);
        T.Run("events: ability activated, ended and failed", AbilityEvents);
        T.Run("events: GameplayEvent carries tag, magnitude and target", GameplayEvents);
        T.Run("events: a restore raises nothing", RestoreIsSilent);
        T.Run("events: C# events mirror the bus", CSharpEvents);
    });

    private static AbilitySystem Listening()
    {
        AbilitySystem s = T.Hero(Defs);
        T.Host.ListenToAll = true;
        T.Host.Events.Clear();
        return s;
    }

    private static void AttributeChanged()
    {
        AbilitySystem s = Listening();
        s.Apply(EffectSpec.Of("Fx.Dmg").From(T.E(7)));
        List<RaisedEvent> all = T.Host.Events.Where(e => e.Name.StartsWith("GAS.AttributeChanged")).ToList();
        T.Check("generic then specific", all.Count == 2 && all[0].Name == "GAS.AttributeChanged" && all[1].Name == "GAS.AttributeChanged.Health",
                string.Join(",", all.Select(e => e.Name)));
        if (all.Count < 2) return;
        T.Check("sender is the instigator, target the owner", all[0].Sender == 7 && all[0].Target == 1);
        T.Check("args: id, new, old", (int)all[0].Arg(0) == T.Attr("Health").Id && T.Near(all[0].Arg(1), 90f) && T.Near(all[0].Arg(2), 100f));
        T.Check("both names carry the same payload", all[1].Args.Length == 3 && T.Near(all[1].Arg(1), 90f));
    }

    private static void ListenerGating()
    {
        AbilitySystem s = T.Hero(Defs);
        T.Host.Listening.Add("GAS.AttributeChanged");
        T.Host.Events.Clear();
        s.Apply(EffectSpec.Of("Fx.Dmg"));
        T.Check("only the generic name, nobody listens for the specific one",
                T.Host.Events.Count == 1 && T.Host.Events[0].Name == "GAS.AttributeChanged");
        T.Host.Listening.Clear();
        T.Host.Listening.Add("GAS.AttributeChanged.Health");
        T.Host.Events.Clear();
        s.Apply(EffectSpec.Of("Fx.Dmg"));
        T.Check("only the specific name", T.Host.Events.Count == 1 && T.Host.Events[0].Name == "GAS.AttributeChanged.Health");
        T.Host.Listening.Clear();
        T.Host.Events.Clear();
        s.Apply(EffectSpec.Of("Fx.Dmg"));
        T.Check("no listeners: nothing raised", T.Host.Events.Count == 0);
    }

    private static void TagChanged()
    {
        AbilitySystem s = Listening();
        GameplayTag tag = T.Tag("State.EvTag");
        s.AddLooseTag(tag);
        RaisedEvent? on = T.Host.Named("GAS.TagChanged.State.EvTag").FirstOrDefault();
        T.Check("raised under the tag's name", on != null);
        if (on != null)
        {
            T.Check("args: id, present, count", (int)on.Arg(0) == tag.Id && (int)on.Arg(1) == 1 && (int)on.Arg(2) == 1);
            T.Check("target is the owner", on.Target == 1);
        }
        T.Check("the ancestor flipped too", T.Host.Named("GAS.TagChanged.State").Any());
        T.Host.Events.Clear();
        s.AddLooseTag(tag);
        T.Check("a second reference raises nothing", T.Host.Events.Count == 0);
        s.RemoveLooseTag(tag, 2);
        RaisedEvent? off = T.Host.Named("GAS.TagChanged.State.EvTag").FirstOrDefault();
        T.Check("removal: present 0, count 0", off != null && (int)off.Arg(1) == 0 && (int)off.Arg(2) == 0);
    }

    private static void EffectEvents()
    {
        AbilitySystem s = Listening();
        EffectDef def = GameplayDefs.FindEffect("Fx.Buff")!;
        ActiveEffectHandle h = s.Apply(EffectSpec.Of("Fx.Buff").From(T.E(3)));
        RaisedEvent? applied = T.Host.Named("GAS.EffectApplied.Fx.Buff").FirstOrDefault();
        T.Check("applied, with a generic twin", applied != null && T.Host.Named("GAS.EffectApplied").Count() == 1);
        if (applied != null)
        {
            T.Check("args: effect id, handle, stacks", (int)applied.Arg(0) == def.Id && (int)applied.Arg(1) == h.Value && (int)applied.Arg(2) == 1);
            T.Check("sender is the source", applied.Sender == 3 && applied.Target == 1);
        }
        s.Remove(h);
        RaisedEvent? removed = T.Host.Named("GAS.EffectRemoved.Fx.Buff").FirstOrDefault();
        T.Check("removed with the same handle", removed != null && (int)removed.Arg(1) == h.Value);
    }

    private static void Cues()
    {
        AbilitySystem s = Listening();
        T.Tag("Cue.Test");
        ActiveEffectHandle h = s.Apply(EffectSpec.Of("Fx.Buff"));
        RaisedEvent? added = T.Host.Named("GAS.Cue.Cue.Test").FirstOrDefault();
        T.Check("phase 1 on add", added != null && (int)added.Arg(0) == T.Tag("Cue.Test").Id && (int)added.Arg(1) == 1);
        s.Remove(h);
        RaisedEvent? gone = T.Host.Named("GAS.Cue.Cue.Test").LastOrDefault();
        T.Check("phase 0 on removal", gone != null && gone != added && (int)gone.Arg(1) == 0);

        s.Apply(EffectSpec.Of("Fx.Tick"));
        T.At(101.0);
        RaisedEvent? ticked = T.Host.Named("GAS.Cue.Cue.Tick").LastOrDefault();
        T.Check("phase 2 on a periodic execution", ticked != null && (int)ticked.Arg(1) == 2);
    }

    private static void AbilityEvents()
    {
        AbilitySystem s = Listening();
        AbilityDef def = GameplayDefs.FindAbility("Ab.Ev")!;
        AbilityHandle h = s.Grant("Ab.Ev", level: 3);
        s.TryActivate(h);
        RaisedEvent? act = T.Host.Named("GAS.AbilityActivated.Ab.Ev").FirstOrDefault();
        T.Check("activated: id, level", act != null && (int)act.Arg(0) == def.Id && (int)act.Arg(1) == 3);
        T.Check("sender and target are the owner", act != null && act.Sender == 1 && act.Target == 1);
        s.End(h, cancelled: true);
        RaisedEvent? end = T.Host.Named("GAS.AbilityEnded.Ab.Ev").FirstOrDefault();
        T.Check("ended: id, cancelled", end != null && (int)end.Arg(0) == def.Id && (int)end.Arg(1) == 1);

        AbilityHandle needy = s.Grant("Ab.Needy");
        s.TryActivate(needy);
        RaisedEvent? failed = T.Host.Named("GAS.AbilityFailed.Ab.Needy").FirstOrDefault();
        T.Check("failed: id, reason", failed != null && (int)failed.Arg(1) == (int)AbilityFailure.MissingRequiredTag);
        T.Check("a generic twin exists", T.Host.Named("GAS.AbilityFailed").Count() == 1);
    }

    private static void GameplayEvents()
    {
        AbilitySystem s = Listening();
        GameplayTag tag = T.Tag("Event.Ev");
        s.SendGameplayEvent(tag, T.E(5), 2.5f, T.E(6));
        RaisedEvent? ev = T.Host.Named("GAS.GameplayEvent.Event.Ev").FirstOrDefault();
        T.Check("raised", ev != null);
        if (ev == null) return;
        T.Check("sender is the instigator, target the owner", ev.Sender == 5 && ev.Target == 1);
        T.Check("args: id, magnitude, target entity", (int)ev.Arg(0) == tag.Id && T.Near(ev.Arg(1), 2.5f) && (int)ev.Arg(2) == 6);
    }

    private static void RestoreIsSilent()
    {
        AbilitySystem s = Listening();
        s.AddLooseTag(T.Tag("State.Quiet"));
        s.Apply(EffectSpec.Of("Fx.Buff").From(T.E(1)));
        s.Grant("Ab.Ev");
        string text = s.SaveText();
        T.Host.Events.Clear();
        bool ok = GasSave.Read(T.E(2), text);
        T.Check("restored", ok && AbilitySystem.Of(T.E(2)) != null);
        T.Check("no bus events", T.Host.Events.Count == 0, string.Join(",", T.Host.Events.Select(e => e.Name)));
        T.Check("the restored state is real", AbilitySystem.Of(T.E(2))!.HasTag(T.Tag("State.Quiet")) && AbilitySystem.Of(T.E(2))!.ActiveEffects.Count == 1);
    }

    private static void CSharpEvents()
    {
        AbilitySystem s = T.Hero(Defs);
        int attr = 0, tags = 0, applied = 0, removed = 0, activated = 0, ended = 0;
        s.AttributeChanged += _ => attr++;
        s.TagChanged += (_, _, _) => tags++;
        s.EffectApplied += _ => applied++;
        s.EffectRemoved += _ => removed++;
        s.AbilityActivated += (_, _) => activated++;
        s.AbilityEnded += (_, _) => ended++;
        ActiveEffectHandle h = s.Apply(EffectSpec.Of("Fx.Buff"));
        s.Remove(h);
        AbilityHandle a = s.Grant("Ab.Ev");
        s.TryActivate(a);
        s.End(a);
        T.Check("attribute changes (+1 and -1)", attr == 2, $"{attr}");
        T.Check("tag flips (Tag.EvBuff and its root, on and off)", tags == 4, $"{tags}");
        T.Check("effect applied and removed", applied == 1 && removed == 1);
        T.Check("ability activated and ended", activated == 1 && ended == 1);
    }
}
