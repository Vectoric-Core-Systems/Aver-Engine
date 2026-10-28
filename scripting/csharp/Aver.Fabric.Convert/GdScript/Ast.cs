// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// GDScript 2.0 syntax tree (G3 F4a). Overview and conventions: docs/plans/fabric/G3_SCRIPTS.md, "The GDScript front end".

using System.Reflection;

namespace Aver.Fabric.Convert.GdScript;

/// <summary>A source position: 1-based line and column (a tab is one column).</summary>
public readonly record struct Pos(int Line, int Col)
{
    public override string ToString() => $"{Line}:{Col}";
}

/// <summary>A syntax error. The parser records it and goes on with the next statement or member.</summary>
public sealed record ParseError(int Line, int Col, string Message)
{
    public override string ToString() => $"{Line}:{Col}: {Message}";
}

/// <summary>A comment kept aside by the tokenizer. <see cref="IsDoc"/> is a <c>##</c> comment; <see cref="OwnLine"/> means nothing else is on its line.</summary>
public sealed record Comment(int Line, int Col, string Text, bool IsDoc, bool OwnLine);

/// <summary>Base of every tree node. <see cref="Start"/> is the first character, <see cref="End"/> the position just after the last one.</summary>
public abstract class Node
{
    public Pos Start;
    public Pos End;

    private static readonly Dictionary<Type, FieldInfo[]> s_fields = new();

    /// <summary>Child nodes in source order (read from the node's public fields, so a new field is picked up without a visitor).</summary>
    public IEnumerable<Node> Children()
    {
        Type t = GetType();
        FieldInfo[] fields;
        lock (s_fields)
        {
            if (!s_fields.TryGetValue(t, out fields!))
            {
                fields = t.GetFields(BindingFlags.Public | BindingFlags.Instance)
                          .Where(f => typeof(Node).IsAssignableFrom(f.FieldType) || typeof(IEnumerable<Node>).IsAssignableFrom(f.FieldType))
                          .ToArray();
                s_fields[t] = fields;
            }
        }
        var list = new List<Node>();
        foreach (FieldInfo f in fields)
        {
            object? v = f.GetValue(this);
            if (v is Node n) list.Add(n);
            else if (v is IEnumerable<Node> e) list.AddRange(e.Where(x => x != null));
        }
        return list.OrderBy(n => n.Start.Line).ThenBy(n => n.Start.Col).ToList();
    }

    /// <summary>Every node below this one, depth first, parents before children.</summary>
    public IEnumerable<Node> Descendants()
    {
        foreach (Node c in Children())
        {
            yield return c;
            foreach (Node d in c.Descendants()) yield return d;
        }
    }
}

// ---- types and annotations ----

/// <summary>A type hint: <c>int</c>, <c>Node3D</c>, <c>Outer.Inner</c>, <c>Array[int]</c>, <c>Dictionary[String, int]</c>, <c>void</c>.</summary>
public sealed class TypeRef : Node
{
    public List<string> Parts = new();
    public List<TypeRef> Args = new();
    public string Name => string.Join(".", Parts);
    public bool IsVoid => Parts.Count == 1 && Parts[0] == "void";
    public override string ToString() => Args.Count == 0 ? Name : Name + "[" + string.Join(", ", Args) + "]";
}

/// <summary><c>@export</c>, <c>@export_range(0, 1)</c>, <c>@onready</c>, <c>@rpc("any_peer")</c>. <see cref="Name"/> has no <c>@</c>.</summary>
public sealed class Annotation : Node
{
    public string Name = "";
    public List<Expr> Args = new();
}

// ---- statements and members ----

/// <summary>Base of everything that can stand in a block or a class body. <see cref="Annotations"/> are the ones written before it.</summary>
public abstract class Stmt : Node
{
    public List<Annotation> Annotations = new();
    public bool HasAnnotation(string name) => Annotations.Any(a => a.Name == name);
    public Annotation? GetAnnotation(string name) => Annotations.FirstOrDefault(a => a.Name == name);
}

/// <summary>A declaration. <see cref="DocComment"/> is the <c>##</c> comment block right above it (lines joined with a newline), or null.</summary>
public abstract class Member : Stmt
{
    public string? DocComment;
}

/// <summary>A script's root class or an inner class. On the root, <c>@tool</c>, <c>@icon</c> and <c>@static_unload</c> are in <see cref="Stmt.Annotations"/>.</summary>
public sealed class ClassDecl : Member
{
    public string? Name;           // class_name, or the inner class name; null for an anonymous script
    public ExtendsClause? Extends; // null: RefCounted
    public bool IsInner;
    public List<Member> Members = new();
    public bool IsTool => HasAnnotation("tool");
    public bool IsStaticUnload => HasAnnotation("static_unload");
    public string? Icon => (GetAnnotation("icon")?.Args.FirstOrDefault() as Literal)?.Value as string;
}

/// <summary><c>extends Node3D</c>, <c>extends Outer.Inner</c>, <c>extends "res://base.gd"</c>, <c>extends "res://base.gd".Inner</c>.</summary>
public sealed class ExtendsClause : Node
{
    public string? ResPath;
    public List<string> Path = new(); // identifiers; after ResPath: the inner-class path
    public override string ToString() => (ResPath != null ? "\"" + ResPath + "\"" + (Path.Count > 0 ? "." : "") : "") + string.Join(".", Path);
}

/// <summary>An annotation line that stands on its own (<c>@export_group("Movement")</c>, <c>@warning_ignore_start(...)</c>).</summary>
public sealed class StandaloneAnnotation : Member
{
    public Annotation Annotation = null!;
}

public sealed class Param : Node
{
    public string Name = "";
    public TypeRef? Type;
    public Expr? Default;
    public bool Inferred; // name := default
    public bool IsRest;   // ...args
}

public sealed class SignalDecl : Member
{
    public string Name = "";
    public List<Param> Params = new();
}

public sealed class EnumValue : Node
{
    public string Name = "";
    public Expr? Value;
}

public sealed class EnumDecl : Member
{
    public string? Name; // null: anonymous enum
    public List<EnumValue> Values = new();
}

public sealed class ConstDecl : Member
{
    public string Name = "";
    public TypeRef? Type;
    public bool Inferred; // const X := expr
    public Expr? Value;
}

/// <summary>One accessor of a property: <c>set(v): ...</c>, <c>get: ...</c> (<see cref="Body"/>) or <c>set = fn</c> (<see cref="Delegate"/>).</summary>
public sealed class PropertyAccessor : Node
{
    public bool IsSetter;
    public string? ParamName;
    public string? Delegate;
    public Block? Body;
}

/// <summary>A class variable or a local one (a local is a <see cref="VarDecl"/> in a <see cref="Block"/>; its <see cref="Stmt.Annotations"/> are usually empty).</summary>
public sealed class VarDecl : Member
{
    public string Name = "";
    public TypeRef? Type;
    public bool Inferred; // var x := expr
    public Expr? Init;
    public bool IsStatic;
    public PropertyAccessor? Setter;
    public PropertyAccessor? Getter;
}

public sealed class FuncDecl : Member
{
    public string Name = "";
    public List<Param> Params = new();
    public TypeRef? ReturnType;
    public Block Body = null!;
    public bool IsStatic;
}

public sealed class Block : Stmt
{
    public List<Stmt> Statements = new();
}

public sealed class ExprStmt : Stmt
{
    public Expr Expr = null!;
}

public sealed class IfBranch : Node
{
    public Expr Cond = null!;
    public Block Body = null!;
}

/// <summary><see cref="Branches"/> holds the <c>if</c> and every <c>elif</c> in order; <see cref="Else"/> is the else block or null.</summary>
public sealed class IfStmt : Stmt
{
    public List<IfBranch> Branches = new();
    public Block? Else;
}

public sealed class WhileStmt : Stmt
{
    public Expr Cond = null!;
    public Block Body = null!;
}

public sealed class ForStmt : Stmt
{
    public string VarName = "";
    public TypeRef? VarType; // for i: int in ...
    public Expr Iterable = null!;
    public Block Body = null!;
}

public sealed class MatchStmt : Stmt
{
    public Expr Subject = null!;
    public List<MatchBranch> Branches = new();
}

public sealed class MatchBranch : Node
{
    public List<Pattern> Patterns = new(); // "a, b:" has two
    public Expr? Guard;                    // when ...
    public Block Body = null!;
}

public sealed class BreakStmt : Stmt { }
public sealed class ContinueStmt : Stmt { }
public sealed class PassStmt : Stmt { }
public sealed class BreakpointStmt : Stmt { }

public sealed class ReturnStmt : Stmt
{
    public Expr? Value;
}

public sealed class AssertStmt : Stmt
{
    public Expr Cond = null!;
    public Expr? Message;
}

/// <summary>Stands where a statement or member failed to parse; the error is in the result's error list.</summary>
public sealed class ErrorStmt : Member { }

// ---- match patterns ----

public abstract class Pattern : Node { }

/// <summary>A literal or a constant expression: <c>1</c>, <c>"a"</c>, <c>-1</c>, <c>Dir.UP</c>, <c>SOME_CONST</c>.</summary>
public sealed class PatValue : Pattern { public Expr Expr = null!; }
/// <summary><c>var name</c></summary>
public sealed class PatBind : Pattern { public string Name = ""; }
/// <summary><c>_</c></summary>
public sealed class PatWildcard : Pattern { }
/// <summary><c>..</c> inside an array or dictionary pattern</summary>
public sealed class PatRest : Pattern { }
public sealed class PatArray : Pattern
{
    public List<Pattern> Items = new(); // may contain a PatRest
    public bool HasRest => Items.Any(i => i is PatRest);
}
public sealed class PatDictEntry : Node
{
    public Expr Key = null!;
    public Pattern? Value; // null: the key alone
}
public sealed class PatDict : Pattern
{
    public List<PatDictEntry> Entries = new();
    public bool HasRest;
}

// ---- expressions ----

public abstract class Expr : Node
{
    /// <summary>Written inside parentheses. <see cref="Node.Start"/> and <see cref="Node.End"/> include them.</summary>
    public bool Parenthesized;
}

public enum LitKind { Int, Float, String, StringName, NodePath, Bool, Null }

public sealed class Literal : Expr
{
    public LitKind Kind;
    public object? Value; // long, double, string, bool or null
    public string Text = ""; // source text
    public bool Raw;         // r"..."
    public bool Triple;      // """..."""
}

public sealed class Identifier : Expr { public string Name = ""; }
public sealed class SelfExpr : Expr { }

/// <summary><c>super</c>: the callee of <c>super(args)</c> or the target of <c>super.method(args)</c>.</summary>
public sealed class SuperExpr : Expr { }

/// <summary>Op is one of <c>-</c> <c>+</c> <c>~</c> <c>not</c> (<c>!</c> is normalised to <c>not</c>).</summary>
public sealed class Unary : Expr
{
    public string Op = "";
    public Expr Operand = null!;
}

public sealed class AwaitExpr : Expr { public Expr Operand = null!; }

/// <summary>Op is the source operator, except that <c>&amp;&amp;</c> is <c>and</c>, <c>||</c> is <c>or</c>, and <c>x not in y</c> is <c>not in</c>.</summary>
public sealed class Binary : Expr
{
    public string Op = "";
    public Expr Left = null!;
    public Expr Right = null!;
}

public sealed class Ternary : Expr
{
    public Expr Then = null!; // Then if Cond else Else
    public Expr Cond = null!;
    public Expr Else = null!;
}

/// <summary>Op is <c>=</c> or a compound operator such as <c>+=</c> or <c>**=</c>. Assignment is an expression node that only a statement (or a lambda body) can hold.</summary>
public sealed class Assign : Expr
{
    public string Op = "=";
    public Expr Target = null!;
    public Expr Value = null!;
}

public sealed class Call : Expr
{
    public Expr Callee = null!;
    public List<Expr> Args = new();
}

public sealed class MemberAccess : Expr
{
    public Expr Target = null!;
    public string Name = "";
}

public sealed class Subscript : Expr
{
    public Expr Target = null!;
    public Expr Index = null!;
}

/// <summary><c>$Player/Camera</c>, <c>$"../Sibling"</c>, <c>%Unique</c> and <c>$%Unique</c>. <see cref="Path"/> keeps a <c>%</c> where the source had one.</summary>
public sealed class GetNodeExpr : Expr
{
    public string Path = "";
    public bool IsUnique => Path.StartsWith('%');
}

public sealed class ArrayLit : Expr { public List<Expr> Items = new(); }

public sealed class DictEntry : Node
{
    public Expr Key = null!;
    public Expr Value = null!;
}

/// <summary><see cref="LuaStyle"/> is <c>{ name = 1 }</c>: the key is then a string literal made from the identifier.</summary>
public sealed class DictLit : Expr
{
    public List<DictEntry> Entries = new();
    public bool LuaStyle;
}

/// <summary><c>func(x): return x</c> (<see cref="IsInline"/>) or a multi-line one. <see cref="Name"/> is set for <c>func name(x):</c>.</summary>
public sealed class LambdaExpr : Expr
{
    public string? Name;
    public List<Param> Params = new();
    public TypeRef? ReturnType;
    public Block Body = null!;
    public bool IsInline;
}

public sealed class PreloadExpr : Expr { public Expr Path = null!; }

/// <summary><c>x is Type</c></summary>
public sealed class TypeTest : Expr
{
    public Expr Value = null!;
    public TypeRef Type = null!;
}

/// <summary><c>x as Type</c></summary>
public sealed class Cast : Expr
{
    public Expr Value = null!;
    public TypeRef Type = null!;
}
