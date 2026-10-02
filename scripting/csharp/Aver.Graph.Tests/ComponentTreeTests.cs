// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// Tests for the COMP record -- a class graph's COMPONENT TREE. See Graph.cs's GraphComponent and
// OcGraphParser.cs's "COMP" case for what these prove parses correctly.
//
// PARSE-LEVEL ONLY, the same limitation GraphClassRecordTests.cs's header states for CLASS and for
// the same reason: turning these records into real child entities needs aver_scene_create and the
// framework's component ABI, which this bare test process has no native host for.
//
// AND DELIBERATELY NOT A SECOND COPY OF THE STRUCTURAL RULES. Duplicate ids, a parent naming nothing
// and a parent cycle are refused by modules/formats' reader (tests/formats/src/OcGraphTest.cpp's
// testComponentTree), which is the reader the editor runs when it opens a file. What this side owns,
// and therefore what this file tests, is the MEANING of the line: which keys are transform and which
// are kind-specific, what units they are in, and what an unparseable one falls back to.
using System;
using Aver.Graph;

static class ComponentTreeTests
{
    public static int RunAll()
    {
        int failures = 0;

        failures += TestComponentTreeParses();
        failures += TestTransformDefaultsAreIdentity();
        failures += TestMalformedTransformKeepsItsDefault();
        failures += TestKindSpecificAttributesStayInTheDictionary();
        failures += TestComponentWithNoKindIsRejected();
        failures += TestGraphWithNoComponentsHasAnEmptyList();

        return failures;
    }

    // The shape the record exists for: a character whose body, held weapon, muzzle point and camera
    // are four things at four transforms. Before COMP, a CLASS graph could describe exactly one of
    // them, through `mesh=` on the CLASS line.
    private static int TestComponentTreeParses()
    {
        Console.WriteLine("Test: a four-component tree parses with its parents and transforms");
        try
        {
            var text = "OCGRAPH 1\n" +
                       "CLASS AN_FPCharacter Character\n" +
                       "COMP body Mesh mesh=Content/Meshes/Body.ocmesh\n" +
                       "COMP gun Mesh parent=body mesh=Content/Meshes/Blaster.ocmesh pos=12,0,-8\n" +
                       "COMP muzzle Scene parent=gun pos=0,40,0\n" +
                       "COMP eye Camera parent=body pos=0,0,70 fov=90\n" +
                       "NODE seed OnStart\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            if (graph.Components.Count != 4)
            {
                Console.WriteLine($"  FAIL: expected 4 components, got {graph.Components.Count}");
                return 1;
            }

            GraphComponent gun = graph.Components[1];
            bool ok = graph.Components[0].Id == "body" && graph.Components[0].Kind == "Mesh" &&
                      graph.Components[0].Parent is null &&
                      gun.Parent == "body" &&
                      gun.Position[0] == 12f && gun.Position[1] == 0f && gun.Position[2] == -8f &&
                      graph.Components[2].Parent == "gun" &&
                      graph.Components[3].Kind == "Camera" && graph.Components[3].AttrFloat("fov", 0f) == 90f;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: body.Parent='{graph.Components[0].Parent}' gun.Parent='{gun.Parent}' " +
                                  $"gun.Position=({gun.Position[0]},{gun.Position[1]},{gun.Position[2]}) " +
                                  $"eye.fov={graph.Components[3].AttrFloat("fov", 0f)}");
                return 1;
            }
            Console.WriteLine("  PASS: ids, kinds, parents, a negative coordinate and a kind attribute all read");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: threw {ex}");
            return 1;
        }
    }

    // SCALE DEFAULTS TO ONE AND THE OTHER TWO TO ZERO, which is the only set of defaults that makes an
    // omitted transform mean "where the parent is". A scale defaulting to zero -- the value a plain
    // `new float[3]` gives -- would make every component that does not mention scale invisible, and
    // the symptom (an actor that spawns, logs nothing and draws nothing) is a bad one to debug.
    private static int TestTransformDefaultsAreIdentity()
    {
        Console.WriteLine("Test: a component that mentions no transform is at the parent, unrotated, at scale 1");
        try
        {
            if (!OcGraphParser.Parse("OCGRAPH 1\nCOMP c Mesh mesh=x.ocmesh\n", out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            GraphComponent c = graph.Components[0];
            bool ok = c.Position[0] == 0f && c.Position[1] == 0f && c.Position[2] == 0f &&
                      c.RotationDeg[0] == 0f && c.RotationDeg[1] == 0f && c.RotationDeg[2] == 0f &&
                      c.Scale[0] == 1f && c.Scale[1] == 1f && c.Scale[2] == 1f;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: scale=({c.Scale[0]},{c.Scale[1]},{c.Scale[2]})");
                return 1;
            }
            Console.WriteLine("  PASS: identity, with scale 1 rather than a zero-filled array");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: threw {ex}");
            return 1;
        }
    }

    // A HALF-TYPED VALUE MUST NOT COLLAPSE THE ACTOR. ParseVec3 leaves a component it cannot read at
    // whatever was already there, so `scale=2,,2` gives 2,1,2 -- not 2,0,2, which would flatten the
    // thing to nothing on one axis while looking like a successful parse.
    private static int TestMalformedTransformKeepsItsDefault()
    {
        Console.WriteLine("Test: an unreadable axis keeps its default instead of becoming zero");
        try
        {
            if (!OcGraphParser.Parse("OCGRAPH 1\nCOMP c Mesh scale=2,,2 pos=5\n", out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            GraphComponent c = graph.Components[0];
            bool ok = c.Scale[0] == 2f && c.Scale[1] == 1f && c.Scale[2] == 2f &&
                      c.Position[0] == 5f && c.Position[1] == 0f && c.Position[2] == 0f;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: scale=({c.Scale[0]},{c.Scale[1]},{c.Scale[2]}) " +
                                  $"pos=({c.Position[0]},{c.Position[1]},{c.Position[2]})");
                return 1;
            }
            Console.WriteLine("  PASS: the bad axis kept 1, and a short `pos=5` filled only x");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: threw {ex}");
            return 1;
        }
    }

    // The transform and parent keys become FIELDS; everything else stays a string in Attributes,
    // because the set of kind-specific keys is open -- a Kind added later brings its own, and neither
    // parser should need editing for that.
    private static int TestKindSpecificAttributesStayInTheDictionary()
    {
        Console.WriteLine("Test: transform keys become fields, every other key stays an attribute");
        try
        {
            if (!OcGraphParser.Parse(
                    "OCGRAPH 1\nCOMP l Light pos=0,0,200 kind=point intensity=1500 range=800 unheard=of\n",
                    out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            GraphComponent c = graph.Components[0];
            bool ok = c.Position[2] == 200f &&
                      !c.Attributes.ContainsKey("pos") && !c.Attributes.ContainsKey("parent") &&
                      c.AttrString("kind") == "point" && c.AttrFloat("intensity", 0f) == 1500f &&
                      c.AttrString("unheard") == "of" &&
                      c.AttrFloat("range", -1f) == 800f && c.AttrFloat("absent", -1f) == -1f;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: pos.z={c.Position[2]} attrs={string.Join(",", c.Attributes.Keys)}");
                return 1;
            }
            Console.WriteLine("  PASS: pos left the dictionary, an unknown key stayed in it, absent falls back");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: threw {ex}");
            return 1;
        }
    }

    private static int TestComponentWithNoKindIsRejected()
    {
        Console.WriteLine("Test: COMP without a kind is rejected");
        try
        {
            if (OcGraphParser.Parse("OCGRAPH 1\nCOMP lonely\n", out _, out var err))
            {
                Console.WriteLine("  FAIL: a COMP with no kind parsed");
                return 1;
            }
            if (err is null || !err.Contains("COMP"))
            {
                Console.WriteLine($"  FAIL: error did not name the record: '{err}'");
                return 1;
            }
            Console.WriteLine("  PASS: rejected, and the message names COMP");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: threw {ex}");
            return 1;
        }
    }

    // Every graph that predates this record, and every graph that is not a class, has no components --
    // an empty list, not a null one, so a walk over it needs no guard.
    private static int TestGraphWithNoComponentsHasAnEmptyList()
    {
        Console.WriteLine("Test: a graph with no COMP records has an empty component list");
        try
        {
            if (!OcGraphParser.Parse("OCGRAPH 1\nNODE seed OnStart\n", out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            if (graph.Components.Count != 0)
            {
                Console.WriteLine($"  FAIL: expected 0 components, got {graph.Components.Count}");
                return 1;
            }
            Console.WriteLine("  PASS: empty, not null");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: threw {ex}");
            return 1;
        }
    }
}
