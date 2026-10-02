// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// Tests for the DOMAIN record -- which LANGUAGE a .ocgraph's nodes are written in.
//
// WHY THE RECORD EXISTS, in one sentence: HostBridge.DeclareGraphClasses and GameApp's
// discoverProjectGraphs each walk EVERY *.ocgraph under a project's content directory without being
// asked, so once material graphs share that tree there has to be something in the file that says
// "not yours". See Graph.GraphDomain and aver::fmt::OcGraphDomain (the C++ mirror, which owns the
// authoritative comment) for the full account.
//
// THE ASYMMETRY IS THE POINT AND IS TESTED DIRECTLY. Absent means Gameplay -- every graph written
// before the record exists is one, and must keep working. An UNRECOGNISED name means Unknown, NOT
// Gameplay: a file naming a domain this build has never heard of has said out loud that it is not a
// gameplay graph, and a build one version behind must skip it rather than compile it.
//
// GraphHost's own refusal is tested here too, because it is the one chokepoint every gameplay
// compile in the process funnels through (both Load(path) and LoadFromText land there).
using System;
using Aver.Graph;

static class GraphDomainTests
{
    public static int RunAll()
    {
        int failures = 0;

        failures += TestAbsentDomainIsGameplay();
        failures += TestMaterialDomainParses();
        failures += TestDomainIsCaseInsensitiveButKeptVerbatim();
        failures += TestUnknownDomainIsNotGameplay();
        failures += TestDomainWithNoNameIsRejected();
        failures += TestDuplicateDomainIsRejected();
        failures += TestGraphHostRefusesAForeignGraph();
        failures += TestGraphHostStillLoadsADomainlessGraph();

        return failures;
    }

    private static int Check(string what, bool ok, string detail)
    {
        Console.WriteLine($"Test: {what}");
        if (ok) { Console.WriteLine($"  PASS: {detail}"); return 0; }
        Console.WriteLine($"  FAIL: {detail}");
        return 1;
    }

    private static int TestAbsentDomainIsGameplay()
    {
        if (!OcGraphParser.Parse("OCGRAPH 1\nNODE k ConstFloat value=1.0\nOUT k value\n",
                                 out var g, out var err))
            return Check("a graph with no DOMAIN parses", false, $"parse error: {err}");
        return Check("a graph with no DOMAIN is a GAMEPLAY graph",
                     g.Domain == null && g.DomainKind == GraphDomain.Gameplay,
                     $"Domain='{g.Domain ?? "<null>"}' DomainKind={g.DomainKind}");
    }

    private static int TestMaterialDomainParses()
    {
        if (!OcGraphParser.Parse("OCGRAPH 1\nDOMAIN material\nNAME M_Puddle\nNODE k ConstFloat value=1.0\nOUT k value\n",
                                 out var g, out var err))
            return Check("a material graph parses", false, $"parse error: {err}");
        return Check("DOMAIN material parses and classifies",
                     g.Domain == "material" && g.DomainKind == GraphDomain.Material,
                     $"Domain='{g.Domain}' DomainKind={g.DomainKind}");
    }

    // Every other record key in this format is matched case-insensitively; the VALUE is compared the
    // same way, but the text itself is kept exactly as written -- a build must not rewrite what it
    // merely recognised.
    private static int TestDomainIsCaseInsensitiveButKeptVerbatim()
    {
        if (!OcGraphParser.Parse("OCGRAPH 1\ndomain MATERIAL\nNODE k ConstFloat value=1.0\nOUT k value\n",
                                 out var g, out var err))
            return Check("DOMAIN is matched case-insensitively", false, $"parse error: {err}");
        return Check("DOMAIN and its value are case-insensitive, and the text is kept verbatim",
                     g.DomainKind == GraphDomain.Material && g.Domain == "MATERIAL",
                     $"Domain='{g.Domain}' DomainKind={g.DomainKind}");
    }

    private static int TestUnknownDomainIsNotGameplay()
    {
        if (!OcGraphParser.Parse("OCGRAPH 1\nDOMAIN vfx\nNODE k ConstFloat value=1.0\nOUT k value\n",
                                 out var g, out var err))
            return Check("a domain from a LATER build is not a parse error", false, $"parse error: {err}");
        return Check("an UNRECOGNISED domain is Unknown, not Gameplay -- skip it, do not compile it",
                     g.DomainKind == GraphDomain.Unknown,
                     $"Domain='{g.Domain}' DomainKind={g.DomainKind}");
    }

    // Said and left blank is a DEFECT, not an absence: the two mean opposite things to DomainKind.
    private static int TestDomainWithNoNameIsRejected()
    {
        bool parsed = OcGraphParser.Parse("OCGRAPH 1\nDOMAIN\nNODE k ConstFloat value=1.0\nOUT k value\n",
                                          out _, out var err);
        return Check("a DOMAIN record with no name is REFUSED rather than read as gameplay",
                     !parsed, parsed ? "it parsed" : $"refused: {err}");
    }

    private static int TestDuplicateDomainIsRejected()
    {
        bool parsed = OcGraphParser.Parse(
            "OCGRAPH 1\nDOMAIN gameplay\nDOMAIN material\nNODE k ConstFloat value=1.0\nOUT k value\n",
            out _, out var err);
        return Check("two DOMAIN records are refused -- a graph belongs to at most one domain",
                     !parsed, parsed ? "it parsed" : $"refused: {err}");
    }

    // The chokepoint. Without this the graph would reach GraphCompiler and fail with "unknown node
    // type", which reads like a broken graph rather than a graph handed to the wrong compiler.
    private static int TestGraphHostRefusesAForeignGraph()
    {
        var host = new GraphHost();
        bool loaded = host.LoadFromText(
            "OCGRAPH 1\nDOMAIN material\nNODE k ConstFloat value=1.0\nOUT k value\n", out var err);
        return Check("GraphHost REFUSES a material graph, and says which domain it was",
                     !loaded && err != null && err.Contains("material", StringComparison.Ordinal),
                     loaded ? "it loaded" : $"refused: {err}");
    }

    // The other half of the same claim: the refusal must not have cost every existing graph its load.
    private static int TestGraphHostStillLoadsADomainlessGraph()
    {
        var host = new GraphHost();
        bool loaded = host.LoadFromText("OCGRAPH 1\nNODE k ConstFloat value=1.0\nOUT k value\n", out var err);
        return Check("and a graph with no DOMAIN at all still loads, exactly as before",
                     loaded, loaded ? "loaded" : $"refused: {err}");
    }
}
