// averdesign — read an actor script with a real C# parser and print what it declares as JSON.
//
//   averdesign <path-to.cs>            -> JSON on stdout, exit 0
//   averdesign --probe                 -> prints its own version, exit 0. The C++ side uses this to
//                                         decide whether a Roslyn backend is available at all.
//
// Everything it emits mirrors, field for field, what the built-in scanner in modules/formats
// produces, because the C++ side maps this straight onto the same structs. Where the two disagree
// the scanner is the specification: this exists to read the files the scanner DECLINES, not to
// reinterpret the ones it accepts.
using System.Globalization;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;   // TextSpan: char offsets, converted to bytes before they leave

namespace Aver.Design;

internal static class Program
{
    private const string OpenMarkerPrefix = "<aver-generated region=\"models\" schema=\"";
    private const string CloseMarker = "</aver-generated>";

    // The one schema this build understands. An unknown one is REPORTED rather than guessed at, for
    // the same reason the scanner reports it: a future schema may mean the same tokens differently,
    // and reading it with today's rules would silently produce wrong coordinates.
    private const int KnownSchema = 1;

    private static int Main(string[] args)
    {
        if (args.Length == 1 && args[0] == "--probe")
        {
            // Deliberately trivial and deliberately not a parse: the caller is asking "can you run at
            // all", and answering that by parsing something would conflate a missing runtime with a
            // bad file.
            Console.Out.Write("averdesign 1\n");
            return 0;
        }
        if (args.Length != 1)
        {
            Console.Error.Write("usage: averdesign <file.cs> | --probe\n");
            return 2;
        }

        byte[] bytes;
        try { bytes = File.ReadAllBytes(args[0]); }
        catch (Exception ex) { Console.Error.Write($"cannot read: {ex.Message}\n"); return 3; }

        // BYTES ARE THE UNIT, and this is the subtlest thing in the tool.
        //
        // Roslyn works in UTF-16 character offsets. The C++ side holds the file as UTF-8 bytes and
        // every span it is given is used to slice that byte array. The two agree only while the file
        // is pure ASCII; one accented letter, one em dash, one emoji in a comment above a placement
        // shifts every subsequent span and the rewriter then writes coordinates into the middle of
        // some other token. So every offset that leaves this program is converted, via the map built
        // below, and none of them is a char index.
        string text = DecodeUtf8(bytes, out int bomChars);
        var toByte = new ByteOffsets(text, bomChars);

        SyntaxTree tree = CSharpSyntaxTree.ParseText(text);
        CompilationUnitSyntax root = tree.GetCompilationUnitRoot();

        var sb = new StringBuilder(64 * 1024);
        sb.Append("{\"schemaVersion\":1,");
        WriteRegionAndModels(sb, text, root, toByte);
        sb.Append(',');
        WriteClasses(sb, root, toByte);
        sb.Append('}');
        Console.Out.Write(sb.ToString());
        return 0;
    }

    // ---------------------------------------------------------------------------------------------
    // UTF-16 char offset -> UTF-8 byte offset
    // ---------------------------------------------------------------------------------------------
    private sealed class ByteOffsets
    {
        // prefix_[i] is the number of UTF-8 bytes in text[0..i). One entry per char plus a tail, so a
        // lookup is an array index rather than a re-encode -- a file with two hundred spans would
        // otherwise re-encode its own prefix two hundred times.
        private readonly int[] prefix_;
        private readonly int bomBytes_;

        public ByteOffsets(string text, int bomChars)
        {
            bomBytes_ = bomChars > 0 ? 3 : 0;   // a UTF-8 BOM is three bytes and zero characters of content
            prefix_ = new int[text.Length + 1];
            int n = 0;
            for (int i = 0; i < text.Length; ++i)
            {
                prefix_[i] = n;
                char c = text[i];
                if (char.IsHighSurrogate(c) && i + 1 < text.Length && char.IsLowSurrogate(text[i + 1]))
                {
                    // A surrogate PAIR is four UTF-8 bytes across two chars. Credit them to the first
                    // and give the second a zero-width step, so an offset landing between them (which
                    // Roslyn never produces, but a hand-built one might) does not go backwards.
                    n += 4;
                    prefix_[i + 1] = n;
                    ++i;
                    continue;
                }
                n += c switch
                {
                    < (char)0x80   => 1,
                    < (char)0x800  => 2,
                    _              => 3,
                };
            }
            prefix_[text.Length] = n;
        }

        public int Of(int charOffset)
        {
            if (charOffset <= 0) return bomBytes_;
            if (charOffset >= prefix_.Length) return bomBytes_ + prefix_[^1];
            return bomBytes_ + prefix_[charOffset];
        }
    }

    private static string DecodeUtf8(byte[] bytes, out int bomChars)
    {
        bomChars = 0;
        if (bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
        {
            bomChars = 1;   // signals "there was a BOM", not a character count in the decoded string
            return Encoding.UTF8.GetString(bytes, 3, bytes.Length - 3);
        }
        return Encoding.UTF8.GetString(bytes);
    }

    // ---------------------------------------------------------------------------------------------
    // The generated region, and the b.Place rows inside it
    // ---------------------------------------------------------------------------------------------
    private static void WriteRegionAndModels(StringBuilder sb, string text, CompilationUnitSyntax root,
                                             ByteOffsets toByte)
    {
        // The markers are found in the TEXT, not in the syntax tree, and on purpose: they live inside
        // comment trivia, the file format defines them as literal text, and modules/formats finds
        // them the same way. Two implementations locating the same delimiter by two different rules
        // is how they come to disagree about where a region starts.
        int open = text.IndexOf(OpenMarkerPrefix, StringComparison.Ordinal);
        if (open < 0)
        {
            sb.Append("\"status\":\"NoRegion\",\"models\":[]");
            return;
        }

        int schemaStart = open + OpenMarkerPrefix.Length;
        int schemaEnd = text.IndexOf('"', schemaStart);
        if (schemaEnd < 0)
        {
            sb.Append("\"status\":\"Malformed\",\"error\":");
            WriteJsonString(sb, "the open marker's schema attribute is unterminated");
            sb.Append(",\"models\":[]");
            return;
        }
        if (!int.TryParse(text.AsSpan(schemaStart, schemaEnd - schemaStart), NumberStyles.Integer,
                          CultureInfo.InvariantCulture, out int schema) || schema != KnownSchema)
        {
            sb.Append("\"status\":\"UnknownSchema\",\"error\":");
            WriteJsonString(sb, $"region schema '{text[schemaStart..schemaEnd]}' is not schema {KnownSchema}");
            sb.Append(",\"models\":[]");
            return;
        }

        // The region body is what lies strictly BETWEEN the marker lines, matching the C++ side's
        // regionBegin/regionEnd exactly -- a rewriter that disagreed by even one line would be able
        // to write over a marker, and the markers are the one thing that must survive.
        int afterOpenLine = text.IndexOf('\n', open);
        if (afterOpenLine < 0) afterOpenLine = open;
        else ++afterOpenLine;
        int close = text.IndexOf(CloseMarker, afterOpenLine, StringComparison.Ordinal);
        if (close < 0)
        {
            sb.Append("\"status\":\"Malformed\",\"error\":");
            WriteJsonString(sb, "the region has an open marker but no </aver-generated>");
            sb.Append(",\"models\":[]");
            return;
        }
        // Back up to the start of the line the close marker sits on, so the body excludes its
        // leading comment slashes and indentation.
        int closeLineStart = text.LastIndexOf('\n', Math.Max(close - 1, 0));
        int bodyEnd = closeLineStart < afterOpenLine ? close : closeLineStart + 1;

        sb.Append("\"status\":\"Ok\",\"regionBegin\":").Append(toByte.Of(afterOpenLine));
        sb.Append(",\"regionEnd\":").Append(toByte.Of(bodyEnd));
        sb.Append(",\"models\":[");

        bool first = true;
        foreach (InvocationExpressionSyntax inv in root.DescendantNodes().OfType<InvocationExpressionSyntax>())
        {
            if (inv.SpanStart < afterOpenLine || inv.Span.End > bodyEnd) continue;
            if (inv.Expression is not MemberAccessExpressionSyntax ma) continue;
            if (ma.Name.Identifier.ValueText != "Place") continue;

            var m = ReadPlace(inv);
            if (m is null) continue;

            // The STATEMENT span, not the invocation's: the C++ rewriter replaces whole statements,
            // and `Body = b.Place(...);` has an assignment and a semicolon around the call. Walking up
            // to the enclosing statement is what makes the two agree.
            SyntaxNode stmt = inv;
            while (stmt.Parent is not null && stmt is not StatementSyntax) stmt = stmt.Parent;

            if (!first) sb.Append(',');
            first = false;
            sb.Append("{\"objectId\":\"").Append(m.ObjectId.ToString(CultureInfo.InvariantCulture)).Append('"');
            sb.Append(",\"property\":"); WriteJsonString(sb, PropertyNameOf(inv));
            sb.Append(",\"meshPath\":");  WriteJsonString(sb, m.MeshPath);
            sb.Append(",\"material\":");  WriteJsonString(sb, m.Material);
            WriteVec(sb, "pos", m.Pos);
            WriteVec(sb, "rot", m.Rot);
            WriteVec(sb, "scale", m.Scale);
            sb.Append(",\"begin\":").Append(toByte.Of(stmt.SpanStart));
            sb.Append(",\"end\":").Append(toByte.Of(stmt.Span.End));
            sb.Append('}');
        }
        sb.Append(']');
    }

    private sealed class Placement
    {
        public ulong ObjectId;
        public string MeshPath = "";
        public string Material = "";
        public float[] Pos = { 0f, 0f, 0f };
        public float[] Rot = { 0f, 0f, 0f };
        public float[] Scale = { 1f, 1f, 1f };
    }

    // The whole reason this tool exists: arguments are read BY NAME where they are named and by
    // position where they are not, in either order, with any of them omitted.
    //
    // The locked grammar the scanner implements requires one exact token sequence. Roslyn has already
    // done the work of knowing which argument is which, so honouring named arguments here is a few
    // lines rather than a second parser -- and "somebody moved `material:` after `pos:`" is the most
    // likely single reason a hand-edited region stops scanning.
    private static Placement? ReadPlace(InvocationExpressionSyntax inv)
    {
        var p = new Placement();
        var args = inv.ArgumentList.Arguments;
        bool sawId = false, sawMesh = false;
        int positional = 0;

        foreach (ArgumentSyntax a in args)
        {
            string? name = a.NameColon?.Name.Identifier.ValueText;
            if (name is null)
            {
                // Positional arguments follow Place's own signature: (objectId, meshPath, ...).
                // Anything past the second unnamed argument is not something this understands, and is
                // skipped rather than guessed at.
                name = positional switch { 0 => "objectId", 1 => "meshPath", _ => null };
                ++positional;
            }
            switch (name)
            {
                case "objectId":
                    if (TryReadUlong(a.Expression, out ulong id)) { p.ObjectId = id; sawId = true; }
                    break;
                case "meshPath":
                    if (a.Expression is LiteralExpressionSyntax ls && ls.Token.Value is string s)
                    { p.MeshPath = s; sawMesh = true; }
                    break;
                case "material":
                    if (a.Expression is LiteralExpressionSyntax ms && ms.Token.Value is string mv)
                        p.Material = mv;
                    break;
                case "pos":   TryReadVec3(a.Expression, p.Pos);   break;
                case "rot":   TryReadVec3(a.Expression, p.Rot);   break;
                case "scale": TryReadVec3(a.Expression, p.Scale); break;
            }
        }
        // An id and a mesh are what make a row a placement. Without the id there is nothing to match
        // it by on a rewrite, and a row that cannot be matched must not be reported as one that can.
        return sawId && sawMesh ? p : null;
    }

    private static string PropertyNameOf(InvocationExpressionSyntax inv)
    {
        // `Body = b.Place(...)` -> "Body". A placement not assigned to anything is legal C# and is
        // reported with an empty property rather than skipped: it still draws, and the editor names
        // it by its id.
        for (SyntaxNode? n = inv.Parent; n is not null; n = n.Parent)
        {
            if (n is AssignmentExpressionSyntax asg)
                return asg.Left is IdentifierNameSyntax idn ? idn.Identifier.ValueText
                     : asg.Left is MemberAccessExpressionSyntax mac ? mac.Name.Identifier.ValueText
                     : "";
            if (n is StatementSyntax) break;
        }
        return "";
    }

    private static bool TryReadUlong(ExpressionSyntax e, out ulong v)
    {
        v = 0;
        // `0x9E1C6A4B7F0D2233UL` arrives already converted by the lexer, which is exactly the sort of
        // thing the hand-written scanner has to do itself and can get wrong at the top of the range.
        if (e is LiteralExpressionSyntax l)
        {
            switch (l.Token.Value)
            {
                case ulong u: v = u; return true;
                case long sl when sl >= 0: v = (ulong)sl; return true;
                case int si when si >= 0: v = (ulong)si; return true;
                case uint ui: v = ui; return true;
            }
        }
        return false;
    }

    // `(0f, 0f, 45f)`, and also `(0, 0, 45)`, and also `(-120f, 80f, 20f)` where the minus is a unary
    // operator rather than part of the literal. Anything that is not three constant numbers is left
    // alone: a coordinate written as an expression is real C# and this reports the row without
    // pretending to know its value, rather than dropping the row.
    private static void TryReadVec3(ExpressionSyntax e, float[] into)
    {
        if (e is not TupleExpressionSyntax t || t.Arguments.Count != 3) return;
        var tmp = new float[3];
        for (int i = 0; i < 3; ++i)
        {
            if (!TryReadFloat(t.Arguments[i].Expression, out tmp[i])) return;
        }
        Array.Copy(tmp, into, 3);
    }

    private static bool TryReadFloat(ExpressionSyntax e, out float v)
    {
        v = 0f;
        if (e is PrefixUnaryExpressionSyntax u &&
            (u.IsKind(SyntaxKind.UnaryMinusExpression) || u.IsKind(SyntaxKind.UnaryPlusExpression)))
        {
            if (!TryReadFloat(u.Operand, out float inner)) return false;
            v = u.IsKind(SyntaxKind.UnaryMinusExpression) ? -inner : inner;
            return true;
        }
        if (e is not LiteralExpressionSyntax l) return false;
        switch (l.Token.Value)
        {
            case float f:  v = f; return true;
            case double d: v = (float)d; return true;
            case int i:    v = i; return true;
            case long g:   v = g; return true;
            case decimal m: v = (float)m; return true;
            default: return false;
        }
    }

    // ---------------------------------------------------------------------------------------------
    // What each class DECLARES about itself
    // ---------------------------------------------------------------------------------------------
    private static void WriteClasses(StringBuilder sb, CompilationUnitSyntax root, ByteOffsets toByte)
    {
        sb.Append("\"classes\":[");
        bool first = true;
        foreach (ClassDeclarationSyntax cls in root.DescendantNodes().OfType<ClassDeclarationSyntax>())
        {
            string typeName = cls.Identifier.ValueText;
            string baseType = cls.BaseList?.Types.FirstOrDefault()?.Type.ToString() ?? "";
            string className = "";
            bool marked = false;

            foreach (AttributeListSyntax al in cls.AttributeLists)
                foreach (AttributeSyntax at in al.Attributes)
                {
                    string an = at.Name.ToString();
                    if (an is not ("AverClass" or "AverClassAttribute" or "AverGameMode" or "AverGameModeAttribute"))
                        continue;
                    marked = true;
                    if (at.ArgumentList?.Arguments.Count > 0 &&
                        at.ArgumentList.Arguments[0].Expression is LiteralExpressionSyntax ls &&
                        ls.Token.Value is string nm)
                        className = nm;
                }

            // A class with no marker attribute and no framework base is not an actor. Reporting every
            // class in the file would fill the tab's picker with helpers and enums' companions.
            if (!marked && !LooksLikeActorBase(baseType)) continue;

            if (!first) sb.Append(',');
            first = false;
            sb.Append('{');
            sb.Append("\"className\":");  WriteJsonString(sb, className);
            sb.Append(",\"typeName\":");  WriteJsonString(sb, typeName);
            sb.Append(",\"baseType\":");  WriteJsonString(sb, baseType);

            var c = new ClassFacts();
            ReadClassBody(cls, c);

            sb.Append(",\"hasMesh\":").Append(c.HasMesh ? "true" : "false");
            sb.Append(",\"meshPath\":"); WriteJsonString(sb, c.MeshPath);
            sb.Append(",\"material\":");  WriteJsonString(sb, c.Material);
            WriteSpan(sb, "meshPathSpan", c.MeshPathSpan, toByte);
            WriteSpan(sb, "materialSpan", c.MaterialSpan, toByte);

            WriteNum(sb, "capsuleHeight", c.CapsuleHeight);
            WriteNum(sb, "capsuleRadius", c.CapsuleRadius);
            WriteNum(sb, "eyeHeight", c.EyeHeight);
            WriteSpan(sb, "capsuleHeightSpan", c.CapsuleHeightSpan, toByte);
            WriteSpan(sb, "capsuleRadiusSpan", c.CapsuleRadiusSpan, toByte);
            WriteSpan(sb, "eyeHeightSpan", c.EyeHeightSpan, toByte);

            sb.Append(",\"hasCamera\":").Append(c.HasCamera ? "true" : "false");
            WriteNum(sb, "cameraFovDeg", c.CameraFov);
            WriteNum(sb, "cameraNearCm", c.CameraNear);
            WriteNum(sb, "cameraFarCm", c.CameraFar);
            WriteSpan(sb, "cameraFovSpan", c.CameraFovSpan, toByte);
            WriteSpan(sb, "cameraNearSpan", c.CameraNearSpan, toByte);
            WriteSpan(sb, "cameraFarSpan", c.CameraFarSpan, toByte);

            sb.Append(",\"hasPointLight\":").Append(c.HasLight ? "true" : "false");
            WriteNum(sb, "lightIntensityLux", c.LightIntensity);
            WriteNum(sb, "lightRangeCm", c.LightRange);
            WriteSpan(sb, "lightIntensitySpan", c.LightIntensitySpan, toByte);
            WriteSpan(sb, "lightRangeSpan", c.LightRangeSpan, toByte);
            sb.Append('}');
        }
        sb.Append(']');
    }

    // Mirrors the C++ side's suffix match. A SUFFIX rather than an equality because a project's own
    // intermediate base (`SkyForgeCharacter : AverCharacter`) is still a character, and the kind is
    // what decides whether the tab shows a viewport at all.
    private static bool LooksLikeActorBase(string b) =>
        b.EndsWith("Actor", StringComparison.Ordinal) ||
        b.EndsWith("Pawn", StringComparison.Ordinal) ||
        b.EndsWith("Character", StringComparison.Ordinal) ||
        b.EndsWith("PlayerController", StringComparison.Ordinal) ||
        b.EndsWith("GameMode", StringComparison.Ordinal) ||
        b.EndsWith("GameInstance", StringComparison.Ordinal);

    private sealed class ClassFacts
    {
        public bool HasMesh;
        public string MeshPath = "", Material = "";
        public TextSpan? MeshPathSpan, MaterialSpan;
        public float? CapsuleHeight, CapsuleRadius, EyeHeight;
        public TextSpan? CapsuleHeightSpan, CapsuleRadiusSpan, EyeHeightSpan;
        public bool HasCamera;
        public float? CameraFov, CameraNear, CameraFar;
        public TextSpan? CameraFovSpan, CameraNearSpan, CameraFarSpan;
        public bool HasLight;
        public float? LightIntensity, LightRange;
        public TextSpan? LightIntensitySpan, LightRangeSpan;
    }

    private static void ReadClassBody(ClassDeclarationSyntax cls, ClassFacts c)
    {
        // b.Mesh(...) / b.Camera(...) / b.PointLight(...) anywhere in the class. Anywhere rather than
        // "inside Configure" because the C++ side reads them that way too, and because an actor is
        // free to call them from a helper -- an editor that only looked in Configure would show an
        // empty preview for a perfectly ordinary actor.
        foreach (InvocationExpressionSyntax inv in cls.DescendantNodes().OfType<InvocationExpressionSyntax>())
        {
            if (inv.Expression is not MemberAccessExpressionSyntax ma) continue;
            var args = inv.ArgumentList.Arguments;
            switch (ma.Name.Identifier.ValueText)
            {
                case "Mesh":
                    if (args.Count >= 1 && args[0].Expression is LiteralExpressionSyntax mls &&
                        mls.Token.Value is string mp)
                    {
                        c.HasMesh = true;
                        c.MeshPath = mp;
                        c.MeshPathSpan = mls.Span;
                        if (args.Count >= 2 && args[1].Expression is LiteralExpressionSyntax mats &&
                            mats.Token.Value is string mv)
                        { c.Material = mv; c.MaterialSpan = mats.Span; }
                    }
                    break;
                case "Camera":
                    c.HasCamera = true;
                    TakeArg(args, 0, ref c.CameraFov,  ref c.CameraFovSpan);
                    TakeArg(args, 1, ref c.CameraNear, ref c.CameraNearSpan);
                    TakeArg(args, 2, ref c.CameraFar,  ref c.CameraFarSpan);
                    break;
                case "PointLight":
                    c.HasLight = true;
                    TakeArg(args, 0, ref c.LightIntensity, ref c.LightIntensitySpan);
                    TakeArg(args, 1, ref c.LightRange,     ref c.LightRangeSpan);
                    break;
            }
        }

        // Height / Radius / EyeHeight, wherever in the class they are set. The framework's own
        // character template assigns them in OnBeginPlay, which is where real characters set them,
        // so a reader that only looked at field initialisers would find nothing on the actors that
        // matter most. Both forms are taken -- `public float Height = 180f;` and `Height = 180f;` --
        // and the LAST one seen wins, matching a reader that walks the file top to bottom.
        foreach (SyntaxNode n in cls.DescendantNodes())
        {
            switch (n)
            {
                case VariableDeclaratorSyntax v when v.Initializer?.Value is not null:
                    TakeNamed(v.Identifier.ValueText, v.Initializer.Value, c);
                    break;
                case AssignmentExpressionSyntax a when a.IsKind(SyntaxKind.SimpleAssignmentExpression):
                    string? nm = a.Left switch
                    {
                        IdentifierNameSyntax i => i.Identifier.ValueText,
                        MemberAccessExpressionSyntax m => m.Name.Identifier.ValueText,
                        _ => null,
                    };
                    if (nm is not null) TakeNamed(nm, a.Right, c);
                    break;
                case PropertyDeclarationSyntax pd when pd.Initializer?.Value is not null:
                    TakeNamed(pd.Identifier.ValueText, pd.Initializer.Value, c);
                    break;
            }
        }
    }

    private static void TakeNamed(string name, ExpressionSyntax value, ClassFacts c)
    {
        if (!TryReadFloat(value, out float f)) return;
        switch (name)
        {
            case "Height":    c.CapsuleHeight = f; c.CapsuleHeightSpan = value.Span; break;
            case "Radius":    c.CapsuleRadius = f; c.CapsuleRadiusSpan = value.Span; break;
            case "EyeHeight": c.EyeHeight     = f; c.EyeHeightSpan     = value.Span; break;
        }
    }

    private static void TakeArg(SeparatedSyntaxList<ArgumentSyntax> args, int i,
                                ref float? into, ref TextSpan? span)
    {
        if (i >= args.Count) return;
        ExpressionSyntax e = args[i].Expression;
        if (!TryReadFloat(e, out float f)) return;
        into = f;
        span = e.Span;
    }

    // ---------------------------------------------------------------------------------------------
    // JSON, written by hand
    // ---------------------------------------------------------------------------------------------
    // By hand rather than through System.Text.Json because the output is a handful of shapes, the
    // reader on the other side is this repo's own parser, and a serialiser would mean a DTO layer
    // whose only job is to be shaped like the C++ structs already are.
    private static void WriteJsonString(StringBuilder sb, string s)
    {
        sb.Append('"');
        foreach (char ch in s)
        {
            switch (ch)
            {
                case '"':  sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\n': sb.Append("\\n");  break;
                case '\r': sb.Append("\\r");  break;
                case '\t': sb.Append("\\t");  break;
                default:
                    if (ch < ' ') sb.Append("\\u").Append(((int)ch).ToString("x4", CultureInfo.InvariantCulture));
                    else sb.Append(ch);
                    break;
            }
        }
        sb.Append('"');
    }

    // "R" round-trips: the value the C++ side parses back is bit-for-bit the one Roslyn read, which
    // matters because these numbers are compared against what the built-in scanner produced.
    private static void WriteFloat(StringBuilder sb, float f) =>
        sb.Append(f.ToString("R", CultureInfo.InvariantCulture));

    private static void WriteVec(StringBuilder sb, string name, float[] v)
    {
        sb.Append(",\"").Append(name).Append("\":[");
        WriteFloat(sb, v[0]); sb.Append(',');
        WriteFloat(sb, v[1]); sb.Append(',');
        WriteFloat(sb, v[2]); sb.Append(']');
    }

    // An ABSENT value is omitted, not written as zero. Zero is a legal height and a legal fov, so a
    // reader could not tell "not stated" from "stated as nothing" -- and the C++ side substitutes a
    // default for the first and honours the second.
    private static void WriteNum(StringBuilder sb, string name, float? v)
    {
        if (v is null) return;
        sb.Append(",\"").Append(name).Append("\":");
        WriteFloat(sb, v.Value);
    }

    private static void WriteSpan(StringBuilder sb, string name, TextSpan? s, ByteOffsets toByte)
    {
        if (s is null) return;
        sb.Append(",\"").Append(name).Append("\":[")
          .Append(toByte.Of(s.Value.Start)).Append(',')
          .Append(toByte.Of(s.Value.End)).Append(']');
    }
}
