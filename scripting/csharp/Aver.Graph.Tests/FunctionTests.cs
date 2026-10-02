// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// USER-DEFINED FUNCTIONS: FUNC / FUNCIN / FUNCOUT, and the FuncEntry / FuncReturn / CallFunc nodes.
//
// These tests INVOKE the compiled delegate and check real numbers wherever they can, rather than
// only asserting that compilation succeeded. That distinction matters more here than usual: a
// function is emitted into its own DynamicMethod and reached by a direct IL `Call`, so "it compiled"
// only proves the IL was accepted -- it says nothing about whether arguments arrived in the right
// order, whether outputs came back, or whether the callee ran at all.
//
// The pure-arithmetic functions below need no native surface, so they run end to end in this process.
using System;
using Aver.Graph;

// No namespace, matching every other file in this suite.
static class FunctionTests
{
    public static int RunAll()
    {
        int failures = 0;
        failures += PureFunctionComputesAndReturns();
        failures += FunctionArgumentsArriveInDeclaredOrder();
        failures += SeveralOutputsComeBack();
        failures += RecursionWorks();
        failures += RunawayRecursionIsStoppedNotFatal();
        failures += DepthUnwindsSoRepeatedCallsWork();
        failures += LinkCrossingASubgraphBoundaryIsRefused();
        failures += ImpureFunctionPulledAsDataIsRefused();
        failures += PureFunctionDeclaredWrongIsRefused();
        failures += TwoEntriesRefused();
        failures += CallToUndeclaredFunctionIsRefused();
        failures += EventGraphIsUnaffectedByAFunctionExisting();
        return failures;
    }

    // ------------------------------------------------------------------ helpers

    private static bool Build(string text, out Graph graph, out string? err)
    {
        graph = null!;
        if (!OcGraphParser.Parse(text, out graph!, out err)) return false;
        return true;
    }

    private static int Expect(string what, bool ok, string detail = "")
    {
        if (ok) { Console.WriteLine($"  PASS: {what}"); return 0; }
        Console.WriteLine($"  FAIL: {what}{(detail.Length > 0 ? " -- " + detail : "")}");
        return 1;
    }

    // Compiles a pure-dataflow graph and invokes it, returning the single OUT value.
    private static object? RunPull(string text, out string? err, params object[] args)
    {
        err = null;
        if (!OcGraphParser.Parse(text, out var graph, out err)) return null;
        var compiled = new GraphCompiler(graph).Compile(out err);
        if (compiled == null) return null;
        return compiled.DynamicInvoke(args);
    }

    // ------------------------------------------------------------------ tests

    // The smallest thing that has to work: a pure function with one input and one output, called
    // from the event graph's dataflow, producing a number this test checks.
    private static int PureFunctionComputesAndReturns()
    {
        Console.WriteLine("Test: a pure function is compiled to its own method and called for a real value");
        const string text = @"OCGRAPH 1
NAME PureFn

FUNC Double pure
FUNCIN Double x float
FUNCOUT Double result float

NODE dEntry FuncEntry func=Double
NODE dTwo ConstFloat value=2 func=Double
NODE dMul Multiply func=Double
LINK dEntry.x dMul.a
LINK dTwo.value dMul.b
NODE dRet FuncReturn func=Double
LINK dMul.result dRet.result

NODE seed ConstFloat value=21
NODE call CallFunc call=Double
LINK seed.value call.x
OUT call result
";
        object? got = RunPull(text, out var err);
        return Expect("Double(21) == 42", got is float f && Math.Abs(f - 42.0f) < 0.0001f,
                      err ?? $"got {got?.ToString() ?? "null"}");
    }

    // ARGUMENT ORDER IS THE FUNCIN ORDER. A function whose two inputs are the same type would pass
    // this test by accident if they were swapped, so the body computes a NON-COMMUTATIVE expression
    // (a - b) and the test checks the sign.
    private static int FunctionArgumentsArriveInDeclaredOrder()
    {
        Console.WriteLine("Test: arguments arrive in FUNCIN declaration order, not reversed");
        const string text = @"OCGRAPH 1
NAME ArgOrder

FUNC Diff pure
FUNCIN Diff a float
FUNCIN Diff b float
FUNCOUT Diff result float

NODE dEntry FuncEntry func=Diff
NODE dSub Subtract func=Diff
LINK dEntry.a dSub.a
LINK dEntry.b dSub.b
NODE dRet FuncReturn func=Diff
LINK dSub.result dRet.result

NODE ten ConstFloat value=10
NODE three ConstFloat value=3
NODE call CallFunc call=Diff
LINK ten.value call.a
LINK three.value call.b
OUT call result
";
        object? got = RunPull(text, out var err);
        return Expect("Diff(10, 3) == 7, not -7", got is float f && Math.Abs(f - 7.0f) < 0.0001f,
                      err ?? $"got {got?.ToString() ?? "null"}");
    }

    // Several outputs box through an object[]. Both have to come back, and to the RIGHT pins.
    private static int SeveralOutputsComeBack()
    {
        Console.WriteLine("Test: a function with two outputs returns both, to the right pins");
        const string text = @"OCGRAPH 1
NAME TwoOut

FUNC SumAndDiff pure
FUNCIN SumAndDiff a float
FUNCIN SumAndDiff b float
FUNCOUT SumAndDiff sum float
FUNCOUT SumAndDiff diff float

NODE sEntry FuncEntry func=SumAndDiff
NODE sAdd Add func=SumAndDiff
LINK sEntry.a sAdd.a
LINK sEntry.b sAdd.b
NODE sSub Subtract func=SumAndDiff
LINK sEntry.a sSub.a
LINK sEntry.b sSub.b
NODE sRet FuncReturn func=SumAndDiff
LINK sAdd.result sRet.sum
LINK sSub.result sRet.diff

NODE p ConstFloat value=10
NODE q ConstFloat value=4
NODE call CallFunc call=SumAndDiff
LINK p.value call.a
LINK q.value call.b
NODE combine Multiply
LINK call.sum combine.a
LINK call.diff combine.b
OUT combine result
";
        // sum=14, diff=6, product=84 -- a product rather than either value alone, because reading
        // ONE output correctly and the other as zero would still look plausible on its own.
        object? got = RunPull(text, out var err);
        return Expect("sum(14) * diff(6) == 84", got is float f && Math.Abs(f - 84.0f) < 0.0001f,
                      err ?? $"got {got?.ToString() ?? "null"}");
    }

    // THE CAPABILITY THAT MADE THIS DESIGN WORTH ITS COST. An inlined function cannot do this at
    // all -- the expansion would not terminate.
    //
    // IT HAS TO BE AN IMPURE FUNCTION, and finding out why was worth the test. A PURE function has no
    // exec pins, so its only way to choose between a base case and a recursive step is Select -- and
    // Select in the PULL compiler evaluates BOTH sides regardless of its condition (EmitSelect says so
    // in its own comment; it is not short-circuiting the way Branch's exec fan-out is). So a pure
    // recursive function evaluates its recursive step on every call including the base case, and never
    // terminates. Branch, on the exec chain, is the only thing in this vocabulary that genuinely does
    // not evaluate the path it did not take.
    private static int RecursionWorks()
    {
        Console.WriteLine("Test: an impure function can call ITSELF (this is what inlining could never do)");
        // Countdown(n) = n > 0 ? 1 + Countdown(n - 1) : 0  -- i.e. it returns n, the long way round,
        // so a wrong answer reads as a wrong DEPTH rather than merely "not zero".
        //
        // The recursive call sits on the exec chain, so its result lands in an exec local; the Select
        // below then READS that local rather than re-invoking anything, which is what makes evaluating
        // both of its sides harmless here and fatal in the pure version.
        const string text = @"OCGRAPH 1
NAME Recurse

FUNC Countdown
FUNCIN Countdown n float
FUNCOUT Countdown depth float

NODE cEntry FuncEntry func=Countdown
NODE cZero ConstFloat value=0 func=Countdown
NODE cOne ConstFloat value=1 func=Countdown
NODE cTest Greater func=Countdown
LINK cEntry.n cTest.a
LINK cZero.value cTest.b
NODE cBr Branch func=Countdown
LINK cEntry.then cBr.exec
LINK cTest.result cBr.cond
NODE cNext Subtract func=Countdown
LINK cEntry.n cNext.a
LINK cOne.value cNext.b
NODE cRec CallFunc call=Countdown func=Countdown
LINK cBr.true cRec.exec
LINK cNext.result cRec.n
NODE cPlus Add func=Countdown
LINK cRec.depth cPlus.a
LINK cOne.value cPlus.b
NODE cPick Select func=Countdown
LINK cTest.result cPick.cond
LINK cPlus.result cPick.ifTrue
LINK cZero.value cPick.ifFalse
NODE cRet FuncReturn func=Countdown
LINK cRec.then cRet.exec
LINK cBr.false cRet.exec
LINK cPick.result cRet.depth

NODE go OnStart
ENTRY go OnStart
NODE start ConstFloat value=5
NODE call CallFunc call=Countdown
LINK go.exec call.exec
LINK start.value call.n
OUT call depth
";
        if (!OcGraphParser.Parse(text, out var graph, out var perr))
            return Expect("recursive graph parses", false, perr ?? "");
        var compiled = new GraphCompiler(graph).CompileEntryPoint("OnStart", out var cerr);
        if (compiled == null) return Expect("recursive graph compiles", false, cerr ?? "");
        GraphCallGuard.Reset();
        object? got;
        try { got = compiled.DynamicInvoke(); }
        catch (Exception ex) { return Expect("it runs", false, (ex.InnerException ?? ex).Message); }
        return Expect("Countdown(5) == 5, by five nested self-calls",
                      got is float f && Math.Abs(f - 5.0f) < 0.0001f,
                      $"got {got?.ToString() ?? "null"}");
    }
    // A missing base case must not take the process down. This is the whole reason GraphCallGuard
    // exists -- a StackOverflowException cannot be caught, and in a running editor it would cost the
    // author their unsaved work.
    private static int RunawayRecursionIsStoppedNotFatal()
    {
        Console.WriteLine("Test: unbounded recursion throws a catchable error instead of killing the process");
        // A PURE self-call, which is the natural example precisely because -- per RecursionWorks
        // above -- a pure function can never terminate a recursion anyway: Select evaluates both
        // of its sides, so even a correctly-wired base case would not stop it. An author who
        // writes this gets an error naming the function instead of a dead process.
        const string text = @"OCGRAPH 1
NAME Runaway

FUNC Forever pure
FUNCIN Forever n float
FUNCOUT Forever out float

NODE fEntry FuncEntry func=Forever
NODE fRec CallFunc call=Forever func=Forever
LINK fEntry.n fRec.n
NODE fRet FuncReturn func=Forever
LINK fRec.out fRet.out

NODE start ConstFloat value=1
NODE call CallFunc call=Forever
LINK start.value call.n
OUT call out
";
        try
        {
            if (!OcGraphParser.Parse(text, out var graph, out var perr))
                return Expect("runaway graph parses", false, perr ?? "");
            var compiled = new GraphCompiler(graph).Compile(out var cerr);
            if (compiled == null) return Expect("runaway graph compiles", false, cerr ?? "");
            try
            {
                compiled.DynamicInvoke();
                return Expect("invoking it throws rather than returning", false, "it returned normally");
            }
            catch (Exception ex)
            {
                // DynamicInvoke wraps whatever the body threw.
                var inner = ex.InnerException ?? ex;
                bool named = inner.Message.IndexOf("Forever", StringComparison.OrdinalIgnoreCase) >= 0;
                bool explained = inner.Message.IndexOf("base case", StringComparison.OrdinalIgnoreCase) >= 0;
                return Expect("it throws a catchable error naming the function and the likely cause",
                              named && explained, inner.Message);
            }
        }
        catch (Exception ex)
        {
            return Expect("no unexpected exception escapes", false, $"{ex.GetType().Name}: {ex.Message}");
        }
    }

    // THE OTHER HALF OF THE GUARD, and the one a depth counter usually gets wrong: Exit() has to
    // actually run, or the hundred-and-twenty-first ordinary call fails for no reason.
    private static int DepthUnwindsSoRepeatedCallsWork()
    {
        Console.WriteLine("Test: the depth counter unwinds, so calling a function many times in a row still works");
        const string text = @"OCGRAPH 1
NAME Repeated

FUNC Inc pure
FUNCIN Inc x float
FUNCOUT Inc result float

NODE iEntry FuncEntry func=Inc
NODE iOne ConstFloat value=1 func=Inc
NODE iAdd Add func=Inc
LINK iEntry.x iAdd.a
LINK iOne.value iAdd.b
NODE iRet FuncReturn func=Inc
LINK iAdd.result iRet.result

NODE seed ConstFloat value=0
NODE call CallFunc call=Inc
LINK seed.value call.x
OUT call result
";
        if (!OcGraphParser.Parse(text, out var graph, out var perr))
            return Expect("graph parses", false, perr ?? "");
        var compiled = new GraphCompiler(graph).Compile(out var cerr);
        if (compiled == null) return Expect("graph compiles", false, cerr ?? "");

        GraphCallGuard.Reset();
        try
        {
            // Comfortably more than MaxDepth. If Exit() never ran, this fails partway through.
            for (int i = 0; i < GraphCallGuard.MaxDepth * 3; i++)
            {
                object? got = compiled.DynamicInvoke();
                if (got is not float f || Math.Abs(f - 1.0f) > 0.0001f)
                    return Expect($"call #{i + 1} returns 1", false, $"got {got?.ToString() ?? "null"}");
            }
        }
        catch (Exception ex)
        {
            var inner = ex.InnerException ?? ex;
            return Expect("360 sequential calls all succeed", false, inner.Message);
        }
        return Expect($"{GraphCallGuard.MaxDepth * 3} sequential calls succeed, and depth is back to {GraphCallGuard.Depth}",
                      GraphCallGuard.Depth == 0, $"depth left at {GraphCallGuard.Depth}");
    }

    // The rule that makes a function a function rather than a naming convention. Two subgraphs are
    // two METHODS; a wire between them would be a wire between two methods' locals.
    private static int LinkCrossingASubgraphBoundaryIsRefused()
    {
        Console.WriteLine("Test: a wire from inside a function to the event graph is refused, by name");
        const string text = @"OCGRAPH 1
NAME Crossing

FUNC F pure
FUNCIN F x float
FUNCOUT F result float

NODE fEntry FuncEntry func=F
NODE fRet FuncReturn func=F
LINK fEntry.x fRet.result

NODE outside ConstFloat value=1
NODE alsoOutside Add
LINK outside.value alsoOutside.a
LINK fEntry.x alsoOutside.b
OUT alsoOutside result
";
        bool parsed = OcGraphParser.Parse(text, out _, out var err);
        return Expect("refused, and the message says a wire cannot leave its subgraph",
                      !parsed && err != null &&
                      err.IndexOf("cannot leave the subgraph", StringComparison.OrdinalIgnoreCase) >= 0,
                      err ?? "it was accepted");
    }

    // An impure function has a notion of WHEN. The dataflow compiler has none, so it must refuse --
    // the same rule Spawn/CharacterMove/FireEvent/SetVar already live under.
    private static int ImpureFunctionPulledAsDataIsRefused()
    {
        Console.WriteLine("Test: calling an IMPURE function from a pure-dataflow graph is refused with a reason");
        const string text = @"OCGRAPH 1
NAME ImpurePull

FUNC Effectful
FUNCIN Effectful x float
FUNCOUT Effectful result float

NODE eEntry FuncEntry func=Effectful
NODE eRet FuncReturn func=Effectful
LINK eEntry.then eRet.exec
LINK eEntry.x eRet.result

NODE seed ConstFloat value=1
NODE call CallFunc call=Effectful
LINK seed.value call.x
OUT call result
";
        if (!OcGraphParser.Parse(text, out var graph, out var perr))
            return Expect("graph parses", false, perr ?? "");
        var compiled = new GraphCompiler(graph).Compile(out var cerr);
        return Expect("Compile() refuses it and says to declare it pure or give the graph an ENTRY",
                      compiled == null && cerr != null &&
                      cerr.IndexOf("pure", StringComparison.OrdinalIgnoreCase) >= 0,
                      cerr ?? "it compiled");
    }

    // Purity is DECLARED, so it has to be CHECKED -- including through a call, which is the case an
    // inference-based rule cannot see.
    private static int PureFunctionDeclaredWrongIsRefused()
    {
        Console.WriteLine("Test: a function declared pure whose body has control flow is refused");
        const string text = @"OCGRAPH 1
NAME LyingPure

FUNC Sneaky pure
FUNCIN Sneaky x float
FUNCOUT Sneaky result float

NODE sEntry FuncEntry func=Sneaky
NODE sPrint Print func=Sneaky
LINK sEntry.x sPrint.value
NODE sRet FuncReturn func=Sneaky
LINK sEntry.x sRet.result
";
        bool parsed = OcGraphParser.Parse(text, out _, out var err);
        return Expect("refused, naming the function and telling the author what to do",
                      !parsed && err != null &&
                      err.IndexOf("Sneaky", StringComparison.OrdinalIgnoreCase) >= 0 &&
                      err.IndexOf("pure", StringComparison.OrdinalIgnoreCase) >= 0,
                      err ?? "it was accepted");
    }

    private static int TwoEntriesRefused()
    {
        Console.WriteLine("Test: a function with two FuncEntry nodes is refused");
        const string text = @"OCGRAPH 1
NAME TwoEntry

FUNC F pure
FUNCOUT F result float

NODE a FuncEntry func=F
NODE b FuncEntry func=F
NODE z ConstFloat value=1 func=F
NODE fRet FuncReturn func=F
LINK z.value fRet.result
";
        bool parsed = OcGraphParser.Parse(text, out _, out var err);
        return Expect("refused, saying a function begins in exactly one place",
                      !parsed && err != null && err.IndexOf("FuncEntry", StringComparison.OrdinalIgnoreCase) >= 0,
                      err ?? "it was accepted");
    }

    private static int CallToUndeclaredFunctionIsRefused()
    {
        Console.WriteLine("Test: calling a function that does not exist is refused, by name");
        const string text = @"OCGRAPH 1
NAME NoSuchFn

NODE call CallFunc call=Nowhere
";
        bool parsed = OcGraphParser.Parse(text, out _, out var err);
        return Expect("refused, naming the function that is missing",
                      !parsed && err != null && err.IndexOf("Nowhere", StringComparison.OrdinalIgnoreCase) >= 0,
                      err ?? "it was accepted");
    }

    // BACKWARD COMPATIBILITY, checked the only way that means anything: a graph that declares a
    // function must still compile and run its EVENT graph exactly as it would have without one --
    // including when nothing ever calls the function. The topological pass walks every node in the
    // FILE, so a function body sitting in it is precisely the thing that could break this.
    private static int EventGraphIsUnaffectedByAFunctionExisting()
    {
        Console.WriteLine("Test: an uncalled function does not disturb the event graph around it");
        const string text = @"OCGRAPH 1
NAME Bystander

FUNC Unused pure
FUNCIN Unused x float
FUNCOUT Unused result float

NODE uEntry FuncEntry func=Unused
NODE uRet FuncReturn func=Unused
LINK uEntry.x uRet.result

NODE a ConstFloat value=6
NODE b ConstFloat value=7
NODE mul Multiply
LINK a.value mul.a
LINK b.value mul.b
OUT mul result
";
        object? got = RunPull(text, out var err);
        return Expect("6 * 7 == 42, with an entire unused function sitting in the same file",
                      got is float f && Math.Abs(f - 42.0f) < 0.0001f,
                      err ?? $"got {got?.ToString() ?? "null"}");
    }
}
