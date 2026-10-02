// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// averdesign — reads an actor script with Roslyn and prints what it declares as JSON.
// Output mirrors, field for field, the built-in scanner in modules/formats; that scanner is the
// specification. `averdesign <file.cs>` prints JSON; `averdesign --probe` prints its version.
using System.Globalization;
using System.Reflection;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace Aver.Design;

// The command line entry point and the whole of the reader.
internal static class Program
{
    private const string OpenMarkerPrefix = "<aver-generated region=\"models\" schema=\"";
    private const string CloseMarker = "</aver-generated>";

    private const int KnownSchema = 1;

    // Parses the file named by args, or answers --probe. Exit codes: 0 ok, 2 usage, 3 unreadable.
    private static int Main(string[] args)
    {
        if (args.Length == 1 && args[0] == "--probe")
        {
            // The leading "averdesign <schema>" is the CONTRACT: AverDesign.cpp accepts the tool on
            // `rfind("averdesign", 0) == 0` and reads nothing further, so the Roslyn version rides
            // along without changing what the caller parses. It is here because a tool built against
            // one Roslyn and staged beside another is otherwise invisible, and the editor logs this
            // whole line at AverDesign.cpp:147.
            Console.Out.Write($"averdesign 1 roslyn {RoslynVersion()}\n");
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

        // Every offset that leaves this program is a UTF-8 BYTE offset, never a Roslyn char index.
        string text = DecodeUtf8(bytes, out int bomChars);
        var toByte = new ByteOffsets(text, bomChars);

        // LanguageVersion.Latest, STATED RATHER THAN INHERITED. The no-options overload this used to
        // call means LanguageVersion.Default, which is "the newest major this package happens to
        // support" -- a value that moves silently with the PackageReference and reads, in source, as
        // no decision at all. The tool's job is to read what the SDK's own csc compiles, and that is
        // C# 14 today, so the intent is written down. Nothing here inspects diagnostics: a construct
        // the parser does not know does not fail, it recovers, and the walk below then reports
        // whatever the recovered tree happens to say.
        SyntaxTree tree = CSharpSyntaxTree.ParseText(
            text, new CSharpParseOptions(LanguageVersion.Latest));
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

    // The Roslyn build actually loaded, e.g. "5.6.0-2.26263.10" for the 5.6.0 package -- Roslyn stamps
    // an internal build suffix that the package version does not carry, which is exactly why the
    // assembly is asked rather than the version being written out here as a constant. Informational
    // version where there is one, since the assembly version alone rounds every 5.6.x to the same four
    // numbers; the "+<commit>" tail is cut because it is longer than the rest of the line and
    // identifies nothing a reader here can act on.
    private static string RoslynVersion()
    {
        Assembly asm = typeof(CSharpSyntaxTree).Assembly;
        string? v = asm.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion;
        if (string.IsNullOrEmpty(v)) return asm.GetName().Version?.ToString() ?? "unknown";
        int plus = v.IndexOf('+');
        return plus < 0 ? v : v[..plus];
    }

    // Maps a UTF-16 char offset in the decoded text to a UTF-8 byte offset in the file.
    private sealed class ByteOffsets
    {
        private readonly int[] prefix_;   // prefix_[i] = UTF-8 bytes in text[0..i)
        private readonly int bomBytes_;

        // Builds the prefix table for one decoded file.
        public ByteOffsets(string text, int bomChars)
        {
            bomBytes_ = bomChars > 0 ? 3 : 0;
            prefix_ = new int[text.Length + 1];
            int n = 0;
            for (int i = 0; i < text.Length; ++i)
            {
                prefix_[i] = n;
                char c = text[i];
                if (char.IsHighSurrogate(c) && i + 1 < text.Length && char.IsLowSurrogate(text[i + 1]))
                {
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

        // Byte offset for a char offset, clamped to the file.
        public int Of(int charOffset)
        {
            if (charOffset <= 0) return bomBytes_;
            if (charOffset >= prefix_.Length) return bomBytes_ + prefix_[^1];
            return bomBytes_ + prefix_[charOffset];
        }
    }

    // Decodes the file as UTF-8. bomChars is nonzero when a BOM was skipped.
    private static string DecodeUtf8(byte[] bytes, out int bomChars)
    {
        bomChars = 0;
        if (bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
        {
            bomChars = 1;
            return Encoding.UTF8.GetString(bytes, 3, bytes.Length - 3);
        }
        return Encoding.UTF8.GetString(bytes);
    }

    // Writes the generated region's status, byte bounds and every b.Place row inside it.
    private static void WriteRegionAndModels(StringBuilder sb, string text, CompilationUnitSyntax root,
                                             ByteOffsets toByte)
    {
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

        // The region body lies strictly between the marker lines, matching the C++ regionBegin/End.
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

            // The STATEMENT span, not the invocation's: the C++ rewriter replaces whole statements.
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

    // One b.Place row: what it places and where.
    private sealed class Placement
    {
        public ulong ObjectId;
        public string MeshPath = "";
        public string Material = "";
        public float[] Pos = { 0f, 0f, 0f };
        public float[] Rot = { 0f, 0f, 0f };
        public float[] Scale = { 1f, 1f, 1f };
    }

    // Reads one b.Place call, by argument name or by position, in any order. Null without id or mesh.
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
        return sawId && sawMesh ? p : null;
    }

    // The name a placement is assigned to: `Body = b.Place(...)` -> "Body". Empty when unassigned.
    private static string PropertyNameOf(InvocationExpressionSyntax inv)
    {
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

    // Reads an unsigned integer literal. False for anything that is not one.
    private static bool TryReadUlong(ExpressionSyntax e, out ulong v)
    {
        v = 0;
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

    // Reads a three-element tuple of constant numbers into `into`. Leaves it alone if it is not one.
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

    // Reads a numeric literal, with an optional unary sign. False for anything else.
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

    // Writes one JSON object per actor class in the file.
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

    // Whether a base type name ends in one of the framework actor kinds. Mirrors the C++ suffix match.
    private static bool LooksLikeActorBase(string b) =>
        b.EndsWith("Actor", StringComparison.Ordinal) ||
        b.EndsWith("Pawn", StringComparison.Ordinal) ||
        b.EndsWith("Character", StringComparison.Ordinal) ||
        b.EndsWith("PlayerController", StringComparison.Ordinal) ||
        b.EndsWith("GameMode", StringComparison.Ordinal) ||
        b.EndsWith("GameInstance", StringComparison.Ordinal);

    // What one class declares about its mesh, capsule, camera and light, and where each was written.
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

    // Fills c from b.Mesh/b.Camera/b.PointLight calls and Height/Radius/EyeHeight assignments.
    private static void ReadClassBody(ClassDeclarationSyntax cls, ClassFacts c)
    {
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

        // Height / Radius / EyeHeight, wherever in the class they are set; the last one seen wins.
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

    // Records a capsule field if `name` is one and `value` is a constant number.
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

    // Records argument i and its span when it is a constant number.
    private static void TakeArg(SeparatedSyntaxList<ArgumentSyntax> args, int i,
                                ref float? into, ref TextSpan? span)
    {
        if (i >= args.Count) return;
        ExpressionSyntax e = args[i].Expression;
        if (!TryReadFloat(e, out float f)) return;
        into = f;
        span = e.Span;
    }

    // Appends s as a quoted, escaped JSON string.
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

    // Appends a float in round-trip form, so the C++ side parses back the same bits.
    private static void WriteFloat(StringBuilder sb, float f) =>
        sb.Append(f.ToString("R", CultureInfo.InvariantCulture));

    // Appends a named three-element JSON array.
    private static void WriteVec(StringBuilder sb, string name, float[] v)
    {
        sb.Append(",\"").Append(name).Append("\":[");
        WriteFloat(sb, v[0]); sb.Append(',');
        WriteFloat(sb, v[1]); sb.Append(',');
        WriteFloat(sb, v[2]); sb.Append(']');
    }

    // Appends a named number. An absent value is omitted, never written as zero.
    private static void WriteNum(StringBuilder sb, string name, float? v)
    {
        if (v is null) return;
        sb.Append(",\"").Append(name).Append("\":");
        WriteFloat(sb, v.Value);
    }

    // Appends a named [begin, end] byte-offset pair. An absent span is omitted.
    private static void WriteSpan(StringBuilder sb, string name, TextSpan? s, ByteOffsets toByte)
    {
        if (s is null) return;
        sb.Append(",\"").Append(name).Append("\":[")
          .Append(toByte.Of(s.Value.Start)).Append(',')
          .Append(toByte.Of(s.Value.End)).Append(']');
    }
}
