# G3 F3: the GodotSharp script converter (`Aver.Fabric.Convert`)

`Aver.Fabric.Convert scripts` turns the C# scripts of a Godot 4.1+ project into C# that compiles against `Aver.Fabric` (G3_RUNTIME.md) and drops it into the Aver project the scene converter (`AverAssetC fabric`, G3_SCENES.md) wrote. The code is `scripting/csharp/Aver.Fabric.Convert` (console app, net10.0, Roslyn `Microsoft.CodeAnalysis.CSharp` 5.6.0 restored offline from `third_party/nuget`, staged to `bin/Tools` beside `averdesign`; CMake target `Aver.Fabric.Convert`, also a dependency of `Aver.Scripting.Host`). Tests: `ConvertScriptsTests` in `scripting/csharp/Aver.Fabric.Tests` (141 checks; ctest `FabricManagedTests`) over the hand-written GodotSharp project `tests/fabric/data/csharp/` and `tests/fabric/data/csharp_unsupported/`.

GDScript is the F4 package: its tokenizer and parser (F4a) and its type inference and emitter (F4b) are described from "The GDScript front end" on. It writes its classes to the same place (`Content/Scripts/<Class>.cs`), uses the same `FabricReport.jsonl` entries for `kind` `script`, and reuses `ReportFile`, `References` and the compile/generate/verify steps of this project: the emitter writes C# text, and that text goes through steps 2 to 5 of the pipeline below like any GodotSharp script.

## Command line

```
Aver.Fabric.Convert scripts --project <godot-dir> --out <aver-project-dir>
                            [--manifest scripts.json] [--refs <bin/Scripting>]
                            [--report <FabricReport.jsonl>] [--engine <engine-checkout>]
```

| Option | Meaning |
|---|---|
| `--project` | The Godot project folder (the one with `project.godot`). `res://x` is read from `<project>/x`. |
| `--out` | The **Aver project folder** the scene converter wrote: `<out-dir>/<Name>` of `AverAssetC fabric <godot> <out-dir>`, the folder that holds `Fabric/scripts.json` and `Content/`. |
| `--manifest` | The script list. Default `<out>/Fabric/scripts.json` (G3_SCENES.md). A bare name that is not a file is looked up under `<out>/Fabric/`. With no manifest at all the project is scanned for `.cs` files (not under `.godot`, `bin`, `obj`): class = the file's primary class, output `Content/Scripts/<Class>.cs`, duplicates numbered. |
| `--refs` | The folder with `Aver.Fabric.dll` and `Aver.Framework.dll` (default `../Scripting` beside the tool, i.e. `bin/Scripting`). Every managed `Aver.*.dll` in it is referenced except the bridge and the tools. |
| `--report` | The report to merge into. Default: `<out>/FabricReport.jsonl` if it exists, else `<out>/../FabricReport.jsonl` if it exists (where `AverAssetC fabric` puts it), else a new `<out>/FabricReport.jsonl`. `FabricReport.md` next to it gets a "Scripts" section when it exists. |
| `--engine` | The engine checkout. When `<out>/Content/Scripts/Scripts.csproj` does not exist it is written with `ProjectReference`s (relative paths) to Aver.Scripting, Aver.Framework, Aver.UI, Aver.Materials and Aver.Fabric, `net10.0`, `Nullable` and `ImplicitUsings` enabled, as the editor's own template. An existing file is never touched. |

Exit code: `0` converted and compiled with no errors; `1` compile errors remain (the files are still written, the lines marked), or the project/manifest could not be read; `2` bad usage; `3` the reference folder is unusable (the scripts are written but were not compiled; the report rates them approximated). stdout has one JSON summary line, stderr one `Content/Scripts/X.cs(line,col): error CSxxxx: message` line per compile error.

### Where Aver projects keep C# and how it is compiled

`<project>/Content/Scripts/*.cs`, built by `<project>/Content/Scripts/Scripts.csproj` (`ProjectScaffold.cpp`): Tools > Compile Scripts runs `dotnet build` on that one file into `<project>/Binaries/Scripts`; the editor creates the file for a new script when it is absent, and the projects it makes reference Aver.Fabric (G3_RUNTIME.md). The csproj turns `ImplicitUsings` and `Nullable` on, which a Godot project does not: the converter compiles with the same implicit usings (below) and writes `#nullable disable` so GodotSharp code keeps its meaning. The converted output of the fixture project was also built once with real MSBuild through a `--engine`-written `Scripts.csproj` (success; one CS0108 warning, below). That check is not in ctest.

## Pipeline

1. **Rewrite** each script syntactically (`GodotSharpRewriter`): namespaces, attributes, the registry name, a header.
2. **Compile** all scripts together in memory (Roslyn `CSharpCompilation`, `net10.0` framework from the running tool plus the `Aver.*` assemblies of `--refs`, C# latest, nullable disabled, the Godot preprocessor symbols and an implicit-usings tree) so classes see each other.
3. **Generate** from the compiler's view of the classes (`GeneratedMembers`): the name tables, events and emit helpers GodotSharp's source generators would have written, as a `partial class` placed right after each Godot-derived class.
4. **Settle ambiguities** between `System.*` (implicit usings) and `Aver.Fabric` names (below), up to four passes.
5. **Verify**: `Emit` the whole set. Every error becomes a `// FABRIC:` comment on its line, a compile error, and a report entry.

Output is deterministic for a given input and runtime: converting twice gives the same files and report, and converting an already converted file is stable (generation skips members that exist).

## Output

- `<out>/<output from scripts.json>`, normally `Content/Scripts/<ClassName>.cs`, UTF-8 without BOM, the source's line endings kept (CRLF stays CRLF). One output per input; everything in the input file (enums, structs, helper classes, nested types) goes with it.
- First lines of every output: `// FABRIC: converted from res://... by Aver.Fabric.Convert ...`, `#nullable disable`, and, only when the source has an `#if`/`#elif` that names `GODOT`, `#define GODOT`, `GODOT4`, `GODOT4_1_OR_GREATER` to `GODOT4_4_OR_GREATER`, `GODOT_PC` (what the Godot editor defines). `TOOLS` is never defined: `#if TOOLS` blocks (editor code) are dropped by the compiler, which is the intended behaviour.
- Line numbers in the report and in the compiler messages are lines of the **converted** file.
- GDScript entries of the manifest (`"kind": "gdscript"`) are converted by F4b (section "GDScript to C#"), to the same place. A manifest entry with `"exists": false` or a missing file is reported unsupported, its `type` `csharp` or `gdscript`.

## The mapping, GodotSharp to Aver.Fabric

| GodotSharp | Converted | Report |
|---|---|---|
| `using Godot;`, `using static Godot.Mathf;`, `using X = Godot.Y;`, `Godot.Vector3`, `global::Godot.X`, `typeof(Godot.Node)` | the leftmost `Godot` becomes `Aver.Fabric` (`global::Aver.Fabric` after `global::`) | counted in the script's summary |
| `Godot.Collections.Array`, `Array<T>`, `Dictionary`, `Dictionary<K,V>` | `Aver.Fabric.Collections.*` (F2a: reference types over Variants, typed views) | |
| `[Export]`, `[Export(PropertyHint.X, "hint")]`, `[ExportGroup]`, `[ExportSubgroup]`, `[ExportCategory]` | **kept as written**: Aver.Fabric has the same attributes (`Core/Enums.cs`, `PropertyHint` with Godot's numbers). The attribute is what Fabric reads; the **value** reaches the member through the level's `COMPONENT Props` record (G3_RUNTIME 1.2), which is the G1 per-entity props store `[Editable]` also reads. The converter does not add `[Editable]`: Fabric applies the Props itself (`Node.ApplyLevelValues` marks the Framework's `[Editable]` pass done), and `[Editable]` cannot hold Godot types (Vector3, NodePath, resources). | counted |
| `[Signal] public delegate void HitEventHandler(int damage);` | kept, plus generated `public event HitEventHandler Hit { add => ConnectEvent(SignalName.Hit, value); remove => DisconnectEvent(SignalName.Hit, value); }` and `protected void EmitSignalHit(int damage) => EmitSignal(SignalName.Hit, damage);` | counted |
| `SignalName.X`, `MethodName.X`, `PropertyName.X` (nested classes the generators write) | generated `public new class SignalName : <Base>.SignalName` (and `MethodName`, `PropertyName`) with `static readonly StringName` per signal / method / instance field and property, `new` where an ancestor's table has the name. `<Base>` is the compiler's base type, so a script deriving from another script chains. A table the script wrote by hand is left alone. Static, const, indexer and explicit interface members, and generic classes, are not listed. `GodotObject.MethodName` / `PropertyName` (and `Node`, `Node3D` property names) were added to Aver.Fabric for this. | counted |
| `partial class` | kept; `partial` is added to a Godot-derived class that lacks it when members are generated | |
| `[Tool]` | kept (Aver.Fabric has the attribute, no effect) and marked `// FABRIC:` | **unsupported**: no editor scripts in Aver; the class runs in the game only |
| `[GlobalClass]` | kept | converted: the class is registered under its name (framework registry); there is no editor Create dialog |
| `[Icon("res://x.svg")]` | kept (`IconAttribute` added to Aver.Fabric, no effect) | approximated |
| `[Rpc(MultiplayerApi.RpcMode.AnyPeer, CallLocal = true)]` | `[Rpc("AnyPeer")]`; the other options are dropped; marked `// FABRIC:` | approximated: recorded, nothing is networked (wave 10) |
| `[ExportToolButton]`, `GodotSharp`-only attributes | left; the compiler's missing-type error is reported (below) | unsupported by name |
| a class whose name `scripts.json` changed (reserved name such as `Node`, or a duplicate) | `[global::Aver.Framework.AverClass("<scripts.json name>")]` on the class, so the scenes' placements find it | approximated |
| `Timer`, `Array`, `Dictionary<,>`, any name `System.*` and `Aver.Fabric` both have | `using Timer = Aver.Fabric.Timer;` in the file; a generic name (an alias cannot be an open generic) is qualified at the use: `global::Aver.Fabric.Collections.Dictionary<string, int>` | counted ("using alias for ...") |
| `#nullable` | `#nullable disable` at the top (GodotSharp's reference types are not annotated) | |
| `_Ready`, `_Process`, `_PhysicsProcess`, `_Input`, `_EnterTree`, `_ExitTree` ... | untouched: plain virtuals of `Aver.Fabric.Node` | |
| `GD.*`, `Input`, `InputMap`, `Tween`, `Timer`, `SceneTree`, `GetNode<T>`, `await ToSignal(...)`, `Callable.From`, `CallDeferred`, `QueueFree`, `CharacterBody3D.MoveAndSlide`, `Mathf`, `Vector3` ... | untouched: same names and semantics in Aver.Fabric (G3_RUNTIME sections 2 and 5) | |
| `<PackageReference>` in the project's `.csproj` (not `Godot*`) | not carried over: Aver restores offline | **unsupported** per package, `file` = `res://<proj>.csproj` |
| anything Aver.Fabric lacks (`Label3D`, `NavigationAgent3D`, `ApplyFloorSnap()` ...) | left as written, `// FABRIC: Aver.Fabric has no 'X' (CSxxxx: ...)` at the end of the line | **unsupported**, one entry per error with the exact member |

Not rewritten, by design: the code's behaviour. A script is a plain C# class over the Godot-shaped API; `Position` is in metres, a `Vector3` is Godot's.

## What the conversion knows about member collisions

Aver.Fabric's `Node` derives from the framework's `AverActor`, so a script member can hide an inherited one (the fixture's `Locator.Spawn` hides `AverActor.Spawn(...)`: warning CS0108, harmless there). Warnings are not reported. A hidden member that the framework calls through the base class would not be the script's; none is known today.

## Report entries

All lines are `kind` `script` (G3_SCENES.md schema, eight fields after `schema`). `file` is the `res://` script; `line` is the line of the converted file (0 for the summary).

| `type` | `node` | `property` | Meaning |
|---|---|---|---|
| `csharp` | the registry class name | `""` | The script as a whole. `converted`: compiles and nothing was dropped. `approximated`: compiles, but a note below is approximated/unsupported, or it was not compiled (exit 3). `unsupported`: compile errors remain. `reason` lists what was rewritten and the verdict. One per script. |
| `attribute` | class | `Tool`, `GlobalClass`, `Icon`, `Rpc` | See the table above. |
| `class` | class | the declared name | Registered under another name. |
| `CSxxxx` (the compiler's id) | enclosing type | the exact name the compiler could not find (the source text at the error) | A compile error, `unsupported`. `reason` = `Content/Scripts/X.cs(line,col): message`. |
| `PackageReference` | `""` | the package id | A NuGet dependency of the Godot project, `file` = the `.csproj`. |

Merging: for every script path this run handles (including missing ones and the `.csproj`), the existing `kind` `script` lines with that `file` are removed first (the scene converter's "converted by a later package" placeholder, or an earlier run), then this run's lines are appended; everything else in the file is kept in order. A rerun therefore reproduces the same file. `FabricReport.md` gets (or has replaced) the section between `<!-- fabric-scripts-csharp:begin -->` and `<!-- fabric-scripts-csharp:end -->`; its counts table at the top is the scene converter's and is not recomputed. (Run through `AverAssetC fabric`, F6 reads the merged jsonl back and rewrites the whole `FabricReport.md` from it, so that section and the counts are replaced by one merged report; the tool's own md merge matters only when it is run alone.)

## Verified, and limits

Run headless in `Aver.Fabric.Tests` (the managed node host, no engine): the fixture project (seven scripts: a `CharacterBody3D` controller with `Input`/`MoveAndSlide`, a node with `[Export]` fields and three `[Signal]`s connected and emitted, an `await` on a `SceneTreeTimer` with a `Timer` node, `Tween`s, `GetNode` path forms, attributes, helpers/enums/structs/generic classes) converts with **zero compile errors**, the compiled assembly is loaded and the scripts run: signals reach `+=` handlers, `Connect` and `EmitSignalX`; the timer await resumes; the tween lands; paths resolve; gravity applies in `_PhysicsProcess`. Also tested: every rewrite above on small sources, CRLF, `#if GODOT`, idempotence, the report merge, exit codes, `--engine`.

Not verified or not done:
- Nothing ran in the engine (engine-run ban): how the bridge declares the converted classes (`[AverClass]` renames, `Parent` default) and how a level's Props reach their `[Export]` members is G3_RUNTIME's contract and F2's tests.
- The editor's per-instance details panel lists `[Editable]` fields only, so Fabric `[Export]` members are not shown there (values still arrive from the level).
- Coverage is what Aver.Fabric has: the missing classes and members (G3_RUNTIME 1.4 "not yet": joints, AnimationPlayer, audio, navigation, particles, UI controls beyond placeholders, `ApplyFloorSnap`, ...) surface as reported compile errors, which is the intended signal for the runtime work to prioritise.
- Semantic differences are not detected: only what fails to compile is. For example a Godot `[Export] Node3D[]` (arrays of nodes) compiles but the runtime does not map it; `Engine.IsEditorHint()`-style code is only checked for existence.
- A class name in another namespace that `scripts.json` renamed is only renamed through the attribute; other files that name the C# type still use the declared name (which is right, the attribute changes the registry name only).
- Partial classes split over several files get their generated members in the first declaration's file.
- GodotSharp's `GodotObject.Get/Set/Call` property tables for scripts (`_Get`, `_Set`, `_GetPropertyList`) are Aver.Fabric's reflection by name, not generated.
- Compilation uses the running .NET's implementation assemblies as references, not the reference pack: an API that exists only in the implementation would compile here and not in MSBuild (none seen).
- The projects' `.csproj` settings (`AllowUnsafeBlocks`, other `LangVersion`, `DefineConstants`) are not read.

# The GDScript front end (F4a)

`scripting/csharp/Aver.Fabric.Convert/GdScript/` turns GDScript 2.0 source into a typed syntax tree. It is the first half of the GDScript converter (F4a); the type inference and the C# emitter (F4b, "GDScript to C#" below) are built on the tree described here.

```csharp
using Aver.Fabric.Convert.GdScript;
ParseResult r = GdScriptParser.Parse(File.ReadAllText(path));   // never throws on bad source
ClassDecl script = r.Script;                                    // the root class
foreach (ParseError e in r.Errors) Console.WriteLine($"{path}:{e}");   // "line:col: message"
Expr? e2 = GdScriptParser.ParseExpression("a + b * 2", out List<ParseError> errs);
string dump = AstDump.Dump(script);                              // one-line S-expression, see below
```

| Type | Role |
|---|---|
| `Lexer` / `Token` / `TokKind` | `Lexer.Tokenize(text)` gives `Tokens`, `Errors` and `Comments`. Kinds: `Ident`, `Keyword`, `Int`, `Float`, `String`, `Annotation`, `NodePath` (the `$`/`%` sugar), `Op`, `Newline`, `Indent`, `Dedent`, `Eof`. |
| `GdScriptParser` | `Parse(text)` -> `ParseResult { Script, Errors, Comments, Tokens, Ok }`; `ParseExpression(text, out errors)`. |
| `AstDump.Dump(node)` | The one-line form the tests assert on. |
| `Node` | Base of every tree node: `Start`, `End` (`Pos(Line, Col)`, 1-based, a tab is one column, `End` is just past the last character), `Children()` (source order, read from the node's public fields) and `Descendants()`. |

## Tokenizer

- **Indentation** is blocks of `Indent`/`Dedent` tokens, as in Godot. A tab counts as 4 columns when levels are compared; a file that indents with one character and then the other is reported ("Mixed use of tabs and spaces"). Blank and comment-only lines carry no tokens. A line that dedents to a column that is not on the stack is reported and read as the nearest level. CRLF, a BOM and a missing final newline are accepted; the tree and positions are the same as for LF.
- **Line joining**: `\` before the newline, and any newline inside `()`, `[]`, `{}`. A newline that ends a statement is a `Newline` token; there is one per logical line and never two in a row.
- **Multi-line lambdas inside brackets** (`foo(func(x):` then an indented body, the `)` on its own line or at the end of the last body line) get their own indentation, the way Godot's tokenizer does it: after a `func(...) [-> T]:` met inside brackets with nothing but a comment left on the line, the lexer turns indentation back on until the body dedents or its bracket closes. The body then reads `Newline Indent ... Newline Dedent`. A lambda at statement level (`var f = func(x):` then a body) is plain indentation; the statement has no `Newline` after the closing `Dedent`, which the parser accepts.
- **Numbers**: decimal with `_`, `0x` hex, `0b` binary, floats with exponent, `.5` and `1.`. Integers are 64-bit (`0xFFFFFFFFFFFFFFFF` is -1, as in Godot); one too large for 64 bits is reported. `1.foo` and `1..2` keep the dot as a token.
- **Strings**: `""`, `''`, `""" """`, `''' '''` (multi-line, line numbers keep counting), the prefixes `r` (raw: backslashes kept, `\"` does not end the string), `&` (StringName) and `^` (NodePath). Escapes `\n \t \r \a \b \f \v \\ \" \' \uXXXX \UXXXXXX` and backslash-newline; any other escape is reported. The token holds the decoded value and the source text.
- **Node sugar**: `$Player/Camera`, `$"../Other Node"`, `$%Unique`, `$A/%B`, `%Unique`, `%"Quoted Name"` are one `NodePath` token whose text is the path, with `%` kept where the source had one. `%` is the unique-node sugar only where a value cannot precede it (after `=`, `(`, `,`, an operator or at the start of a line); `a % b`, `"x" % y`, `f() % 2` are the operator.
- **Annotations** are one `Annotation` token (`@export_range` -> text `export_range`). **Comments** go to `Lexer.Comments` (`Line`, `Col`, text without the marker, `IsDoc` for `##`, `OwnLine`).
- **Keywords**: `if elif else for while match break continue pass return class class_name extends is in as self signal func static const enum var breakpoint preload await assert void true false null and or not super`. `when`, `set`, `get`, `_`, `PI`, `TAU`, `INF`, `NAN` and every built-in type are identifiers.

## Tree overview

Declarations are `Member`s (they carry `Annotations`, `DocComment`); `Stmt` is the base of everything that can stand in a block, so a local `var` or `const` is the same node as a class one.

| Node | Fields |
|---|---|
| `ClassDecl` | `Name` (class_name or inner name), `Extends` (`ExtendsClause`: `ResPath`, `Path`), `IsInner`, `Members`; `IsTool`, `IsStaticUnload`, `Icon` read the root's `Annotations` (`@tool`, `@icon`, `@static_unload` stay there, also for the Godot 3 form `class_name X, "icon"`) |
| `StandaloneAnnotation` | `@export_group`, `@export_subgroup`, `@export_category`, `@warning_ignore_start`, `@warning_ignore_restore`: member of their own, in order |
| `SignalDecl` | `Name`, `Params` (`Param`: `Name`, `Type`, `Default`, `Inferred`, `IsRest`) |
| `EnumDecl` | `Name` (null: anonymous), `Values` (`Name`, `Value` expression or null) |
| `ConstDecl` | `Name`, `Type`, `Inferred` (`:=`), `Value` |
| `VarDecl` | `Name`, `Type`, `Inferred`, `Init`, `IsStatic`, `Setter`, `Getter` (`PropertyAccessor`: `IsSetter`, `ParamName`, `Body` or `Delegate` for `set = fn`) |
| `FuncDecl` | `Name`, `Params`, `ReturnType`, `Body` (`Block`), `IsStatic`; a rest parameter (`...args`) is a `Param` with `IsRest` |
| `TypeRef` | `Parts` (dotted), `Args` (`Array[int]`, `Dictionary[String, int]`), `Name`, `IsVoid` |
| `Annotation` | `Name`, `Args` (expressions); on `Stmt.Annotations` of what follows |
| `Block`, `ExprStmt`, `PassStmt`, `BreakStmt`, `ContinueStmt`, `BreakpointStmt`, `ReturnStmt` (`Value`), `AssertStmt` (`Cond`, `Message`) | |
| `IfStmt` | `Branches` (`IfBranch`: `Cond`, `Body`; the `if` and each `elif`), `Else` |
| `WhileStmt`, `ForStmt` | `ForStmt`: `VarName`, `VarType`, `Iterable`, `Body` |
| `MatchStmt` | `Subject`, `Branches` (`MatchBranch`: `Patterns` (a, b), `Guard` (`when`), `Body`) |
| Patterns | `PatValue` (a literal or constant expression), `PatBind` (`var x`), `PatWildcard` (`_`), `PatRest` (`..`), `PatArray` (`Items`, `HasRest`), `PatDict` (`Entries` of key expression and optional value pattern, `HasRest`) |
| `ErrorStmt` | placeholder where a statement or member failed to parse |
| `Literal` | `Kind` (`Int`, `Float`, `String`, `StringName`, `NodePath`, `Bool`, `Null`), `Value` (`long`, `double`, `string`, `bool`), `Text`, `Raw`, `Triple` |
| `Identifier`, `SelfExpr`, `SuperExpr` | `super` is the callee of `super(args)` or the target of `super.method(args)` |
| `Unary` | `Op` in `-` `+` `~` `not` (`!` is `not`) |
| `Binary` | `Op`: `+ - * / % ** << >> & \| ^ == != < > <= >= in`, `not in`, `and`, `or` (`&&`/`\|\|` are `and`/`or`) |
| `Ternary` | `Then if Cond else Else` |
| `Assign` | `Op` (`=`, `+=`, ..., `>>=`), `Target`, `Value`. An expression node; only a statement or an inline lambda body can hold one. |
| `Call`, `MemberAccess`, `Subscript` | `Callee`+`Args`; `Target`+`Name`; `Target`+`Index` |
| `GetNodeExpr` | `Path` (`$A/B` -> `A/B`, `%U` and `$%U` -> `%U`), `IsUnique` |
| `ArrayLit`, `DictLit` | `DictLit.Entries` (`Key`, `Value`), `LuaStyle` for `{ name = 1 }` (the key is then a string literal) |
| `LambdaExpr` | `Name` (optional), `Params`, `ReturnType`, `Body`, `IsInline` (`func(x): return x`) |
| `PreloadExpr` | `Path`; `load(...)` is an ordinary `Call` |
| `AwaitExpr`, `TypeTest` (`x is T`), `Cast` (`x as T`) | |

Every `Expr` has `Parenthesized`; its `Start`/`End` then include the parentheses. `ClassDecl.DocComment` and `Member.DocComment` hold the `##` block touching the declaration from above (lines joined with `\n`); a script's own doc comment is read from above `class_name`/`extends` or from the block right under it, unless code follows that block directly (then it belongs to the member).

### Operator precedence (lowest first)

Assignment (right-assoc, statement level only), `as`, `x if c else y` (right-assoc), `or`/`||`, `and`/`&&`, `not`/`!`, `in`/`not in`, comparisons, `|`, `^`, `&`, `<<` `>>`, `+ -`, `* / %`, unary `+ -`, `~`, `**`, `is`, `await`, call, `.`, `[]`. All binary operators are left-associative. This follows Godot's own table. Two points rest on reading Godot's source and docs from memory and are not checked against a running Godot: `as` binds looser than the ternary (`a if b else c as T` casts the whole ternary), and `**` is left-associative (`a ** b ** c` is `(a ** b) ** c`); `-a ** 2` is `-(a ** 2)` either way. F4b must not assume either until a Godot is available to compare.

### AST dump format

`AstDump.Dump(node)`: literals as written (`1`, `1.5`, `"s"`, `&"n"`, `^"p"`, `true`, `null`); `(op a b)` for `Binary`/`Assign`, `(not a)`; `(?: then cond else)`; `(call f a b)`; `(. t name)`; `([] t i)`; `($ A/B)`; `(array ...)`; `(dict ("k" v) ...)` / `(dict-lua ...)`; `(lambda name (a:int b:=2) -> T (block ...))`; `(await x)`, `(is x T)`, `(as x T)`, `(preload x)`; statements `(var x :T = init)` / `(var x := init)`, `(const ...)`, `(if (cond (block ...)) (cond ...) (else (block ...)))`, `(for i :T in e (block ...))`, `(match s (branch (p1 p2) when g (block ...)))`, `(return x)`; patterns `(var x)`, `_`, `..`, `(parr ...)`, `(pdict ("k" p) ... ..)`; a declaration is prefixed by its annotations (`@export_range(0 1) (var x :float)`).

## Errors

`ParseResult.Errors` is sorted by position and holds both tokenizer and parser errors as `Line:Col: message`. The parser reports a problem, drops the rest of the line and the indented block under it (and any `elif`/`else` that belonged to a dropped `if`), puts an `ErrorStmt` in its place and goes on with the next statement or member; an error inside a function, lambda body, inner class or match branch stays inside it. A damaged file therefore still gives the tree of everything around the damage. Limits: an unclosed bracket swallows the rest of the file into one logical line (reported at the opening bracket), and an error in a lambda header inside brackets can cascade into a second error at the closing bracket.

## Not covered by the front end

Godot 3 syntax (`setget`, `onready var`, `export(...)`, `yield`, `tool`, `.method()` super calls, `func _init(a).(a)`), `@abstract` (kept as an ordinary annotation, nothing checks it), `x is not T` (rejected), `trait`/`namespace`. A compound statement on the same line as an `if x:` (`if a: if b: c`) is rejected. The tree has no name resolution and no types beyond the written hints; the emitter (next section) adds both.

## Verified

`Aver.Fabric.Tests`, `GdScriptParserTests` (593 checks; `dotnet run -c Debug -- gdscript` in `scripting/csharp/Aver.Fabric.Tests` runs only this suite, ctest `FabricManagedTests` runs all of them): the tokenizer on small sources (indentation, continuation, numbers, strings, operators, node sugar, annotations, comments, lambda blocks), precedence and associativity on small expressions, and the hand-written fixtures in `tests/fabric/data/gdscript/` with AST assertions: `02_declarations.gd`, `03_properties.gd`, `04_functions.gd`, `05_control_flow.gd`, `06_match.gd`, `07_expressions.gd`, `08_lambdas.gd`, `09_strings.gd`, the realistic `player_controller.gd` and `ui_menu.gd`, and `10_errors.gd` for recovery (six errors at known line:column, the good members and statements around them intact). Also: CRLF, BOM, space indentation and a missing final newline give the same tree; every node's range holds its children's; 25 randomly damaged copies of each fixture (275, fixed seed) parse without an exception in good time. The fixtures are written by hand from the language reference; no Godot project was parsed.

# GDScript to C# (F4b)

`Aver.Fabric.Convert scripts` converts the `"kind": "gdscript"` entries of `Fabric/scripts.json` (or, without a manifest, the `.gd` files it finds: class name from `class_name`, else the file name in PascalCase) to readable C# against `Aver.Fabric`, written to the manifest's `output` (normally `Content/Scripts/<Class>.cs`). The code is `scripting/csharp/Aver.Fabric.Convert/GdScript/`: `ProjectModel.cs` (the symbol table), `ApiModel.cs` (Aver.Fabric as Roslyn symbols), `GdTypes.cs`, `Naming.cs`, and the emitter, a partial class `GdEmitter` in `Emitter*.cs` (class level, functions, statements, match, expressions, calls, built-ins). Entry point: `GdScriptConverter.Convert(sources, api, godotProjectDir)`; `ScriptConverter.Convert` calls it for `ScriptSource.Kind == "gdscript"` and runs the result through the same steps as GodotSharp (generated `SignalName`/`MethodName`/`PropertyName`, `EmitSignalX`, events, `using` aliases for `Timer` and friends, compile, report).

## How it works

1. **Project table first.** Every `.gd` is parsed (F4a), then one table holds all of them: `class_name` to the manifest's class name, `extends "res://x.gd"` to the converted class, inner classes, enums, signals, functions, and `project.godot`'s `[autoload]` list (read from `ConvertSettings.ProjectDir`, the `--project` folder). Overrides are found: a function the base script or the API base class has is `override`; one a descendant overrides is `virtual`.
2. **The API is the runtime.** The reference folder's `Aver.Fabric.dll` and `Aver.Framework.dll` are read as Roslyn symbols (`ApiModel`): a `snake_case` name is looked up as `PascalCase` on the receiver's C# type, return and parameter types come from the symbols, overloads are chosen by the argument types, `get_x()` / `set_x(v)` / `is_x()` become the property `X`, `Input.MOUSE_MODE_CAPTURED` is found in the nested enum `Input.MouseModeEnum`, `KEY_ESCAPE` in `Key`, `Color.RED` in `Colors`. There is no table to keep in step: a class or member the runtime gains is used on the next conversion; one it lacks is a compile error. Without a reference folder (`RefsDir == null`) the C# is written from the GDScript types alone.
3. **An expression is typed while it is written** (`Ex` carries the C# text and a `GdType`), so inference and emission cannot disagree. Sources of types: declared types, `:=`, literals, API symbols, script members, inferred function returns (all `return`s agree and the function cannot fall off the end with a value type), parameters typed by a literal default.
4. **Untyped variables** (`var x = 1`) take the type of their first value. When a later assignment contradicts it (a float to an int variable, a Variant, a string), the whole script is written again with that variable widened (int to float) or left a `Variant` (up to six rounds). A variable that only ever meets values of its kind stays typed.
5. **Fallback.** A receiver whose type is not known is a `Variant`; a member or call on it goes through `GDScript.GetMember/SetMember/CallMethod`. On a `Node` that is only a guess (`$Path` with no type) it goes through `Get/Set/Call` by name. These are `approximated` report entries (`dynamic`). A name found nowhere (a global the project does not declare, a function nobody defines) is read or called by name on `this` and reported `unsupported`.

## Types

| GDScript | C# | Note |
|---|---|---|
| `int` | `long` | GDScript's int is 64-bit. A value the API returns as `int` (`Count`, `Length`) stays `int` where it is only read and is cast where it meets a `long`; an inferred function return is `long` |
| `float` | `double` | Vector components, `Color` channels and many API parameters are `float`: the cast is written (`(float)(Velocity.Y - Gravity * delta)`), literals get an `f` |
| `bool`, `String`, `StringName`, `NodePath` | `bool`, `string`, `StringName`, `NodePath` | |
| `Array`, `Array[T]`, `Dictionary`, `Dictionary[K, V]` | `Collections.Array`, `Collections.Array<T>`, `Collections.Dictionary`, `Collections.Dictionary<K, V>` | the file has `using Collections = Aver.Fabric.Collections;` |
| `PackedStringArray`, `PackedInt32Array`, `PackedVector3Array` ... | `string[]`, `int[]`, `Vector3[]` ... | |
| `Variant`, an untyped parameter without a literal default | `Variant` | |
| `Callable`, `Signal` | `Callable`, `Signal` | |
| `Vector3`, `Color`, `Transform3D`, `Node3D`, `RayCast3D` ... | the same names | |
| a script class, an inner class, a script enum | its C# name (`Outer.Inner` when written from outside) | |

## The mapping

| GDScript | C# |
|---|---|
| `class_name X` | `[GlobalClass] public partial class X`, named as `scripts.json` says; `@icon` is `[Icon]`; `@tool` is `[Tool]` and reported `unsupported` (no editor scripts) |
| `extends Node3D`, `extends Foo` (a `class_name`), `extends "res://a.gd"`, `extends Outer.Inner`, none | that base class; the converted class; the converted class; the nested class; `RefCounted` |
| `class Inner extends Foo:` | a nested `public partial class Inner : Foo` (generated members included) |
| `var x: int = 1`, `var x := 1`, `var x = 1` | `public long X = 1;`. Names: `PascalCase`; `_private` becomes `_private` (underscore kept); a name that collides with a class, an enum or another member becomes `camelCase`, else gets a `_` |
| a field initialiser that reads other members | assigned in the constructor, in source order, before `_init`'s body |
| `static var`, `static func` | `static` |
| `const A = 1`, `const B := "x"`, `const L = [1, 2]`, `const T = preload("res://x.png")` | `public const long A = 1;`, `const string`, `public static readonly Collections.Array L = ...`, `static readonly Texture2D T = GD.Preload<Texture2D>(...)` (typed by the file extension). `const C = preload("res://c.gd")` names the class: uses write that class |
| `enum State { IDLE, RUN = 5 }` | `public enum State { Idle, Run = 5 }`; an anonymous `enum { A, B = 2 }` is `const long A = 0, B = 2`; an enum value is an int to arithmetic (`(long)State.Idle`); `State.keys()` and `State.values()` work |
| `@export`, `@export_range(a, b, s, ...)`, `_enum`, `_flags`, `_file`, `_dir`, `_multiline`, `_placeholder`, `_color_no_alpha`, `_node_path`, `_exp_easing`, `_flags_3d_physics` ... | `[Export]`, `[Export(PropertyHint.Range, "a,b,s,...")]`, `PropertyHint.Enum`, `Flags`, `File`, `Dir`, `MultilineText`, ... The value reaches the member through the level's `Props` (G3_RUNTIME 1.2), as for GodotSharp |
| `@export_group("G")`, `_subgroup`, `_category` | `[ExportGroup("G")]` ... on the next exported member |
| `@onready var n: T = $Path` | a field, assigned at the start of `_Ready` in GDScript order, before the body of `_ready` (a synthesized `_Ready` when the script has none; it calls `base._Ready()` when a base script has one) |
| `var v: T: set(x): ... get: ...`, `set = f`, `get = f` | `private T _v;` and `public T V { get {...} set {...} }`; inside the accessors (and inside the functions `set = f` names) `v` is the backing field, outside it is the property. The initial value goes to the field, not through the setter |
| `signal s(a: int, b)` | `[Signal] public delegate void SEventHandler(long a, Variant b);` and, from the F3 steps, `SignalName.S`, the event `S` and `EmitSignalS(...)` |
| `func f(a: int, b := 2, c = null) -> T` | `public T F(long a, long b = 2, Variant c = default)`; a default C# cannot write as a constant (`Vector3.ONE`) makes forwarding overloads instead; `...rest` is `params Variant[] restArgs` and an `Array` named `rest` inside |
| `_init(args)` | a constructor; defaults give the parameterless constructor the runtime needs; `super(args)` in it is dropped (`approximated`) |
| `_ready`, `_process(delta)`, `_physics_process`, `_input(event)`, `_unhandled_input`, `_enter_tree`, `_exit_tree`, `_notification` | `public override void _Ready()`, `_Process(double delta)`, ... with the runtime's signatures |
| other functions | `public` (`virtual` when a descendant script overrides it); `_name` becomes `Name` (leading underscore dropped, as the scenes' `_connections` lookup does); a function without `-> T` takes the type its `return`s agree on, else `Variant`; `void` when it returns nothing |
| `@rpc("any_peer", ...)` | `[Rpc("AnyPeer")]` (`approximated`: nothing is networked) |
| `await sig`, `await obj.sig`, `await get_tree().create_timer(t).timeout` | `await ToSignal(owner, Owner.SignalName.X)` (`ToSignal(obj, "name")` for a signal the runtime does not list); the value of `await` is `GDScript.AwaitResult(...)` (null, the argument, or an Array). A function that contains `await` is `async void` (an override or a handler) or `async Task` / `Task<T>` when another script awaits it |
| `x.connect(f)`, `.connect(f, CONNECT_ONE_SHOT)`, `.disconnect(f)`, `.is_connected(f)`, `.emit(a)` | `x.Connect(Name, callable, (uint)flags)`, `Disconnect`, `IsConnected`; our own signal: `EmitSignalX(a)`, another object's: `EmitSignal(Name, a)` |
| a function as a value (`f`, `self.f`) | `Callable.From<A, B>(F)` (typed); by name with `MethodName.F` when it has defaults or `...rest` |
| `Callable(self, "m")`, `f.bind(1)`, `f.call(1)`, `callv`, `call_deferred` | `new Callable(this, MethodName.M)`, `.Bind(1)`, `.Call(1)` (a `Variant`), `.Callv(a)`, `.CallDeferred(...)` |
| `func(x): return x + 1`, a multi-line `func(x):` | `Callable.From((Variant x) => GDScript.Add(x, 1))`, `Callable.From(() => { ... })`. Typed parameters stay typed; a lambda with defaults is `(Delegate)(...)` so the Callable fills them in; a named lambda that calls itself is `GDScript.Named(name => ...)`; `await` inside makes it `(Action)(async () => ...)` |
| `array.map/filter/reduce/any/all/sort_custom(func ...)` on a typed array | C# lambdas: `items.Map((long i) => i * 2)`, `.Filter`, `.Reduce`, `.Any`, `.SortCustom((long a, long b) => a > b)`; on an untyped array the Callable overloads |
| `if/elif/else`, `while`, `break`, `continue`, `pass` | the same; a condition is a `bool` by the value's truthiness (below) |
| `x is T` | `x is T`; in an `and` chain or an `if` it binds a pattern variable the rest of the chain and the branch use (`@event is InputEventMouseMotion inputEventMouseMotion && inputEventMouseMotion.Relative.X`); on a Variant `.AsGodotObject() is T` or `.VariantType == Variant.Type.X` |
| `x as T` | `x as T`; `GetNode<T>(...)` for `get_node(..) as T`; a conversion for built-in types |
| `for i in range(n)`, `(a, b)`, `(a, b, step)` | `for (long i = 0; i < n; i++)`; `i += step` / `i -= step` for a literal step; a bound that can change (`items.size()`, a field) is read once (`for (long i = 0, iEnd = items.Count; i < iEnd; i++)`). A loop that assigns its variable, or has a computed step, uses `foreach (long i in GDScript.Range(...))` |
| `for x in int`, `in array`, `in dict`, `in "text"`, `in <Variant>` | as `range`; `foreach (T x in array)`; `foreach (K k in dict.Keys)`; `foreach (string c in GDScript.Chars(s))`; `foreach (Variant x in GDScript.Iterate(v))` |
| `match v:` with literals, constants, enum members, `_`, a final `var x` | a `switch` (int, string or enum subject, no guard, and no `break` that is meant for a loop around it), else an if / else-if chain on a subject evaluated once |
| `match` with `var x`, `when`, `[a, ..]`, `{"k": v, ..}`, nested | the chain: `x is var v`, `&& guard`, `GDScript.AsArrayOrNull(x) is { } array && array.Count >= 1 && array[0] is var a`, `GDScript.AsDictOrNull(x) is { } dict && dict.ContainsKey(...)` |
| `a if c else b` | `c ? a : b` (types joined; null with a value type makes a Variant) |
| `assert(c, m)` | `GDScript.Assert(c, m)` (logs, does not throw) |
| `$Path`, `$"A B/C"`, `%Name`, `$A/%B` | `GetNode<T>("Path")` with T from the declared type or a cast, else `GetNode("Path")` (a `Node` the code may call dynamically) |
| `Foo.new(args)`, `Vector3(1, 2, 3)`, `Array[int]()` | `new Foo(args)` (parameters from `_init`), `new Vector3(1f, 2f, 3f)`, `new Collections.Array<long>()` |
| `preload("res://a.tscn")`, `load(...)` | `GD.Preload<PackedScene>(...)`, `GD.Load<T>(...)`; T by extension (`PackedScene`, `Texture2D`, `AudioStream`, `Mesh`, `Material`, `Shader`, `Font`, else `Resource`) |
| an autoload name (`Global`) | `GetNode<Global>("/root/Global")` (typed by its script) |
| `self`, `super.f()`, `super()` | `this`, `base.F()`, the same function of the base class; a missing base function logs an error when it runs (`unsupported`) |

### Operators and semantics

| GDScript | C# |
|---|---|
| `a / b`, `a % b` on ints | the same: integer division truncates toward zero and `%` takes the dividend's sign (GDScript's own rule). Division by zero throws `DivideByZeroException` where Godot reports a script error |
| `a ** b` | `GDScript.IntPow(a, b)` for two ints (a negative exponent gives 0), `Math.Pow` for floats |
| `<<`, `>>`, `&`, `\|`, `^`, `~` | the same (`1 << 40` is written `1L << 40`; two literals whose result does not fit an int are written folded) |
| `"fmt %s" % x`, `% [a, b]` | `GDScript.Format("fmt %s", x)` / `(..., new Collections.Array { a, b })`: `%s %d %f %x %X %o %c %e %v %%`, flags `- + 0 space`, width, `.precision`, `*` |
| `a in b`, `a not in b` | `b.Contains(a)` (array, string), `b.ContainsKey(a)` (dictionary), `GDScript.In(a, b)` for a Variant |
| `==`, `!=` | `==` on numbers, strings, enums of one type, structs, references; `Variant ==` otherwise (Godot's equality: `1 == 1.0`, a String equals a StringName) |
| `<`, `<=` on strings | `string.CompareOrdinal(a, b) < 0` |
| `and`, `or`, `not` | `&&`, `\|\|`, `!` on truthiness |
| truthiness of `x` | int `x != 0`, float `x != 0.0`, String `!string.IsNullOrEmpty(x)`, Array / Dictionary `.Count > 0`, vector `!= Vector3.Zero`, a Godot object `IsInstanceValid(x)` (a freed node is false), another object `!= null`, Variant `.Booleanize()` |
| arithmetic on a `Variant` | `GDScript.Add/Sub/Mul/Div/Mod/Pow/Neg/BitAnd/...` with Godot's rules (int + float is float, String + String, vector * number, Quaternion * Vector3, Array + Array); comparisons `GDScript.Lt/Le/Gt/Ge` |
| arithmetic on vectors, colours, transforms | the C# operator the runtime type declares, found by overload (a scalar is cast to `float`) |
| `v.x = 1`, `velocity.y -= g * dt` where the vector is a property | `Velocity = Velocity with { Y = (float)(Velocity.Y - g * dt) };` (a field of a struct property is written back whole; of a local or a field it is assigned in place) |
| `d.key`, `d.key = v` | `d["key"]`, `d["key"] = v` |
| `a[i]` | `a[(int)i]`, `a[^1]` for a negative literal; on a Variant `GDScript.Index(a, i)` / `SetIndex` |

### Built-in functions and methods

`print` / `prints` / `printt` / `print_rich` / `printraw` / `printerr` / `push_error` / `push_warning` are `GD.*`; `str` is `GD.Str`; `len` is `.Length` / `.Count` / `GDScript.Len`; `range` as a value is `new Collections.Array<long>(GDScript.Range(...))`; `int()` / `float()` / `bool()` / `String()` convert by the argument's type. `abs` `sign` `floor` `ceil` `round` `sqrt` `pow` `sin` `cos` `tan` `asin` `acos` `atan` `atan2` `exp` `log` `fmod` are `Math.*` (`round` half away from zero) or `GDScript.Sign`; `min` / `max` / `clamp` (and the `i` / `f` forms) are `Math.Min/Max/Clamp` by type, a Variant uses `GDScript.Min/Max`; `lerp` is `Mathf.Lerp` for numbers and `a.Lerp(b, (float)t)` for vectors and colours; `lerp_angle`, `inverse_lerp`, `remap`, `move_toward`, `smoothstep`, `deg_to_rad`, `rad_to_deg`, `ease`, `snappedf`, `is_equal_approx`, `is_zero_approx` are `Mathf.*`; `posmod`, `wrapi` / `wrapf`, `snapped`, `pingpong`, `fposmod`, `linear_to_db`, `db_to_linear` are `GDScript.*`; `floori` / `ceili` / `roundi` are `(long)Math.*`; `randf` / `randi` / `randf_range` / `randi_range` / `randfn` / `randomize` / `seed` are `GD.*` (the G1 PCG32); `typeof` `char` `ord` `type_string` are `GDScript.*`; `is_instance_valid` is `IsInstanceValid`; also `instance_from_id`, `hash`, `var_to_str`, `str_to_var`, and `weakref` (a strong reference, reported). `Engine`, `OS`, `ProjectSettings` are the Aver.Fabric classes below.

Methods of the built-in types: String (`length`, `is_empty`, `to_upper`, `to_lower`, `begins_with`, `ends_with`, `contains`, `find`, `rfind`, `replace`, `strip_edges`, `split`, `substr`, `left`, `right`, `to_int`, `to_float`, `is_valid_int`, `is_valid_float`, `capitalize`, `pad_zeros`, `repeat`, `format`, `join`, `get_file`, `get_base_dir`, `get_extension`, `to_snake_case`, `to_pascal_case`, `unicode_at`), Array (`size`, `is_empty`, `append`, `push_back`, `push_front`, `pop_back`, `pop_front`, `has`, `find`, `erase`, `remove_at`, `insert`, `clear`, `resize`, `front`, `back`, `reverse`, `shuffle`, `sort`, `sort_custom`, `pick_random`, `duplicate`, `slice`, `min`, `max`, `count`, `fill`, `append_array`, `map`, `filter`, `reduce`, `any`, `all`), Dictionary (`size`, `is_empty`, `has`, `get`, `erase`, `clear`, `keys`, `values`, `merge`, `duplicate`), Callable, Signal. Anything else on these types is left under its PascalCase name and shows up as a compile error.

## What was added to Aver.Fabric for this

All in `scripting/csharp/Aver.Fabric/Core/` (API list in `docs/api/Aver.Fabric.md`):

- `GDScript` (`GDScript.cs`): the Variant operators and comparisons, `IntPow`, `PosMod` / `FPosMod` / `Wrap` / `Snapped` / `Sign` / `PingPong` / `LinearToDb` / `DbToLinear`, `Format` (the `%` operator), `Range`, `Iterate`, `Chars`, `Len`, `TypeOf`, `Char` / `Ord`, `ToInt` / `ToFloat`, `Index` / `SetIndex`, `GetMember` / `SetMember` / `CallMethod` (the dynamic fallbacks: Array, Dictionary and String methods by their Godot names, objects through `Call`, math types by reflection), `AwaitResult`, `ToSignal(Variant)`, `Named`, `Assert`, `AsArrayOrNull` / `AsDictOrNull`.
- `StringExtensions`, `ArrayExtensions`, `DictionaryExtensions` (`GDScriptExtensions.cs`): GodotSharp's string methods and GDScript's Array / Dictionary methods that C# lacks (`Substr`, `Left`, `StripEdges`, `Split(delimiter, allowEmpty)`, `Find`, `ToInt`, `PopBack`, `PushFront`, `Front`, `Back`, `Count(x)`, `Map`, `Filter`, `Reduce`, a stable `SortCustom`, `Get(key, default)` ...). GodotSharp scripts get them too.
- `Engine` (`IsEditorHint()` is false), `OS` (`GetName`, `HasFeature`, `GetTicksMsec`), `ProjectSettings.GetSetting` (Godot's gravity, tick rate, name and window size from the Aver manifest, the caller's default for the rest) in `Singletons.cs`. They shadow `Aver.Framework`'s statics of the same names inside `namespace Aver.Fabric`; framework code that means its own writes the namespace.
- `Callable` now fills a delegate's optional parameters (lambdas with defaults).

## Report entries

`type` `gdscript` is the summary of a script (counts and the verdict; `converted` when it compiles and nothing below applies). Then: `parse` (a syntax error, `GDScript line:col: message`, the statement or member left out; `unsupported`, the script `approximated`), `attribute` (`Tool` unsupported, `Rpc` approximated), `dynamic` (approximated: read, written or called by name when it runs), `identifier` / `function` (unsupported: a name nobody declares, read or called by name), `member` / `call` / `constructor` (unsupported: the runtime has no such member, method or constructor), `variant` (approximated: an operator on typed operands that only the Variant rules can do), `await` (approximated: the operand is not a signal or a coroutine of this project, so it does not wait), `CSxxxx` (a compile error, as for GodotSharp). `line` is the line of the converted file, and that line carries a `// FABRIC: reason` comment.

## Verified, and limits

Run headless in `Aver.Fabric.Tests` (`GdScriptConvertTests`, 252 checks; `dotnet run -c Debug -- gdconvert-tests` runs this suite alone, ctest `FabricManagedTests` runs it with the rest):

- **Compile.** The ten F4a fixtures that parse (`02_declarations` to `09_strings`, the realistic `player_controller.gd` and `ui_menu.gd`), plus the new `base/base_actor.gd` and `base/outer.gd` (the base class and the `Outer.Inner` the fixtures name) and `behaviour/calc.gd`, `counter.gd`, `enemy.gd`, are converted as one project and compile with **zero errors**. The two realistic scripts use eight classes Aver.Fabric does not have yet (`AudioStream`, `AudioStreamPlayer`, `AudioStreamPlayer3D`, `AnimationPlayer`, `Button`, `HSlider`, `TextureRect`, `AudioServer`), so **the compile check supplies stand-ins for exactly those** (`ConvertSettings.ExtraSources`; the `Stubs` string in the test). Without them these scripts report those classes as compile errors, which is the intended signal for the runtime work. `10_errors.gd` converts what parsed and reports its six syntax errors.
- **Readability.** `player_controller.gd` converts with no `Variant` and no run-time helper in the text.
- **Each row of the table** above on a small script, compiled, with the text checked (`gdscript map: ...`).
- **Behaviour.** `Calc` runs through the compiled assembly and is compared with what GDScript's rules give, worked out by hand (there was no Godot to compare against): `match` with literals, lists, defaults, and array patterns with bindings and guards; `for` over `range` with steps; fizzbuzz; `%` formatting with width, precision and flags; integer division and `%` on negatives; `**`; the number built-ins; dictionaries; typed arrays; `map` / `filter` / `reduce` / `sort_custom`; string comparison and indexing; Variant arithmetic; truthiness. `Counter` and `Enemy` run as nodes in the managed loop: exports, `@onready`, a setter that emits, `await` on a `Timer` and on `create_timer`, `_process`, groups, a constructor-assigned field, `queue_free`.
- **CLI.** `scripts.json` with `.gd` entries (a class renamed by the manifest), the report merge and a rerun that changes nothing, the scan fallback on `ConvertGame`, exit 1 with the output written and its lines marked, exit 3, and `project.godot`'s autoload list reaching the converter.
- **Runtime.** `GDScript`, the singletons and the extension methods have direct tests. Damaged copies of the fixtures (a line removed, five removed, a line cut in half, a line doubled; fixed seed) convert without the converter failing.

Not done, or different from GDScript:

- Godot 3 syntax (the F4a parser rejects it). `@abstract`, `trait`, `namespace`.
- GDScript's `int` is `long` here, but a value the API returns as `int` is an `int` until it meets a `long`; arithmetic on those wraps at 32 bits. Integer division by zero throws.
- Untyped parameters are `Variant` (or the type of a literal default), and the body then goes through `GDScript.*` where types are not known. Nothing infers a parameter from its call sites.
- A member variable is typed from its initialiser and the assignments in its own script. Another script assigning it a different kind (`other.score = "x"`) is not seen.
- `get_node` without a type gives a `Node` the converter treats as a guess: a property or method the C# `Node` lacks becomes a read-by-name (`approximated`). Scene knowledge (the real node type) is not used.
- `await` of a value that is neither a signal nor a project coroutine does not wait. A coroutine called without `await` runs on, as in GDScript, but its result is a `Task`.
- `super(args)` in `_init` cannot reach the base constructor. `_get` / `_set` / `_get_property_list` overrides, `set_script`, `@export` arrays of nodes, `for` over an object with `_iter_init`, and an inner class reading its outer instance's variables are not converted.
- A signal handler with default parameters is connected by name (it works); a lambda is called with exactly the signal's arguments unless it has defaults.
- The editor's per-instance panel does not list Fabric `[Export]` members (as for GodotSharp).
- Nothing ran in the engine (engine-run ban): the converted classes ran in the managed node host only.
