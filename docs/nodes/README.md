# Aver Node reference (generated)

One page per node family. The pages are generated from the C# node tables and the editor palette, so a pin
list here cannot disagree with the code: if it did, `NodeCatalogCheck` would fail.

| Page | What it lists |
|---|---|
| `<Family>.md` | The table-driven nodes of one family (`scripting/csharp/Aver.Graph/Graph<Family>NodeTable.cs`): type, display name, palette category, path (D pure, P exec, X trigger), pins with defaults, attributes (optional ones marked), and the C# method each calls with the first sentence of its XML summary. `GameSystem.md` is the main table; the rest are one family each. |
| [`Core.md`](Core.md) | Every node the parser and compiler know by hand (the `OcGraphParser.AddDefaultPins` vocabulary) and every palette entry that has no table row, read from the palette: type, display name, category, pins, attributes. The material-graph vocabulary is a separate section at the end. |

**Do not edit these pages.** To change one, change the C# row, the palette entry or the owner method's XML
summary, then run:

    python scripts/node-catalog.py --write

Notes that belong above a family's table (a caveat, an enum legend) go in `docs/nodes/<Family>.notes.md`, which is
included verbatim. This README and `*.notes.md` are the only hand-written files in this folder.

## Commands

| Command | What it does |
|---|---|
| `python scripts/node-catalog.py --check` | Exit 1 on any disagreement (ctest `NodeCatalogCheck`). |
| `python scripts/node-catalog.py --write` | Regenerate every page here. |
| `python scripts/node-catalog.py --counts` | Print the table, palette and parser counts. Use this instead of writing a count in a document. |
| `python scripts/node-catalog.py --scaffold-palette Family --category Cat` | Print a `GraphNodeDefs<Family>.hpp` body from the family's C# rows, to be hand-tuned (display names, category). |

## What `--check` holds

1. Every `Reg(` row parses.
2. Every row has a palette entry whose `typeId` equals the row key (any case).
3. The pins match the palette entry: name, type, direction, order and default values. A bare `NODE` line gets
   exec and then (Exec rows), the inputs, the outputs; a Trigger row is a single exec output.
4. The palette's attribute keys match the row's `@` tokens (a palette key may be any alternative of `a|b`; trigger rows are skipped).
5. A palette entry in a family header without a row is a node the parser knows by hand.
6. Keys are unique across the tables, the palette and the parser's `case` labels (a table key equal to a parser
   label would never be reached).
7. A family that is not one of the original eight keeps its keys lower case and its `typeId` to letters, digits and underscore, with a category.
8. These pages are current.
9. A family that is not one of the original eight has `scripting/csharp/Aver.Graph.Tests/<Family>NodeTests.cs`, and
   that class is marked `[AutoRun]` or run from `Program.cs`.

## Adding a node family

Add `Graph<Family>NodeTable.cs` (a `private static GenericNodeSpec[] <Family>Table()` in `partial class GameSystemNodes`),
`sandbox/src/GraphNodeDefs<Family>.hpp` (`inline void append<Family>NodeDefs(std::vector<GraphNodeDesc>& t)`) and
`Aver.Graph.Tests/<Family>NodeTests.cs` (`[AutoRun]`, built from `NodeRowKit`). No shared file changes: the C# side
finds the table by reflection, CMake finds the header (`cmake/GraphNodeFamilies.cmake`), the test runner finds the
class. Re-run cmake once so the palette include list is regenerated, then `--write`. The full recipe is in
`docs/VISUAL_SCRIPTING.md`, "Adding a node family".
