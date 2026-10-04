# Branding assets — provenance

Every visual asset in the repo is listed here, with who authored it. The point of the split is that
anything shipped in the product should be traceable to a human author, and anything AI-generated
should be obvious rather than discovered later.

## Master

| File | Author | Notes |
|------|--------|-------|
| `master-lockup.png` | **Human (project owner)** | 1920×1080. The authoritative artwork: isocube AE mark + "Aver Engine" wordmark on a #262626 banner, with white margin to the right and below. |

Every shipped asset below is a **crop or rescale of this master** — nothing is redrawn, so the marks
that ship are the author's own. Regenerate them with `scripts/brand.py` (see below) if the master
changes; do not hand-edit the derived files, they will be overwritten.

## Shipped — derived from the master

| File | Slot | Consumed by |
|------|------|-------------|
| `splash.png` | 1200×520 RGB | Startup splash. `sandbox/CMakeLists.txt` copies it next to the exe; `modules/runtime/src/Engine.cpp` shows it as a layered window **at native size**, so changing the dimensions changes its on-screen size. |
| `icon.ico` | 16/24/32/48/64/128/256 | Window + exe icon, via `sandbox/Sandbox.rc`. The full size ladder is supplied deliberately: left to rescale 256→16 itself, the shell turns the mark to mush in the taskbar. |
| `logo.png` | 512×512 RGBA | The editor's start screen. `sandbox/CMakeLists.txt` copies it next to the exe; `SandboxApp` decodes it and uploads it through the RHI, and `ProjectBrowser` blits it in the header. Shown at 46dp, so the transparent margin is part of the composition — do not crop it tighter. |
| `icon512.png` | 512×512 RGBA | Not referenced by code. |
| `logo256.png` | 256×256 RGBA | Not referenced by code. |
| `compile-status.png` | 768×256 RGBA, 3 tiles | The Compile C# toolbar button's status icon. **Human-made** (user-provided source, sliced — not AI-generated): the three states — built (green tick), failed (red no-entry), stale/compiling (yellow `?`) — auto-cropped from the user's single graphic, background flood-keyed to transparent, assembled left-to-right. `sandbox/CMakeLists.txt` stages it next to the exe; `SandboxApp::loadCompileIcon` uploads it and `ToolsMenu::drawCompileButton` blits tile 0/1/2 by UV onto the button face. Regenerate with `scripts/make-icon-sheet.ps1` (below). |
| `compile-status.source.png` | 1920×1080 RGB | The author's original of the three states, kept so the sheet can be re-cut at another size without going back to the user. Not referenced by code. |
| `compile-status.legacy.png` | 480×160 RGBA, 3 tiles | The **previous** status sheet, superseded July 2026 by the artwork above. Kept unreferenced, on request, in case the older look is wanted back — restoring it is a rename, no code change (the button slices by UV fraction, so tile size does not matter). |
| `file-icons.png` | 1284×432 RGBA, 4 **portrait** tiles (321×432) | The Content Browser's file-type icons — C# Script / C# Class / C++ Class / C++ Module. **Human-made** (user-provided source, sliced). Landed in `5021bad`; the slicing process was not recorded, and it predates `make-icon-sheet.ps1` — which cannot reproduce it as shipped without `-TileW 321 -TileH 432`. `SandboxApp::loadIconSheet` uploads it and `drawEntryIcon` blits tiles by UV. |
| `folder-icons.png` | 512×206 RGBA, 2 tiles | The Content Browser's folder icons — plain folder, and the "Module" folder used for engine content and C++ modules. **Human-made** (user-provided source, sliced). Cut from `folder-icons.source.png` with `make-icon-sheet.ps1` (below). |
| `folder-icons.source.png` | 1920×1080 RGB | The author's original of the two folder states, stacked vertically. Not referenced by code. |

The transparent marks are keyed by flood-filling from the corners, **not** by replacing the
background colour globally: the cube's own outline is near-black and close enough to the #262626
banner that a global replace punches holes through it.

## Generated — drawn by a script, and it ships

Listed apart from the human artwork above deliberately. The rule this file exists for is that
anything shipped be traceable to its author; the honest way to add an icon nobody had drawn was to
draw a **new sheet** rather than a fifth tile inside `file-icons.png`, which would have quietly
mixed machine-drawn art into the project owner's own.

| File | Slot | Consumed by |
|------|------|-------------|
| `asset-icons.png` | 963×432 RGBA, 3 **portrait** tiles (321×432) — ANIM / SKELETON / MESH | The Content Browser's icons for `.ocanim`, `.ocskel` and `.ocmesh`, which previously fell back to the C# script tile. **Machine-drawn by `scripts/make-asset-icons.py`** — a running figure with motion streaks, a bone chain, an isometric cube. Requested by the project owner ("make some running look for it"). |
| `player-start-icon.png` | 256×256 RGBA, single image | The Player Start's marker in the **3D viewport** — the only entry in this file that is not UI chrome. `sandbox/CMakeLists.txt` stages it next to the exe; `SandboxApp` hands it to `editor::ViewportIconRenderer`, which draws it on a camera-facing quad in `transparentPass` so it is occluded by level geometry. **Machine-drawn by `scripts/make-editor-icons.py`** — a teal map pin with a standing figure, chosen so it collides with nothing already in a viewport (the gizmo owns red/green/blue, the selection outline owns orange). Two nested rims, light inside dark, so the silhouette reads against both a bright sky and dark interiors. It REPLACES a plain white cube; that cube still draws if this file is missing. |

The house style is **matched from measurements of `file-icons.png`, not guessed** — page `#999999`,
banner `#262626` showing through a folded corner at (232,0)–(320,0)–(320,88) with a `#4D4D4D` crease,
a pointy-top hexagon badge 245×265 centred at (159,215), and a Roboto Medium caption centred on
y=398. Everything is drawn at 4× and downsampled, which is where the antialiasing comes from. Rerun
the script to change it; do not hand-edit the sheet.

That house style applies to `asset-icons.png` alone. **`player-start-icon.png` is drawn against
different constraints and deliberately shares none of it**: there is no tile for it to sit on, so it
has to read as a transparent silhouette; it is composited over whatever the level looks like, so it
carries a dark rim outside a light one rather than a single outline; and it is centred on a world
position, so a map-pin shape says "at the tip" where a circular badge would say "somewhere in here".
It is drawn at 4× and downsampled like the sheet, and it is likewise regenerated, not hand-edited --
`python scripts/make-editor-icons.py`.

The badge hues are new on purpose — amber, teal and green against the human sheet's purple (C#) and
blue (C++) — so an asset type is distinguishable from a source file at a glance in a mixed folder.

## Removed: the AI-generated vector concepts (2026-10-04)

Claude's SVG concepts (`logo.svg`, `wordmark.svg` and four `ae-mark-*.svg`) were written before the
human artwork existed and were referenced by nothing. They were deleted at the owner's request; git
history still holds them. The master is raster-only, so **a vector version of the real mark, if
ever needed, has to be authored from the master.**

## Other AI-authored visual work, for completeness

Not assets, so not files, but they are front-facing and were designed by Claude rather than by a
human. Recorded here because "no AI-generated assets" would otherwise be misleading:

- The editor's dark theme — every ImGui colour, spacing and rounding value in `sandbox/src/SandboxApp.cpp`.
- The default editor layout (panel arrangement and dock split ratios).
- The viewport gizmo geometry and its axis colour convention (X red / Y green / Z blue).
- The procedural sky and its default zenith/horizon/fog colours.
- The start screen's **fallback** badge — a rounded orange square lettered "AE", drawn with ImGui
  primitives in `sandbox/src/ProjectBrowser.cpp`. It is not the mark and is not an approximation of
  it: it appears only when `logo.png` is missing or will not decode, because a decoration must never
  stop the editor coming up. When the file is there, the human artwork above is what ships.

## Regenerating

```powershell
python scripts/brand.py
```
Reads `branding/master-lockup.png`, writes every derived slot listed above.

### Icon sheets

`compile-status.png` and `folder-icons.png` are cut from a single supplied graphic by

```powershell
powershell -File scripts/make-icon-sheet.ps1 -In "<source.png>" -Out "branding/compile-status.png" -Tiles 3 -TileW 256 -TileH 256
```

```powershell
powershell -File scripts/make-icon-sheet.ps1 -In "<source.png>" -Out "branding/folder-icons.png" -Tiles 2 -Layout Rows -TileW 256 -Align PerTile
```

It splits the source into `-Tiles` equal columns (or rows, with `-Layout Rows`), keys the flat
background to transparent, crops to the artwork, and writes the tiles side by side — the output is
always a horizontal strip, because that is what the renderers slice. Four details are deliberate:

- **The background is flood-filled from the border**, not colour-replaced globally — the same lesson
  recorded above. The stale badge's `?` is nearly the background's own colour, so a global replace
  punches it straight out and the badge ships hollow.
- **`-Align Shared`** (the default) uses one crop window for every tile, so the compile badge's three
  states keep their true relative size and position and the button does not twitch as it changes.
  **`-Align PerTile`** gives each icon a common window *size* centred on its own bounds — right for
  the folders, which the artist did not align with each other in the source.
- **Tile height is derived from the artwork** unless `-TileH` pins it, and the crop window is grown
  to the output tile's aspect before padding, so a sheet is never itself the reason an icon looks
  stretched. (`file-icons.png` is portrait 321×432 for exactly this reason — re-cut it with
  `-TileW 321 -TileH 432`, never with square tiles.)
- **The sample window is clamped to each tile's own source cell**, so artwork tall enough to need a
  crop wider than its column cannot drag a sliver of the neighbouring icon in with it.

`SandboxApp::loadIconSheet` measures each sheet's tile aspect from the decoded image rather than
assuming one, so a re-cut at another shape renders correctly instead of silently stretched.
