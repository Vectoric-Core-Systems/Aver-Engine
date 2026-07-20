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
| `logo.png` | 512×512 RGBA | Not referenced by code. General-purpose mark. |
| `icon512.png` | 512×512 RGBA | Not referenced by code. |
| `logo256.png` | 256×256 RGBA | Not referenced by code. |

The transparent marks are keyed by flood-filling from the corners, **not** by replacing the
background colour globally: the cube's own outline is near-black and close enough to the #262626
banner that a global replace punches holes through it.

## AI-generated — `ai-generated/`

Written by Claude before the human artwork existed. **None is referenced by the build or by any
code path**, so nothing here ships. Kept rather than deleted because they are the only vector
sources — the master is raster-only, so the shipped marks above have no SVG equivalent.

| File | Notes |
|------|-------|
| `ai-generated/logo.svg` | Superseded by the master. |
| `ai-generated/wordmark.svg` | Superseded by the master. |
| `ai-generated/ae-mark-isocube.svg` | The concept the master's mark descends from. |
| `ai-generated/ae-mark-ligature.svg` | Rejected concept. |
| `ai-generated/ae-mark-monogram.svg` | Rejected concept. |
| `ai-generated/ae-mark-velocity.svg` | Rejected concept. |

**If a vector version of the real mark is ever needed, it has to be authored from the master — do
not promote one of these back into the shipped set.**

## Other AI-authored visual work, for completeness

Not assets, so not files, but they are front-facing and were designed by Claude rather than by a
human. Recorded here because "no AI-generated assets" would otherwise be misleading:

- The editor's dark theme — every ImGui colour, spacing and rounding value in `sandbox/src/SandboxApp.cpp`.
- The default editor layout (panel arrangement and dock split ratios).
- The viewport gizmo geometry and its axis colour convention (X red / Y green / Z blue).
- The procedural sky and its default zenith/horizon/fog colours.

## Regenerating

```powershell
python scripts/brand.py
```
Reads `branding/master-lockup.png`, writes every derived slot listed above.
