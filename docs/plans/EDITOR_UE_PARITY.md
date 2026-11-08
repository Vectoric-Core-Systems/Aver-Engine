# Editor towards Unreal 5: polish, UI designer, particle editor

Plan from a read-only survey (2026-11-07) of the editor (`sandbox/src`), `modules/ui` and `modules/particles` against UE5. The rule stands: upgrade what exists, never build a parallel system. Each track is in steps, and each step is approved before it runs.

## What the survey found

- **UI designer: one exists.**
  - Engine side: a retained widget tree (`modules/ui`: anchors, stacks, grid, DPI modes, focus and gamepad navigation, themes and 9-slice, localisation), the `.ocui` asset, C# `Aver.UI`, and graph nodes.
  - Editor side: `UiLayoutEditor` has a palette, hierarchy, canvas with resize handles, inspector, undo and culture preview.
  - Missing against UMG: widget animation, property bindings, user widgets (nested layouts), most panel types (Overlay, WrapBox, SizeBox, ScaleBox, Border, Spacer, WidgetSwitcher), per-state styling, designer event hookup, and 3D widgets.
- **Particles: the runtime is mostly built**, including the GPU tier: 8 emitters per system, 21 modules, user parameters, events, sprites, ribbons and meshes. An editor exists with an emitter list, a module stack with enable/solo/mute, and a preview.
  - Missing, mostly editor: a stats panel, playback speed and seek, graph curve and gradient editors, a better preview (bounds, a moving emitter, ground and background), templates, a live user-parameter panel, scalability tiers, and event payloads.
- **Editor chrome:** one font size, a small icon set (about 30 Material Icons), a slightly blue-grey theme with Aver orange, a Details panel whose labels clip, two close buttons per tab, heavy blue viewport-overlay borders, and low-contrast secondary text (about 3.7:1).
  - Missing panels: Place Actors, Message Log, Undo History. Outliner type icons, eye and lock columns, and folders are also missing.
  - The Content Browser has no filters, collections or real thumbnails.

## Tracks

### A. Editor polish
Steps 1-4 give most of the visible change for the least risk. Each step is checked with screenshots at 100% and 150% DPI, and the gates stay green.
1. Theme as data (`theme.json`), the neutral ladder untinted, textDim raised to about 0.62 (AA contrast), and flat viewport overlays. Orange is kept for selection and Play only.
2. Type scale: 18 for headers, 16 for body, 13-14 for secondary text. Medium for headers.
3. Icons everywhere: grow `EditorIcons.hpp` to about 110 glyphs from the existing Material Icons font; menus, toolbar, tabs, drawers.
4. Panel chrome: one close button per tab, no collapse arrow, splitters, toolbar capsules, a wider right column, layout reset.
5. Details rebuild: resizable label/value table, search, reset-to-default arrows, modified marks, and X/Y/Z in red, green and blue.
6. Outliner: type icons, eye and lock columns, folders, a type filter.
7. Content Browser: filter chips, collections, favourites.
8. Toasts and status bar: stacked toasts with icons and actions; a secondary font and a background-task progress area.
9. New panels: Place Actors palette, Message Log, Undo History.
10. Viewport chrome: one options bar, a grid and snap strip, viewport layouts.
11. Real asset thumbnails with a capped disk cache.

Note: `FramePadding.y` is pinned by the `--probe-rel` gates (the tab-bar height). Row height changes are pushed locally inside panels unless the gates are re-based.

### B. UI designer (UMG)
1. Baseline: run the UI tests, open the designer through `aver_session`, fix any breakage, and update the stale `docs/GAME_UI.md` status.
2. `.ocui` v2: nested-layout reference (`USE`), plus `ANIM`, `BIND` and `EVENT` records. The v1 loader is kept, and unknown records are already skipped.
3. Panel widgets: Overlay, UniformGrid, WrapBox, SizeBox, ScaleBox, Border, Spacer, WidgetSwitcher. Port the useful container maths from Fabric's Godot containers.
4. User widgets: instantiate referenced layouts with exposed properties and named slots, with cycle detection.
5. Widget animation: tracks over opacity, colour, offset and transform; play, reverse, loop, a finish event. Widget transforms may need draw-list support (check first). **Done** (the draw list already had a transform stack). Hit testing, hover, drag and focus navigation honour the render transform too (2026-11-09).
6. Bindings and events: property paths against a C# view-model or a graph variable. Events go to a command, a graph event or a C# method. **Done.**
7. Designer: animation timeline, bindings column, events tab, palette search, device presets and safe zones, multi-select, per-state style editing, theme editor. **Done**, including the theme editor (`.octheme` sheets with live preview; needs a headed check).
8. Runtime: player-scoped viewport stacks, an activatable-widget stack, gamepad rebinding.
9. 3D widget component: a tree rendered to a texture or quad, with ray-picked input. Heaviest, last.

### C. Particle editor (Niagara feel)
Steps 1-7 are editor-only.
1. Stats panel: alive/max per emitter, a CPU/GPU tier badge, GPU memory.
2. Playback: speed, frame step, seek (resimulate from 0 with the fixed seed), loop, time readout.
3. Graph curve editor and gradient bar over the existing curves.
4. Per-module isolate.
5. Preview: bounds, a moving emitter (for trails and inherited velocity), ground and background toggle.
6. Templates: about 10 neutral starters (smoke, sparks, rain, snow, embers, dust, mist, trail, debris, flipbook burst), plus module presets.
7. Live user-parameter panel.
8. Scalability tiers per emitter (format v2 extension).
9. Event payloads and more triggers.
10. Later: depth/scene collision, mesh surface sampling, system-level and scripted modules.

## Open questions, per track (asked when the track starts)

- **B:** bindings graph-first, C# or both; whether panels are new kinds or layout modes (new kinds touch three ABI mirrors); widget rotate/scale; whether 3D widgets are in v1.
- **C:** per-effect or global scalability; whether there is a system-level stage; preview lighting fixed or following the viewport; scripted modules or a closed table.
- **A:** whether the content browser, log and console stay drawers or become docked tabs; detachable OS windows; re-basing the gates for row height.
